#include "am/storage/asset_zip/sha256.hpp"
#include <new>
#include <optional>
#include <openssl/evp.h>
namespace am::asset_zip {
namespace {
Error error(std::string detail, bool oom = false) {
  return Error::make(oom ? Errc::resource_exhausted : Errc::io_error, "SHA-256 operation failed", std::move(detail))
      .in("storage.asset_zip").op("zip.sha256").id(oom ? "am.zip.out_of_memory" : "am.zip.io_error");
}
}
struct Sha256::Impl {
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  std::optional<Sha256Digest> digest;
  ~Impl() { EVP_MD_CTX_free(context); }
};
Sha256::Sha256(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;
Sha256::~Sha256() = default;
Result<Sha256> Sha256::create() {
  try {
    auto impl = std::make_unique<Impl>();
    if (!impl->context) return error("EVP_MD_CTX_new failed", true);
    if (EVP_DigestInit_ex(impl->context, EVP_sha256(), nullptr) != 1) return error("EVP_DigestInit_ex failed");
    return Sha256(std::move(impl));
  } catch (const std::bad_alloc&) { return error("Allocation failed", true); }
  catch (const std::exception& e) { return error(e.what()); }
}
Result<void> Sha256::update(std::span<const std::byte> bytes) {
  try {
    if (impl_->digest) return error("Digest already finalized");
    if (!bytes.empty() && EVP_DigestUpdate(impl_->context, bytes.data(), bytes.size()) != 1) return error("EVP_DigestUpdate failed");
    return {};
  } catch (const std::bad_alloc&) { return error("Allocation failed", true); }
  catch (const std::exception& e) { return error(e.what()); }
}
Result<Sha256Digest> Sha256::finish() {
  try {
    if (impl_->digest) return *impl_->digest;
    Sha256Digest digest{};
    unsigned int count = 0;
    if (EVP_DigestFinal_ex(impl_->context, reinterpret_cast<unsigned char*>(digest.data()), &count) != 1 || count != digest.size())
      return error("EVP_DigestFinal_ex failed");
    impl_->digest = digest;
    return digest;
  } catch (const std::bad_alloc&) { return error("Allocation failed", true); }
  catch (const std::exception& e) { return error(e.what()); }
}
}  // namespace am::asset_zip
