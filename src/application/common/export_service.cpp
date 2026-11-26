#include "am/application/export_service.hpp"

namespace am::application {
namespace {
Error error(Errc code, std::string_view id) {
  return Error::make(code,"Export command failed").in("application.export").id(std::string(id));
}
template <class T> T take(Result<T> result) { if (!result) throw result.error(); return std::move(*result); }
AssetOutcome outcome(Json summary, bool enqueue_only) {
  AssetOutcome result;
  for (const auto& asset : summary.at("assets"))
    if (asset.at("status") != "exported" && !(enqueue_only && asset.at("status") == "queued"))
      result.status = ExitCode::operation_failed;
  result.value = std::move(summary);
  return result;
}
}  // namespace
Result<AssetOutcome> export_assets(const std::filesystem::path& root, const ExportOptions& options, std::stop_token stop) {
  try {
    if (options.library.mode != library::Mode::read_only) {
      auto opened = JobService::open(root,options.jobs,options.library);
      if (opened) {
        auto service = std::move(*opened);
        const auto parent = take(exporting::enqueue_export(service.catalog(),options.request));
        if (!options.enqueue_only) take(service.run(stop));
        auto result = outcome(take(exporting::export_summary(service.catalog(),parent)),options.enqueue_only);
        result.diagnostics = service.diagnostics();
        return result;
      }
      if (opened.error().code != Errc::read_only && opened.error().code != Errc::locked) return opened.error();
    }
    auto library_options = options.library; library_options.mode = library::Mode::read_only;
    auto catalog = take(catalog::Catalog::open(root,library_options));
    if (options.enqueue_only) return error(Errc::read_only,"am.export.queue_unavailable");
    return outcome(take(exporting::export_direct(catalog,options.request,stop)),false);
  } catch (const Error& exception) { return exception; }
  catch (const std::bad_alloc&) { return error(Errc::resource_exhausted,"am.export.out_of_memory"); }
  catch (const std::exception& exception) {
    return Error::make(Errc::io_error,"Export command failed",exception.what()).in("application.export").id("am.export.io_error");
  }
}
}  // namespace am::application
