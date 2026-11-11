#include "am/storage/asset_zip/profile.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/unicode.hpp"
#include <map>
#include <new>

namespace am::asset_zip {
Result<std::vector<ProfileViolation>> check_normative_profile(const ZipArchive& archive) {
  try {
    std::vector<ProfileViolation> result;
    std::map<std::string, std::size_t> names;
    bool manifest = false;
    auto add = [&](std::string_view code, std::optional<std::size_t> index, std::string detail) {
      result.push_back({std::string(code), index, std::move(detail)});
    };
    for (const auto& entry : archive.entries()) {
      const auto index = entry.index;
      if (entry.encrypted) add(profile_code::encrypted, index, "Encrypted entry");
      if (entry.method != 0 && entry.method != 8) add(profile_code::method, index, "Compression method is not 0 or 8");
      if (entry.data_descriptor) add(profile_code::descriptor, index, "Data descriptor flag is set");
      if (!entry.utf8_flag) add(profile_code::utf8_flag, index, "UTF-8 flag is absent");
      const bool valid = unicode::is_valid_utf8(entry.raw_name);
      if (!valid) add(profile_code::invalid_utf8, index, "Name contains invalid UTF-8 bytes");
      if (entry.kind != EntryKind::regular) add(profile_code::kind, index, "Entry is not a regular file");
      const auto checked = paths::check_container_path(entry.raw_name);
      if (!checked) {
        if (checked.error().code == Errc::resource_exhausted) return checked.error();
        add(profile_code::path, index, checked.error().ctx.detail);
      }
      const auto [previous, inserted] = names.emplace(unicode::path_collision_key(entry.raw_name), index);
      if (!inserted) add(profile_code::collision, index, "Collision with entry=" + std::to_string(previous->second));
      manifest = manifest || entry.raw_name == "manifest.json";
      const bool allowed = entry.raw_name == "manifest.json" || entry.raw_name.starts_with("content/") ||
          entry.raw_name.starts_with("generated/") || entry.raw_name.starts_with("extensions/");
      if (!allowed) add(profile_code::space, index, "Entry is outside container namespaces");
    }
    if (!manifest) add(profile_code::manifest, {}, "Root manifest.json is missing");
    if (archive.summary().unaccounted_bytes) add(profile_code::gaps, {}, "Unaccounted bytes=" + std::to_string(archive.summary().unaccounted_bytes));
    // Proposal by the manager in T-007; not an approved format rule.
    if (archive.summary().comment_length) add(profile_code::comment, {}, "EOCD comment is not empty (proposal)");
    return result;
  } catch (const std::bad_alloc&) {
    return Error::make(Errc::resource_exhausted, "ZIP allocation failed", "Profile allocation failed")
        .in("storage.asset_zip").id("am.zip.out_of_memory");
  } catch (const std::exception& e) {
    return Error::make(Errc::io_error, "ZIP profile failed", e.what()).in("storage.asset_zip").id("am.zip.io_error");
  }
}
}  // namespace am::asset_zip
