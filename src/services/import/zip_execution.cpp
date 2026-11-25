#include "am/services/import/zip_execution.hpp"
#include "am/services/import/exclusion_rules.hpp"
#include "am/foundation/uuid.hpp"
#include "am/foundation/unicode.hpp"
#include "detail.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <set>

namespace am::import {
namespace {
using J=PlanJson;
using namespace detail;
std::string uuid() { return Uuid::v4().to_string(); }
std::string base64(std::string_view bytes) {
  constexpr char chars[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for(std::size_t i=0;i<bytes.size();i+=3) {
    const auto a=static_cast<unsigned char>(bytes[i]);
    const auto b=i+1<bytes.size()?static_cast<unsigned char>(bytes[i+1]):0;
    const auto c=i+2<bytes.size()?static_cast<unsigned char>(bytes[i+2]):0;
    out+=chars[a>>2]; out+=chars[((a&3)<<4)|(b>>4)];
    out+=i+1<bytes.size()?chars[((b&15)<<2)|(c>>6)]:'='; out+=i+2<bytes.size()?chars[c&63]:'=';
  }
  return out;
}
J problem(const PlanProblem& p) { return {{"code",p.code},{"message_id",p.message_id}}; }
J exclusion(const PlanEntry& d) {
  return {{"rule",d.rule_id.empty()?"user":d.rule_id},{"rules_version",kExclusionRulesVersion},
    {"by",d.confidence=="certain" && !d.rule_id.empty()?"auto":"user"}};
}
std::string crc(std::uint32_t n) { std::ostringstream s; s<<std::hex<<std::setw(8)<<std::setfill('0')<<n; return s.str(); }
asset_zip::EntryKind kind(NodeKind k) {
  return k==NodeKind::file?asset_zip::EntryKind::regular:k==NodeKind::directory?asset_zip::EntryKind::directory:
    k==NodeKind::symlink?asset_zip::EntryKind::symlink:asset_zip::EntryKind::special;
}
std::string storage_name(std::string_view original) {
  auto value=take(unicode::nfc(original));
  // ZipPathMapper's historical byte policy predates the strict DEL/C1 ban.
  for(std::size_t i=0;i<value.size();++i) {
    if(value[i]=='\x7f') value[i]='_';
    else if(static_cast<unsigned char>(value[i])==0xc2 && i+1<value.size() &&
        static_cast<unsigned char>(value[i+1])>=0x80 && static_cast<unsigned char>(value[i+1])<=0x9f)
      value.replace(i,2,"_");
  }
  return value;
}
}
PlanEntry archive_node_decision(const PlanCandidate& c,std::size_t node) {
  const auto at=c.node_decisions.find(node);
  return at==c.node_decisions.end()?PlanEntry{node,EntryDecision::include,{},{},{}}:c.entries.at(at->second);
}
PlanEntry archive_entry_decision(const PlanView& p,const PlanCandidate& c,std::size_t index) {
  const auto& entry=p.archive_entries().at(index);
  const auto outer=archive_node_decision(c,p.archives().at(entry.archive).node);
  if(outer.decision!=EntryDecision::include) return outer;
  // Excluding a directory excludes its descendants with the same provenance (V3).
  for(std::size_t slash=entry.path.find('/');slash!=std::string::npos;slash=entry.path.find('/',slash+1)) {
    const auto at=c.excluded_archive_directories.find({entry.archive,entry.path.substr(0,slash+1)});
    if(at!=c.excluded_archive_directories.end()) return c.entries.at(at->second);
  }
  const auto at=c.archive_decisions.find(index);
  if(at==c.archive_decisions.end()) throw error("am.import.plan_invalid");
  return c.entries.at(at->second);
}
ZipExecution zip_execution(const PlanView& p,const PlanCandidate& c,const RunHooks& hooks,std::stop_token stop,
                           const std::filesystem::path& library) {
  ZipExecution out;
  std::set<std::size_t> active;
  bool temporary=false;
  for(std::size_t ai=0;ai<p.archives().size();++ai) {
    const auto& archive=p.archives()[ai];
    if(archive.parent_entry) {
      // Parents precede nested archives, so the parent's activity is already known.
      const auto parent=p.archive_entries().at(*archive.parent_entry).archive;
      if(!active.contains(parent) || archive_entry_decision(p,c,*archive.parent_entry).decision!=EntryDecision::expand) continue;
      temporary=temporary || archive.temp;
    } else {
      bool member=false;
      for(const auto& in:c.inputs) for(const auto& ref:in.objects) if(ref.node==archive.node) member=true;
      if(!member) continue;
    }
    active.insert(ai);
    out.archives.push_back(ai);
    for(const auto ei:p.archives()[ai].entries) {
      out.ids[ei]={{"entry_id",uuid()},{"file_id",uuid()},{"object_id",uuid()}};
      const auto& e=p.archive_entries().at(ei);
      if(!e.available && e.problem && (e.problem->code=="entry_corrupt" || e.problem->code=="read_failed") && p.mode()==ImportMode::partial)
        out.failed.emplace(ei,*e.problem);
    }
  }
  if(!out.archives.empty()) {
    archive::ZipInputHooks io;
    io.cancelled=[stop]{return stop.stop_requested();}; io.on_open=hooks.on_open;
    io.clock=hooks.archive_clock; io.before_read=hooks.before_archive_read;
    out.job=std::make_shared<archive::ZipInputJob>(archive_limits(*p.limits()),std::move(io));
  }
  if(temporary) out.temp=std::make_shared<TempArea>(library);
  return out;
}
Result<void> prepare_zip_execution(ZipExecution& out,const PlanView& p,const PlanCandidate& c,std::stop_token stop) {
  try {
    std::map<std::size_t,archive::ZipInput> opened;  // verified readers, parents for nested archives
    for(const auto ai:out.archives) {
      const auto& a=p.archives().at(ai); const auto& n=p.nodes().at(a.node);
      if(archive_node_decision(c,a.node).decision!=EntryDecision::include) continue;
      if(a.status!=PlanArchiveStatus::listed) {
        if(a.status==PlanArchiveStatus::limit_exceeded)
          throw error("am.import.archive.limit_exceeded",n.id,Errc::unsupported)
            .caused_by(error(a.problem?a.problem->message_id:"am.import.archive.limit_exceeded",{},Errc::resource_exhausted));
        throw error("am.import.run.archive_unreadable",n.id,Errc::corrupt_data);
      }
      const auto* record=a.parent_entry?&p.archive_entries().at(*a.parent_entry):nullptr;
      if(record && !opened.contains(record->archive)) continue;
      auto attempt=record ? opened.at(record->archive).open_nested(record->index,out.temp?out.temp->path:std::filesystem::path{},stop)
                          : archive::ZipInput::open(take(source_path(p,n)),out.job);
      if(!attempt && attempt.error().code==Errc::cancelled) return attempt.error();
      // A sticky input limit wins over the stream error it produced (e.g. a budget hit inside a nested CRC pass).
      if(!attempt && !out.job->exceeded_limit().empty())
        throw error("am.import.archive.limit_exceeded",n.id,Errc::unsupported).caused_by(attempt.error());
      if(!attempt && (attempt.error().code==Errc::corrupt_data || attempt.error().code==Errc::not_found ||
          (attempt.error().code==Errc::unsupported && attempt.error().message_id!="am.import.archive.limit_exceeded")))
        throw error("am.import.source_changed",n.id,Errc::conflict).caused_by(attempt.error());
      auto zip=take(std::move(attempt));
      const auto identities=zip.identity();
      if(identities.size()!=a.entries.size()) throw error("am.import.source_changed",n.id,Errc::conflict);
      for(std::size_t i=0;i<a.entries.size();++i) {
        const auto& e=p.archive_entries().at(a.entries[i]);
        const archive::ZipInputIdentity expected{e.raw_path,e.path,e.name_encoding,e.size,e.compressed_size,e.crc32,e.method,kind(e.kind),e.encrypted};
        if(e.index!=i || identities[i]!=expected) throw error("am.import.source_changed",n.id,Errc::conflict);
        const auto d=archive_entry_decision(p,c,a.entries[i]);
        if(d.decision==EntryDecision::include && !out.failed.contains(a.entries[i]) && !e.available && p.mode()==ImportMode::strict)
          throw error("am.import.run.source_unavailable",e.id,Errc::io_error);
      }
      const auto digest=take(zip.sha256(stop));
      const auto& planned=record?a.sha256:n.sha256;
      if((planned && *planned!=digest) || (out.snapshots.contains(ai) && out.snapshots.at(ai)->sha256!=digest))
        throw error("am.import.source_changed",n.id,Errc::conflict);
      auto snapshot=std::make_shared<archive::ZipInputSnapshot>();
      snapshot->path=record?zip.label():take(source_path(p,n)); snapshot->entries=identities; snapshot->sha256=digest; snapshot->job=out.job;
      if(record) {
        snapshot->parent=out.snapshots.at(record->archive); snapshot->parent_index=record->index;
        if(out.temp) snapshot->temp_dir=out.temp->path;
      }
      out.snapshots[ai]=std::move(snapshot);
      opened.insert_or_assign(ai,std::move(zip));
    }
    return {};
  } catch(const Error& e) { return e; }
}
void append_zip_execution(ZipExecution& out,const PlanView& p,const PlanCandidate& c,const J& params,
    J& input,repository::AddFiles& add,paths::ZipPathMapper& mapper,const std::map<std::size_t,std::string>& prefixes) {
  std::map<std::size_t,std::string> archive_prefix;
  auto object_of=[&](std::size_t ai) -> std::string {
    const auto& a=p.archives().at(ai);
    return a.parent_entry ? out.ids.at(*a.parent_entry).at("object_id").get<std::string>()
                          : params.at("archives").at(p.nodes().at(a.node).id).get<std::string>();
  };
  for(const auto ai:out.archives) {
    const auto& a=p.archives().at(ai); const auto& n=p.nodes().at(a.node);
    const auto outer=archive_node_decision(c,a.node);
    const auto object_id=object_of(ai);
    J object;
    if(a.parent_entry) {
      // Nested archive expanded from a record (T-032 p. 6): no relative_path, linked to its parent record.
      const auto& record=p.archive_entries().at(*a.parent_entry);
      const auto slash=record.path.find_last_of('/');
      object={{"id",object_id},{"kind","archive"},{"container","zip"},{"name",slash==std::string::npos?record.path:record.path.substr(slash+1)},
        {"size_bytes",std::to_string(record.size)},{"parent_object_id",object_of(record.archive)},
        {"parent_entry_id",out.ids.at(*a.parent_entry).at("entry_id")},{"nested_action","extract"}};
      if(out.snapshots.contains(ai)) object["sha256"]=*out.snapshots.at(ai)->sha256;
      else if(a.sha256) object["sha256"]=*a.sha256;
    } else {
      auto relative=n.path.empty()?n.name:n.path;
      if(!c.base_path.empty() && relative.starts_with(c.base_path+"/")) relative.erase(0,c.base_path.size()+1);
      object={{"id",object_id},{"kind","archive"},{"container","zip"},{"name",n.name},{"relative_path",relative}};
      if(n.name_raw) object["name_raw"]={{"base64",base64(n.name_raw->bytes)},{"encoding_guess",n.name_raw->encoding}};
      if(n.size) object["size_bytes"]=std::to_string(*n.size);
      if(out.snapshots.contains(ai)) object["sha256"]=*out.snapshots.at(ai)->sha256;
      else if(n.sha256) object["sha256"]=*n.sha256;
    }
    std::string encodings;
    std::set<std::string> seen;
    for(const auto ei:a.entries) {
      const auto& enc=p.archive_entries().at(ei).name_encoding;
      if(seen.insert(enc).second) { if(!encodings.empty()) encodings+=','; encodings+=enc; }
    }
    if(!encodings.empty()) object["name_encoding"]=encodings;
    input["objects"].push_back(std::move(object));
    std::string prefix;
    paths::RenameReason prefix_reason=paths::RenameReason::none;
    if(a.parent_entry) {
      // a/inner.zip expands under a/inner/ inside its parent's placement.
      const auto& record=p.archive_entries().at(*a.parent_entry);
      auto stem=record.path;
      const auto dot=stem.find_last_of('.');
      if(dot!=std::string::npos && (stem.find_last_of('/')==std::string::npos || dot>stem.find_last_of('/'))) stem.resize(dot);
      const auto mapped=mapper.map(storage_name(archive_prefix[record.archive]+stem));
      prefix=mapped.zip_path.substr(mapper.root().size())+"/"; prefix_reason=mapped.reason;
    } else if(outer.decision==EntryDecision::include && prefixes.contains(a.node)) {
      const auto mapped=mapper.map(storage_name(prefixes.at(a.node))); prefix=mapped.zip_path.substr(mapper.root().size())+"/"; prefix_reason=mapped.reason;
    }
    archive_prefix[ai]=prefix;
    for(const auto ei:a.entries) {
      const auto& e=p.archive_entries().at(ei); const auto d=archive_entry_decision(p,c,ei);
      auto display=e.path; if(e.kind==NodeKind::directory && display.ends_with('/')) display.pop_back();
      J entry{{"id",out.ids.at(ei).at("entry_id")},{"object_id",object_id},{"entry_kind",e.kind==NodeKind::directory?"directory":"file"},
        {"path",display},{"result","included"},{"crc32",crc(e.crc32)},{"size_bytes",std::to_string(e.size)}};
      // Dangerous names and duplicates retain their source bytes even when already UTF-8.
      if(e.raw_path!=display || !e.encoding_exact || d.explanation_id=="unsafe_entry_path" || d.explanation_id=="duplicate_entry_name" || d.explanation_id=="case_collision")
        entry["path_raw"]={{"base64",base64(e.raw_path)},{"encoding_guess",e.name_encoding}};
      if(d.decision==EntryDecision::exclude) { entry["result"]="excluded"; entry["exclusion"]=exclusion(d); }
      else if(d.decision==EntryDecision::expand) {
        // Container bytes are not stored; its records live under the nested extract object (T-032 p. 6).
        entry["result"]="excluded";
        entry["exclusion"]={{"rule","nested_extract"},{"by",d.explanation_id=="nested_archive_profile_expand"?"profile":"user"}};
      }
      else if(d.decision==EntryDecision::skip || e.kind==NodeKind::symlink || e.kind==NodeKind::other) {
        entry["result"]="not_processed";
        const auto code=e.kind==NodeKind::other?"special_not_materialized":"link_not_followed";
        entry["problem"]={{"code",code},{"message_id",e.kind==NodeKind::other?"am.import.special_not_materialized":"am.import.link_not_followed"}};
      } else if(out.failed.contains(ei)) {
        entry["result"]="failed"; entry["problem"]=problem(out.failed.at(ei)); input["completeness"]="incomplete";
      } else if(!e.available) {
        entry["result"]="unavailable"; entry["problem"]=problem(*e.problem); input["completeness"]="incomplete";
      } else if(e.kind==NodeKind::file) {
        auto mapped=mapper.map(storage_name(prefix+e.path)); auto reason=mapped.reason;
        if(d.explanation_id=="unsafe_entry_path") reason=paths::RenameReason::invalid_chars;
        else if(d.explanation_id=="duplicate_entry_name" || d.explanation_id=="case_collision" || prefix_reason==paths::RenameReason::collision) reason=paths::RenameReason::collision;
        else if(!e.encoding_exact && reason==paths::RenameReason::none) reason=paths::RenameReason::encoding;
        if(reason!=paths::RenameReason::none) { entry["result"]="included_renamed"; entry["rename_reason"]=paths::to_string(reason); }
        entry["file_id"]=out.ids.at(ei).at("file_id");
        repository::FileInput file; file.path=mapped.zip_path.substr(mapper.root().size()); file.origin_kind="imported";
        file.file_id=out.ids.at(ei).at("file_id").get<std::string>();
        file.archive_entry=repository::ArchiveEntryInput{out.snapshots.at(ai),static_cast<std::size_t>(e.index)};
        add.files.push_back(std::move(file));
      }
      const auto nested=(e.detected=="archive.zip" || e.detected=="archive.rar4" || e.detected=="archive.rar5" || e.detected=="archive.7z") && !specialized_zip_hint(e.path,e.detected);
      if(nested && (d.decision==EntryDecision::include || d.decision==EntryDecision::exclude)) {
        const auto slash=e.path.find_last_of('/');
        input["objects"].push_back({{"id",out.ids.at(ei).at("object_id")},{"kind","archive"},{"container",e.detected.substr(8)},
          {"name",slash==std::string::npos?e.path:e.path.substr(slash+1)},{"size_bytes",std::to_string(e.size)},
          {"parent_object_id",object_id},{"parent_entry_id",entry.at("id")},{"nested_action",d.decision==EntryDecision::include?"preserve":"exclude"}});
      }
      input["entries"].push_back(std::move(entry));
    }
  }
}
} // namespace am::import
