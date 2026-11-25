#include "am/services/import/zip_support.hpp"
#include "am/services/import/exclusion_rules.hpp"
#include "am/foundation/signature.hpp"
#include "am/foundation/unicode.hpp"
#include "am/foundation/uuid.hpp"
#include "detail.hpp"
#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>

namespace am::import {
using J=PlanJson;
PlanLimits snapshot_import_limits(const ImportProfile& profile) {
  // Missing optional profile settings inherit the existing default.toml hypotheses.
  PlanLimits out{100000,214748364800ULL,3600,107374182400ULL,200,3};
  const auto& p=profile.settings;
  if(p.contains("nested")) out.max_depth=p.at("nested").value("max_depth",out.max_depth);
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
  out.max_temp_bytes=p.max_temp_bytes;
  // Plans before v3 never expand nested archives; their records keep depth 1.
  out.max_depth=p.max_depth?p.max_depth:1;
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
    {"max_temp_bytes",item(std::to_string(l.max_temp_bytes),"plan")},
    {"max_depth",item(std::to_string(l.max_depth),p.max_depth>l.max_depth?"unconditional":"plan")},
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
TempArea::TempArea(const std::filesystem::path& library)
  : path(library/".am"/".am-tmp"/"import"/Uuid::v4().to_string()) {}
TempArea::~TempArea() {
  std::error_code ignored;
  // The shared parent stays: a concurrent scan or attempt may be creating its own area in it.
  std::filesystem::remove_all(path,ignored);
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
namespace {
// Shared state of one scan's archive inventory, including nested archives (plan v3).
struct Inventory {
  ImportPlan& plan;
  const ScanRequest& request;
  std::shared_ptr<archive::ZipInputJob> job;
  PlanLimits limits;
  ExclusionEngine engine;
  std::vector<std::string> enabled;
  std::string action;           // [nested] action: expand, preserve, exclude; empty means ask
  std::uint64_t max_depth=1;    // effective, already lowered to the unconditional ceiling
  std::filesystem::path temp_dir;
  std::map<std::string,std::vector<J>> decisions;     // by top-level node id
  std::map<std::string,std::set<std::string>> reasons; // by top-level node id
  std::map<std::string,bool> guessed;                  // heuristic names anywhere under a top-level node

  void hit(const std::string& name) {
    plan.value["scan"]["completeness"]="incomplete";
    auto& list=plan.value["scan"]["limits_hit"];
    if(!name.empty() && std::find(list.begin(),list.end(),J(name))==list.end()) list.push_back(name);
  }
  J nested_summary(const std::string& entry,std::uint64_t depth) {
    return {{"entry",entry},{"depth",depth},{"container","zip"},{"status","unreadable"},{"entry_count",0},{"name_encodings",J::array()},
      {"zip64",false},{"comment_length",0},{"encrypted_entries",0},{"unsupported_entries",0}};
  }
  // Records the outcome of a failed listing in the archive summary at `at`.
  void failed(std::size_t at,const Error& error) {
    auto& summary=plan.value["archives"][at];
    summary["problem"]={{"code",std::string("archive_")+std::string(archive::zip_input_failure_kind(error))},{"message_id",error.message_id}};
    if(!job->exceeded_limit().empty()) { summary["status"]="limit_exceeded"; summary["problem"]=archive_limit_problem(limits,*job); }
  }
  void list(archive::ZipInput& zip,std::size_t at,const std::string& key,const std::string& top,
            std::vector<std::string> ancestors,std::uint64_t depth);
};
// Digests of the enclosing archives; the top-level one is read only when a nested ZIP appears.
std::vector<std::string> lineage_of(archive::ZipInput& zip,std::vector<std::string> ancestors,std::uint64_t depth) {
  if(depth==1 && ancestors.empty()) ancestors.push_back(detail::take(zip.sha256()));
  return ancestors;
}
void Inventory::list(archive::ZipInput& zip,std::size_t at,const std::string& key,const std::string& top,
                     std::vector<std::string> ancestors,std::uint64_t depth) {
  {
    auto& summary=plan.value["archives"][at];
    summary["status"]="listed"; summary["entry_count"]=zip.entries().size();
    summary["zip64"]=zip.summary().zip64; summary["comment_length"]=zip.summary().comment_length;
  }
  for(const auto& e:zip.entries()) {
    const auto& info=e.info;
    const auto id=number(plan.value["archive_entries"].size()+1);
    const auto kind=info.kind==asset_zip::EntryKind::regular?"file":info.kind==asset_zip::EntryKind::directory?"directory":info.kind==asset_zip::EntryKind::symlink?"symlink":"other";
    J entry{{"id",id},{"archive",key},{"index",info.index},{"path",e.path},{"path_raw",detail::hex(std::as_bytes(std::span(info.raw_name.data(),info.raw_name.size())))},
      {"name_encoding",e.name_encoding},{"encoding_exact",e.encoding_exact},{"kind",kind},{"size",std::to_string(info.uncompressed_size)},
      {"compressed_size",std::to_string(info.compressed_size)},{"crc32",crc(info.crc32)},{"method",info.method},{"encrypted",info.encrypted},
      {"detected","unknown"},{"status","available"}};
    if(e.encoding_confidence) entry["encoding_confidence"]=*e.encoding_confidence;
    if(!e.problem_code.empty()) entry["problem"]=issue(e.problem_code);
    {
      auto& summary=plan.value["archives"][at];
      auto& encodings=summary["name_encodings"];
      if(std::find(encodings.begin(),encodings.end(),J(e.name_encoding))==encodings.end()) encodings.push_back(e.name_encoding);
      if(e.name_encoding.ends_with("(heuristic)")) guessed[top]=true;
      if(info.encrypted) summary["encrypted_entries"]=summary["encrypted_entries"].get<std::uint64_t>()+1;
      if(info.method!=0 && info.method!=8) summary["unsupported_entries"]=summary["unsupported_entries"].get<std::uint64_t>()+1;
    }
    std::optional<FileKind> detected;
    if(info.kind==asset_zip::EntryKind::regular) {
      if(info.encrypted) { entry["status"]="unavailable"; entry["problem"]=issue("encrypted_unsupported"); }
      else if(info.method!=0 && info.method!=8) { entry["status"]="unavailable"; entry["problem"]=issue("method_unsupported"); }
      else if(job->exceeded_limit().empty()) {
        auto sample=zip.sample(info.index,kSignatureProbeBytes);
        if(sample) {
          detected=detect_kind(std::span(reinterpret_cast<const std::uint8_t*>(sample->data()),sample->size()),info.uncompressed_size);
          entry["detected"]=std::string(to_string(*detected));
        } else {
          if(sample.error().code==Errc::cancelled) throw sample.error();
          entry["status"]="unavailable";
          entry["problem"]={{"code",sample.error().code==Errc::corrupt_data?"entry_corrupt":"read_failed"},{"message_id",sample.error().message_id}};
        }
      }
    }
    if(!job->exceeded_limit().empty()) {
      auto& summary=plan.value["archives"][at];
      summary["status"]="limit_exceeded"; summary["problem"]=archive_limit_problem(limits,*job);
      entry["status"]="unavailable"; entry["problem"]=summary["problem"];
    }
    J decision{{"archive_entry",id},{"decision","include"}};
    // The parent's decision keeps its place ahead of the records of a nested archive listed below.
    const auto slot=decisions[top].size(); decisions[top].emplace_back();
    std::optional<std::size_t> nested_at;
    bool entry_written=false;
    if(info.kind==asset_zip::EntryKind::symlink || info.kind==asset_zip::EntryKind::special) {
      decision["decision"]="skip"; decision["explanation_id"]=info.kind==asset_zip::EntryKind::symlink?"link_not_followed":"special_not_materialized";
    } else {
      // Exclusion rules run before any nested container is considered (ADR-0011).
      const auto rule=engine.evaluate({e.path,info.uncompressed_size,detected},enabled);
      if(rule.confidence!=Confidence::none) {
        decision["rule_id"]=rule.rule_id; decision["confidence"]=rule.confidence==Confidence::certain?"certain":"probable";
        decision["decision"]=rule.confidence==Confidence::certain?"exclude":"ask"; decision["explanation_id"]=rule.explanation_id;
      }
      if(decision["decision"]!="exclude" && detected && is_archive(*detected)) {
        if(specialized_zip_hint(e.path,entry["detected"].get<std::string>())) {
          decision["confidence"]="probable"; decision["explanation_id"]="specialized_container_unverified";
        } else if(entry["detected"]!="archive.zip") {
          decision["decision"]="ask"; decision["explanation_id"]="nested_archive_expand_unsupported";
        } else if(decision.contains("rule_id")) {
          // A probable exclusion rule asks first; its explanation stays visible.
        } else if(action=="preserve") {
          decision["explanation_id"]="nested_archive_profile_preserve";
        } else if(action=="exclude") {
          decision["decision"]="exclude"; decision["explanation_id"]="nested_archive_profile_exclude";
        } else if(decision["decision"]=="include" && entry["status"]=="available" && e.path_problem.empty()) {
          decision["decision"]="ask";
          if(depth>=max_depth) { decision["explanation_id"]="nested_archive_depth_limit"; hit("max_depth"); reasons[top].insert("nested_archive_depth_limit"); }
          else if(info.method!=0 && temp_dir.empty()) {
            decision["explanation_id"]="nested_archive_temp_unavailable"; hit({}); reasons[top].insert("nested_archive_temp_unavailable");
          } else {
            std::optional<archive::ZipInput> nested;
            std::optional<Error> problem;
            std::optional<std::string> digest;
            if(auto opened=zip.open_nested(info.index,temp_dir,{})) {
              if(auto hashed=opened->sha256()) { digest=*hashed; nested=std::move(*opened); }
              else problem=hashed.error();
            } else problem=opened.error();
            if(problem && problem->code==Errc::cancelled) throw *problem;
            if(digest) ancestors=lineage_of(zip,std::move(ancestors),depth);
            if(digest && std::find(ancestors.begin(),ancestors.end(),*digest)!=ancestors.end()) {
              decision["explanation_id"]="nested_archive_cycle"; reasons[top].insert("nested_archive_cycle");
            } else {
              plan.value["archives"].push_back(nested_summary(id,depth+1));
              nested_at=plan.value["archives"].size()-1;
              if(nested) {
                auto& summary=plan.value["archives"][*nested_at];
                summary["sha256"]=*digest; summary["access"]=nested->temporary()?"temp":"range";
              } else {
                failed(*nested_at,*problem);
                hit(job->exceeded_limit());
              }
              decision["explanation_id"]="nested_archive_expandable";
              if(nested) {
                plan.value["archive_entries"].push_back(entry); entry_written=true;
                auto lineage=ancestors; lineage.push_back(*digest);
                list(*nested,*nested_at,id,top,std::move(lineage),depth+1);
                if(action=="expand" && plan.value["archives"][*nested_at]["status"]=="listed") {
                  decision["decision"]="expand"; decision["explanation_id"]="nested_archive_profile_expand";
                }
              }
            }
          }
        } else if(decision["decision"]!="exclude") {
          decision["decision"]="ask"; decision["explanation_id"]="nested_archive_expandable";
        }
      }
      if(!e.path_problem.empty()) { decision["decision"]="ask"; decision["explanation_id"]=e.path_problem; }
      if(entry["status"]=="unavailable" && decision["decision"]!="exclude") decision["decision"]="ask";
    }
    if(nested_at && plan.value["archives"][*nested_at]["status"]!="listed") hit(job->exceeded_limit());
    decisions[top][slot]=std::move(decision);
    // A listed nested archive already wrote its parent record before its own records.
    if(!entry_written) plan.value["archive_entries"].push_back(std::move(entry));
  }
}
} // namespace
Result<void> inventory_zip_entries(ImportPlan& plan,const ScanRequest& request,std::shared_ptr<archive::ZipInputJob> job) {
  try {
    const auto& nodes=plan.value.at("nodes");
    if(std::none_of(nodes.begin(),nodes.end(),[](const auto& n){return n.value("detected",std::string{})=="archive.zip";})) return {};
    const auto view=detail::take(parse_plan_view(serialize_plan(plan)));
    std::set<std::size_t> ready,volumes;
    for(const auto& c:view.candidates()) {
      if(c.kind==CandidateKind::ready_asset) for(const auto& in:c.inputs) for(const auto& o:in.objects) if(o.node) ready.insert(*o.node);
      for(const auto& v:c.volume_sets) volumes.insert(v.members.begin(),v.members.end());
    }
    Inventory inv{plan,request,job,*view.limits()};
    inv.enabled=request.profile.settings.value("exclusions",J::object()).value("rules",inv.engine.rule_ids());
    const auto nested=request.profile.settings.value("nested",J::object());
    inv.action=nested.value("action",std::string{});
    if(inv.action=="extract") inv.action="expand";
    inv.max_depth=job->limits().max_depth;
    // Compressed nested records are unpacked only into the library's managed temporary area.
    std::optional<TempArea> area;
    if(request.library) { area.emplace(*request.library); inv.temp_dir=area->path; }
    std::map<std::string,std::size_t> summaries;
    for(std::size_t ni=0;ni<view.nodes().size();++ni) {
      const auto& node=view.nodes()[ni];
      if(node.kind!=NodeKind::file || node.detected!="archive.zip" || ready.contains(ni) || volumes.contains(ni)) continue;
      J summary{{"node",node.id},{"depth",1},{"container","zip"},{"status","unreadable"},{"entry_count",0},{"name_encodings",J::array()},
        {"zip64",false},{"comment_length",0},{"encrypted_entries",0},{"unsupported_entries",0}};
      std::optional<archive::ZipInput> zip;
      auto opened=node.available ? archive::ZipInput::open(detail::take(source_path(view,node)),job) :
        Result<archive::ZipInput>(detail::error("am.import.node_unavailable",{},Errc::io_error));
      if(opened) zip=std::move(*opened);
      else if(opened.error().code==Errc::cancelled) return opened.error();
      if(zip) {
        // Specialized top-level ZIPs keep the T-028 marker and remain whole files.
        const auto specialized=std::any_of(zip->entries().begin(),zip->entries().end(),[](const auto& e){return e.info.raw_name=="3D/3dmodel.model";});
        if(specialized) {
          plan.value["nodes"][ni]["detected"]="project.3mf";
          for(auto& c:plan.value["candidates"]) for(auto& in:c["inputs"])
            if(in["kind"]=="archive" && in["objects"].size()==1 && in["objects"][0].value("node",std::string{})==node.id) in["kind"]="file";
          continue;
        }
      }
      plan.value["archives"].push_back(std::move(summary));
      const auto at=plan.value["archives"].size()-1;
      summaries[node.id]=at;
      if(!zip) inv.failed(at,opened.error());
      else inv.list(*zip,at,node.id,node.id,{},1);
      const auto& recorded=plan.value["archives"][at];
      if(recorded["status"]!="listed") plan.value["scan"]["completeness"]="incomplete";
      if(recorded["status"]=="limit_exceeded") inv.hit(job->exceeded_limit());
    }
    for(auto& c:plan.value["candidates"]) {
      if(c["kind"]=="ready_asset") continue;
      std::vector<J> additions;
      for(const auto& outer:c["entries"]) if(outer.contains("node")) {
        const auto id=outer["node"].get<std::string>();
        if(!summaries.contains(id)) continue;
        const auto& a=plan.value["archives"][summaries.at(id)];
        if(a["status"]!="listed" && outer["decision"]!="exclude") { c["action"]="ask"; reason(c,a["status"]=="limit_exceeded"?"archive_limit_exceeded":"archive_unreadable"); }
        for(const auto& entry:inv.decisions[id]) {
          additions.push_back(entry);
          if(entry["decision"]=="ask" && outer["decision"]!="exclude") c["action"]="ask";
        }
        // Encoding guesses are visible independently of include/exclude decisions.
        if(inv.guessed[id] && !inv.decisions[id].empty()) reason(c,"name_encoding_guessed");
        if(outer["decision"]!="exclude") for(const auto& r:inv.reasons[id]) reason(c,r);
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
