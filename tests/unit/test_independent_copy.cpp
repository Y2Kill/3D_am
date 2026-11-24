#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <stdexcept>
#include "am/application/library_service.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/catalog/indexer.hpp"
#include "zip_builder.hpp"

namespace {
namespace fs = std::filesystem;
namespace repo = am::repository;
namespace zip = am::asset_zip;
namespace lib = am::library;
namespace cat = am::catalog;
using Json = repo::Json;
template<class T> T value(am::Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(r).value();
}
void value(am::Result<void> r) { if (!r) throw std::runtime_error(am::to_log_string(r.error())); }
std::string id() { return am::Uuid::v4().to_string(); }
constexpr auto now = "2026-11-17T00:00:00.000Z";
std::string bytes(const fs::path& p) { std::ifstream f(am::paths::native(p),std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
void write(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path()); std::ofstream f(am::paths::native(p),std::ios::binary); f << s;
  if (!f) throw std::runtime_error("Cannot write synthetic fixture");
}
std::string hash(const fs::path& p) {
  const auto data = bytes(p); auto h = value(zip::Sha256::create()); value(h.update(std::as_bytes(std::span(data))));
  const auto digest = value(h.finish()); std::string out; constexpr char hex[] = "0123456789abcdef";
  for (auto b : digest) { const auto n = std::to_integer<unsigned>(b); out += hex[n>>4]; out += hex[n&15]; } return out;
}
lib::OpenOptions options() { lib::OpenOptions o; o.capabilities.emplace(); o.capabilities->mode = repo::StorageMode::full; return o; }
fs::path fresh() {
  const auto root = fs::path(AM_LIBRARY_TEST_ROOT)/id(); fs::create_directories(root.parent_path()); value(lib::Library::init(root)); return root;
}
fs::path target(const fs::path& root, const std::string& asset) { return root/"assets"/asset.substr(34)/(asset+".zip"); }
struct World {
  fs::path root = fresh();
  cat::Catalog catalog;
  repo::ChangeResult asset;
  explicit World(cat::IndexerHooks hooks = {}) : catalog(value(cat::Catalog::open(root,options(),std::move(hooks)))) {
    value(catalog.indexer().rebuild());
    repo::CreateRequest r; r.metadata = Json{{"title","Independent copy"}};
    repo::AddFiles add;
    add.input = {{"id",id()},{"imported_at",now},{"mode","strict"},{"completeness","complete"},{"objects",Json::array()},{"entries",Json::array()}};
    for (int i = 0; i < 2; ++i) {
      repo::FileInput f; f.path = "file-"+std::to_string(i)+".bin"; f.source = root/("source-"+std::to_string(i));
      write(f.source,std::string(4000,static_cast<char>('a'+i))); add.files.push_back(f);
    }
    r.files.push_back(std::move(add)); asset = value(catalog.create(r)).change;
  }
  repo::AssetRepository& repository() { return catalog.library().assets(); }
  Json plain() { return Json::parse(value(repository().snapshot(asset.ref)).manifest_text()); }
  void install(const Json& j, bool opaque = false, const std::optional<std::string>& text = {}) {
    std::vector<zip_test::Entry> records;
    zip_test::Entry manifest; manifest.name = "manifest.json"; manifest.data = text.value_or(j.dump()); records.push_back(manifest);
    for (std::size_t i = 0; i < j["files"].size(); ++i) {
      zip_test::Entry e; e.name = j["files"][i]["storage_path"].get<std::string>();
      e.data = std::string(4000,static_cast<char>('a'+i)); e.method = 8; records.push_back(e);
    }
    if (opaque) { zip_test::Entry e; e.name = "unknown.bin"; records.push_back(e); }
    const auto built = zip_test::build(records);
    write(target(root,asset.ref.asset_id),std::string(reinterpret_cast<const char*>(built.bytes.data()),built.bytes.size()));
  }
  void clean() {
    EXPECT_TRUE(value(value(am::db::StateJournal::open(root))->list()).empty());
    EXPECT_TRUE(fs::is_empty(root/".am/.am-tmp"));
  }
};
Json claim(const Json& target, const Json& evidence = Json::array()) {
  return {{"id",id()},{"target",target},{"relation","supports_selected"},{"method","user"},{"created_at",now},{"evidence",evidence}};
}
Json complete(World& w) {
  auto j = w.plain();
  const auto f0 = j["files"][0]["id"], f1 = j["files"][1]["id"];
  const auto source = id(), outer = id(), inner = id(), e0 = id(), e1 = id(), group = id(), child = id(), preview = id();
  j["origin"]["sources"] = Json::array({{{"id",source},{"kind","test"},{"claims",Json::array({claim({{"kind","self"}})})}}});
  auto& input = j["inputs"][0]; input["source_refs"] = Json::array({source});
  input["objects"] = Json::array({{{"id",outer},{"kind","archive"},{"name","outer.zip"},{"container","zip"}},
    {{"id",inner},{"kind","archive"},{"name","inner.zip"},{"container","zip"},{"parent_object_id",outer},{"parent_entry_id",e0}}});
  input["entries"] = Json::array({{{"id",e0},{"object_id",outer},{"entry_kind","file"},{"path","inner.zip"},{"result","included"},{"file_id",f0}},
    {{"id",e1},{"object_id",inner},{"entry_kind","file"},{"path","part.bin"},{"size_bytes","4000"},{"result","included"},{"file_id",f0}}});
  j["files"][0]["origin_kind"] = "imported";
  j["files"][0]["type"] = {{"id","mesh.test"},{"by","user"}};
  j["files"][1]["origin_kind"] = "generated"; j["files"][1]["storage_path"] = "generated/derived.bin";
  j["files"][1]["generation"] = {{"tool","core:test"},{"params",{{"exact",9007199254740993ULL}}}};
  j["files"][1]["derived_from"] = Json::array({{{"file_id",f0},{"sha256",j["files"][0]["sha256"]}}});
  j["groups"] = Json::array({{{"id",group},{"name","Root"},{"type","model"},{"file_refs",Json::array({f0})}},
    {{"id",child},{"name","Child"},{"type","model"},{"parent_id",group},{"file_refs",Json::array({f1})}}});
  j["previews"] = {{"primary_id",preview},{"items",Json::array({{{"id",preview},{"file_id",f1},{"group_refs",Json::array({child})}}})}};
  const std::string field = "user:"+id();
  j["field_definitions"] = Json::array({{{"id",field},{"schema_revision",1},{"name","File"},{"type","file_ref"},
    {"scopes",Json::array({"asset","file","group"})},{"default",f0}}});
  for (auto* owner : {&j["asset"],&j["files"][0],&j["groups"][0]}) {
    const auto author = id(); auto& m = (*owner)["metadata"];
    m["authors"] = Json::array({{{"id",author},{"name","Author"}}});
    m["license"] = {{"file_ref",f0}};
    m["custom_values"] = Json::array({{{"field_id",field},{"value",f0}}});
    const Json evidence = Json::array({{{"file_id",f0}},{{"source_id",source}},{{"input_object_id",inner}},{{"source_entry_id",e1}}});
    auto authors = claim({{"kind","standard_item"},{"field","authors"},{"item_id",author}},evidence);
    auto custom = claim({{"kind","custom"},{"field_id",field}},evidence); custom["relation"] = "alternative"; custom["value"] = f1;
    auto alt_authors = claim({{"kind","standard"},{"field","authors"}}); alt_authors["relation"] = "alternative";
    alt_authors["value"] = Json::array({{{"id",id()},{"name","Other author"}}});
    auto license = claim({{"kind","standard"},{"field","license"}}); license["relation"] = "alternative"; license["value"] = {{"file_ref",f1}};
    m["claims"] = Json::array({authors,custom,alt_authors,license});
  }
  return j;
}
std::vector<std::byte> raw(const zip::ZipArchive& archive, std::size_t index) {
  const auto size = archive.entries()[index].compressed_size; std::vector<std::byte> out(static_cast<std::size_t>(size));
  if (value(archive.read_raw(index,0,out)) != out.size()) throw std::runtime_error("Short raw read");
  return out;
}
// Independent oracle: pair the same structural scalar locations, then check
// every UUID-bearing string, including references and unchanged contract IDs.
void compare(const Json& before, const Json& after, std::map<std::string,std::string>& mapping) {
  if (before.is_object()) {
    for (auto i = before.begin(); i != before.end(); ++i) {
      if (i.key() == "history" || i.key() == "created_at" || i.key() == "modified_at" || i.key() == "revision") continue;
      ASSERT_TRUE(after.contains(i.key())) << i.key(); compare(i.value(),after[i.key()],mapping);
    }
  } else if (before.is_array()) {
    ASSERT_EQ(before.size(),after.size()); for (std::size_t i = 0; i < before.size(); ++i) compare(before[i],after[i],mapping);
  } else if (before.is_string()) {
    const auto old = before.get<std::string>(), next = after.get<std::string>();
    const auto uuid = am::Uuid::parse_canonical(old);
    if (uuid && uuid->version() == 4) {
      ASSERT_NE(old,next); const auto generated = am::Uuid::parse_canonical(next); ASSERT_TRUE(generated); EXPECT_EQ(generated->version(),4);
      auto [it,inserted] = mapping.emplace(old,next); (void)inserted; EXPECT_EQ(it->second,next);
    } else if (!old.starts_with("content/")) { EXPECT_EQ(old,next); }
  } else { EXPECT_EQ(before,after); }
}
}  // namespace

TEST(IndependentCopy, D05RevisionTwelveSourceUnchangedIndexAndUserdata) {
  RecordProperty("req","K-3");
  World w;
  for (int i = 2; i <= 12; ++i) {
    repo::ChangeRequest r; r.ref = w.asset.ref; r.expected_rev = w.asset.revision;
    r.ops.emplace_back(repo::SetMetadata{"asset","title",std::optional<Json>(Json("Revision "+std::to_string(i)))});
    w.asset = value(w.catalog.change(r)).change;
  }
  auto db = am::db::Connection::borrow(w.catalog.library().database(lib::DatabaseKind::userdata));
  value(db.exec("INSERT INTO collections(coll_id,name,created_at) VALUES('test','Favorites','2026-11-17')"));
  value(db.exec("INSERT INTO collection_items(coll_id,asset_id,added_at) VALUES('test','"+w.asset.ref.asset_id+"','2026-11-17')"));
  value(db.exec("PRAGMA wal_checkpoint(TRUNCATE)"));
  const auto original = hash(target(w.root,w.asset.ref.asset_id)), userdata = hash(w.root/".am/userdata.sqlite");
  const auto copied = value(w.catalog.copy_independent({w.asset.ref,{},{}})).copy;
  EXPECT_NE(copied.ref.asset_id,w.asset.ref.asset_id); EXPECT_EQ(copied.source_revision,12u); EXPECT_EQ(copied.change.revision,1u);
  const auto snap = value(w.repository().snapshot(copied.ref)); const auto j = Json::parse(snap.manifest_text());
  ASSERT_EQ(j["history"]["entries"].size(),1u); EXPECT_FALSE(j["history"].contains("compacted_ranges"));
  const auto& h = j["history"]["entries"][0]; EXPECT_EQ(h["operation"],"asset_created");
  EXPECT_EQ(h["details"],(Json{{"creation_kind","independent_copy"},{"source_asset",{{"asset_id",w.asset.ref.asset_id},{"revision",12}}}}));
  EXPECT_EQ(j["asset"]["created_at"],j["asset"]["modified_at"]);
  EXPECT_EQ(hash(target(w.root,w.asset.ref.asset_id)),original); EXPECT_EQ(hash(w.root/".am/userdata.sqlite"),userdata);
  auto count = value(db.prepare("SELECT count(*) FROM collection_items WHERE asset_id=?1")); value(count.bind(1,copied.ref.asset_id));
  ASSERT_EQ(value(count.step()),am::db::Step::row); EXPECT_EQ(count.integer(0),0);
  EXPECT_EQ(value(w.catalog.indexer().list()).size(),2u); w.clean();
}
TEST(IndependentCopy, D06EveryReferenceAndRawCompressedPayloadIsPreserved) {
  RecordProperty("req","K-3");
  World w; const auto before = complete(w); w.install(before); value(w.catalog.indexer().update(w.asset.ref));
  const auto checked = value(w.repository().verify(w.asset.ref)); ASSERT_FALSE(checked.error) << (checked.error ? am::to_log_string(*checked.error) : "");
  const auto copied = value(w.catalog.copy_independent({w.asset.ref,{},{}})).copy;
  auto source = value(w.repository().snapshot(w.asset.ref)), next = value(w.repository().snapshot(copied.ref));
  const auto after = Json::parse(next.manifest_text()); std::map<std::string,std::string> mapping; compare(before,after,mapping);
  std::set<std::string> old_ids,new_ids; for (const auto& [old,new_id] : mapping) { old_ids.insert(old); EXPECT_TRUE(new_ids.insert(new_id).second); }
  for (const auto& new_id : new_ids) EXPECT_FALSE(old_ids.contains(new_id));
  EXPECT_GE(mapping.size(),24u);
  for (std::size_t i = 0; i < before["files"].size(); ++i) {
    const auto old_path = before["files"][i]["storage_path"].get<std::string>(), path = after["files"][i]["storage_path"].get<std::string>();
    if (old_path.starts_with("content/")) { EXPECT_EQ(path,"content/"+mapping.at(before["inputs"][0]["id"].get<std::string>())+old_path.substr(44)); }
    const auto& a = source.archive().entries()[i+1]; const auto& b = next.archive().entries()[i+1];
    EXPECT_EQ(a.crc32,b.crc32); EXPECT_EQ(a.compressed_size,b.compressed_size); EXPECT_EQ(a.method,b.method);
    EXPECT_EQ(raw(source.archive(),a.index),raw(next.archive(),b.index));
    EXPECT_EQ(before["files"][i]["sha256"],after["files"][i]["sha256"]);
  }
  const auto report = value(w.repository().verify(copied.ref)); EXPECT_EQ(report.v2,repo::CheckStatus::passed); EXPECT_FALSE(report.error);
  EXPECT_EQ(after["files"][1]["generation"]["params"]["exact"],9007199254740993ULL); w.clean();
}
TEST(IndependentCopy, D07RefusalsLeaveSourceJournalTemporaryAndIndexUntouched) {
  RecordProperty("req","K-3");
  for (const auto* kind : {"extensions","plugin","historical","derived","opaque","metadata","params","key"}) {
    SCOPED_TRACE(kind); World w; auto j = w.plain(); std::string message;
    if (std::string_view(kind) == "extensions") { j["extensions"] = {{"org.test.copy",{{"schema_version","1"},{"data",Json::object()},{"attachments",Json::array({{{"id",id()},{"path","extensions/test.bin"},{"size_bytes","0"},{"sha256",std::string(64,'0')}}})}}}}; message = "copy_extensions_unsupported"; }
    if (std::string_view(kind) == "plugin") {
      const auto field = "user:"+id(); j["field_definitions"] = Json::array({{{"id",field},{"schema_revision",1},{"name","Plugin"},{"type","plugin:org.test.copy:data"},{"scopes",Json::array({"asset"})}}});
      j["asset"]["metadata"]["custom_values"] = Json::array({{{"field_id",field},{"value",Json::object()}}}); message = "copy_plugin_values_unsupported";
    }
    if (std::string_view(kind) == "historical") { j["historical_files"] = Json::array({{{"id",id()},{"removed_at_revision",1}}}); message = "copy_history_unsupported"; }
    if (std::string_view(kind) == "derived") { j["files"][1]["derived_from"] = Json::array({{{"file_id",id()},{"sha256",std::string(64,'0')}}}); message = "copy_history_unsupported"; }
    if (std::string_view(kind) == "opaque") message = "copy_opaque_entries";
    if (std::string_view(kind) == "metadata") { j["asset"]["metadata"]["description"] = "Embedded "+j["files"][0]["id"].get<std::string>(); message = "copy_remap_incomplete"; }
    if (std::string_view(kind) == "params" || std::string_view(kind) == "key") {
      auto& f = j["files"][1]; f["origin_kind"] = "generated"; f["generation"] = {{"tool","core:test"},{"params",Json::object()}};
      if (std::string_view(kind) == "key") f["generation"]["params"][j["files"][0]["id"].get<std::string>()] = true;
      else f["generation"]["params"]["ref"] = j["files"][0]["id"];
      message = "copy_remap_incomplete";
    }
    w.install(j,std::string_view(kind) == "opaque");
    const auto before = hash(target(w.root,w.asset.ref.asset_id)); const auto new_id = id();
    auto copied = w.catalog.copy_independent({w.asset.ref,new_id,{}}); ASSERT_FALSE(copied);
    EXPECT_EQ(copied.error().code,am::Errc::unsupported); EXPECT_EQ(copied.error().message_id,"am.repo."+message);
    if (message == "copy_remap_incomplete") { EXPECT_TRUE(copied.error().ctx.detail.starts_with("/")); }
    EXPECT_FALSE(fs::exists(target(w.root,new_id))); EXPECT_EQ(hash(target(w.root,w.asset.ref.asset_id)),before);
    EXPECT_EQ(value(w.catalog.indexer().list()).size(),1u); w.clean();
  }
}
TEST(IndependentCopy, RepeatedCopiesCopyOfCopyAndSourceRestrictions) {
  RecordProperty("req","K-3"); World w;
  const auto a = value(w.catalog.copy_independent({w.asset.ref,{},{}})).copy, b = value(w.catalog.copy_independent({w.asset.ref,{},{}})).copy;
  EXPECT_NE(a.ref.asset_id,b.ref.asset_id);
  const auto c = value(w.catalog.copy_independent({a.ref,{},{}})).copy;
  const auto j = Json::parse(value(w.repository().snapshot(c.ref)).manifest_text());
  EXPECT_EQ(j["history"]["entries"][0]["details"]["source_asset"]["asset_id"],a.ref.asset_id);
  auto occupied = w.catalog.copy_independent({w.asset.ref,a.ref.asset_id,{}}); ASSERT_FALSE(occupied); EXPECT_EQ(occupied.error().message_id,"am.repo.asset_id_exists");
  auto without_membership = w.plain();
  without_membership["groups"] = Json::array({{{"id",id()},{"name","Empty group"},{"type","custom"}}});
  w.install(without_membership);
  const auto empty_group = value(w.catalog.copy_independent({w.asset.ref,{},{}})).copy;
  const auto written = Json::parse(value(w.repository().snapshot(empty_group.ref)).manifest_text());
  ASSERT_TRUE(written["groups"][0].contains("file_refs")); EXPECT_EQ(written["groups"][0]["file_refs"],Json::array());
  value(w.catalog.trash(w.asset.ref)); auto trashed = w.catalog.copy_independent({w.asset.ref,{},{}}); ASSERT_FALSE(trashed); EXPECT_EQ(trashed.error().code,am::Errc::not_found);
  value(w.catalog.restore(w.asset.ref)); auto newer = w.plain(); newer["format"]["version"]["minor"] = 1; w.install(newer);
  auto refused = w.catalog.copy_independent({w.asset.ref,{},{}}); ASSERT_FALSE(refused); EXPECT_EQ(refused.error().message_id,"am.repo.write_unsupported_version"); w.clean();
}
TEST(IndependentCopy, D07AllDependencyKindsAndOwnerScopesAreRefusedInP0) {
  RecordProperty("req","K-3");
  World w; auto base = complete(w);
  const auto field = base["field_definitions"][0]["id"].get<std::string>();
  const Json refs = Json::array({
    {{"kind","file"},{"file_id",base["files"][0]["id"]},{"sha256",base["files"][0]["sha256"]}},
    {{"kind","group"},{"group_id",base["groups"][0]["id"]}},
    {{"kind","field_value"},{"owner_scope","asset"},{"owner_id",base["asset"]["id"]},{"field_id",field}},
    {{"kind","field_value"},{"owner_scope","file"},{"owner_id",base["files"][0]["id"]},{"field_id",field}},
    {{"kind","field_value"},{"owner_scope","group"},{"owner_id",base["groups"][0]["id"]},{"field_id",field}}});
  for (const bool extension : {false,true}) for (const auto& ref : refs) {
    SCOPED_TRACE(std::string(extension ? "extension:" : "plugin:")+ref.dump()); auto j = base;
    const Json dependencies{{"state","declared"},{"refs",Json::array({ref})}};
    if (extension) j["extensions"] = {{"org.test.copy",{{"schema_version","1"},{"data",Json::object()},{"dependencies",dependencies}}}};
    else {
      const auto plugin_field = "plugin:org.test.copy:value";
      j["field_definitions"].push_back({{"id",plugin_field},{"schema_revision",1},{"name","Plugin"},
        {"type","plugin:org.test.copy:data"},{"scopes",Json::array({"asset"})}});
      j["asset"]["metadata"]["custom_values"].push_back({{"field_id",plugin_field},{"value",Json::object()},{"dependencies",dependencies}});
    }
    w.install(j);
    const auto verified = value(w.repository().verify(w.asset.ref));
    ASSERT_FALSE(verified.error) << (verified.error ? am::to_log_string(*verified.error) : "");
    const auto before = hash(target(w.root,w.asset.ref.asset_id)), state = hash(w.root/".am/state.sqlite"), index = hash(w.root/".am/index.sqlite");
    const auto requested = id(); const auto result = w.catalog.copy_independent({w.asset.ref,requested,{}}); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().message_id,extension ? "am.repo.copy_extensions_unsupported" : "am.repo.copy_plugin_values_unsupported");
    EXPECT_EQ(result.error().code,am::Errc::unsupported); EXPECT_FALSE(fs::exists(target(w.root,requested)));
    EXPECT_EQ(hash(target(w.root,w.asset.ref.asset_id)),before); EXPECT_EQ(hash(w.root/".am/state.sqlite"),state); EXPECT_EQ(hash(w.root/".am/index.sqlite"),index);
    w.clean();
  }
}
TEST(IndependentCopy, OpaqueNumericLexemesSurviveAndEscapedOldIdsAreRefused) {
  RecordProperty("req","K-3");
  World w; auto j = complete(w); auto text = j.dump();
  const auto number = text.find("9007199254740993"); ASSERT_NE(number,std::string::npos);
  const std::string exact = "[1e9999,-0,1.000,9007199254740993]";
  text.replace(number,16,exact); w.install(j,false,text);
  const auto copied = value(w.catalog.copy_independent({w.asset.ref,{},{}})).copy;
  const auto snap = value(w.repository().snapshot(copied.ref)); EXPECT_NE(snap.manifest_text().find(exact),std::string::npos);
  auto& params = j["files"][1]["generation"]["params"]; const auto old = j["files"][0]["id"].get<std::string>();
  params = {{"ref",old}}; text = j.dump();
  std::string escaped; constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : old) { escaped += "\\u00"; escaped += hex[c>>4]; escaped += hex[c&15]; }
  const auto ref = text.find("\"ref\":\""+old+"\""); ASSERT_NE(ref,std::string::npos);
  text.replace(ref,8+old.size(),"\"ref\":\""+escaped+"\""); w.install(j,false,text);
  const auto refused = w.catalog.copy_independent({w.asset.ref,{},{}}); ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().message_id,"am.repo.copy_remap_incomplete");
  EXPECT_EQ(refused.error().ctx.detail,"/files/1/generation/params/ref"); w.clean();
}
TEST(IndependentCopy, CommitFaultsRecoverWithIndexAndUnchangedSource) {
  RecordProperty("req","K-3");
  for (auto point : {am::commit::FaultPoint::after_lock,am::commit::FaultPoint::after_prepared,am::commit::FaultPoint::mid_write,
       am::commit::FaultPoint::after_finish,am::commit::FaultPoint::after_verify,am::commit::FaultPoint::after_verified_journal,
       am::commit::FaultPoint::before_replace,am::commit::FaultPoint::after_replace,am::commit::FaultPoint::after_committed_journal,am::commit::FaultPoint::after_indexed_journal}) {
    SCOPED_TRACE(am::commit::point_name(point)); World w; repo::RepositoryOptions o;
    o.library_id = w.asset.ref.library_id; o.capabilities = options().capabilities;
    o.on_indexed = [&](const auto& e) { value(w.catalog.indexer().update(e.ref)); EXPECT_EQ(e.kind,"create"); };
    o.test_hooks.commit.point = [point](auto current) -> am::Result<void> {
      if (current == point) return am::Error::make(am::Errc::io_error,"Injected copy fault").id("am.repo.io_error");
      return {};
    };
    o.test_hooks.commit.mid_write_after_bytes = 1;
    auto repository = value(repo::AssetRepository::open(w.root,o)); const auto new_id = id(), original = hash(target(w.root,w.asset.ref.asset_id));
    ASSERT_FALSE(repository.copy_independent({w.asset.ref,new_id,{}}));
    value(repository.recover()); const bool exists = fs::exists(target(w.root,new_id));
    EXPECT_EQ(value(w.catalog.indexer().list()).size(),exists ? 2u : 1u);
    if (exists) { EXPECT_FALSE(value(repository.verify({w.asset.ref.library_id,new_id})).error); }
    EXPECT_EQ(hash(target(w.root,w.asset.ref.asset_id)),original); w.clean();
  }
}
TEST(IndependentCopy, IndexFailureWarnsAfterCommittedCopy) {
  RecordProperty("req","K-3");
  bool fail = false; cat::IndexerHooks hooks; hooks.before_update = [&](auto) -> am::Result<void> {
    if (fail) return am::Error::make(am::Errc::io_error,"Injected index error");
    return {};
  };
  World w(hooks); fail = true; const auto copied = value(w.catalog.copy_independent({w.asset.ref,{},{}}));
  ASSERT_FALSE(copied.warnings.empty()); EXPECT_EQ(copied.warnings[0].message_id,"am.catalog.index_update_failed");
  EXPECT_TRUE(fs::exists(target(w.root,copied.copy.ref.asset_id))); EXPECT_TRUE(value(w.catalog.indexer().rebuild_required())); w.clean();
}
TEST(IndependentCopy, RetainsStartingSnapshotWhileSourceCanAdvance) {
  RecordProperty("req","K-3"); World w;
  repo::RepositoryOptions o; o.library_id = w.asset.ref.library_id; o.capabilities = options().capabilities;
  o.test_hooks.after_preflight = [&] {
    repo::ChangeRequest change; change.ref = w.asset.ref; change.expected_rev = 1;
    change.ops.emplace_back(repo::SetMetadata{"asset","title",std::optional<Json>(Json("Source advanced"))});
    w.asset = value(w.catalog.change(change)).change;
  };
  o.on_indexed = [&](const auto& event) { value(w.catalog.indexer().update(event.ref)); };
  auto repository = value(repo::AssetRepository::open(w.root,o));
  const auto copied = value(repository.copy_independent({w.asset.ref,{},{}}));
  EXPECT_EQ(copied.source_revision,1u); EXPECT_EQ(w.asset.revision,2u);
  const auto snapshot = value(repository.snapshot(copied.ref)); const auto j = Json::parse(snapshot.manifest_text());
  EXPECT_EQ(j["asset"]["metadata"]["title"],"Independent copy");
  EXPECT_EQ(j["history"]["entries"][0]["details"]["source_asset"]["revision"],1);
  EXPECT_EQ(value(w.catalog.indexer().list()).size(),2u); w.clean();
}
