#include "am/storage/archive/zip_input.hpp"
#include "am/foundation/text_encoding.hpp"
#include "am/foundation/unicode.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <zlib.h>

namespace am::archive {
namespace {
Error failure(Errc code, std::string id, std::string detail = {}) {
  return Error::make(code,"ZIP input operation failed",std::move(detail)).in("storage.archive").id(std::move(id));
}
template<class T> T take(Result<T> r) { if(!r) throw r.error(); return std::move(*r); }
template<class T,class F> Result<T> checked(F&& f) {
  try { return f(); }
  catch(const Error& e) { return e; }
  catch(const std::bad_alloc&) { return failure(Errc::resource_exhausted,"am.archive.out_of_memory"); }
  catch(const std::exception& e) { return failure(Errc::io_error,"am.archive.io_error",e.what()); }
  catch(...) { return failure(Errc::internal,"am.archive.internal_error"); }
}
std::uint32_t little(std::span<const std::byte> b,std::size_t p,unsigned width) {
  std::uint32_t n=0;
  for(unsigned i=0;i<width;++i) n|=std::to_integer<std::uint32_t>(b[p+i])<<(8*i);
  return n;
}
std::string repaired(std::string_view raw) {
  std::string out;
  for(std::size_t i=0;i<raw.size();) {
    const auto c=static_cast<unsigned char>(raw[i]);
    const std::size_t n=c<128?1:c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
    if(n && i+n<=raw.size() && text::is_valid_utf8(raw.substr(i,n))) { out.append(raw.substr(i,n)); i+=n; }
    else { out+="\xef\xbf\xbd"; ++i; }
  }
  return out;
}
ZipInputEntry decoded(const asset_zip::EntryInfo& info) {
  ZipInputEntry e; e.info=info;
  const auto& raw=info.raw_name;
  if(info.utf8_flag) {
    e.path=repaired(raw); e.name_encoding="utf-8(bit11)";
    if(!text::is_valid_utf8(raw)) { e.encoding_exact=false; e.problem_code="name_invalid_utf8"; }
    return e;
  }
  const auto extra=std::span(info.central_extra);
  for(std::size_t p=0;p+4<=extra.size();) {
    const auto id=little(extra,p,2), size=little(extra,p+2,2); p+=4;
    if(size>extra.size()-p) break; // The structural reader has already rejected malformed extras.
    if(id==0x7075) {
      const auto crc=static_cast<std::uint32_t>(::crc32(0,reinterpret_cast<const Bytef*>(raw.data()),static_cast<uInt>(raw.size())));
      if(size>=5 && extra[p]==std::byte{1} && little(extra,p+1,4)==crc) {
        const std::string value(reinterpret_cast<const char*>(extra.data()+p+5),size-5);
        if(text::is_valid_utf8(value)) { e.path=value; e.name_encoding="utf-8(0x7075)"; return e; }
        e.problem_code="name_invalid_utf8";
      } else e.problem_code="unicode_path_crc_mismatch";
    }
    p+=size;
  }
  const auto bytes=std::span(reinterpret_cast<const std::uint8_t*>(raw.data()),raw.size());
  const auto guess=text::guess_name_encoding(bytes);
  e.path=guess.utf8; e.encoding_exact=guess.exact;
  if(guess.exact) e.name_encoding="utf-8(valid)";
  else { e.name_encoding=std::string(text::codepage_name(guess.codepage))+"(heuristic)"; e.encoding_confidence=guess.confidence; }
  return e;
}
std::string path_key(std::string_view p,asset_zip::EntryKind kind) {
  if(kind==asset_zip::EntryKind::directory && p.ends_with('/')) p.remove_suffix(1);
  return unicode::path_collision_key(p);
}
bool unsafe(std::string_view p,asset_zip::EntryKind kind) {
  if(kind==asset_zip::EntryKind::directory && p.ends_with('/')) p.remove_suffix(1);
  if(p.empty() || p.front()=='/' || p.find('\\')!=p.npos || p.find(':')!=p.npos) return true;
  for(std::size_t i=0;i<p.size();++i) {
    const auto c=static_cast<unsigned char>(p[i]);
    if(c<32 || c==127 || (c==0xc2 && i+1<p.size() && static_cast<unsigned char>(p[i+1])>=0x80 && static_cast<unsigned char>(p[i+1])<=0x9f)) return true;
  }
  for(std::size_t pos=0;pos<=p.size();) {
    auto end=p.find('/',pos); if(end==p.npos) end=p.size();
    const auto part=p.substr(pos,end-pos);
    if(part.empty() || part=="." || part=="..") return true;
    if(end==p.size()) break;
    pos=end+1;
  }
  return false;
}
class TimedSource final : public asset_zip::RandomAccessSource {
 public:
  std::shared_ptr<const asset_zip::RandomAccessSource> source;
  std::function<void()> check;
  std::uint64_t size() const noexcept override { return source->size(); }
  Result<std::size_t> read_at(std::uint64_t offset,std::span<std::byte> output) const override {
    return checked<std::size_t>([&] {
      check();
      const auto result=source->read_at(offset,output);
      check();
      return take(result);
    });
  }
};
} // namespace
struct ZipInputJob::Impl {
  ZipInputLimits limits;
  ZipInputHooks hooks;
  asset_zip::ReadBudget bytes;
  mutable std::mutex mutex;
  std::map<std::filesystem::path,std::chrono::steady_clock::time_point> starts;
  std::string limit;
  Impl(ZipInputLimits l,ZipInputHooks h):limits(std::move(l)),hooks(std::move(h)),bytes(limits.max_unpacked_bytes) {}
  [[noreturn]] void exceeded(std::string name) {
    { const std::lock_guard lock(mutex); if(limit.empty()) limit=std::move(name); }
    throw failure(Errc::resource_exhausted,"am.import.archive.limit_exceeded");
  }
  void check(const std::filesystem::path& path) {
    const auto now=hooks.clock?hooks.clock():std::chrono::steady_clock::now();
    bool timed_out=false, failed=false;
    {
      const std::lock_guard lock(mutex);
      const auto [at,inserted]=starts.try_emplace(path.lexically_normal(),now);
      (void)inserted;
      timed_out=now-at->second>=limits.max_seconds_per_container;
      failed=!limit.empty();
    }
    if(timed_out) exceeded("max_seconds_per_container");
    if(failed) throw failure(Errc::resource_exhausted,"am.import.archive.limit_exceeded");
  }
};
ZipInputJob::ZipInputJob(ZipInputLimits l,ZipInputHooks h):impl_(std::make_shared<Impl>(std::move(l),std::move(h))) {}
std::uint64_t ZipInputJob::remaining() const noexcept { return impl_->bytes.remaining(); }
std::string ZipInputJob::exceeded_limit() const { const std::lock_guard lock(impl_->mutex); return impl_->limit; }
struct ZipInput::Impl {
  asset_zip::ZipArchive zip;
  std::filesystem::path path;
  std::shared_ptr<ZipInputJob> job;
  std::vector<ZipInputEntry> entries;
  Impl(asset_zip::ZipArchive z,std::filesystem::path p,std::shared_ptr<ZipInputJob> j):zip(std::move(z)),path(std::move(p)),job(std::move(j)) {}
};
struct ZipInputStream::Impl {
  asset_zip::EntryStream stream;
  std::filesystem::path path;
  std::shared_ptr<ZipInputJob> job;
  std::size_t index;
  std::uint64_t compressed, produced=0;
  std::optional<Error> error;
  Impl(asset_zip::EntryStream s,std::filesystem::path p,std::shared_ptr<ZipInputJob> j,const asset_zip::EntryInfo& e)
    :stream(std::move(s)),path(std::move(p)),job(std::move(j)),index(e.index),compressed(e.compressed_size) {}
};
ZipInput::ZipInput(std::shared_ptr<Impl> p):impl_(std::move(p)) {}
Result<ZipInput> ZipInput::open(const std::filesystem::path& path,std::shared_ptr<ZipInputJob> job) {
  return checked<ZipInput>([&] {
    if(!job) throw failure(Errc::invalid_argument,"am.archive.invalid_job");
    if(!std::isfinite(job->impl_->limits.max_ratio) || job->impl_->limits.max_ratio<=0)
      throw failure(Errc::invalid_argument,"am.archive.invalid_limits");
    job->impl_->check(path);
    if(job->impl_->hooks.on_open) job->impl_->hooks.on_open(path);
    auto source=std::make_shared<TimedSource>();
    source->source=take(asset_zip::file_source(path));
    source->check=[job,path] { job->impl_->check(path); };
    auto opened=asset_zip::ZipArchive::open(source,job->impl_->limits.zip,asset_zip::ZipReadProfile::input);
    if(!opened && opened.error().message_id.starts_with("am.archive.limit."))
      job->impl_->exceeded(opened.error().message_id.substr(std::string_view("am.archive.limit.").size()));
    auto zip=take(std::move(opened));
    auto state=std::make_shared<Impl>(std::move(zip),path,std::move(job));
    std::map<std::string,std::vector<std::size_t>> raw_names, keys;
    for(const auto& info:state->zip.entries()) {
      state->job->impl_->check(path);
      auto e=decoded(info);
      if(unsafe(e.path,info.kind)) e.path_problem="unsafe_entry_path";
      raw_names[info.raw_name].push_back(info.index);
      keys[path_key(e.path,info.kind)].push_back(info.index);
      state->entries.push_back(std::move(e));
    }
    for(const auto& [key,indices]:keys) if(indices.size()>1) {
      (void)key;
      for(const auto i:indices) if(state->entries[i].path_problem.empty()) state->entries[i].path_problem="case_collision";
    }
    for(const auto& [key,indices]:raw_names) if(indices.size()>1) {
      (void)key;
      for(const auto i:indices) if(state->entries[i].path_problem!="unsafe_entry_path") state->entries[i].path_problem="duplicate_entry_name";
    }
    state->job->impl_->check(path);
    return ZipInput(std::move(state));
  });
}
std::span<const ZipInputEntry> ZipInput::entries() const noexcept { return impl_->entries; }
const asset_zip::ArchiveSummary& ZipInput::summary() const noexcept { return impl_->zip.summary(); }
Result<ZipInputStream> ZipInput::open_entry(std::size_t index,bool sha) const {
  return checked<ZipInputStream>([&] {
    impl_->job->impl_->check(impl_->path);
    if(index>=entries().size()) throw failure(Errc::invalid_argument,"am.archive.invalid_index");
    const auto& e=entries()[index].info;
    if(e.kind!=asset_zip::EntryKind::regular)
      throw failure(Errc::unsupported,e.kind==asset_zip::EntryKind::symlink ? "am.archive.link_not_followed" : "am.archive.special_not_materialized");
    auto stream=take(impl_->zip.open_entry(index,impl_->job->impl_->bytes,sha));
    return ZipInputStream(std::make_unique<ZipInputStream::Impl>(std::move(stream),impl_->path,impl_->job,e));
  });
}
ZipInputStream::ZipInputStream(std::unique_ptr<Impl> p):impl_(std::move(p)) {}
ZipInputStream::ZipInputStream(ZipInputStream&&) noexcept=default;
ZipInputStream& ZipInputStream::operator=(ZipInputStream&&) noexcept=default;
ZipInputStream::~ZipInputStream()=default;
Result<std::size_t> ZipInputStream::read(std::span<std::byte> output) {
  if(impl_->error) return *impl_->error;
  auto result=checked<std::size_t>([&] {
    auto& job=*impl_->job->impl_;
    job.check(impl_->path);
    if(job.hooks.before_read) {
      const auto r=job.hooks.before_read(impl_->path,impl_->index);
      job.check(impl_->path);
      if(!r) throw r.error();
    }
    const auto r=impl_->stream.read(output);
    job.check(impl_->path);
    if(!r && r.error().message_id=="am.zip.limit_exceeded" && job.bytes.remaining()==0) job.exceeded("max_unpacked_bytes");
    const auto n=take(r);
    impl_->produced+=n;
    // The warm-up output threshold is a hypothesis (O-17), not a format limit.
    constexpr std::uint64_t ratio_warmup=1024*1024;
    if(impl_->produced>ratio_warmup && static_cast<long double>(impl_->produced)>
        static_cast<long double>(impl_->compressed)*job.limits.max_ratio) job.exceeded("max_ratio");
    job.check(impl_->path);
    return n;
  });
  if(!result) impl_->error=result.error();
  return result;
}
std::optional<asset_zip::EntryVerification> ZipInputStream::verification() const noexcept { return impl_->stream.verification(); }
Result<std::vector<std::byte>> ZipInput::sample(std::size_t index,std::size_t count) const {
  return checked<std::vector<std::byte>>([&] {
    auto stream=take(open_entry(index));
    std::vector<std::byte> bytes(count);
    std::size_t done=0;
    while(done<count) { const auto n=take(stream.read(std::span(bytes).subspan(done))); if(!n) break; done+=n; }
    bytes.resize(done);
    return bytes;
  });
}
std::string_view zip_input_failure_kind(const Error& e) noexcept {
  if(e.code==Errc::unsupported) return "unsupported";
  if(e.code==Errc::resource_exhausted || e.code==Errc::cancelled || e.code==Errc::io_error || e.code==Errc::not_found) return "incomplete";
  return "invalid";
}
} // namespace am::archive
