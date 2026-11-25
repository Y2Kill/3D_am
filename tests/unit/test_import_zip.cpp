#include <gtest/gtest.h>
#include <fstream>
#include <array>
#include <zlib.h>
#include "am/foundation/uuid.hpp"
#include "am/storage/archive/zip_input.hpp"
#include "am/storage/repository/repository.hpp"
#include "zip_builder.hpp"

namespace {
namespace fs=std::filesystem;
namespace zip=am::asset_zip;
namespace input=am::archive;
template<class T> T take(am::Result<T> r) { if(!r) throw std::runtime_error(am::to_log_string(r.error())); return std::move(*r); }
fs::path install(const zip_test::Built& built) {
  const auto path=fs::path(AM_IMPORT_SCAN_ROOT)/"zip-input"/(am::Uuid::v4().to_string()+".zip");
  fs::create_directories(path.parent_path());
  std::ofstream out(path,std::ios::binary);
  out.write(reinterpret_cast<const char*>(built.bytes.data()),static_cast<std::streamsize>(built.bytes.size()));
  if(!out) throw std::runtime_error("ZIP fixture write failed");
  return path;
}
std::string read(input::ZipInput& archive,std::size_t index) {
  auto stream=take(archive.open_entry(index,true));
  std::string result;
  std::array<std::byte,65536> bytes{};
  for(;;) { const auto n=take(stream.read(bytes)); if(!n) break; result.append(reinterpret_cast<const char*>(bytes.data()),n); }
  EXPECT_TRUE(stream.verification());
  return result;
}
std::shared_ptr<input::ZipInputJob> job(input::ZipInputLimits limits={}) { return std::make_shared<input::ZipInputJob>(limits); }
zip_test::Bytes unicode_extra(const std::string& raw,const std::string& decoded,bool correct) {
  zip_test::Bytes value;
  zip_test::append(value,0x7075,2); zip_test::append(value,decoded.size()+5,2); zip_test::append(value,1,1);
  auto crc=static_cast<std::uint32_t>(::crc32(0,reinterpret_cast<const Bytef*>(raw.data()),static_cast<uInt>(raw.size())));
  zip_test::append(value,correct?crc:crc^1,4);
  for(unsigned char c:decoded) value.push_back(static_cast<std::byte>(c));
  return value;
}
} // namespace
TEST(ZipInput, DuplicateNamesRemainDistinctAndAssetModeRemainsStrict) {
  RecordProperty("req","K-2|K-6|ADR-0011|ADR-0025");
  zip_test::Entry a,b; a.name=b.name="same.bin"; a.data="first"; b.data="second";
  const auto path=install(zip_test::build({a,b}));
  auto strict=zip::ZipArchive::open(take(zip::file_source(path)));
  ASSERT_FALSE(strict); EXPECT_EQ(strict.error().message_id,"am.zip.duplicate_name");
  auto archive=take(input::ZipInput::open(path,job()));
  ASSERT_EQ(archive.entries().size(),2u);
  for(const auto& e:archive.entries()) EXPECT_EQ(e.path_problem,"duplicate_entry_name");
  EXPECT_EQ(read(archive,0),"first"); EXPECT_EQ(read(archive,1),"second");
}
TEST(ZipInput, DuplicateLocalHeadersRemainRejectedInBothProfiles) {
  RecordProperty("req","ADR-0011|K-2");
  zip_test::Entry a,b; a.name=b.name="same";
  auto bytes=zip_test::build({a,b}); zip_test::put(bytes.bytes,bytes.central[1]+42,bytes.local[0],4);
  const auto path=install(bytes);
  const auto strict=zip::ZipArchive::open(take(zip::file_source(path)));
  const auto archive=input::ZipInput::open(path,job());
  ASSERT_FALSE(strict); ASSERT_FALSE(archive);
  EXPECT_EQ(strict.error().message_id,"am.zip.overlapping_entries");
  EXPECT_EQ(archive.error().message_id,strict.error().message_id);
}
TEST(ZipInput, NameDecodingRetainsRawBytesAndExplainsHeuristics) {
  RecordProperty("req","K-6|ADR-0012");
  std::vector<zip_test::Entry> entries(6);
  entries[0].name="utf8-\xc3\xa9";
  entries[1].name="valid-\xc3\xa9"; entries[1].flags=0;
  entries[2].name="raw"; entries[2].flags=0; entries[2].central_extra=unicode_extra("raw","unicode-\xc3\xa9",true);
  entries[3].name="fallback"; entries[3].flags=0; entries[3].central_extra=unicode_extra("fallback","ignored",false);
  entries[4].name=std::string("\x8c\xae\xa4\xa5\xab\xec.bin",10); entries[4].flags=0;
  entries[5].name="bad-\xff";
  const auto path=install(zip_test::build(entries));
  auto archive=take(input::ZipInput::open(path,job()));
  auto e=archive.entries();
  EXPECT_EQ(e[0].name_encoding,"utf-8(bit11)"); EXPECT_TRUE(e[0].encoding_exact);
  EXPECT_EQ(e[1].name_encoding,"utf-8(valid)");
  EXPECT_EQ(e[2].name_encoding,"utf-8(0x7075)"); EXPECT_EQ(e[2].path,"unicode-\xc3\xa9");
  EXPECT_EQ(e[3].path,"fallback"); EXPECT_EQ(e[3].problem_code,"unicode_path_crc_mismatch");
  EXPECT_FALSE(e[4].encoding_exact); EXPECT_TRUE(e[4].encoding_confidence); EXPECT_TRUE(e[4].name_encoding.ends_with("(heuristic)"));
  EXPECT_EQ(e[5].problem_code,"name_invalid_utf8"); EXPECT_EQ(e[5].path,"bad-\xef\xbf\xbd");
  for(std::size_t i=0;i<entries.size();++i) EXPECT_EQ(e[i].info.raw_name,entries[i].name);
}
TEST(ZipInput, UnsafePathsAndCaseOrNormalizationCollisionsAreListed) {
  RecordProperty("req","K-6|ADR-0012|ADR-0035");
  const std::vector<std::string> names{"../x","/abs","C:\\x","\\\\srv\\x","a\\b","a//b",std::string("x\0y",3),"A.txt","a.txt","\xc3\xa9","e\xcc\x81"};
  std::vector<zip_test::Entry> entries;
  for(const auto& n:names) { zip_test::Entry e; e.name=n; entries.push_back(e); }
  auto archive=take(input::ZipInput::open(install(zip_test::build(entries)),job()));
  ASSERT_EQ(archive.entries().size(),names.size());
  for(std::size_t i=0;i<7;++i) EXPECT_EQ(archive.entries()[i].path_problem,"unsafe_entry_path");
  for(std::size_t i=7;i<names.size();++i) EXPECT_EQ(archive.entries()[i].path_problem,"case_collision");
}
TEST(ZipInput, StoredAndDeflateStreamsShareActualByteBudgetAcrossPasses) {
  RecordProperty("req","K-2|K-6|ADR-0011");
  zip_test::Entry a,b; a.name="a"; a.data="12345"; b.name="b"; b.data="abcde"; b.method=8;
  input::ZipInputLimits limits; limits.max_unpacked_bytes=12;
  auto context=job(limits);
  const auto path=install(zip_test::build({a,b},true));
  auto archive=take(input::ZipInput::open(path,context));
  EXPECT_TRUE(archive.summary().zip64);
  EXPECT_EQ(read(archive,0),a.data); EXPECT_EQ(read(archive,1),b.data); EXPECT_EQ(context->remaining(),2u);
  auto next=take(input::ZipInput::open(path,context));
  auto stream=take(next.open_entry(0)); std::array<std::byte,64> buffer{};
  EXPECT_EQ(take(stream.read(buffer)),2u);
  const auto limit=stream.read(buffer); ASSERT_FALSE(limit);
  EXPECT_EQ(limit.error().message_id,"am.import.archive.limit_exceeded"); EXPECT_EQ(context->exceeded_limit(),"max_unpacked_bytes");
}
TEST(ZipInput, FalseSizesCannotAvoidActualByteBudgetAndCrcIsVerified) {
  RecordProperty("req","K-2|K-6|ADR-0011");
  zip_test::Entry e; e.method=8; e.data=std::string(200,'x'); e.declared_size=100;
  input::ZipInputLimits limits; limits.max_unpacked_bytes=50;
  auto context=job(limits); auto archive=take(input::ZipInput::open(install(zip_test::build({e})),context));
  auto stream=take(archive.open_entry(0)); std::array<std::byte,128> out{};
  EXPECT_EQ(take(stream.read(out)),50u);
  EXPECT_FALSE(stream.read(out)); EXPECT_EQ(context->exceeded_limit(),"max_unpacked_bytes");
  e.declared_size.reset(); e.declared_crc=0;
  archive=take(input::ZipInput::open(install(zip_test::build({e})),job()));
  stream=take(archive.open_entry(0)); const auto bad=stream.read(out);
  if(bad) EXPECT_FALSE(stream.read(out));
  else EXPECT_EQ(bad.error().code,am::Errc::corrupt_data);
}
TEST(ZipInput, TimeAndRatioLimitsAreStickyAndNameTheLimit) {
  RecordProperty("req","K-2|K-6|ADR-0011");
  zip_test::Entry e; e.data=std::string(2*1024*1024,'x'); e.method=8;
  const auto path=install(zip_test::build({e}));
  input::ZipInputLimits limits; limits.max_ratio=2;
  auto context=job(limits); auto archive=take(input::ZipInput::open(path,context));
  auto stream=take(archive.open_entry(0)); std::array<std::byte,65536> out{};
  bool exceeded=false;
  for(;;) { const auto n=stream.read(out); if(!n) { exceeded=true; break; } if(!*n) break; }
  EXPECT_TRUE(exceeded); EXPECT_EQ(context->exceeded_limit(),"max_ratio"); EXPECT_FALSE(archive.open_entry(0));
  limits.max_seconds_per_container=std::chrono::seconds(1);
  input::ZipInputHooks hooks; std::chrono::steady_clock::time_point now{};
  hooks.clock=[&]{return now;}; context=std::make_shared<input::ZipInputJob>(limits,hooks);
  archive=take(input::ZipInput::open(path,context)); now+=std::chrono::seconds(2);
  EXPECT_FALSE(archive.open_entry(0)); EXPECT_EQ(context->exceeded_limit(),"max_seconds_per_container");
}
TEST(ZipInput, EntryCountLimitAndUnsupportedEntriesDoNotReadPayload) {
  RecordProperty("req","K-2|K-6|ADR-0011");
  zip_test::Entry a,b; a.name="encrypted"; a.flags|=1; b.name="unsupported"; b.method=93; b.compressed=zip_test::Bytes{}; b.declared_size=0;
  const auto path=install(zip_test::build({a,b}));
  input::ZipInputLimits limits; limits.zip.max_entries=1; auto context=job(limits);
  EXPECT_FALSE(input::ZipInput::open(path,context)); EXPECT_EQ(context->exceeded_limit(),"max_entries");
  input::ZipInputHooks hooks; unsigned reads=0; hooks.before_read=[&](auto,auto)->am::Result<void>{++reads;return {};};
  context=std::make_shared<input::ZipInputJob>(input::ZipInputLimits{},hooks);
  auto archive=take(input::ZipInput::open(path,context));
  ASSERT_EQ(archive.entries().size(),2u); EXPECT_FALSE(archive.open_entry(0)); EXPECT_FALSE(archive.open_entry(1)); EXPECT_EQ(reads,0u);
}
TEST(ImportZipManifest, FilesystemArchiveIsAnIndependentTopLevelObject) {
  RecordProperty("req","SC-1|K-5|K-6|ADR-0037");
  namespace repo=am::repository;
  const auto id=[] { return am::Uuid::v4().to_string(); };
  const auto root=fs::path(AM_IMPORT_SCAN_ROOT)/"zip-input"/id();
  fs::create_directories(root);
  const auto payload=root/"payload";
  { std::ofstream out(payload,std::ios::binary); out<<"bytes"; }
  repo::RepositoryOptions options; options.library_id=id(); options.application_version="test";
  options.capabilities.emplace(); options.capabilities->mode=repo::StorageMode::full;
  options.capabilities->filesystem="synthetic unit-test capabilities";
  auto repository=take(repo::AssetRepository::open(root,options));
  const auto directory=id(), archive=id(), file=id();
  repo::AddFiles add;
  add.input={{"id",id()},{"imported_at","2026-11-25T00:00:00.000Z"},{"mode","strict"},{"completeness","complete"},
    {"objects",repo::Json::array({repo::Json{{"id",directory},{"kind","directory"},{"name","source"}},
      repo::Json{{"id",archive},{"kind","archive"},{"container","zip"},{"name","model.zip"},{"relative_path","sub/model.zip"},{"size_bytes","123"},{"sha256",std::string(64,'0')}}})},
    {"entries",repo::Json::array({repo::Json{{"id",id()},{"object_id",directory},{"entry_kind","directory"},{"path",""},{"result","included"}},
      repo::Json{{"id",id()},{"object_id",directory},{"entry_kind","directory"},{"path","sub"},{"result","included"}},
      repo::Json{{"id",id()},{"object_id",archive},{"entry_kind","file"},{"path","part.bin"},{"result","included"},{"file_id",file}}})}};
  repo::FileInput source; source.path="sub/model/part.bin"; source.source=payload; source.file_id=file;
  add.files.push_back(source);
  repo::CreateRequest request; request.files.push_back(add);
  const auto created=repository.create(request);
  ASSERT_TRUE(created)<<am::to_log_string(created.error());
  EXPECT_EQ(created->verification.v1,repo::CheckStatus::passed);
  EXPECT_EQ(created->verification.v2,repo::CheckStatus::passed);
  EXPECT_EQ(created->verification.v3,repo::CheckStatus::passed);
  const auto snapshot=take(repository.snapshot(created->ref));
  const auto& input=snapshot.manifest().known()["inputs"][0];
  ASSERT_EQ(input["objects"].size(),2u);
  EXPECT_FALSE(input["objects"][1].contains("parent_entry_id"));
  EXPECT_FALSE(input["objects"][1].contains("nested_action"));
  for(const auto& entry:input["entries"]) EXPECT_NE(entry["path"],"sub/model.zip");
}
