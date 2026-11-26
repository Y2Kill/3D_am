#include "am/services/export/selection.hpp"

#include <set>

namespace am::exporting {
Result<std::vector<std::size_t>> select_files(const manifest::ManifestDocument& document,
                                             std::span<const Preset> presets,
                                             std::span<const std::string> file_ids) {
  try {
    const auto& manifest = document.known();
    const auto& files = manifest.at("files");
    std::set<std::string, std::less<>> current, requested(file_ids.begin(), file_ids.end()), previews;
    for (const auto& file : files) current.insert(file.at("id").get<std::string>());
    for (const auto& id : requested) if (!current.contains(id))
      return Error::make(Errc::invalid_argument, "Unknown current FileID", id)
          .in("services.export").id("am.export.file_unknown");
    if (manifest.contains("previews"))
      for (const auto& preview : manifest.at("previews").at("items"))
        previews.insert(preview.at("file_id").get<std::string>());
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < files.size(); ++index) {
      const auto& file = files[index];
      const auto& id = file.at("id").get_ref<const std::string&>();
      std::optional<std::string_view> type;
      if (file.contains("type")) type = file.at("type").at("id").get_ref<const std::string&>();
      const auto value = categories(type, file.at("storage_path").get_ref<const std::string&>());
      if (requested.contains(id) || selected(value, previews.contains(id), presets)) result.push_back(index);
    }
    if (result.empty())
      return Error::make(Errc::invalid_argument, "Export selection is empty")
          .in("services.export").id("am.export.selection_empty");
    return result;
  } catch (const std::bad_alloc&) {
    return Error::make(Errc::resource_exhausted, "Export selection allocation failed")
        .in("services.export").id("am.export.out_of_memory");
  } catch (const std::exception& exception) {
    return Error::make(Errc::corrupt_data, "Invalid source manifest", exception.what())
        .in("services.export").id("am.export.source_invalid");
  }
}
}  // namespace am::exporting
