#include "am/services/import/zip_support.hpp"
#include "am/services/import/exclusion_rules.hpp"
#include "am/foundation/signature.hpp"
#include "am/foundation/unicode.hpp"
#include "detail.hpp"
#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>

namespace am::import {
using J=PlanJson;
PlanLimits snapshot_import_limits(const ImportProfile& profile) {
  // Missing optional profile settings inherit the existing default.toml hypotheses.
  PlanLimits out{100000,214748364800ULL,3600,107374182400ULL,200};
  const auto& p=profile.settings;
  if(!p.contains("limits")) return out;
  const auto& l=p.at("limits");
  out.max_entries=l.value("max_entries",out.max_entries);
  out.max_unpacked_bytes=l.value("max_unpacked_bytes",out.max_unpacked_bytes);
  out.max_seconds_per_container=l.value("max_seconds_per_container",out.max_seconds_per_container);
  out.max_temp_bytes=l.value("max_temp_bytes",out.max_temp_bytes);
  out.max_ratio=l.value("max_ratio",out.max_ratio);
  return out;
}
archive::ZipInputLimits archive_limits(const PlanLimits& p) {
  archive::ZipInputLimits out;
  out.zip.max_entries=p.max_entries;
  out.max_unpacked_bytes=p.max_unpacked_bytes;
  out.max_ratio=p.max_ratio;
  out.max_seconds_per_container=std::chrono::duration<long double>(static_cast<long double>(p.max_seconds_per_container));
  return out;
}
std::shared_ptr<archive::ZipInputJob> scan_archive_job(const ScanRequest& r) {
  archive::ZipInputHooks hooks;
  hooks.clock=r.hooks.archive_clock; hooks.cancelled=r.cancelled;
  hooks.on_open=[&r](const auto& path) {
    if(r.hooks.before_io) detail::take(r.hooks.before_io(path,ScanIo::zip_probe));
    if(r.hooks.on_open) r.hooks.on_open(path,false);
  };
  hooks.before_read=[&r](const auto& path,auto index)->Result<void> {
    if(r.hooks.on_archive_read) r.hooks.on_archive_read(path,index);
    if(r.hooks.before_io) return r.hooks.before_io(path,ScanIo::header);
    return {};
  };
  return std::make_shared<archive::ZipInputJob>(archive_limits(snapshot_import_limits(r.profile)),std::move(hooks));
}
J effective_archive_limits(const PlanLimits& p,const archive::ZipInputJob& job) {
  const auto& l=job.limits();
  auto item=[](J value,std::string source) { return J{{"value",std::move(value)},{"source",std::move(source)}}; };
  return {{"max_entries",item(std::to_string(l.zip.max_entries),p.max_entries>l.zip.max_entries?"unconditional":"plan")},
    {"max_unpacked_bytes",item(std::to_string(l.max_unpacked_bytes),p.max_unpacked_bytes>l.max_unpacked_bytes?"unconditional":"plan")},
    {"max_ratio",item(l.max_ratio,"plan")},
    {"max_seconds_per_container",item(std::to_string(p.max_seconds_per_container),"plan")},
    {"max_temp_bytes",{{"value",std::to_string(p.max_temp_bytes)},{"source","plan"},{"applied",false}}},
    {"max_directory_bytes",item(std::to_string(l.zip.max_directory_bytes),"unconditional")},
    {"max_name_bytes",item(std::to_string(l.zip.max_name_bytes),"unconditional")},
    {"max_entry_bytes",item(std::to_string(l.zip.max_entry_bytes),"unconditional")},
    {"manifest_bytes",item(std::to_string(manifest::CodecLimits{}.max_bytes),"unconditional")}};
}
J archive_limit_problem(const PlanLimits& p,const archive::ZipInputJob& job) {
  const auto name=job.exceeded_limit();
  const auto limits=effective_archive_limits(p,job);
  J result{{"code","limit_exceeded"},{"message_id","am.import.archive.limit_exceeded"},{"limit",name}};
  if(limits.contains(name)) { result["effective_value"]=limits.at(name).at("value"); result["source"]=limits.at(name).at("source"); }
  return result;
}
bool specialized_zip_hint(std::string_view path,std::string_view detected) {
  if(detected!="archive.zip") return false;
  const auto folded=unicode::path_collision_key(path);
  // T-028 currently recognizes only 3MF among specialized ZIP containers.
  return folded.ends_with(".3mf");
}
namespace {
std::string number(std::size_t n) { std::ostringstream s; s<<'a'<<std::setw(8)<<std::setfill('0')<<n; return s.str(); }
std::string crc(std::uint32_t n) { std::ostringstream s; s<<std::hex<<std::setw(8)<<std::setfill('0')<<n; return s.str(); }
void reason(J& candidate,const std::string& value) {
  auto& reasons=candidate["reasons"];
  if(std::find(reasons.begin(),reasons.end(),J(value))==reasons.end()) reasons.push_back(value);
}
J issue(std::string code) { return {{"code",code},{"message_id",std::string("am.")+"import.archive."+code}}; }
}
Result<void> inventory_zip_entries(ImportPlan& plan,const ScanRequest& request,std::shared_ptr<archive::ZipInputJob> job) {
  try {
    const auto& nodes=plan.value.at("nodes");
    if(std::none_of(nodes.begin(),nodes.end(),[](const auto& n){return n.value("detected",std::string{})=="archive.zip";})) return {};
    const auto view=detail::take(parse_plan_view(serialize_plan(plan)));
    const auto limits=*view.limits();
    std::set<std::size_t> ready,volumes;
    for(const auto& c:view.candidates()) {
      if(c.kind==CandidateKind::ready_asset) for(const auto& in:c.inputs) for(const auto& o:in.objects) if(o.node) ready.insert(*o.node);
      for(const auto& v:c.volume_sets) volumes.insert(v.members.begin(),v.members.end());
    }
    std::map<std::string,std::vector<J>> decisions;
    std::map<std::string,J> summaries;
    ExclusionEngine engine;
    const auto enabled=request.profile.settings.value("exclusions",J::object()).value("rules",engine.rule_ids());
    for(std::size_t ni=0;ni<view.nodes().size();++ni) {
      const auto& node=view.nodes()[ni];
      if(node.kind!=NodeKind::file || node.detected!="archive.zip" || ready.contains(ni) || volumes.contains(ni)) continue;
      J summary{{"node",node.id},{"container","zip"},{"status","unreadable"},{"entry_count",0},{"name_encodings",J::array()},
        {"zip64",false},{"comment_length",0},{"encrypted_entries",0},{"unsupported_entries",0}};
      std::optional<archive::ZipInput> zip;
      auto opened=node.available ? archive::ZipInput::open(detail::take(source_path(view,node)),job) :
        Result<archive::ZipInput>(detail::error("am.import.node_unavailable",{},Errc::io_error));
      if(opened) zip=std::move(*opened);
      else {
        if(opened.error().code==Errc::cancelled) return opened.error();
        summary["problem"]={{"code",std::string("archive_")+std::string(archive::zip_input_failure_kind(opened.error()))},{"message_id",opened.error().message_id}};
        if(!job->exceeded_limit().empty()) { summary["status"]="limit_exceeded"; summary["problem"]=archive_limit_problem(limits,*job); }
      }
      if(zip) {
        // Specialized top-level ZIPs keep the T-028 marker and remain whole files.
        const auto specialized=std::any_of(zip->entries().begin(),zip->entries().end(),[](const auto& e){return e.info.raw_name=="3D/3dmodel.model";});
        if(specialized) {
          plan.value["nodes"][ni]["detected"]="project.3mf";
          for(auto& c:plan.value["candidates"]) for(auto& in:c["inputs"])
            if(in["kind"]=="archive" && in["objects"].size()==1 && in["objects"][0].value("node",std::string{})==node.id) in["kind"]="file";
          continue;
        }
        summary["status"]="listed"; summary["entry_count"]=zip->entries().size();
        summary["zip64"]=zip->summary().zip64; summary["comment_length"]=zip->summary().comment_length;
        for(const auto& e:zip->entries()) {
          const auto& info=e.info;
          const auto id=number(plan.value["archive_entries"].size()+1);
          const auto kind=info.kind==asset_zip::EntryKind::regular?"file":info.kind==asset_zip::EntryKind::directory?"directory":info.kind==asset_zip::EntryKind::symlink?"symlink":"other";
          J entry{{"id",id},{"archive",node.id},{"index",info.index},{"path",e.path},{"path_raw",detail::hex(std::as_bytes(std::span(info.raw_name.data(),info.raw_name.size())))},
            {"name_encoding",e.name_encoding},{"encoding_exact",e.encoding_exact},{"kind",kind},{"size",std::to_string(info.uncompressed_size)},
            {"compressed_size",std::to_string(info.compressed_size)},{"crc32",crc(info.crc32)},{"method",info.method},{"encrypted",info.encrypted},
            {"detected","unknown"},{"status","available"}};
          if(e.encoding_confidence) entry["encoding_confidence"]=*e.encoding_confidence;
          if(!e.problem_code.empty()) entry["problem"]=issue(e.problem_code);
          auto& encodings=summary["name_encodings"];
          if(std::find(encodings.begin(),encodings.end(),J(e.name_encoding))==encodings.end()) encodings.push_back(e.name_encoding);
          if(info.encrypted) summary["encrypted_entries"]=summary["encrypted_entries"].get<std::uint64_t>()+1;
          if(info.method!=0 && info.method!=8) summary["unsupported_entries"]=summary["unsupported_entries"].get<std::uint64_t>()+1;
          std::optional<FileKind> detected;
          if(info.kind==asset_zip::EntryKind::regular) {
            if(info.encrypted) { entry["status"]="unavailable"; entry["problem"]=issue("encrypted_unsupported"); }
            else if(info.method!=0 && info.method!=8) { entry["status"]="unavailable"; entry["problem"]=issue("method_unsupported"); }
            else if(job->exceeded_limit().empty()) {
              auto sample=zip->sample(info.index,kSignatureProbeBytes);
              if(sample) {
                detected=detect_kind(std::span(reinterpret_cast<const std::uint8_t*>(sample->data()),sample->size()),info.uncompressed_size);
                entry["detected"]=std::string(to_string(*detected));
              } else {
                if(sample.error().code==Errc::cancelled) return sample.error();
                entry["status"]="unavailable";
                entry["problem"]={{"code",sample.error().code==Errc::corrupt_data?"entry_corrupt":"read_failed"},{"message_id",sample.error().message_id}};
              }
            }
          }
          if(!job->exceeded_limit().empty()) {
            summary["status"]="limit_exceeded"; summary["problem"]=archive_limit_problem(limits,*job);
            entry["status"]="unavailable"; entry["problem"]=summary["problem"];
          }
          J decision{{"archive_entry",id},{"decision","include"}};
          if(info.kind==asset_zip::EntryKind::symlink || info.kind==asset_zip::EntryKind::special) {
            decision["decision"]="skip"; decision["explanation_id"]=info.kind==asset_zip::EntryKind::symlink?"link_not_followed":"special_not_materialized";
          } else {
            const auto rule=engine.evaluate({e.path,info.uncompressed_size,detected},enabled);
            if(rule.confidence!=Confidence::none) {
              decision["rule_id"]=rule.rule_id; decision["confidence"]=rule.confidence==Confidence::certain?"certain":"probable";
              decision["decision"]=rule.confidence==Confidence::certain?"exclude":"ask"; decision["explanation_id"]=rule.explanation_id;
            }
            if(decision["decision"]!="exclude" && detected && is_archive(*detected)) {
              if(specialized_zip_hint(e.path,entry["detected"].get<std::string>())) {
                decision["confidence"]="probable"; decision["explanation_id"]="specialized_container_unverified";
              } else { decision["decision"]="ask"; decision["explanation_id"]="nested_archive_expand_unsupported"; }
            }
            if(!e.path_problem.empty()) { decision["decision"]="ask"; decision["explanation_id"]=e.path_problem; }
            if(entry["status"]=="unavailable" && decision["decision"]!="exclude") decision["decision"]="ask";
          }
          decisions[node.id].push_back(std::move(decision)); plan.value["archive_entries"].push_back(std::move(entry));
        }
      }
      summaries[node.id]=summary; plan.value["archives"].push_back(std::move(summary));
      const auto& recorded=plan.value["archives"].back();
      if(recorded["status"]!="listed") plan.value["scan"]["completeness"]="incomplete";
      if(recorded["status"]=="limit_exceeded") {
        const auto name=job->exceeded_limit();
        auto& hit=plan.value["scan"]["limits_hit"];
        if(std::find(hit.begin(),hit.end(),J(name))==hit.end()) hit.push_back(name);
      }
    }
    for(auto& c:plan.value["candidates"]) {
      if(c["kind"]=="ready_asset") continue;
      std::vector<J> additions;
      for(const auto& outer:c["entries"]) if(outer.contains("node")) {
        const auto id=outer["node"].get<std::string>();
        if(!summaries.contains(id)) continue;
        const auto& a=summaries.at(id);
        if(a["status"]!="listed" && outer["decision"]!="exclude") { c["action"]="ask"; reason(c,a["status"]=="limit_exceeded"?"archive_limit_exceeded":"archive_unreadable"); }
        for(const auto& entry:decisions[id]) {
          additions.push_back(entry);
          if(entry["decision"]=="ask" && outer["decision"]!="exclude") c["action"]="ask";
          if(!plan.value["archive_entries"].empty()) {
            // Encoding guesses are visible independently of include/exclude decisions.
            const auto& enc=a["name_encodings"];
            if(std::any_of(enc.begin(),enc.end(),[](const auto& s){return s.template get<std::string>().ends_with("(heuristic)");})) reason(c,"name_encoding_guessed");
          }
        }
      }
      for(auto& e:additions) c["entries"].push_back(std::move(e));
      if(c["inputs"].size()==1 && c["inputs"][0]["kind"]=="archive" && c["inputs"][0]["objects"].size()==1) {
        const auto id=c["inputs"][0]["objects"][0].value("node",std::string{});
        for(const auto& n:plan.value["nodes"]) if(n["id"]==id) {
          auto name=n["name"].get<std::string>(); const auto dot=name.rfind('.');
          if(dot!=std::string::npos) name.resize(dot);
          c["title_hint"]=name;
        }
      }
    }
    return {};
  } catch(const Error& e) { return e; }
  catch(const std::exception& e) { return detail::error("am.import.archive.scan_failed",e.what(),Errc::io_error); }
}
} // namespace am::import
