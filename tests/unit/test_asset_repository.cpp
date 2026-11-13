#include <gtest/gtest.h>
#include "am/storage/repository/repository.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/foundation/json_text.hpp"
#include "zip_builder.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <map>
#include <set>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
using namespace am;
using namespace am::repository;
namespace {
using Path = std::filesystem::path;
template<class T> T value(Result<T> r) { if (!r) throw std::runtime_error(to_log_string(r.error())); return std::move(r).value(); }
void value(Result<void> r) { if (!r) throw std::runtime_error(to_log_string(r.error())); }
std::string id() { return Uuid::v4().to_string(); }
constexpr const char* library_id = "550e8400-e29b-41d4-a716-446655440000";
Path fresh() { auto p = Path(AM_REPO_TEST_ROOT) / id(); std::filesystem::create_directories(p); return p; }
RepositoryOptions options() {
  RepositoryOptions o; o.library_id = library_id; o.application_version = "0.1.0-test";
#ifndef _WIN32
  // fs_support.md v0.2 keeps production Linux storage read-only; these tests exercise
  // the protocol with explicit synthetic capabilities, not a filesystem certification.
  o.capabilities.emplace();
  o.capabilities->mode = StorageMode::full;
  o.capabilities->filesystem = "synthetic unit-test capabilities";
#endif
  o.clock = [] { return std::chrono::system_clock::time_point(std::chrono::seconds(1794441600)) + std::chrono::milliseconds(123); }; return o;
}
AssetRef ref(const std::string& asset) { return {library_id,asset}; }
Path target(const Path& root, const std::string& asset) { return root / "assets" / asset.substr(asset.size()-2) / (asset + ".zip"); }
Path source(const Path& root, const std::string& bytes = "content") {
  auto path = root / (id()+".bin"); std::ofstream file(path,std::ios::binary); file << bytes; if (!file) throw std::runtime_error("Write source failed"); return path;
}
AddFiles addition(const Path& root, std::size_t count = 1) {
  AddFiles a; a.input = {{"id",id()},{"imported_at","2026-11-12T00:00:00.000Z"},{"mode","strict"},{"completeness","complete"},{"objects",Json::array()},{"entries",Json::array()}};
  for (std::size_t i = 0; i < count; ++i) { FileInput f; f.path = "file-"+std::to_string(i)+".bin"; f.source = source(root,"data-"+std::to_string(i)); f.file_id = id(); a.files.push_back(std::move(f)); } return a;
}
ChangeResult create(AssetRepository& repo, const Path& root, std::size_t count = 1) { CreateRequest r; r.files.push_back(addition(root,count)); return value(repo.create(r)); }
ChangeRequest metadata(const ChangeResult& current, std::string field = "title", Json v = "new") { ChangeRequest r; r.ref = current.ref; r.expected_rev = current.revision; r.ops.push_back(SetMetadata{"asset",std::move(field),std::move(v)}); return r; }
std::vector<std::byte> bytes(const Path& path) { std::ifstream in(path,std::ios::binary); std::string data((std::istreambuf_iterator<char>(in)),{}); auto span = std::as_bytes(std::span(data)); return {span.begin(),span.end()}; }
void clean(const Path& root) { auto journal = value(db::StateJournal::open(root)); EXPECT_TRUE(value(journal->list()).empty()); auto dir = root / ".am/.am-tmp"; if (std::filesystem::exists(dir)) EXPECT_TRUE(std::filesystem::is_empty(dir)); }
const asset_zip::EntryInfo& entry(const AssetSnapshot& snap, const std::string& name) { for (const auto& e : snap.archive().entries()) if (e.raw_name == name) return e; throw std::runtime_error("Missing fixture entry"); }
std::vector<std::byte> raw(const AssetSnapshot& snap, std::size_t index) { const auto size = snap.archive().entries()[index].compressed_size; std::vector<std::byte> out(static_cast<std::size_t>(size)); auto n = value(snap.archive().read_raw(index,0,out)); if (n != out.size()) throw std::runtime_error("Read compressed fixture failed"); return out; }
void add_stream(asset_zip::ZipWriter& w, const std::string& name, const std::string& data) { asset_zip::EntryOptions o; o.uncompressed_size = data.size(); o.method = asset_zip::CompressionMethod::stored; auto e = value(w.begin_entry(name,o)); value(e.write(std::as_bytes(std::span(data)))); value(e.finish()); }
std::string sha(const std::string& data) { auto h = value(asset_zip::Sha256::create()); value(h.update(std::as_bytes(std::span(data)))); auto digest = value(h.finish()); std::string text; constexpr char digits[] = "0123456789abcdef"; for (auto b : digest) { auto v=std::to_integer<unsigned>(b); text+=digits[v>>4]; text+=digits[v&15]; } return text; }
void fixture(AssetRepository& repo, const Path& root, const AssetRef& ref, const std::string& manifest, const std::map<std::string,std::string>& extras = {}) {
  auto old = value(repo.snapshot(ref)); auto sink = std::make_shared<asset_zip::MemorySink>(); auto writer = value(asset_zip::ZipWriter::create(sink)); add_stream(writer,"manifest.json",manifest); asset_zip::ReadBudget budget(10000000);
  for (const auto& e : old.archive().entries()) if (e.raw_name != "manifest.json") value(writer.copy_raw(old.archive(),e.index,budget));
  for (const auto& [name,data] : extras) add_stream(writer,name,data); value(writer.finish());
  auto path = target(root,ref.asset_id); std::filesystem::remove(path); std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(sink->bytes().data()),static_cast<std::streamsize>(sink->bytes().size())); if (!out) throw std::runtime_error("Install synthetic fixture failed");
}
Json plain(const AssetSnapshot& snap) { return Json::parse(snap.manifest_text()); }
std::string replace_token(std::string text, const std::string& token, const std::string& fragment) { auto where=text.find('"'+token+'"'); if(where==std::string::npos) throw std::runtime_error("Missing fixture token"); text.replace(where,token.size()+2,fragment); return text; }
std::map<std::string,std::string> opaque_numbers(const std::string& text) {
  auto tree = value(am::json::parse_strict(text)); std::map<std::string,std::string> numbers;
  for(std::size_t i=0;i<tree.nodes.size();++i) if(tree.nodes[i].kind==am::json::Kind::number) {
    std::vector<std::string> components; bool opaque=false;
    for(auto n=i;tree.nodes[n].parent!=am::json::no_node;n=tree.nodes[n].parent) {
      const auto& node=tree.nodes[n]; if(node.edge=="params"||node.edge=="data"||node.edge=="details") opaque=true;
      components.push_back(tree.nodes[node.parent].kind==am::json::Kind::array ? std::to_string(node.index) : node.edge);
    }
    if(opaque) { std::string path; for(auto n=components.rbegin();n!=components.rend();++n) path+="/"+*n; numbers[path]=text.substr(tree.nodes[i].begin,tree.nodes[i].end-tree.nodes[i].begin); }
  } return numbers;
}
#ifdef _WIN32
class Process {
 public:
  HANDLE handle=nullptr;
  Process(std::string_view mode,const Path& root,const std::string& asset,const std::string& arg) {
    auto wide=[](std::string_view s){return std::wstring(s.begin(),s.end());}; const auto exe=Path(AM_REPO_HELPER).wstring();
    std::wstring cmd=L"\""+exe+L"\" "+wide(mode)+L" \""+root.wstring()+L"\" "+wide(asset)+L" \""+wide(arg)+L"\"";
    STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION p{};
    if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&p))throw std::runtime_error("Create repository helper failed");handle=p.hProcess;CloseHandle(p.hThread);
  }
  ~Process(){if(handle){if(WaitForSingleObject(handle,0)==WAIT_TIMEOUT){TerminateProcess(handle,92);WaitForSingleObject(handle,30000);}CloseHandle(handle);}}
  DWORD wait(){if(WaitForSingleObject(handle,30000)!=WAIT_OBJECT_0)throw std::runtime_error("Repository helper timed out");DWORD code=0;GetExitCodeProcess(handle,&code);return code;}
  void kill(){TerminateProcess(handle,92);(void)wait();}
};
void ready(const Path& path){auto until=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(!std::filesystem::exists(path)){if(std::chrono::steady_clock::now()>until)throw std::runtime_error("Lock holder not ready");std::this_thread::sleep_for(std::chrono::milliseconds(10));}}
#endif
class CorruptSink final:public asset_zip::RandomAccessSink {
 public:
  std::shared_ptr<asset_zip::RandomAccessSink> inner;bool changed=false;
  Result<void> write_at(std::uint64_t offset,std::span<const std::byte> data) override {if(!changed&&data.size()>=2048){changed=true;std::vector<std::byte> bad(data.begin(),data.end());bad[0]^=std::byte{1};return inner->write_at(offset,bad);}return inner->write_at(offset,data);}
  Result<void> flush() override{return inner->flush();}Result<void> close() override{return inner->close();}Result<void> discard() override{return inner->discard();}std::uint64_t size() const noexcept override{return inner->size();}
};
}
TEST(AssetRepository, CreateHistoryManifestFirstAndVerificationReport) {
  RecordProperty("req","ADR-0025|ADR-0035");
  const auto root = fresh(); auto o = options(); unsigned events = 0; o.on_indexed = [&](const IndexedEvent& e) { ++events; EXPECT_EQ(e.revision,1u); throw 42; };
  auto repo = value(AssetRepository::open(root,o)); auto created = create(repo,root,2); EXPECT_EQ(created.revision,1u); EXPECT_EQ(events,1u); EXPECT_TRUE(created.commit);
  auto snap = value(repo.snapshot(created.ref)); const auto& j = snap.manifest().known(); EXPECT_EQ(snap.archive().entries()[0].raw_name,"manifest.json"); EXPECT_EQ(snap.archive().entries()[0].local_offset,0u);
  EXPECT_TRUE(std::filesystem::exists(target(root,created.ref.asset_id))); EXPECT_EQ(j["asset"]["created_at"],j["asset"]["modified_at"]); EXPECT_EQ(j["history"]["entries"].size(),1u);
  EXPECT_EQ(j["history"]["entries"][0]["operation_id"],created.op_id); EXPECT_EQ(j["asset"]["modified_at"],j["history"]["entries"][0]["timestamp"]);
  const auto history=plain(snap)["history"]["entries"][0];EXPECT_EQ(history["details"]["creation_kind"],"user");EXPECT_EQ(history["tool"]["application_version"],"0.1.0-test");EXPECT_EQ(created.commit->op_id,created.op_id);
  EXPECT_EQ(created.verification.v1,CheckStatus::passed); EXPECT_EQ(created.verification.v2,CheckStatus::passed); EXPECT_EQ(created.verification.v3,CheckStatus::passed);
  ASSERT_EQ(created.verification.v4.size(),2u); for (const auto& s : created.verification.v4) EXPECT_EQ(s.status,StreamStatus::verified_sha256); EXPECT_EQ(created.verification.container_verification,ContainerVerification::full);
  std::cout << "CREATE_REPORT " << value(verification_json(created.verification)).dump() << '\n'; clean(root);
}
TEST(AssetRepository, MetadataChangeDropsForeignExtraAndCommentWithoutChangingPayload) {
  RecordProperty("req", "ADR-0039");
  const auto root = fresh();
  auto repo = value(AssetRepository::open(root, options()));
  const auto created = create(repo, root);
  std::string manifest_text, content_path, original_sha;
  {
    auto initial = value(repo.snapshot(created.ref));
    manifest_text = initial.manifest_text();
    content_path = initial.manifest().known()["files"][0]["storage_path"].get<std::string>();
    original_sha = initial.manifest().known()["files"][0]["sha256"].get<std::string>();
  }
  zip_test::Entry manifest_entry;
  manifest_entry.name = "manifest.json";
  manifest_entry.data = manifest_text;
  zip_test::Entry payload;
  payload.name = content_path;
  payload.data = "data-0";
  payload.method = 8;
  payload.local_extra = {std::byte{0x55}, std::byte{0x54}, std::byte{1}, std::byte{0}, std::byte{0}};
  payload.central_extra = payload.local_extra;
  payload.comment = std::string("c\0\xff", 3);
  const auto synthetic = zip_test::build({manifest_entry, payload});
  const auto asset_path = target(root, created.ref.asset_id);
  ASSERT_TRUE(std::filesystem::remove(asset_path));
  {
    std::ofstream out(asset_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(synthetic.bytes.data()), static_cast<std::streamsize>(synthetic.bytes.size()));
    ASSERT_TRUE(out);
  }
  auto before = value(repo.snapshot(created.ref));
  const auto& before_entry = entry(before, content_path);
  EXPECT_EQ(before_entry.local_extra, payload.local_extra);
  EXPECT_EQ(before_entry.central_extra, payload.central_extra);
  EXPECT_EQ(before_entry.comment.size(), payload.comment.size());
  const auto compressed = raw(before, before_entry.index);
  const auto original_verification = value(repo.verify(created.ref));
  EXPECT_EQ(original_verification.container_verification, ContainerVerification::full);
  EXPECT_EQ(original_verification.v3, CheckStatus::passed);
  auto changed = repo.change(metadata(created));
  ASSERT_TRUE(changed) << changed.error().message_id << " cause="
                       << (changed.error().cause ? changed.error().cause->message_id : "none")
                       << " " << to_log_string(changed.error());
  EXPECT_EQ(changed->revision, 2u);
  auto after = value(repo.snapshot(created.ref));
  const auto& after_entry = entry(after, content_path);
  EXPECT_TRUE(after_entry.local_extra.empty());
  EXPECT_TRUE(after_entry.central_extra.empty());
  EXPECT_TRUE(after_entry.comment.empty());
  EXPECT_EQ(raw(after, after_entry.index), compressed);
  EXPECT_EQ(after_entry.crc32, before_entry.crc32);
  EXPECT_EQ(after_entry.compressed_size, before_entry.compressed_size);
  EXPECT_EQ(after_entry.uncompressed_size, before_entry.uncompressed_size);
  EXPECT_EQ(after.manifest().known()["files"][0]["sha256"], original_sha);
  EXPECT_EQ(after.manifest().known()["asset"]["metadata"]["title"], "new");
  asset_zip::ReadBudget budget(payload.data.size());
  const auto verified = value(after.archive().verify_entry(after_entry.index, budget, true));
  EXPECT_TRUE(verified.sha256);
  EXPECT_EQ(verified.size, payload.data.size());
  const auto final_verification = value(repo.verify(created.ref));
  EXPECT_EQ(final_verification.container_verification, ContainerVerification::full);
  EXPECT_EQ(final_verification.v3, CheckStatus::passed);
  EXPECT_TRUE(final_verification.v3_findings.empty());
  clean(root);
}
TEST(AssetRepository, TypedOperationsHistoryRawCopyAndRemoval) {
  RecordProperty("req","ADR-0025|ADR-0035|ADR-0039");
  const auto root = fresh(); auto repo = value(AssetRepository::open(root,options())); auto created = create(repo,root,2); auto old = value(repo.snapshot(created.ref));
  const auto file_id = old.manifest().known()["files"][0]["id"].get<std::string>(), path = old.manifest().known()["files"][0]["storage_path"].get<std::string>(); auto original = raw(old,entry(old,path).index);
  const auto derived_hash=old.manifest().known()["files"][0]["sha256"];auto with_dependency=plain(old);with_dependency["files"][1]["derived_from"]=Json::array({Json{{"file_id",file_id},{"sha256",derived_hash}}});fixture(repo,root,created.ref,with_dependency.dump());
  auto current = value(repo.change(metadata(created))); EXPECT_EQ(current.revision,2u); auto now = value(repo.snapshot(created.ref)); EXPECT_EQ(raw(now,entry(now,path).index),original);
  ChangeRequest add; add.ref = created.ref; add.expected_rev = 2; add.ops.push_back(addition(root)); current = value(repo.change(add)); EXPECT_EQ(current.revision,3u);
  ChangeRequest replace; replace.ref = created.ref; replace.expected_rev = 3; replace.ops.push_back(ReplaceFileContent{file_id,source(root,"replacement")}); current = value(repo.change(replace)); EXPECT_EQ(current.revision,4u);
  now = value(repo.snapshot(created.ref)); const auto old_sha = now.manifest().known()["files"][0]["sha256"];EXPECT_EQ(now.manifest().known()["files"][1]["derived_from"][0]["sha256"],derived_hash);
  const auto replacement=plain(now)["history"]["entries"][3];EXPECT_EQ(replacement["operation"],"files_changed");EXPECT_EQ(replacement["changes"][0]["old_sha256"],derived_hash);EXPECT_EQ(replacement["changes"][0]["new_sha256"],old_sha);
  EXPECT_TRUE(now.manifest().known()["history"]["entries"][3]["changes"][0]["old_sha256"].is_string());
  EXPECT_TRUE(now.manifest().known()["history"]["entries"][3]["changes"][0]["new_sha256"].is_string());
  const auto semantic = value(manifest::validate_semantics(now.manifest()));
  EXPECT_TRUE(semantic.complete); EXPECT_TRUE(semantic.findings.empty());
  EXPECT_EQ(current.verification.v3,CheckStatus::passed);
  std::vector<std::string> replacement_owners;
  for (const auto& key : now.manifest().opaque_keys())
    if (key.value.starts_with("/history/"+current.op_id+"/")) replacement_owners.push_back(key.value);
  EXPECT_EQ(replacement_owners,(std::vector<std::string>{"/history/"+current.op_id+"/details"}));
  auto edited = value(repo.change(metadata(current,"title",Json("After replacement"))));
  EXPECT_EQ(edited.revision,5u);
  auto edited_snapshot = value(repo.snapshot(created.ref));
  EXPECT_EQ(edited.verification.v3,CheckStatus::passed);
  EXPECT_TRUE(value(manifest::validate_semantics(edited_snapshot.manifest())).findings.empty());
  manifest::IntendedChanges history_intent;
  const auto original_keys = now.manifest().opaque_keys();
  for (const auto& key : edited_snapshot.manifest().opaque_keys())
    if (std::find(original_keys.begin(),original_keys.end(),key)==original_keys.end()) history_intent.owners.insert(key);
  EXPECT_TRUE(manifest::verify_preserved(now.manifest_text(),edited_snapshot.manifest_text(),history_intent));
  ChangeRequest remove; remove.ref = created.ref; remove.expected_rev = 5; remove.ops.push_back(RemoveFile{file_id}); current = value(repo.change(remove)); EXPECT_EQ(current.revision,6u);
  now = value(repo.snapshot(created.ref)); const auto& j = now.manifest().known(); EXPECT_EQ(j["files"].size(),2u); EXPECT_EQ(j["historical_files"][0]["last_known_sha256"],old_sha); EXPECT_EQ(j["historical_files"][0]["removed_at_revision"],6);
  EXPECT_EQ(j["history"]["entries"].size(),6u); for (const auto& e : now.archive().entries()) EXPECT_NE(e.raw_name,path); clean(root);
}
TEST(AssetRepository, NoOpDoesNotChangeBytesMtimeOrRevision) {
  RecordProperty("req","ADR-0025");
  const auto root = fresh(); auto repo = value(AssetRepository::open(root,options())); auto current = create(repo,root); current = value(repo.change(metadata(current)));
  auto path = target(root,current.ref.asset_id); auto before = bytes(path); auto time = std::filesystem::last_write_time(path);
  auto noop = value(repo.change(metadata(current))); EXPECT_TRUE(noop.no_op); EXPECT_FALSE(noop.commit); EXPECT_EQ(noop.revision,current.revision);
  auto absent = metadata(current,"description"); std::get<SetMetadata>(absent.ops[0]).value.reset(); EXPECT_TRUE(value(repo.change(absent)).no_op);
  EXPECT_EQ(bytes(path),before); EXPECT_EQ(std::filesystem::last_write_time(path),time); clean(root);
}
TEST(AssetRepository, StaleMetadataRebasesAndConflictsAreFailClosed) {
  RecordProperty("req","ADR-0025");
  const auto root=fresh(); auto repo=value(AssetRepository::open(root,options())); auto first=create(repo,root); auto title=metadata(first); auto description=metadata(first,"description","description");
  auto second=value(repo.change(title)); auto third=value(repo.change(description)); EXPECT_EQ(third.revision,3u); EXPECT_EQ(third.rebased_from,1u);
  auto conflict=repo.change(title); ASSERT_FALSE(conflict); EXPECT_EQ(conflict.error().message_id,"am.repo.revision_conflict"); EXPECT_TRUE(conflict.error().allows(ErrorAction::decide));
  auto future=metadata(third); future.expected_rev=99; EXPECT_FALSE(repo.change(future));
  auto snap=value(repo.snapshot(first.ref)); auto file_id=snap.manifest().known()["files"][0]["id"].get<std::string>();
  for(bool remove:{false,true}) { ChangeRequest r; r.ref=first.ref; r.expected_rev=1; if(remove) r.ops.push_back(RemoveFile{file_id}); else r.ops.push_back(ReplaceFileContent{file_id,source(root)}); auto bad=repo.change(r); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().message_id,"am.repo.revision_conflict"); }
  ChangeRequest add; add.ref=first.ref; add.expected_rev=1; add.ops.push_back(addition(root)); auto rebased=value(repo.change(add)); EXPECT_EQ(rebased.rebased_from,1u); clean(root);
}
TEST(AssetRepository, UnknownHistoryCompactedRangesAndMissingChangesRejectRebase) {
  RecordProperty("req","ADR-0025");
  for(int mode=0;mode<3;++mode) {
    const auto root=fresh(); auto repo=value(AssetRepository::open(root,options())); auto created=create(repo,root); auto current=value(repo.change(metadata(created))); current=value(repo.change(metadata(current,"description","text")));
    auto snap=value(repo.snapshot(created.ref)); auto j=plain(snap);
    if(mode==0) j["history"]["entries"][1]["operation"]="future_operation";
    if(mode==1) { j["history"]["entries"].erase(j["history"]["entries"].begin()+1); j["history"]["compacted_ranges"]=Json::array({Json{{"from_revision",2},{"to_revision",2},{"summary",{{"revision_count",1}}}}}); }
    if(mode==2) j["history"]["entries"][1].erase("changes");
    fixture(repo,root,created.ref,j.dump()); auto bad=repo.change(metadata(created,"tags",Json::array({"tag"}))); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().message_id,"am.repo.revision_conflict");
  }
}
TEST(AssetRepository, PathAndFileIdCollisionsIncludeCaseAndNfc) {
  RecordProperty("req","ADR-0035");
  for(int mode=0;mode<3;++mode) {
    const auto root=fresh(); auto repo=value(AssetRepository::open(root,options())); auto created=create(repo,root); value(repo.change(metadata(created))); auto a=addition(root,2);
    if(mode==0) { a.files[0].path="Part.bin"; a.files[1].path="part.bin"; }
    if(mode==1) { a.files[0].path="\xc3\xa9.bin"; a.files[1].path="e\xcc\x81.bin"; }
    if(mode==2) a.files[1].file_id=a.files[0].file_id;
    ChangeRequest r; r.ref=created.ref; r.expected_rev=1; r.ops.push_back(a); auto bad=repo.change(r); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().message_id,"am.repo.path_collision"); clean(root);
  }
}
TEST(AssetRepository, O13PreservesIndependentNumericLexemesAndPaths) {
  RecordProperty("req","O-13|ADR-0038");
  const auto root=fresh(); auto repo=value(AssetRepository::open(root,options())); auto created=create(repo,root,2); auto initial=value(repo.snapshot(created.ref)); auto j=plain(initial);
  j["extensions"]["org.test.one"]={{"schema_version","1"},{"data","OPAQUE_ONE"},{"dependencies",{{"state","none"},{"refs",Json::array()}}}};
  j["extensions"]["org.test.two"]={{"schema_version","1"},{"data","OPAQUE_TWO"},{"dependencies",{{"state","none"},{"refs",Json::array()}}}};
  j["history"]["entries"][0]["details"]="OPAQUE_HISTORY"; j["files"][0]["origin_kind"]="generated"; j["files"][0]["generation"]={{"tool","core:test.generator"},{"params","OPAQUE_PARAMS"}};
  const std::string fragment=R"({"numbers":[12345678901234567890.000000000000000001,184467440737095516160,-92233720368547758090,1e400,-1E-400,-0,-0.0,1.500],"nested":[{"long":0.)"+std::string(2048,'1')+R"(,"exponent":1e+)"+std::string(1024,'9')+"}]}";
  auto text=j.dump(); for(const auto& name:{"OPAQUE_ONE","OPAQUE_TWO","OPAQUE_HISTORY","OPAQUE_PARAMS"}) text=replace_token(text,name,fragment);
  fixture(repo,root,created.ref,text); auto old=value(repo.snapshot(created.ref)); auto before=opaque_numbers(old.manifest_text()); EXPECT_GE(before.size(),40u);
  auto changed=value(repo.change(metadata(created))); auto now=value(repo.snapshot(created.ref)); EXPECT_EQ(opaque_numbers(now.manifest_text()),before); EXPECT_EQ(now.manifest().known()["asset"]["metadata"]["title"],"new");
  manifest::IntendedChanges intended; std::set<manifest::OpaqueKey> previous; for(const auto& k:old.manifest().opaque_keys()) previous.insert(k);
  for(const auto& k:now.manifest().opaque_keys()) if(!previous.contains(k)) { intended.owners.insert(k); std::cout<<"HISTORY_OWNER "<<k.value<<'\n'; }
  EXPECT_TRUE(manifest::verify_preserved(old.manifest_text(),now.manifest_text(),intended)); clean(root);
}
TEST(AssetRepository, OpaqueAttachmentsAndDependenciesHaveExplicitVerification) {
  RecordProperty("req","ADR-0035");
  const auto root=fresh(); auto repo=value(AssetRepository::open(root,options())); auto created=create(repo,root,2); auto old=value(repo.snapshot(created.ref)); auto j=plain(old); const auto attachment=id(); const std::string path="extensions/org.test.one/a.bin";
  j["extensions"]["org.test.one"]={{"schema_version","1"},{"data",{{"x",1}}},{"attachments",Json::array({Json{{"id",attachment},{"path",path},{"size_bytes","10"},{"sha256",sha("attachment")}}})}};
  fixture(repo,root,created.ref,j.dump(),{{path,"attachment"},{"legacy.bin","opaque"}}); auto legacy=value(repo.snapshot(created.ref)); auto original=raw(legacy,entry(legacy,"legacy.bin").index);
  auto changed=value(repo.change(metadata(created))); EXPECT_EQ(changed.verification.container_verification,ContainerVerification::partial);
  EXPECT_TRUE(std::any_of(changed.verification.v4.begin(),changed.verification.v4.end(),[](const auto& s){return s.path=="legacy.bin"&&s.status==StreamStatus::verified_crc_size_only;}));
  EXPECT_TRUE(std::any_of(changed.verification.v4.begin(),changed.verification.v4.end(),[&](const auto& s){return s.id==attachment&&s.status==StreamStatus::verified_sha256;}));
  std::cout<<"OPAQUE_REPORT "<<value(verification_json(changed.verification)).dump()<<'\n'; auto now=value(repo.snapshot(created.ref)); EXPECT_EQ(raw(now,entry(now,"legacy.bin").index),original);
  ChangeRequest remove; remove.ref=created.ref; remove.expected_rev=2; remove.ops.push_back(RemoveFile{now.manifest().known()["files"][0]["id"].get<std::string>()});
  auto blocked=repo.change(remove); ASSERT_FALSE(blocked); EXPECT_EQ(blocked.error().message_id,"am.repo.extension_dependency"); EXPECT_EQ(blocked.error().code,Errc::needs_decision);
  j=plain(now); j["extensions"]["org.test.one"]["dependencies"]={{"state","declared"},{"refs",Json::array({Json{{"kind","file"},{"file_id",j["files"][1]["id"]}}})}}; fixture(repo,root,created.ref,j.dump()); auto allowed=repo.change(remove); EXPECT_TRUE(allowed) << (allowed ? "" : to_log_string(allowed.error())); clean(root);
}
TEST(AssetRepository, RemovedOpaqueOwnersAreExactlyThoseOfExplicitlyRemovedFiles) {
  RecordProperty("req","ADR-0038|ADR-0025");
  const auto root=fresh(); auto repo=value(AssetRepository::open(root,options())); auto created=create(repo,root,2); auto old=value(repo.snapshot(created.ref)); auto j=plain(old);
  const auto generated=j["files"][0]["id"].get<std::string>(), plugin=j["files"][1]["id"].get<std::string>();
  j["files"][0]["origin_kind"]="generated"; j["files"][0]["generation"]={{"tool","core:test.generator"},{"params",{{"value",123}}}};
  j["field_definitions"]=Json::array({Json{{"id","plugin:org.test.one:opaque"},{"schema_revision",1},{"name","Opaque"},{"type","plugin:org.test.one:opaque"},{"scopes",Json::array({"file"})}}});
  j["files"][1]["metadata"]["custom_values"]=Json::array({Json{{"field_id","plugin:org.test.one:opaque"},{"value",{{"n",456}}}}});
  j["extensions"]["org.test.one"]={{"schema_version","1"},{"data",{{"keep",789}}},{"dependencies",{{"state","none"},{"refs",Json::array()}}}};
  fixture(repo,root,created.ref,j.dump()); old=value(repo.snapshot(created.ref)); std::set<manifest::OpaqueKey> removed;
  for(const auto& key:old.manifest().opaque_keys()) if(key.value.starts_with("/files/")) removed.insert(key);
  ChangeRequest replace; replace.ref=created.ref; replace.expected_rev=1; replace.ops.push_back(ReplaceFileContent{generated,source(root)}); auto bad=repo.change(replace); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().message_id,"am.repo.invalid_operation");
  ChangeRequest request; request.ref=created.ref; request.expected_rev=1; request.ops={RemoveFile{generated},RemoveFile{plugin}}; auto changed=value(repo.change(request));
  EXPECT_EQ(std::set<manifest::OpaqueKey>(changed.removed_opaque_owners.begin(),changed.removed_opaque_owners.end()),removed); EXPECT_EQ(removed.size(),2u);
  for(const auto& key:changed.removed_opaque_owners) std::cout<<"REMOVED_OWNER "<<key.value<<'\n';
  auto now=value(repo.snapshot(created.ref)); EXPECT_EQ(now.manifest().known()["files"].size(),0u); EXPECT_EQ(now.manifest().known()["historical_files"].size(),2u); clean(root);
}
TEST(AssetRepository, FutureMinorAndInvalidV2HaveDifferentReadReports) {
  RecordProperty("req","ADR-0035");
  for(int mode=0;mode<4;++mode) {
    const auto root=fresh(); auto repo=value(AssetRepository::open(root,options())); auto created=create(repo,root); auto old=value(repo.snapshot(created.ref)); auto j=plain(old);
    if(mode==0) { j["format"]["version"]["minor"]=1; j["future_key"]=true; }
    if(mode==1) j["future_key"]=true;
    if(mode==2) j["format"]["version"]["major"]=2;
    if(mode==3) j["format"]["id"]="urn:uuid:11111111-1111-4111-8111-111111111111";
    fixture(repo,root,created.ref,j.dump()); auto before=bytes(target(root,created.ref.asset_id)); auto snap=repo.snapshot(created.ref); auto verified=repo.verify(created.ref); auto changed=repo.change(metadata(created)); ASSERT_FALSE(changed);
    if(mode==0) { ASSERT_TRUE(snap); EXPECT_FALSE(snap->writable()); EXPECT_EQ(snap->format_support(),FormatSupport::newer_minor); ASSERT_TRUE(verified); EXPECT_EQ(verified->v2,CheckStatus::not_applicable_newer_minor); EXPECT_EQ(verified->v3,CheckStatus::not_performed); EXPECT_EQ(verified->container_verification,ContainerVerification::partial); EXPECT_EQ(verified->v4[0].status,StreamStatus::verified_sha256); EXPECT_EQ(changed.error().message_id,"am.repo.write_unsupported_version"); }
    if(mode==1) { ASSERT_FALSE(snap); EXPECT_EQ(snap.error().message_id,"am.manifest.schema_violation"); ASSERT_TRUE(verified); EXPECT_EQ(verified->v2,CheckStatus::failed); EXPECT_TRUE(verified->error); EXPECT_EQ(changed.error().message_id,"am.manifest.schema_violation"); }
    if(mode>=2) { EXPECT_FALSE(snap); ASSERT_FALSE(verified); EXPECT_EQ(verified.error().message_id,"am.repo.unsupported_version"); }
    EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before);
  }
}
TEST(AssetRepository, ReadOnlyCapabilitiesSizeAndSpaceAreCheckedBeforeCommit) {
  RecordProperty("req","ADR-0031|K-117");
  const auto root=fresh(); auto repo=value(AssetRepository::open(root,options())); auto created=create(repo,root); auto caps=value(detect_storage_capabilities(root));
#ifdef _WIN32
  EXPECT_EQ(caps.filesystem,"NTFS"); EXPECT_EQ(caps.mode,StorageMode::full); EXPECT_EQ(caps.durability,Durability::assumed);
#else
  EXPECT_EQ(caps.mode,StorageMode::read_only_default);
  // The size-limit scenario uses the same explicit test override as other writes.
  caps=*options().capabilities;
#endif
  auto readonly=options(); readonly.mode=LibraryMode::read_only; auto ro=value(AssetRepository::open(root,readonly)); EXPECT_FALSE(value(ro.snapshot(created.ref)).writable());
  auto bad=ro.change(metadata(created)); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().message_id,"am.repo.read_only"); EXPECT_FALSE(ro.create(CreateRequest{})); EXPECT_FALSE(ro.recover());
  auto unavailable=options(); unavailable.capabilities=StorageCapabilities{}; auto open=AssetRepository::open(root,unavailable); ASSERT_FALSE(open); EXPECT_EQ(open.error().message_id,"am.repo.storage_read_only"); EXPECT_TRUE(open.error().allows(ErrorAction::open_read_only));
  auto limited=options(); limited.capabilities=caps; limited.capabilities->max_file_size=1; auto small=value(AssetRepository::open(root,limited)); bad=small.change(metadata(created)); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().message_id,"am.repo.file_too_large"); clean(root);
  auto full=options(); full.test_hooks.commit.free_space=[](const Path&)->Result<std::uint64_t>{return std::uint64_t{0};}; auto no_space=value(AssetRepository::open(root,full)); auto before=bytes(target(root,created.ref.asset_id)); bad=no_space.change(metadata(created)); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().code,Errc::insufficient_space); EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before); clean(root);
}
TEST(AssetRepository, SourceChangeAndInvalidOperationsNeverReplaceTarget) {
  RecordProperty("req","ADR-0025");
  const auto root=fresh(); auto repo=value(AssetRepository::open(root,options())); auto created=create(repo,root); auto before=bytes(target(root,created.ref.asset_id)); auto a=addition(root); const auto path=a.files[0].source;
  unsigned pass=0;auto o=options(); o.test_hooks.after_preflight=[&]{std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<(pass++==0?"CHANGED":"REHASH!");}; auto changing=value(AssetRepository::open(root,o));
  ChangeRequest r; r.ref=created.ref; r.expected_rev=1; r.ops.push_back(a); auto bad=changing.change(r); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().message_id,"am.repo.source_changed");
  bad=changing.change(r);ASSERT_FALSE(bad);EXPECT_EQ(bad.error().message_id,"am.repo.source_changed");EXPECT_EQ(pass,2u);
  r=metadata(created); r.ops.push_back(RemoveFile{id()}); bad=repo.change(r); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().message_id,"am.repo.mixed_operations");
  r=metadata(created,"custom_values",Json::array()); bad=repo.change(r); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().message_id,"am.repo.invalid_operation");
  r=metadata(created); std::get<SetMetadata>(r.ops[0]).scope="file:"+id(); EXPECT_FALSE(repo.change(r)); EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before); clean(root);
}
TEST(AssetRepository, PrecommitV2AndEveryInvariantRejectInvalidManifest) {
  RecordProperty("req","ADR-0035");
  for(int fault=0;fault<11;++fault) {
    SCOPED_TRACE(fault); const auto root=fresh(); auto initial=value(AssetRepository::open(root,options())); auto created=create(initial,root,2); auto before=bytes(target(root,created.ref.asset_id));
    auto o=options(); o.test_hooks.mutate_manifest=[fault](manifest::ManifestDocument& doc){ auto& j=doc.known(); auto& history=j["history"]["entries"];
      if(fault==0) j["asset"]["metadata"]=123;
      if(fault==1) j["asset"]["id"]=id();
      if(fault==2) j["files"][1]["id"]=j["files"][0]["id"];
      if(fault==3) j["historical_files"]=Json::array({Json{{"id",j["files"][0]["id"]},{"removed_at_revision",2}}});
      if(fault==4) j["inputs"].push_back(j["inputs"][0]);
      if(fault==5) j["inputs"][0]["id"]=id();
      if(fault==6) history[0]["revision"]=2;
      if(fault==7) history[0]["operation"]="metadata_changed";
      if(fault==8) history.back()["timestamp"]="2026-11-11T00:00:00.000Z";
      if(fault==9) j["asset"]["created_at"]="2026-11-11T00:00:00.000Z";
      if(fault==10) history.back()["operation_id"]=id();
    };
    auto repo=value(AssetRepository::open(root,o)); VerificationReport report; auto bad=repo.change(metadata(created),&report); ASSERT_FALSE(bad); EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before); clean(root);
    EXPECT_TRUE(bad.error().message_id.starts_with("am.manifest.") || bad.error().message_id=="am.repo.unexpected_opaque_change");
    if(fault==0) { EXPECT_EQ(bad.error().message_id,"am.manifest.schema_violation"); EXPECT_EQ(report.v2,CheckStatus::failed); }
  }
}
TEST(AssetRepository, UnknownOpaqueMutationIsNeverAuthorized) {
  RecordProperty("req","ADR-0038");
  for(bool new_owner:{false,true}) {
    const auto root=fresh(); auto initial=value(AssetRepository::open(root,options())); auto created=create(initial,root); auto snap=value(initial.snapshot(created.ref)); auto j=plain(snap);
    j["extensions"]["org.test.one"]={{"schema_version","1"},{"data",{{"value",123}}},{"dependencies",{{"state","none"},{"refs",Json::array()}}}}; fixture(initial,root,created.ref,j.dump());
    auto before=bytes(target(root,created.ref.asset_id)); auto o=options(); o.test_hooks.mutate_manifest=[=](manifest::ManifestDocument& doc){
      if(new_owner) doc.known()["extensions"]["org.test.two"]={{"schema_version","1"},{"data",value(doc.make_opaque("{}"))}};
      else doc.known()["extensions"]["org.test.one"]["data"]=value(doc.make_opaque("{\"value\":456}"));
    };
    auto repo=value(AssetRepository::open(root,o)); auto bad=repo.change(metadata(created)); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().message_id,new_owner ? "am.repo.unexpected_opaque_change" : "am.manifest.preservation_failed"); EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before); clean(root);
  }
}
TEST(AssetRepository, QuickSelectiveFullRespectTheirPayloadReadScope) {
  RecordProperty("req","ADR-0035");
  const auto root=fresh(); auto repo=value(AssetRepository::open(root,options())); auto created=create(repo,root,2); auto snap=value(repo.snapshot(created.ref)); const auto file=snap.manifest().known()["files"][0]; auto info=entry(snap,file["storage_path"].get<std::string>());
  // Install a byte-corrupted synthetic fixture after closing its current writer.
  auto all=bytes(target(root,created.ref.asset_id)); all[static_cast<std::size_t>(info.data_offset)]^=std::byte{1}; std::filesystem::remove(target(root,created.ref.asset_id)); {std::ofstream out(target(root,created.ref.asset_id),std::ios::binary);out.write(reinterpret_cast<const char*>(all.data()),static_cast<std::streamsize>(all.size()));}
  auto quick=value(repo.verify(created.ref,VerifyLevel::quick)); EXPECT_FALSE(quick.error); EXPECT_EQ(quick.v4[0].status,StreamStatus::unverifiable);
  auto other=snap.manifest().known()["files"][1]["id"].get<std::string>(); auto selected=value(repo.verify(created.ref,VerifyLevel::selective,{other})); EXPECT_FALSE(selected.error); EXPECT_EQ(selected.v4[0].status,StreamStatus::unverifiable); EXPECT_EQ(selected.v4[1].status,StreamStatus::verified_sha256);
  auto bad=value(repo.verify(created.ref,VerifyLevel::selective,{file["id"].get<std::string>()})); EXPECT_TRUE(bad.error); EXPECT_EQ(bad.v4[0].status,StreamStatus::failed); EXPECT_TRUE(value(repo.verify(created.ref)).error);
}
TEST(AssetRepository, SameAssetThreadsSerializeAndDifferentAssetsOverlapInVerifier) {
  RecordProperty("req","ADR-0025");
  const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root);auto first=metadata(created),second=metadata(created,"description","description");
  std::optional<Result<ChangeResult>> a,b;std::jthread one([&]{a=repo.change(first);}),two([&]{b=repo.change(second);});one.join();two.join();ASSERT_TRUE(a&&a->ok());ASSERT_TRUE(b&&b->ok());EXPECT_EQ(value(repo.snapshot(created.ref)).revision(),3u);
  auto other=create(repo,root);auto current=value(repo.snapshot(created.ref));created.revision=current.revision();
  std::mutex mutex;std::condition_variable cv;int entered=0;auto o=options();o.test_hooks.verifier_barrier=[&]{std::unique_lock lock(mutex);++entered;cv.notify_all();if(!cv.wait_for(lock,std::chrono::seconds(10),[&]{return entered==2;}))throw std::runtime_error("Independent asset writes did not overlap");};
  auto parallel=value(AssetRepository::open(root,o));auto left=metadata(created,"tags",Json::array({"a"})),right=metadata(other);
  std::jthread x([&]{a=parallel.change(left);}),y([&]{b=parallel.change(right);});x.join();y.join();ASSERT_TRUE(a&&a->ok())<<(a?to_log_string(a->error()):"none");ASSERT_TRUE(b&&b->ok())<<(b?to_log_string(b->error()):"none");EXPECT_EQ(entered,2);clean(root);
}
TEST(AssetRepository, BaseChangeRetriesOnceAndPreservesOtherProcessHistory) {
  RecordProperty("req","ADR-0025");
  for(bool repeat:{false,true}) {
    const auto root=fresh();auto other=value(AssetRepository::open(root,options()));auto created=create(other,root);unsigned changes=0;auto o=options();
    o.test_hooks.after_preflight=[&]{if(!repeat&&changes>0)return;auto snap=value(other.snapshot(created.ref));ChangeRequest req;req.ref=created.ref;req.expected_rev=snap.revision();req.ops.push_back(SetMetadata{"asset","description",Json(std::to_string(++changes))});value(other.change(req));};
    auto repo=value(AssetRepository::open(root,o));auto result=repo.change(metadata(created));
    if(repeat){ASSERT_FALSE(result);EXPECT_EQ(result.error().message_id,"am.repo.revision_conflict");EXPECT_EQ(changes,2u);}else{ASSERT_TRUE(result)<<to_log_string(result.error());EXPECT_EQ(result->revision,3u);EXPECT_EQ(result->rebased_from,1u);EXPECT_EQ(changes,1u);}
    clean(root);
  }
}
TEST(AssetRepository, OldSnapshotRetainsEveryStreamAcrossCommit) {
  RecordProperty("req","K-122|ADR-0025");
  const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root,2);auto old=value(repo.snapshot(created.ref));const auto before=old.manifest_text();
  auto result=value(repo.change(metadata(created)));EXPECT_EQ(result.revision,2u);asset_zip::ReadBudget budget(10000000);for(const auto& e:old.archive().entries())EXPECT_TRUE(old.archive().verify_entry(e.index,budget,true));EXPECT_EQ(old.manifest_text(),before);
  auto now=value(repo.snapshot(created.ref));EXPECT_EQ(now.revision(),2u);EXPECT_EQ(old.revision(),1u);EXPECT_NE(now.manifest_text(),before);clean(root);
}
TEST(AssetRepository, AnotherProcessLockPreventsLostRevisionAndIsReleasedByKill) {
  RecordProperty("req","ADR-0025");
#ifdef _WIN32
  const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root);const auto marker=root/"holder.ready";
  Process holder("hold",root,created.ref.asset_id,marker.string());ready(marker);{Process writer("change",root,created.ref.asset_id,"none");EXPECT_EQ(writer.wait(),14u);}EXPECT_EQ(value(repo.snapshot(created.ref)).revision(),1u);
  holder.kill();{Process writer("change",root,created.ref.asset_id,"none");EXPECT_EQ(writer.wait(),0u);}EXPECT_EQ(value(repo.snapshot(created.ref)).revision(),2u);clean(root);
#else
  GTEST_SKIP()<<"Win32 process locks";
#endif
}
TEST(AssetRepository, KillEveryRepositoryCheckpointThreeTimesWithOpenSnapshot) {
  RecordProperty("req","K-122|ADR-0025");
#ifdef _WIN32
  const std::array points{commit::FaultPoint::after_prepared,commit::FaultPoint::after_verify,commit::FaultPoint::after_verified_journal,commit::FaultPoint::after_replace,commit::FaultPoint::after_committed_journal};
  for(std::size_t i=0;i<points.size();++i)for(int n=1;n<=3;++n){
    SCOPED_TRACE(std::string(commit::point_name(points[i]))+" "+std::to_string(n));const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root);auto old=value(repo.snapshot(created.ref));
    Process child("crash",root,created.ref.asset_id,std::string(commit::point_name(points[i])));ASSERT_EQ(child.wait(),91u);
    auto initial=value(repo.verify(created.ref));EXPECT_FALSE(initial.error);auto report=value(repo.recover());ASSERT_EQ(report.size(),1u);
    auto current=value(repo.snapshot(created.ref));EXPECT_EQ(current.revision(),i<2?1u:2u);auto verified=value(repo.verify(created.ref));EXPECT_FALSE(verified.error);EXPECT_EQ(verified.container_verification,ContainerVerification::full);EXPECT_EQ(verified.v3,CheckStatus::passed);
    asset_zip::ReadBudget budget(10000000);for(const auto& e:old.archive().entries())EXPECT_TRUE(old.archive().verify_entry(e.index,budget,true));EXPECT_EQ(old.revision(),1u);
    std::cout<<"REPO_KILL "<<commit::point_name(points[i])<<" outcome="<<commit::outcome_name(report[0].outcome)<<" revision="<<current.revision()<<" run="<<n<<'\n';clean(root);EXPECT_TRUE(commit::AssetLock::acquire(root,created.ref.asset_id));
  }
#else
  GTEST_SKIP()<<"Win32 repository kill tests";
#endif
}
TEST(AssetRepository, CorruptedTemporaryPayloadReturnsFailedV4AndRollsBack) {
  RecordProperty("req","ADR-0035");
  const auto root=fresh();auto initial=value(AssetRepository::open(root,options()));auto created=create(initial,root);auto before=bytes(target(root,created.ref.asset_id));auto o=options();
  o.test_hooks.commit.create_sink=[](const Path& path)->Result<std::shared_ptr<asset_zip::RandomAccessSink>>{auto sink=std::make_shared<CorruptSink>();sink->inner=value(asset_zip::file_sink(path));return std::shared_ptr<asset_zip::RandomAccessSink>(sink);};
  auto repo=value(AssetRepository::open(root,o));auto add=addition(root);add.files[0].path="stored.zip";add.files[0].source=source(root,std::string(4096,'x'));ChangeRequest r;r.ref=created.ref;r.expected_rev=1;r.ops.push_back(add);VerificationReport report;auto failed=repo.change(r,&report);ASSERT_FALSE(failed);EXPECT_EQ(failed.error().code,Errc::corrupt_data);ASSERT_FALSE(report.v4.empty());EXPECT_EQ(report.v4.back().status,StreamStatus::failed);EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before);clean(root);
}
TEST(AssetRepository, AttachmentIdentityBoundaryAndPathCollisionAreInvariants) {
  RecordProperty("req","ADR-0035");
  for(int fault=0;fault<3;++fault) {
    const auto root=fresh();auto initial=value(AssetRepository::open(root,options()));auto created=create(initial,root);auto old=value(initial.snapshot(created.ref));auto j=plain(old);const auto a=id(),b=id();
    j["extensions"]["org.test.one"]={{"schema_version","1"},{"attachments",Json::array({Json{{"id",a},{"path","extensions/org.test.one/a.bin"},{"size_bytes","1"},{"sha256",sha("a")}},Json{{"id",b},{"path","extensions/org.test.one/b.bin"},{"size_bytes","1"},{"sha256",sha("b")}}})}};
    fixture(initial,root,created.ref,j.dump(),{{"extensions/org.test.one/a.bin","a"},{"extensions/org.test.one/b.bin","b"}});auto before=bytes(target(root,created.ref.asset_id));
    auto o=options();o.test_hooks.mutate_manifest=[=](manifest::ManifestDocument& doc){auto& dom=doc.known();if(fault==0)dom["extensions"]["org.test.one"]["attachments"][1]["id"]=a;
      if(fault==1){auto ext=dom["extensions"]["org.test.one"];dom["extensions"].erase("org.test.one");dom["extensions"]["org.test.other"]=ext;}
      if(fault==2)dom["extensions"]["org.test.one"]["attachments"][1]["path"]="extensions/org.test.one/a.bin";};
    auto repo=value(AssetRepository::open(root,o));auto bad=repo.change(metadata(created));ASSERT_FALSE(bad);EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before);clean(root);
    EXPECT_EQ(bad.error().message_id,fault==0?"am.manifest.unique_ids":fault==1?"am.manifest.storage_paths":"am.repo.path_collision");
  }
}
TEST(AssetRepository, QuickVerificationDoesNotSpendThePayloadBudget) {
  RecordProperty("req","ADR-0035");
  const auto root=fresh();auto initial=value(AssetRepository::open(root,options()));auto add=addition(root);add.files[0].source=source(root,std::string(65536,'x'));add.files[0].path="payload.zip";CreateRequest r;r.files.push_back(add);auto created=value(initial.create(r));
  auto limited=options();limited.read_budget=8192;auto repo=value(AssetRepository::open(root,limited));auto quick=value(repo.verify(created.ref,VerifyLevel::quick));EXPECT_FALSE(quick.error);auto full=value(repo.verify(created.ref));ASSERT_TRUE(full.error);EXPECT_EQ(full.error->code,Errc::resource_exhausted);
}
TEST(AssetRepository, OneGiBIncompressibleRawCopyVerificationAndMemory) {
  RecordProperty("req","K-116|ADR-0035");
  const auto root=fresh();const auto path=root/"random.bin";{
    std::ofstream out(path,std::ios::binary);std::array<std::uint64_t,8192> block{};std::uint64_t state=0x123456789abcdefULL;
    for(std::uint64_t offset=0;offset<1024ULL*1024*1024;offset+=sizeof(block)){for(auto& v:block){state^=state<<13;state^=state>>7;state^=state<<17;v=state;}out.write(reinterpret_cast<const char*>(block.data()),sizeof(block));}ASSERT_TRUE(out);
  }
  auto repo=value(AssetRepository::open(root,options()));auto add=addition(root);add.files[0].source=path;CreateRequest r;r.files.push_back(add);auto started=std::chrono::steady_clock::now();auto created=value(repo.create(r));const auto creation=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
  started=std::chrono::steady_clock::now();auto changed=value(repo.change(metadata(created)));const auto update=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
  EXPECT_EQ(changed.verification.container_verification,ContainerVerification::full);ASSERT_TRUE(changed.commit);std::cout<<"REPO 1GiB: create_seconds="<<creation<<" update_seconds="<<update<<" file_hash_seconds="<<changed.commit->file_hash_seconds<<" zip_bytes="<<changed.commit->size;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);ASSERT_TRUE(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)));std::cout<<" peak_working_set_bytes="<<memory.PeakWorkingSetSize<<" peak_commit_bytes="<<memory.PeakPagefileUsage;
#endif
  std::cout<<'\n';clean(root);EXPECT_TRUE(std::filesystem::remove(path));EXPECT_TRUE(std::filesystem::remove(target(root,created.ref.asset_id)));
}

namespace {
class ReportValidator final : public SemanticValidator {
 public:
  bool incomplete=false,warning=false;
  Result<SemanticResult> validate(const manifest::ManifestDocument& doc,const asset_zip::ZipArchive&,const SemanticContext&) const override {
    auto r=manifest::validate_semantics(doc);if(!r)return r.error();SemanticResult out;static_cast<manifest::SemanticReport&>(out)=std::move(r).value();
    if(incomplete)out.complete=false;
    if(warning)out.findings.push_back({"test_warning",manifest::FindingSeverity::warning,"/asset/metadata","am.manifest.test_warning","Synthetic injected warning for report serialization"});
    return out;
  }
};
void physical_fixture(Json& j) {
  const auto field_id="user:"+id();
  j["field_definitions"]=Json::array({Json{{"id",field_id},{"schema_revision",1},{"name","Length"},{"type","decimal"},{"scopes",Json::array({"asset"})},{"dimension","length"},{"constraints",{{"maximum",{{"value","1"},{"unit","cm"}}}}}}});
  j["asset"]["metadata"]["custom_values"]=Json::array({Json{{"field_id",field_id},{"value","2"},{"unit","mm"}}});
}
}
TEST(AssetRepository, SemanticErrorsReturnFindingsAndProtectExistingBytes) {
  RecordProperty("req","ADR-0035|R-05");
  const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root);auto before=bytes(target(root,created.ref.asset_id));
  auto request=metadata(created,"license",Json{{"spdx","MIT OR OR BSD"}});VerificationReport report;auto bad=repo.change(request,&report);ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().message_id,"am.manifest.license_spdx");EXPECT_EQ(bad.error().code,Errc::corrupt_data);EXPECT_EQ(report.v3,CheckStatus::failed);EXPECT_TRUE(report.v3_complete);
  ASSERT_EQ(report.v3_findings.size(),1u);EXPECT_EQ(report.v3_findings[0].pointer,"/asset/metadata/license/spdx");EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before);
  auto json=value(verification_json(report));EXPECT_EQ(json["v3_findings"][0]["severity"],"error");std::cout<<"V3_ERROR_REPORT "<<json.dump()<<'\n';clean(root);
}
TEST(AssetRepository, UnverifiableSemanticsCanCommitAndRetainPartialReport) {
  RecordProperty("req","ADR-0035|X-02");const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root);auto j=plain(value(repo.snapshot(created.ref)));physical_fixture(j);fixture(repo,root,created.ref,j.dump());
  auto changed=value(repo.change(metadata(created)));EXPECT_TRUE(changed.commit);EXPECT_EQ(changed.verification.v3,CheckStatus::partial);EXPECT_TRUE(changed.verification.v3_complete);EXPECT_FALSE(changed.verification.error);
  ASSERT_EQ(changed.verification.v3_findings.size(),1u);EXPECT_EQ(changed.verification.v3_findings[0].severity,manifest::FindingSeverity::unverifiable);
  auto json=value(verification_json(changed.verification));EXPECT_EQ(json["v3_findings"][0]["severity"],"unverifiable");std::cout<<"V3_UNVERIFIABLE_REPORT "<<json.dump()<<'\n';clean(root);
}
TEST(AssetRepository, IncompleteValidatorCannotCommitAndWarningsDoNotBlock) {
  RecordProperty("req","ADR-0035");for(bool incomplete:{true,false}){
    const auto root=fresh();auto initial=value(AssetRepository::open(root,options()));auto created=create(initial,root);auto before=bytes(target(root,created.ref.asset_id));
    auto validator=std::make_shared<ReportValidator>();validator->incomplete=incomplete;validator->warning=!incomplete;auto o=options();o.semantic_validator=validator;auto repo=value(AssetRepository::open(root,o));VerificationReport report;auto result=repo.change(metadata(created),&report);
    if(incomplete){ASSERT_FALSE(result);EXPECT_EQ(result.error().message_id,"am.manifest.incomplete");EXPECT_EQ(report.v3,CheckStatus::partial);EXPECT_FALSE(report.v3_complete);EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before);}
    else{ASSERT_TRUE(result);EXPECT_EQ(result->verification.v3,CheckStatus::passed);auto json=value(verification_json(result->verification));EXPECT_EQ(json["v3_findings"][0]["severity"],"warning");std::cout<<"V3_SYNTHETIC_WARNING_REPORT "<<json.dump()<<'\n';}
    clean(root);
  }
}
TEST(AssetRepository, PartialRepositoryInvariantsRemainTestOnly) {
  RecordProperty("req","ADR-0035");const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root);auto o=options();o.semantic_validator=value(repository_invariants());auto partial=value(AssetRepository::open(root,o));
  EXPECT_EQ(value(partial.verify(created.ref)).v3,CheckStatus::partial);VerificationReport report;EXPECT_FALSE(partial.change(metadata(created),&report));EXPECT_FALSE(report.v3_complete);clean(root);
}
TEST(AssetRepository, OldInvalidV3RemainsReadableAndVerifyReturnsReport) {
  RecordProperty("req","ADR-0035|R-05");const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root);auto j=plain(value(repo.snapshot(created.ref)));j["asset"]["metadata"]["license"]={{"spdx","MIT OR OR BSD"}};fixture(repo,root,created.ref,j.dump());
  EXPECT_TRUE(repo.snapshot(created.ref));auto report=value(repo.verify(created.ref));EXPECT_EQ(report.v3,CheckStatus::failed);EXPECT_TRUE(report.v3_complete);ASSERT_EQ(report.v3_findings.size(),1u);EXPECT_EQ(report.v3_findings[0].rule,"license_spdx");EXPECT_FALSE(repo.change(metadata(created)));clean(root);
}
TEST(AssetRepository, DuplicateOperationIdFailsBeforePreflightJournalOrTemp) {
  RecordProperty("req","ADR-0035|O-18");const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));CreateRequest c;c.asset_id=id();c.op_id=id();auto created=value(repo.create(c));auto before=bytes(target(root,created.ref.asset_id));unsigned preflights=0;auto o=options();o.test_hooks.after_preflight=[&]{++preflights;};auto changing=value(AssetRepository::open(root,o));
  auto r=metadata(created);r.op_id=c.op_id;auto bad=changing.change(r);ASSERT_FALSE(bad);EXPECT_EQ(bad.error().message_id,"am.repo.duplicate_operation_id");EXPECT_EQ(bad.error().code,Errc::conflict);bad=changing.create(c);ASSERT_FALSE(bad);EXPECT_EQ(bad.error().message_id,"am.repo.duplicate_operation_id");EXPECT_EQ(preflights,0u);EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before);clean(root);
}
TEST(AssetRepository, DriveClassifierGrantsFullOnlyForFixedNtfs) {
  RecordProperty("req","ADR-0031");for(auto drive:{DriveKind::fixed,DriveKind::removable,DriveKind::ramdisk,DriveKind::remote,DriveKind::unknown,DriveKind::optical})for(auto fs:{"NTFS","FAT32","exFAT","ReFS","unknown"}){
    const auto caps=value(classify_storage_capabilities(drive,fs));const bool full=drive==DriveKind::fixed&&std::string_view(fs)=="NTFS";EXPECT_EQ(caps.mode,full?StorageMode::full:StorageMode::read_only_default);EXPECT_EQ(caps.durability,full?Durability::assumed:Durability::unknown);EXPECT_EQ(caps.remote,drive==DriveKind::remote);if(std::string_view(fs)=="FAT32")EXPECT_EQ(caps.max_file_size,0xffffffffULL);
  }
}
TEST(AssetRepository, AllAddPathsCheckedBeforeAnySourceOpen) {
  RecordProperty("req","ADR-0035");for(int mode=0;mode<5;++mode){
    const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto a=addition(root,2);for(auto& f:a.files)f.source=root/"does-not-exist";
    if(mode==0)a.files[1].path="../bad";if(mode==1)a.files[1].path=std::string(201,'a');if(mode==2)a.files[1].path="FILE-0.BIN";
    CreateRequest c;
    if(mode<3)c.files.push_back(a);else{auto b=a;b.files.resize(1);a.files.resize(1);if(mode==3)b.files[0].path="../bad";else b.files[0].path="FILE-0.BIN";c.files={a,b};}
    auto failed=repo.create(c);ASSERT_FALSE(failed);EXPECT_EQ(failed.error().message_id,(mode==2||mode==4)?"am.repo.path_collision":"am.repo.invalid_operation");clean(root);
  }
}
TEST(AssetRepository, LowerLayerErrorMappingsPreserveCodeAndCause) {
  RecordProperty("req","ADR-0032");const auto root=fresh();auto initial=value(AssetRepository::open(root,options()));auto created=create(initial,root);auto before=bytes(target(root,created.ref.asset_id));
  auto o=options();o.test_hooks.commit.free_space=[](const Path&)->Result<std::uint64_t>{return 0;};auto limited=value(AssetRepository::open(root,o));auto failed=limited.change(metadata(created));ASSERT_FALSE(failed);EXPECT_EQ(failed.error().message_id,"am.repo.insufficient_space");EXPECT_EQ(failed.error().code,Errc::insufficient_space);ASSERT_TRUE(failed.error().cause);EXPECT_EQ(failed.error().cause->message_id,"am.commit.insufficient_space");
#ifdef _WIN32
  const auto marker=root/"lock.ready";Process holder("hold",root,created.ref.asset_id,marker.string());ready(marker);failed=initial.change(metadata(created));ASSERT_FALSE(failed);EXPECT_EQ(failed.error().message_id,"am.repo.locked");EXPECT_EQ(failed.error().code,Errc::locked);ASSERT_TRUE(failed.error().cause);EXPECT_EQ(failed.error().cause->message_id,"am.commit.locked");holder.kill();
#endif
  EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before);clean(root);
  const auto path=target(root,created.ref.asset_id);std::filesystem::remove(path);{std::ofstream out(path,std::ios::binary);out<<"corrupt zip";}
  auto snap=initial.snapshot(created.ref);ASSERT_FALSE(snap);EXPECT_EQ(snap.error().message_id,"am.repo.corrupt_container");EXPECT_EQ(snap.error().code,Errc::corrupt_data);ASSERT_TRUE(snap.error().cause);EXPECT_TRUE(snap.error().cause->message_id.starts_with("am.zip."));
}
TEST(AssetRepository, UnsupportedPayloadMethodReturnsMappedReportError) {
  RecordProperty("req","ADR-0032");const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root);auto snap=value(repo.snapshot(created.ref));auto all=bytes(target(root,created.ref.asset_id));const auto path=snap.manifest().known()["files"][0]["storage_path"].get<std::string>();const auto info=entry(snap,path);
  all[static_cast<std::size_t>(info.local_offset)+8]=std::byte{99};all[static_cast<std::size_t>(info.local_offset)+9]=std::byte{0};
  for(std::size_t i=0;i+46+path.size()<=all.size();++i)if(all[i]==std::byte{0x50}&&all[i+1]==std::byte{0x4b}&&all[i+2]==std::byte{1}&&all[i+3]==std::byte{2}&&std::memcmp(all.data()+i+46,path.data(),path.size())==0){all[i+10]=std::byte{99};all[i+11]=std::byte{0};}
  std::filesystem::remove(target(root,created.ref.asset_id));{std::ofstream out(target(root,created.ref.asset_id),std::ios::binary);out.write(reinterpret_cast<const char*>(all.data()),static_cast<std::streamsize>(all.size()));}
  auto r=value(repo.verify(created.ref));ASSERT_TRUE(r.error);EXPECT_EQ(r.error->message_id,"am.repo.unsupported_container");EXPECT_EQ(r.error->code,Errc::unsupported);ASSERT_TRUE(r.error->cause);EXPECT_TRUE(r.error->cause->message_id.starts_with("am.zip."));
}
TEST(AssetRepository, RemainingLowerLayerMappingsAtCommitBoundary) {
  RecordProperty("req","ADR-0032");
  struct Case {const char* original;Errc code;const char* mapped;};
  const std::array cases{
    Case{"am.commit.replace_failed",Errc::io_error,"am.repo.target_in_use"},
    Case{"am.commit.journal_error",Errc::io_error,"am.repo.journal_error"},
    Case{"am.commit.journal_version",Errc::unsupported,"am.repo.journal_error"},
    Case{"am.commit.out_of_memory",Errc::resource_exhausted,"am.repo.limit_exceeded"},
    Case{"am.zip.read_budget",Errc::resource_exhausted,"am.repo.limit_exceeded"},
    Case{"am.zip.io_error",Errc::io_error,"am.repo.io_error"},
    Case{"am.commit.io_error",Errc::io_error,"am.repo.io_error"}};
  for(const auto& c:cases){
    SCOPED_TRACE(c.original);const auto root=fresh();auto initial=value(AssetRepository::open(root,options()));auto created=create(initial,root);auto before=bytes(target(root,created.ref.asset_id));
    auto o=options();o.test_hooks.commit.point=[&](commit::FaultPoint p)->Result<void>{if(p==commit::FaultPoint::after_lock)return Error::make(c.code,"Injected boundary failure").id(c.original);return {};};
    auto repo=value(AssetRepository::open(root,o));auto failed=repo.change(metadata(created));ASSERT_FALSE(failed);EXPECT_EQ(failed.error().message_id,c.mapped);EXPECT_EQ(failed.error().code,c.code);ASSERT_TRUE(failed.error().cause);EXPECT_EQ(failed.error().cause->message_id,c.original);EXPECT_EQ(bytes(target(root,created.ref.asset_id)),before);clean(root);
  }
}

TEST(AssetRepository,VerifyContainerSeparatesInvalidZipFromOperationalErrors) {
  RecordProperty("req","K-3|K-6|ADR-0035");
  const auto root=fresh();
  ContainerContents contents;
  auto invalid=verify_container(source(root,"not a ZIP"),{},&contents);
  ASSERT_TRUE(invalid);
  EXPECT_EQ(invalid->v1,CheckStatus::failed);
  EXPECT_EQ(invalid->v2,CheckStatus::not_performed);
  ASSERT_TRUE(invalid->error);
  EXPECT_EQ(invalid->error->code,Errc::corrupt_data);
  EXPECT_FALSE(contents.archive);
  EXPECT_FALSE(contents.document);
  auto missing=verify_container(root/"missing.zip",{},&contents);
  ASSERT_FALSE(missing);
  auto missing_source=asset_zip::file_source(root/"missing.zip",true);
  ASSERT_FALSE(missing_source);
  EXPECT_EQ(missing.error().code,missing_source.error().code);
  EXPECT_EQ(missing.error().root_cause().message_id,missing_source.error().root_cause().message_id);
  EXPECT_FALSE(contents.archive);
  EXPECT_FALSE(contents.document);
}
TEST(AssetRepository,VerifyContainerRejectsRegisteredAndOpaqueSymlinks) {
  RecordProperty("req","K-6|ADR-0037");
  const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root);
  const auto snapshot=value(repo.snapshot(created.ref));
  const auto path=snapshot.manifest().known()["files"][0]["storage_path"].get<std::string>();
  for (bool registered : {true,false}) {
    zip_test::Entry manifest;manifest.name="manifest.json";manifest.data=snapshot.manifest_text();
    zip_test::Entry payload;payload.name=path;payload.data="data-0";
    zip_test::Entry link;link.name=registered ? path : "generated/opaque-link";link.data="data-0";
    link.made_by=0x314;link.attributes=0xa1ff0000;
    const auto built=zip_test::build(registered ? std::vector{manifest,link} : std::vector{manifest,payload,link});
    const auto target=root/(registered ? "registered-symlink.zip" : "opaque-symlink.zip");
    {std::ofstream out(target,std::ios::binary);out.write(reinterpret_cast<const char*>(built.bytes.data()),static_cast<std::streamsize>(built.bytes.size()));}
    auto report=value(verify_container(target));
    EXPECT_EQ(report.v3,CheckStatus::failed);
    EXPECT_NE(std::find(report.v3_rules.begin(),report.v3_rules.end(),"zip_entry_kind"),report.v3_rules.end());
    const auto finding=std::find_if(report.v3_findings.begin(),report.v3_findings.end(),[](const auto& f){return f.rule=="zip_entry_kind";});
    ASSERT_NE(finding,report.v3_findings.end());
    EXPECT_EQ(finding->severity,manifest::FindingSeverity::error);
    EXPECT_EQ(finding->message_id,"am.manifest.zip_entry_kind");
    EXPECT_TRUE(finding->pointer.empty());
    EXPECT_NE(finding->detail.find(link.name),std::string::npos);
    EXPECT_NE(finding->detail.find("symlink"),std::string::npos);
  }
}
TEST(AssetRepository,VerifyContainerSharesReportsAndAcceptsArbitraryFilename) {
  RecordProperty("req","K-15|K-85|K-3|K-6");
  const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root,2);auto standalone=root/"arbitrary-name.zip";std::filesystem::copy_file(target(root,created.ref.asset_id),standalone);
  auto report=value(verify_container(standalone));EXPECT_EQ(report.v3,CheckStatus::passed);EXPECT_TRUE(report.v3_complete);EXPECT_EQ(report.container_verification,ContainerVerification::full);
  EXPECT_EQ(std::count(report.v3_rules.begin(),report.v3_rules.end(),"format_asset_identity"),0);EXPECT_EQ(std::count(report.v3_rules.begin(),report.v3_rules.end(),"history_operation_id"),0);
  auto library=value(verification_json(value(repo.verify(created.ref))));auto external=value(verification_json(report));library.erase("v3_rules");external.erase("v3_rules");EXPECT_EQ(library,external);
  ContainerVerifyOptions settings;settings.streams=false;ContainerContents view;auto validation=value(verify_container(standalone,settings,&view));EXPECT_TRUE(validation.v4.empty());EXPECT_EQ(validation.v3,CheckStatus::passed);ASSERT_TRUE(view.document);ASSERT_TRUE(view.archive);ASSERT_TRUE(view.budget);EXPECT_EQ(view.document->known()["asset"]["id"],created.ref.asset_id);
}
TEST(AssetRepository,VerifyContainerReturnsV1FailureAndZipPathDetail) {
  RecordProperty("req","K-3|K-6|ADR-0035");
  const auto root=fresh();auto repo=value(AssetRepository::open(root,options()));auto created=create(repo,root);auto old=value(repo.snapshot(created.ref));const auto text=old.manifest_text();
  fixture(repo,root,created.ref,text,{{"opaque.bin","data"}});auto all=bytes(target(root,created.ref.asset_id));std::string unsafe="../bad.bin",safe="opaque.bin";ASSERT_EQ(unsafe.size(),safe.size());unsigned changed=0;
  for(std::size_t i=0;i+safe.size()<=all.size();++i)if(std::memcmp(all.data()+i,safe.data(),safe.size())==0){std::memcpy(all.data()+i,unsafe.data(),unsafe.size());++changed;}
  EXPECT_EQ(changed,2u);auto path=root/"unsafe.zip";{std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(all.data()),static_cast<std::streamsize>(all.size()));}
  ContainerVerifyOptions settings;settings.streams=false;auto report=value(verify_container(path,settings));EXPECT_EQ(report.v3,CheckStatus::failed);auto found=std::find_if(report.v3_findings.begin(),report.v3_findings.end(),[](const auto& f){return f.rule=="zip_paths";});ASSERT_NE(found,report.v3_findings.end());EXPECT_TRUE(found->pointer.empty());EXPECT_EQ(found->detail,unsafe);
  fixture(repo,root,created.ref,"{");report=value(verify_container(target(root,created.ref.asset_id),settings));EXPECT_EQ(report.v1,CheckStatus::failed);ASSERT_TRUE(report.error);EXPECT_EQ(report.v2,CheckStatus::not_performed);
}
