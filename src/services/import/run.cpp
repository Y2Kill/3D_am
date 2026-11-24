#include "am/services/import/run.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>

#include "am/foundation/log.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/import/exclusion_rules.hpp"
#include "am/storage/asset_zip/source.hpp"
#include "am/storage/db/job_store.hpp"
#include "am/storage/db/operation_log.hpp"
#include "detail.hpp"

namespace am::import {
namespace {
using namespace detail;
namespace fs = std::filesystem;
namespace jc = contracts;
using J = PlanJson;
using repo = repository::AssetRepository;
template <class T, class F>
Result<T> guard(F&& f) {
  try {
    return f();
  } catch (const Error& e) {
    return e;
  } catch (const std::bad_alloc&) {
    return error("am.import.out_of_memory", {}, Errc::resource_exhausted);
  } catch (const std::exception& e) {
    return error("am.import.run.failed", e.what(), Errc::io_error);
  } catch (...) {
    return error("am.import.run.failed", {}, Errc::internal);
  }
}
std::string utf8(const fs::path& p) {
  const auto s = p.generic_u8string();
  return {s.begin(), s.end()};
}
fs::path path(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string bytes(const fs::path& p) {
  std::ifstream f(paths::native(p), std::ios::binary);
  if (!f) throw error("am.import.plan_invalid");
  std::string out{std::istreambuf_iterator<char>(f), {}};
  if (f.bad()) throw error("am.import.plan_invalid");
  return out;
}
std::string uuid() { return Uuid::v4().to_string(); }
std::string now() {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}
const PlanCandidate& candidate(const PlanView& plan, const std::string& id) {
  const auto at =
      std::find_if(plan.candidates().begin(), plan.candidates().end(), [&](const auto& c) { return c.id == id; });
  if (at == plan.candidates().end()) throw error("am.import.run.plan_changed", {}, Errc::conflict);
  return *at;
}
PlanView load(const J& params) {
  try {
    const auto content = bytes(path(params.at("plan_path").get<std::string>()));
    if (hash(content) != params.at("plan_sha256").get<std::string>())
      throw error("am.import.run.plan_changed", {}, Errc::conflict);
    return take(parse_plan_view(content));
  } catch (const Error& e) {
    throw error("am.import.run.plan_changed", {}, Errc::conflict).caused_by(e);
  }
}
J problem(const PlanProblem& p) { return {{"code", p.code}, {"message_id", p.message_id}}; }
J failure(const Error& e) { return {{"code", errc_name(e.code)}, {"message_id", e.message_id}}; }
J base_row(const PlanView& p, const PlanCandidate& c) {
  J row{{"candidate_id", c.id}};
  row["suggested_links"] = J::array();
  for (const auto& l : c.suggested_links)
    row["suggested_links"].push_back(
        {{"url", l.url}, {"node", p.nodes().at(l.node).id}, {"ad", l.ad}, {"confirmed", false}});
  return row;
}
std::optional<J> deferred(const PlanView& p, const PlanCandidate& c) {
  auto row = base_row(p, c);
  const auto unresolved =
      c.action == CandidateAction::ask ||
      std::any_of(c.entries.begin(), c.entries.end(), [](const auto& e) { return e.decision == EntryDecision::ask; });
  if (!unresolved && c.action == CandidateAction::skip) {
    row["status"] = "skipped";
    return std::make_optional(std::move(row));
  }
  const auto archive =
      c.kind == CandidateKind::new_asset && std::any_of(c.inputs.begin(), c.inputs.end(), [](const auto& i) {
        return i.kind == InputKind::archive || i.kind == InputKind::multivolume_archive;
      });
  if (!unresolved && !archive) return {};
  row["status"] = "deferred";
  row["message_id"] = unresolved ? "am.import.run.unresolved" : "am.import.run.archive_input_unsupported";
  if (unresolved)
    for (const auto& e : c.entries)
      if (e.decision == EntryDecision::ask && !p.nodes().at(e.node).available) {
        row["has_unavailable"] = true;
        if (p.nodes().at(e.node).problem) row["unavailable_problem"] = problem(*p.nodes().at(e.node).problem);
      }
  return std::make_optional(std::move(row));
}
std::vector<std::size_t> members(const PlanCandidate& c) {
  std::set<std::size_t> nodes(c.directories.begin(), c.directories.end());
  for (const auto& e : c.entries) nodes.insert(e.node);
  for (const auto& i : c.inputs)
    for (const auto& r : i.objects)
      if (r.node) nodes.insert(*r.node);
  return {nodes.begin(), nodes.end()};
}
std::vector<PlanEntry> execution_entries(const PlanView& p, const PlanCandidate& c) {
  auto entries = c.entries;
  for (const auto node : members(c)) {
    if (p.nodes().at(node).kind != NodeKind::file) continue;
    if (std::none_of(entries.begin(), entries.end(), [&](const auto& e) { return e.node == node; }))
      entries.push_back({node, EntryDecision::include, {}, {}, {}});
  }
  return entries;
}
// Objects are real roots. A profile boundary inside a selected tree is itself a real directory.
std::vector<std::size_t> object_roots(const PlanView& p, const PlanCandidate& c) {
  std::set<std::size_t> roots;
  for (const auto n : members(c)) roots.insert(p.nodes().at(n).root);
  std::vector<std::size_t> out;
  for (const auto root : roots) {
    const auto base = roots.size() == 1 && root == c.root ? c.base_path : std::string{};
    auto at = std::find_if(p.nodes().begin(), p.nodes().end(),
                           [&](const auto& n) { return n.root == root && n.path == base; });
    if (at == p.nodes().end()) throw error("am.import.plan_invalid");
    out.push_back(static_cast<std::size_t>(at - p.nodes().begin()));
  }
  return out;
}
J allocate(const PlanView& p, const PlanCandidate& c, const J& parent) {
  J params = parent;
  params["candidate_id"] = c.id;
  params["asset_id"] = c.kind == CandidateKind::ready_asset && c.asset_id ? *c.asset_id : uuid();
  params["input_id"] = uuid();
  params["operation_id"] = uuid();
  params["nodes"] = J::object();
  for (const auto n : members(c)) params["nodes"][p.nodes().at(n).id] = {{"entry_id", uuid()}, {"file_id", uuid()}};
  params["objects"] = J::object();
  for (const auto n : object_roots(p, c)) params["objects"][p.nodes().at(n).id] = uuid();
  params["sources"] = J::array();
  for (const auto& source : c.sources) {
    (void)source;
    params["sources"].push_back({{"id", uuid()}, {"claim_id", uuid()}});
  }
  return params;
}
template <class Enqueue>
void ensure_children(const PlanView& p, const J& params, const std::vector<jc::JobRecord>& existing, Enqueue enqueue) {
  std::set<std::string> present;
  for (const auto& r : existing)
    if (r.type == "import.asset") present.insert(J::parse(r.params_json).at("candidate_id").get<std::string>());
  for (const auto& c : p.candidates())
    if (!deferred(p, c) && !present.contains(c.id)) {
      jc::JobSpec spec;
      spec.type = "import.asset";
      spec.params_json = allocate(p, c, params).dump();
      take(enqueue(std::move(spec)));
    }
}
J summary(const PlanView& p, const std::vector<jc::JobRecord>& children, bool reused) {
  J out{{"candidates", J::array()}};
  for (const auto& c : p.candidates()) {
    if (auto row = deferred(p, c)) {
      out["candidates"].push_back(*row);
      continue;
    }
    const jc::JobRecord* record = nullptr;
    for (const auto& r : children)
      if (r.type == "import.asset" && J::parse(r.params_json).at("candidate_id") == c.id) {
        record = &r;
        break;
      }
    auto row = base_row(p, c);
    row["status"] = "already_queued";
    if (record) {
      row["job_id"] = record->job_id;
      if (record->result_json) row.update(J::parse(*record->result_json));
      if (record->state == jc::JobState::succeeded) {
        if (reused &&
            (row.value("status", std::string{}) == "created" || row.value("status", std::string{}) == "imported"))
          row["status"] = "already_imported";
      } else if (record->state == jc::JobState::needs_decision) {
        row["status"] = "deferred";
      } else if (record->state == jc::JobState::failed || record->state == jc::JobState::resource_error ||
                 record->state == jc::JobState::cannot_continue_safely) {
        row["status"] = "failed";
        row["retry_hint"] = "am jobs retry";
        if (record->error_json) row["error"] = J::parse(*record->error_json);
      } else if (record->state == jc::JobState::cancelled)
        row["status"] = "cancelled";
    }
    out["candidates"].push_back(std::move(row));
  }
  return out;
}
std::string base64(std::string_view bytes) {
  constexpr char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const auto a = static_cast<unsigned char>(bytes[i]);
    const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
    const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
    out += chars[a >> 2];
    out += chars[((a & 3) << 4) | (b >> 4)];
    out += i + 1 < bytes.size() ? chars[((b & 15) << 2) | (c >> 6)] : '=';
    out += i + 2 < bytes.size() ? chars[c & 63] : '=';
  }
  return out;
}
void raw(J& j, const char* key, const std::string& display, const std::optional<RawBytes>& value) {
  if (!value || value->bytes == display) return;
  j[key] = {{"base64", base64(value->bytes)}};
  if (!value->encoding.empty()) j[key]["encoding_guess"] = value->encoding;
}
std::optional<RawBytes> relative_raw(const PlanView& p, const PlanNode& n, const PlanNode& root) {
  if (!n.path_raw && !n.name_raw) return {};
  auto source = take(source_path(p, n));
  auto relative = n.kind != NodeKind::directory && n.id == root.id
                      ? source.filename()
                      : source.lexically_relative(take(source_path(p, root)));
#ifdef _WIN32
  std::string bytes;
  for (const auto c : relative.generic_wstring()) {
    bytes += static_cast<char>(c & 255);
    bytes += static_cast<char>((c >> 8) & 255);
  }
  return RawBytes{bytes, "utf-16le"};
#else
  return RawBytes{relative.native(), "posix"};
#endif
}
struct Shared {
  catalog::Catalog& catalog;
  std::string device;
  RunHooks hooks;
  // Catalog mutations, snapshots, index updates and warnings are one critical section.
  std::mutex catalog_mutex;
  Shared(catalog::Catalog& c, std::string d, RunHooks h) : catalog(c), device(std::move(d)), hooks(std::move(h)) {}
};
paths::NodeInfo info(Shared& s, const fs::path& p) {
  if (s.hooks.node_data) return paths::classify_windows_node(take(s.hooks.node_data(p)));
  return take(paths::node_info(p));
}
void fresh(Shared& s, const PlanView& p, const PlanNode& n) {
  const auto file = take(source_path(p, n));
  if (!n.path.empty()) {
    auto parent = file.parent_path();
    const auto selected = path(p.roots().at(n.root).path).lexically_normal();
    while (!parent.empty()) {
      const auto node = info(s, parent);
      if (node.kind != paths::NodeKind::directory || node.cloud_placeholder)
        throw error("am.import.run.source_unavailable", n.id, Errc::io_error);
      if (parent.lexically_normal() == selected) break;
      const auto next = parent.parent_path();
      if (next == parent) break;
      parent = next;
    }
  }
  auto data = info(s, file);
  if (data.cloud_placeholder || data.kind != paths::NodeKind::file)
    throw error("am.import.run.source_unavailable", n.id, Errc::io_error);
  if (data.size != n.size || data.mtime_ns != n.mtime_ns) throw error("am.import.source_changed", n.id, Errc::conflict);
}
std::string source_hash(Shared& s, const PlanView& p, const PlanNode& n, std::stop_token stop) {
  const auto file = take(source_path(p, n));
  if (s.hooks.on_open) s.hooks.on_open(file);
  auto input = take(asset_zip::file_source(file));
  auto state = take(asset_zip::Sha256::create());
  std::array<std::byte, 65536> block{};
  for (std::uint64_t offset = 0; offset < input->size();) {
    if (stop.stop_requested()) throw error("am.commit.cancelled", {}, Errc::cancelled);
    const auto nread = take(input->read_at(offset, block));
    if (!nread) throw error("am.import.source_changed", n.id, Errc::conflict);
    take(state.update(std::span(block).first(nread)));
    offset += nread;
  }
  return hex(take(state.finish()));
}
void operation(Shared& s, const jc::JobRecord& job, const J& params, const J& row) {
  db::LogQuery q;
  q.job_id = job.job_id;
  q.limit = static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)());
  for (const auto& entry : take(s.catalog.operation_log().query(q)))
    if (entry.code == "import.asset.finished") return;
  db::LogEntry entry;
  entry.code = "import.asset.finished";
  entry.level = "info";
  entry.job_id = job.job_id;
  entry.asset_id = row.at("asset_id").get<std::string>();
  entry.detail_json = J{
      {"plan_sha256", params.at("plan_sha256")},
      {"candidate_id", params.at("candidate_id")},
      {"asset_id", row.at("asset_id")},
      {"result",
       row.at("status")}}.dump();
  take(s.catalog.operation_log().append(entry));
}
repository::CreateRequest build(Shared& s, const PlanView& p, const PlanCandidate& c, const J& params,
                                const std::map<std::size_t, PlanProblem>& failed,
                                const std::map<std::size_t, std::string>& hashes, std::stop_token stop) {
  repository::CreateRequest request;
  request.asset_id = params.at("asset_id").get<std::string>();
  request.op_id = params.at("operation_id").get<std::string>();
  request.creation_kind = "import";
  request.stop = stop;
  request.metadata = repository::Json{{"title", c.title_hint}};
  const auto input_id = params.at("input_id").get<std::string>();
  J input{{"id", input_id},
          {"imported_at", now()},
          {"mode", p.mode() == ImportMode::strict ? "strict" : "partial"},
          {"completeness", p.scan_complete() ? "complete" : "incomplete"},
          {"objects", J::array()},
          {"entries", J::array()}};
  const auto roots = object_roots(p, c);
  auto root_mapper = take(paths::ZipPathMapper::create("content/" + input_id));
  auto mapper = take(paths::ZipPathMapper::create("content/" + input_id));
  std::map<std::size_t, paths::MappedPath> names;
  for (const auto ri : roots) {
    const auto& n = p.nodes().at(ri);
    J object{{"id", params.at("objects").at(n.id)},
             {"kind", n.kind == NodeKind::directory ? "directory" : "file"},
             {"name", n.name}};
    raw(object, "name_raw", n.name, n.name_raw);
    if (!c.base_path.empty() && n.root == c.root && n.path.starts_with(c.base_path + "/"))
      object["relative_path"] = n.path.substr(c.base_path.size() + 1);
    if (n.kind == NodeKind::file) {
      if (n.size) object["size_bytes"] = std::to_string(*n.size);
      if (n.sha256)
        object["sha256"] = *n.sha256;
      else if (hashes.contains(ri))
        object["sha256"] = hashes.at(ri);
    }
    input["objects"].push_back(std::move(object));
    auto mapped = root_mapper.map(n.name);
    mapped.zip_path = mapped.zip_path.substr(root_mapper.root().size());
    names.emplace(ri, std::move(mapped));
  }
  repository::AddFiles add;
  for (const auto ni : members(c)) {
    const auto& n = p.nodes().at(ni);
    const auto root =
        std::find_if(roots.begin(), roots.end(), [&](auto ri) { return p.nodes().at(ri).root == n.root; });
    if (root == roots.end()) throw error("am.import.plan_invalid");
    const auto& object = p.nodes().at(*root);
    auto relative = n.id == object.id ? (n.kind == NodeKind::directory ? std::string{} : n.name)
                                      : n.path.substr(object.path.empty() ? 0 : object.path.size() + 1);
    J entry{{"id", params.at("nodes").at(n.id).at("entry_id")},
            {"object_id", params.at("objects").at(object.id)},
            {"entry_kind", n.kind == NodeKind::directory ? "directory" : "file"},
            {"path", relative},
            {"result", "included"}};
    raw(entry, "path_raw", relative, relative_raw(p, n, object));
    if (n.size) entry["size_bytes"] = std::to_string(*n.size);
    const auto decision = std::find_if(c.entries.begin(), c.entries.end(), [&](const auto& e) { return e.node == ni; });
    const auto d = decision == c.entries.end() ? EntryDecision::include : decision->decision;
    if (d == EntryDecision::exclude) {
      entry["result"] = "excluded";
      entry["exclusion"] = {{"rule", decision->rule_id.empty() ? "user" : decision->rule_id},
                            {"rules_version", kExclusionRulesVersion},
                            {"by", decision->confidence == "certain" && !decision->rule_id.empty() ? "auto" : "user"}};
    } else if (d == EntryDecision::skip) {
      entry["result"] = "not_processed";
      entry["problem"] = {{"code", "link_not_followed"}, {"message_id", "am.import.link_not_followed"}};
    } else if (failed.contains(ni)) {
      entry["result"] = "failed";
      entry["problem"] = problem(failed.at(ni));
      input["completeness"] = "incomplete";
    } else if (!n.available) {
      entry["result"] = "unavailable";
      entry["problem"] = problem(*n.problem);
      input["completeness"] = "incomplete";
    } else if (n.kind == NodeKind::file) {
      auto original = relative;
      if (roots.size() > 1 && n.id != object.id)
        original = names.at(*root).zip_path + "/" + relative;
      else if (roots.size() > 1)
        original = names.at(*root).zip_path;
      auto mapped = mapper.map(original);
      auto reason = mapped.reason;
      if (roots.size() > 1 && names.at(*root).reason != paths::RenameReason::none) reason = names.at(*root).reason;
      if ((n.path_raw || n.name_raw) && reason == paths::RenameReason::none) reason = paths::RenameReason::encoding;
      if (reason != paths::RenameReason::none) {
        entry["result"] = "included_renamed";
        entry["rename_reason"] = paths::to_string(reason);
      }
      const auto file_id = params.at("nodes").at(n.id).at("file_id").get<std::string>();
      entry["file_id"] = file_id;
      repository::FileInput file;
      file.path = mapped.zip_path.substr(mapper.root().size());
      file.source = take(source_path(p, n));
      file.file_id = file_id;
      file.origin_kind = "imported";
      file.expected_sha256 = n.sha256;
      if (!file.expected_sha256 && hashes.contains(ni)) file.expected_sha256 = hashes.at(ni);
      add.files.push_back(std::move(file));
    }
    input["entries"].push_back(std::move(entry));
  }
  if (!c.sources.empty()) {
    J origin{{"sources", J::array()}};
    input["source_refs"] = J::array();
    for (std::size_t i = 0; i < c.sources.size(); ++i) {
      const auto& ids = params.at("sources").at(i);
      const auto& source = c.sources[i];
      origin["sources"].push_back({{"id", ids.at("id")},
                                   {"kind", source.kind},
                                   {"name", source.name},
                                   {"claims", J::array({J{{"id", ids.at("claim_id")},
                                                          {"target", {{"kind", "self"}}},
                                                          {"relation", "supports_selected"},
                                                          {"method", "directory_inference"},
                                                          {"created_at", input.at("imported_at")}}})}});
      input["source_refs"].push_back(ids.at("id"));
    }
    request.origin = repository::Json::parse(origin.dump());
  }
  add.input = repository::Json::parse(input.dump());
  request.files.push_back(std::move(add));
  (void)s;
  return request;
}
class ImportAsset final : public jobs::JobHandler {
 public:
  explicit ImportAsset(std::shared_ptr<Shared> state) : s_(std::move(state)) {}
  jobs::JobOutcome run(jobs::JobContext& ctx) override {
    J row;
    try {
      const auto params = J::parse(ctx.record().params_json);
      row = {{"candidate_id", params.at("candidate_id")}};
      const auto plan = load(params);
      const auto candidate_id = params.at("candidate_id").get<std::string>();
      const auto& c = candidate(plan, candidate_id);
      row = base_row(plan, c);
      const std::array resources{jobs::ResourceRequest{jobs::ResourceKind::write, s_->device},
                                 jobs::ResourceRequest{jobs::ResourceKind::cpu, {}}};
      auto lease = take(ctx.acquire(resources));
      if (ctx.stop().stop_requested()) return jobs::JobOutcome::interrupted();
      if (c.kind == CandidateKind::ready_asset) return ready(ctx, plan, c, params, std::move(row));
      if (ctx.answer() == "skip") {
        row["status"] = "skipped";
        row["asset_id"] = params.at("asset_id");
        return jobs::JobOutcome::success(row.dump());
      }
      const repository::AssetRef ref{s_->catalog.library().config().library_id,
                                     params.at("asset_id").get<std::string>()};
      {
        const std::lock_guard lock(s_->catalog_mutex);
        auto old = s_->catalog.library().assets().snapshot(ref, true);
        if (old) {
          const auto& manifest = old->manifest().known();
          bool own = false;
          if (manifest.contains("inputs"))
            for (const auto& input : manifest.at("inputs"))
              if (input.at("id") == params.at("input_id")) own = true;
          if (!own || manifest.at("asset").at("id") != ref.asset_id)
            throw error("am.import.run.asset_id_conflict", c.id, Errc::conflict);
          if (old->in_trash()) {
            row["status"] = "deferred";
            row["asset_id"] = ref.asset_id;
            row["in_trash"] = true;
            row["message_id"] = "am.import.run.asset_in_trash";
            row["reason"] = "source_changed_during_import";
            row["warning_message_id"] = "am.import.run.restored_source_may_differ";
            auto out = jobs::JobOutcome::ask({"am.import.run.asset_in_trash", {"retry", "skip"}, row.dump()});
            out.result_json = row.dump();
            return out;
          }
          row["status"] = "created";
          row["asset_id"] = ref.asset_id;
          operation(*s_, ctx.record(), params, row);
          return jobs::JobOutcome::success(row.dump());
        }
        if (old.error().message_id == "am.repo.asset_id_exists")
          throw error("am.import.run.asset_id_conflict", c.id, Errc::conflict).caused_by(old.error());
        if (old.error().code != Errc::not_found) throw old.error();
      }
      std::map<std::size_t, PlanProblem> failed;
      std::map<std::size_t, std::string> hashes;
      const auto selected = execution_entries(plan, c);
      std::size_t included = 0;
      for (const auto ni : members(c))
        if (plan.nodes().at(ni).kind == NodeKind::directory && !plan.nodes().at(ni).available &&
            plan.mode() == ImportMode::strict)
          throw error("am.import.run.source_unavailable", plan.nodes().at(ni).id, Errc::io_error);
      for (const auto& e : selected)
        if (e.decision == EntryDecision::include) {
          const auto& n = plan.nodes().at(e.node);
          if (!n.available) {
            if (plan.mode() == ImportMode::strict)
              throw error("am.import.run.source_unavailable", n.id, Errc::io_error);
            continue;
          }
          if (n.kind == NodeKind::file) ++included;
        }
      for (std::size_t attempt = 0; attempt <= included; ++attempt) {
        for (const auto& e : selected)
          if (e.decision == EntryDecision::include && !failed.contains(e.node)) {
            const auto& n = plan.nodes().at(e.node);
            if (!n.available || n.kind != NodeKind::file) continue;
            // Stat checks remain mandatory on every retry. Hashing is outside the Catalog lock.
            row["node_id"] = n.id;
            fresh(*s_, plan, n);
            if (!n.sha256 && !hashes.contains(e.node)) {
              try {
                hashes.emplace(e.node, source_hash(*s_, plan, n, ctx.stop()));
              } catch (const Error& err) {
                if (plan.mode() == ImportMode::partial && (err.code == Errc::io_error || err.code == Errc::not_found))
                  failed.emplace(e.node, PlanProblem{"read_failed", err.message_id});
                else
                  throw;
              }
            }
          }
        row.erase("node_id");
        auto request = build(*s_, plan, c, params, failed, hashes, ctx.stop());
        std::optional<repository::SourceFailure> source_failure;
        request.source_failure = &source_failure;
        if (s_->hooks.before_attempt) s_->hooks.before_attempt(request);
        Result<catalog::CatalogChange> created = error("am.import.run.failed");
        {
          const std::lock_guard lock(s_->catalog_mutex);
          if (ctx.stop().stop_requested()) return jobs::JobOutcome::interrupted();
          created = s_->catalog.create(request);
        }
        if (!created) {
          if (created.error().code == Errc::cancelled) return jobs::JobOutcome::interrupted();
          if (created.error().message_id == "am.repo.asset_id_exists")
            throw error("am.import.run.asset_id_conflict", c.id, Errc::conflict).caused_by(created.error());
          if (source_failure) {
            auto at = std::find_if(selected.begin(), selected.end(), [&](const auto& e) {
              return params.at("nodes").at(plan.nodes().at(e.node).id).at("file_id") == source_failure->file_id;
            });
            if (at != selected.end()) {
              if (plan.mode() == ImportMode::partial && source_failure->read_failure && !failed.contains(at->node)) {
                failed.emplace(at->node, PlanProblem{"read_failed", source_failure->error.message_id});
                continue;
              }
              row["node_id"] = plan.nodes().at(at->node).id;
            }
          }
          if (created.error().message_id == "am.repo.source_changed")
            throw error("am.import.source_changed", row.value("node_id", c.id), Errc::conflict)
                .caused_by(created.error());
          throw created.error();
        }
        row["asset_id"] = ref.asset_id;
        row["status"] = "created";
        {
          const std::lock_guard lock(s_->catalog_mutex);
          operation(*s_, ctx.record(), params, row);
        }
        if (s_->hooks.after_created) take(s_->hooks.after_created(ref.asset_id));
        for (const auto& e : selected) {
          const auto& n = plan.nodes().at(e.node);
          if (e.decision != EntryDecision::include || !n.available || n.sha256 || failed.contains(e.node) ||
              n.kind != NodeKind::file)
            continue;
          try {
            fresh(*s_, plan, n);
          } catch (const Error&) {
            const std::lock_guard lock(s_->catalog_mutex);
            take(s_->catalog.trash(ref));
            row["in_trash"] = true;
            row["node_id"] = n.id;
            throw error("am.import.source_changed_during_import", n.id, Errc::conflict);
          }
        }
        return jobs::JobOutcome::success(row.dump());
      }
      throw error("am.import.run.failed", c.id, Errc::io_error);
    } catch (const Error& e) {
      if (e.code == Errc::cancelled) return jobs::JobOutcome::interrupted();
      row["status"] = "failed";
      row["error"] = failure(e);
      const auto diagnostic = error(e.message_id, {}, e.code);
      AM_LOG(
          warning, "services.import", "import.run.candidate_failed", "Import candidate failed",
          {{"plan_path", J::parse(ctx.record().params_json).value("plan_path", std::string{}), log::FieldKind::path}},
          &diagnostic);
      auto out = jobs::JobOutcome::failure(e);
      out.result_json = row.dump();
      return out;
    } catch (const std::exception& e) {
      return jobs::JobOutcome::failure(error("am.import.run.failed", e.what(), Errc::internal));
    }
  }

 private:
  std::shared_ptr<Shared> s_;
  jobs::JobOutcome ready(jobs::JobContext& ctx, const PlanView& plan, const PlanCandidate& c, const J& params, J row) {
    if (ctx.answer() == "skip") {
      row["status"] = "skipped";
      return jobs::JobOutcome::success(row.dump());
    }
    const auto ni = *c.inputs.at(0).objects.at(0).node;
    const auto& n = plan.nodes().at(ni);
    if (!n.available) throw error("am.import.run.source_unavailable", n.id, Errc::io_error);
    fresh(*s_, plan, n);
    if (n.sha256 && source_hash(*s_, plan, n, ctx.stop()) != *n.sha256)
      throw error("am.import.source_changed", n.id, Errc::conflict);
    repository::ImportReadyResult detail;
    Result<repository::ImportReadyResult> imported = error("am.import.run.failed");
    {
      const std::lock_guard lock(s_->catalog_mutex);
      imported = s_->catalog.import_ready({take(source_path(plan, n)), {}, ctx.stop()}, &detail);
      if (imported) detail = *imported;
      row["classification"] = detail.classification;
      if (!detail.ref.asset_id.empty()) row["asset_id"] = detail.ref.asset_id;
      if (!imported) {
        if (imported.error().code == Errc::cancelled) return jobs::JobOutcome::interrupted();
        throw imported.error();
      }
      if (detail.classification == "new") {
        row["status"] = "imported";
        operation(*s_, ctx.record(), params, row);
        return jobs::JobOutcome::success(row.dump());
      }
    }
    row["status"] = "deferred";
    row["message_id"] = "am.import.run.ready_conflict";
    auto out = jobs::JobOutcome::ask({"am.import.run.ready_conflict", {"skip"}, row.dump()});
    out.result_json = row.dump();
    return out;
  }
};
class ImportRun final : public jobs::JobHandler {
 public:
  explicit ImportRun(std::shared_ptr<Shared> s) : s_(std::move(s)) {}
  jobs::JobOutcome run(jobs::JobContext& ctx) override {
    try {
      AM_LOG(info, "services.import", "import.run.started", "Import run started", {});
      auto params = J::parse(ctx.record().params_json);
      auto children = ctx.children();
      if (children.error) return jobs::JobOutcome::failure(*children.error);
      auto loaded = guard<PlanView>([&]() -> Result<PlanView> { return load(params); });
      if (!loaded) {
        if (!children.settled()) return jobs::JobOutcome::await_children();
        auto out = jobs::JobOutcome::failure(loaded.error());
        J result{{"candidates", J::array()}};
        for (const auto& child : children.records) {
          if (child.result_json)
            result["candidates"].push_back(J::parse(*child.result_json));
          else
            result["candidates"].push_back({{"candidate_id", J::parse(child.params_json).at("candidate_id")},
                                            {"status", "failed"},
                                            {"error", failure(loaded.error())}});
        }
        out.result_json = result.dump();
        return out;
      }
      const auto& p = *loaded;
      ensure_children(p, params, children.records, [&](auto spec) { return ctx.enqueue_child(std::move(spec)); });
      children = ctx.children();
      if (children.error) return jobs::JobOutcome::failure(*children.error);
      if (!children.settled()) return jobs::JobOutcome::await_children();
      auto result = summary(p, children.records, false);
      auto out = jobs::JobOutcome::success(result.dump());
      for (const auto& child : children.records) {
        if (child.state == jc::JobState::failed || child.state == jc::JobState::resource_error ||
            child.state == jc::JobState::cannot_continue_safely) {
          out = jobs::JobOutcome::failure(error("am.import.run.candidate_failed", {}, Errc::io_error));
          break;
        }
        if (child.state == jc::JobState::needs_decision)
          out = jobs::JobOutcome::ask({"am.import.run.child_decision", {"retry"}, result.dump()});
      }
      out.result_json = result.dump();
      AM_LOG(info, "services.import", "import.run.finished", "Import run finished", {});
      return out;
    } catch (const Error& e) {
      return jobs::JobOutcome::failure(e);
    } catch (...) {
      return jobs::JobOutcome::failure(error("am.import.run.failed", {}, Errc::internal));
    }
  }

 private:
  std::shared_ptr<Shared> s_;
};
}  // namespace
Result<PlanView> validate_run_plan(const fs::path& file, const fs::path& library) {
  return guard<PlanView>([&]() -> Result<PlanView> {
    auto p = take(parse_plan_view(bytes(file)));
    ScanRequest request;
    request.library = library;
    for (std::size_t i = 0; i < p.roots().size(); ++i) {
      PlanNode selected;
      selected.root = i;
      request.roots.push_back(take(source_path(p, selected)));
    }
    take(validate_scan_paths(request));
    return p;
  });
}
Result<QueuedRun> enqueue_run(catalog::Catalog& catalog, const fs::path& file) {
  return guard<QueuedRun>([&]() -> Result<QueuedRun> {
    const auto content = bytes(file);
    const auto p = take(parse_plan_view(content));
    const auto sha = hash(content);
    jc::JobFilter roots;
    roots.top_level_only = true;
    for (const auto& r : take(catalog.job_store().list(roots)))
      if (r.type == "import.run") {
        const auto params = J::parse(r.params_json);
        if (params.at("plan_sha256") == sha) return QueuedRun{r.job_id, sha, true};
      }
    J params{{"plan_path", utf8(fs::absolute(file).lexically_normal())}, {"plan_sha256", sha}};
    jc::JobSpec parent;
    parent.type = "import.run";
    parent.params_json = params.dump();
    const auto record = take(catalog.job_store().enqueue(parent));
    ensure_children(p, params, {}, [&](auto spec) {
      spec.parent_id = record.job_id;
      return catalog.job_store().enqueue(spec);
    });
    return QueuedRun{record.job_id, sha, false};
  });
}
Result<PlanJson> run_summary(catalog::Catalog& catalog, const QueuedRun& run) {
  return guard<J>([&]() -> Result<J> {
    const auto parent = take(catalog.job_store().get(run.job_id));
    if (!parent) return error("am.jobs.not_found");
    auto loaded = guard<PlanView>([&]() -> Result<PlanView> { return load(J::parse(parent->params_json)); });
    if (!loaded) {
      if (parent->result_json) {
        auto result = J::parse(*parent->result_json);
        result["job_id"] = run.job_id;
        result["plan_sha256"] = run.plan_sha256;
        return result;
      }
      return loaded.error();
    }
    const auto& p = *loaded;
    jc::JobFilter filter;
    filter.parent_id = run.job_id;
    auto result = summary(p, take(catalog.job_store().list(filter)), run.reused);
    result["job_id"] = run.job_id;
    result["plan_sha256"] = run.plan_sha256;
    return result;
  });
}
Result<void> register_import_jobs(jobs::HandlerRegistry& registry, catalog::Catalog& catalog, std::string device,
                                  RunHooks hooks) {
  return guard<void>([&]() -> Result<void> {
    auto state = std::make_shared<Shared>(catalog, std::move(device), std::move(hooks));
    take(registry.add("import.run", [state](const auto&) { return std::make_unique<ImportRun>(state); }));
    return registry.add("import.asset", [state](const auto&) { return std::make_unique<ImportAsset>(state); });
  });
}
}  // namespace am::import
