#include "am/application/asset_service.hpp"
#include "am/application/library_service.hpp"
#include "am/application/development.hpp"
#include <CLI/CLI.hpp>
#include <filesystem>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif
namespace {
namespace app = am::application;
using app::Json;
std::filesystem::path path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
void print_error(const am::Error& e, bool verbose) {
  std::cerr << app::message_text(e) << '\n';
  if (verbose) std::cerr << am::to_log_string(e) << '\n';
}
Json error_value(const am::Error& e) {
  auto j = app::error_json(e);
  if (j) return std::move(j).value();
  return Json{{"code", "am.resource_exhausted"}, {"message_id", "am.app.out_of_memory"}, {"detail", ""}};
}
void print_diagnostics(const app::AssetOutcome& result, bool verbose) {
  const auto& report = result.value.contains("verification") ? result.value["verification"] : result.value;
  if (report.contains("v3_findings"))
    for (const auto& finding : report["v3_findings"]) {
      std::cerr << finding["severity"].get<std::string>() << ' ' << finding["rule"].get<std::string>() << ' '
                << finding["pointer"].get<std::string>() << ": "
                << app::message_text(finding["message_id"].get_ref<const std::string&>()) << '\n';
      if (verbose) std::cerr << finding["detail"].get<std::string>() << '\n';
    }
  for (const auto& error : result.diagnostics) {
    bool shown = false;
    if (report.contains("v3_findings"))
      for (const auto& finding : report["v3_findings"])
        if (finding["message_id"] == error.message_id) shown = true;
    if (!shown)
      print_error(error, verbose);
    else if (verbose)
      std::cerr << am::to_log_string(error) << '\n';
  }
}
void print_text(const std::string& command, const app::AssetOutcome& result, bool verbose) {
  const auto& j = result.value;
  if (command.starts_with("library ")) {
    for (const auto& [key, value] : j.items()) {
      std::cout << app::message_text(std::string("am.app.label.") + key) << ": ";
      if (value.is_string()) std::cout << value.get<std::string>();
      else std::cout << value.dump();
      std::cout << '\n';
    }
  } else if (command == "validate" || command == "verify") {
    for (auto key : {"v1", "v2", "v3"})
      if (j.contains(key)) std::cout << key << ": " << j[key].get<std::string>() << '\n';
    if (j.contains("container_verification"))
      std::cout << app::message_text("am.app.label.container_verification") << ": "
                << j["container_verification"].get<std::string>() << '\n';
  } else {
    if (command == "inspect")
      for (auto key : {"v1", "v2"}) std::cout << key << ": " << j.at(key).get<std::string>() << '\n';
    for (auto key : {"asset_id", "revision", "format_support", "created_at", "modified_at", "title", "file_count",
                     "historical_file_count", "input_count", "history_entry_count", "path"})
      if (j.contains(key)) {
        std::cout << app::message_text(std::string("am.app.label.") + key) << ": ";
        if (j[key].is_string())
          std::cout << j[key].get<std::string>();
        else
          std::cout << j[key];
        std::cout << '\n';
      }
    if (j.contains("format"))
      std::cout << app::message_text("am.app.label.format") << ": " << j["format"]["version"]["major"] << '.'
                << j["format"]["version"]["minor"] << '\n';
    if (j.contains("files"))
      for (const auto& f : j["files"])
        std::cout << f["id"].get<std::string>() << ' ' << f["path"].get<std::string>() << ' '
                  << f["size_bytes"].get<std::string>() << ' ' << f["origin_kind"].get<std::string>() << '\n';
    if (j.contains("extracted"))
      for (const auto& f : j["extracted"])
        std::cout << f["id"].get<std::string>() << ' ' << f["path"].get<std::string>() << '\n';
  }
  print_diagnostics(result, verbose);
}
}  // namespace
int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif
  CLI::App cli{"3D/2D asset catalog"};
  cli.require_subcommand(1);
  cli.footer("Exit codes: 0 success, 1 failed check, 2 usage error, 3 partial check, 4 operation not completed.");
  argv = cli.ensure_utf8(argv);
  bool json = false, verbose = false;
  std::string selected_command = "asset";
  int code = 0;
  auto render = [&](const std::string& command, am::Result<app::AssetOutcome> result) {
    selected_command = command;
    if (!result) {
      code = static_cast<int>(app::error_exit_code(result.error()));
      if (result.error().message_id == "am.library.config_invalid") code = 1;
      if (json) std::cout << Json{{"command", command}, {"error", error_value(result.error())}}.dump() << '\n';
      print_error(result.error(), verbose);
      return;
    }
    code = static_cast<int>(result->status);
    if (json) {
      auto j = result->value;
      if(command!="validate"&&command!="verify"&&!result->diagnostics.empty()&&
         (result->status==app::ExitCode::check_failed||result->status==app::ExitCode::operation_failed))
        j["error"]=error_value(result->diagnostics.front());
      j["command"] = command;
      std::cout << j.dump() << '\n';
      print_diagnostics(*result, verbose);
    } else
      print_text(command, *result, verbose);
  };
  auto presentation = [&](CLI::App* cmd) {
    cmd->add_flag("--json", json, "Print one UTF-8 JSON object");
    cmd->add_flag("--verbose", verbose, "Include technical details and causes");
  };
  auto* library = cli.add_subcommand("library", "Create a library or read its status");
  library->require_subcommand(1);
  std::string library_root, library_name, library_info_root;
  auto* library_init = library->add_subcommand("init", "Initialize a new or empty library directory");
  library_init->add_option("dir", library_root)->required();
  library_init->add_option("--name", library_name, "Library name");
  presentation(library_init);
  library_init->callback([&] { render("library init", app::init_library(path(library_root), library_name)); });
  auto* library_info = library->add_subcommand("info", "Read library status without changing library data");
  library_info->add_option("dir", library_info_root)->required();
  presentation(library_info);
  library_info->callback([&] { render("library info", app::inspect_library(path(library_info_root))); });
  std::string library_rebuild_root, library_list_root; bool library_list_trash = false;
  auto* library_rebuild = library->add_subcommand("rebuild", "Rebuild the index from asset and trash ZIPs");
  library_rebuild->add_option("dir", library_rebuild_root)->required();
  presentation(library_rebuild);
  library_rebuild->callback([&] { render("library rebuild", app::rebuild_library(path(library_rebuild_root))); });
  auto* library_list = library->add_subcommand("list", "List indexed assets without changing library data");
  library_list->add_option("dir", library_list_root)->required();
  library_list->add_flag("--trash", library_list_trash, "Include indexed assets from trash");
  presentation(library_list);
  library_list->callback([&] { render("library list", app::list_library(path(library_list_root),library_list_trash)); });
  std::string library_sync_root;
  bool library_sync_full = false;
  auto* library_sync =
      library->add_subcommand("sync", "Update the index from changed ZIP files and mark absent assets");
  library_sync->add_option("dir", library_sync_root)->required();
  library_sync->add_flag("--full", library_sync_full, "Read every candidate, including files with unchanged stat");
  presentation(library_sync);
  library_sync->callback(
      [&] { render("library sync", app::sync_library(path(library_sync_root), library_sync_full)); });
  auto* asset = cli.add_subcommand("asset", "Create, inspect, check, and extract a standalone asset ZIP");
  asset->require_subcommand(1);
  std::vector<std::string> inputs;
  std::string output, title;
  auto* create = asset->add_subcommand("create", "Create a new asset ZIP without replacing an existing file");
  create->add_option("files", inputs, "Input files (directories are not supported)")->required()->expected(-1);
  create->add_option("-o,--output", output, "New output ZIP")->required();
  auto* title_option = create->add_option("--title", title, "Asset title");
  presentation(create);
  create->footer(
      "Interrupted create may leave a .am-create-<uuid> directory beside the output. Automatic cleanup of interrupted "
      "runs is not provided.");
  create->callback([&] {
    app::CreateAssetOptions o;
    for (const auto& f : inputs) o.files.push_back(path(f));
    o.output = path(output);
    if (title_option->count()) o.title = title;
    render("create", app::create_asset(o));
  });
  std::string inspect_path, validate_path, verify_path, extract_path, directory, level = "full";
  std::vector<std::string> verify_files, extract_files;
  bool manifest = false;
  auto* inspect = asset->add_subcommand("inspect", "Read the manifest summary (V1/V2, no payload reads)");
  inspect->add_option("zip", inspect_path)->required();
  presentation(inspect);
  inspect->callback([&] { render("inspect", app::inspect_asset(path(inspect_path))); });
  auto* validate = asset->add_subcommand("validate", "Check V1-V3 without reading payload streams");
  validate->add_option("zip", validate_path)->required();
  presentation(validate);
  validate->callback([&] { render("validate", app::validate_asset(path(validate_path))); });
  auto* verify = asset->add_subcommand("verify", "Check V1-V4 (full by default)");
  verify->add_option("zip", verify_path)->required();
  verify->add_option("--level", level, "quick, selective, or full")
      ->check(CLI::IsMember({"quick", "selective", "full"}));
  verify->add_option("--file", verify_files, "FileID to verify (repeatable, selective level)")
      ->expected(1)
      ->take_all()
      ->allow_extra_args(false);
  presentation(verify);
  verify->callback([&] {
    if (level == "selective" && verify_files.empty())
      throw CLI::ValidationError("--file", "Required with --level selective");
    app::VerifyAssetOptions o;
    o.file_ids = verify_files;
    o.level = level == "quick"       ? app::VerificationLevel::quick
              : level == "selective" ? app::VerificationLevel::selective
                                     : app::VerificationLevel::full;
    render("verify", app::verify_asset(path(verify_path), o));
  });
  auto* extract = asset->add_subcommand("extract", "Extract current files into a new or empty directory");
  extract->add_option("zip", extract_path)->required();
  extract->add_option("-d,--directory", directory, "New or empty destination")->required();
  extract->add_option("--file", extract_files, "FileID to extract (repeatable)")
      ->expected(1)
      ->take_all()
      ->allow_extra_args(false);
  extract->add_flag("--manifest", manifest, "Also write the original manifest bytes");
  presentation(extract);
  extract->callback([&] {
    app::ExtractAssetOptions o;
    o.directory = path(directory);
    o.file_ids = extract_files;
    o.manifest = manifest;
    render("extract", app::extract_asset(path(extract_path), o));
  });
  auto dev = [&](const std::string& verb, const std::vector<std::string>& args) {
    auto r = app::development_command(verb, args);
    if (!r) {
      print_error(r.error(), true);
      code = 4;
    } else {
      std::cout << r->text;
      code = r->exit_code;
    }
  };
  cli.add_subcommand("version", "Print the build version")->group("Development")->callback([&] { dev("version", {}); });
  bool v4 = false;
  int count = 1;
  auto* uuid = cli.add_subcommand("uuid", "Generate UUIDv7 identifiers (UUIDv4 with --v4)")->group("Development");
  uuid->add_flag("--v4", v4);
  uuid->add_option("count", count)->check(CLI::NonNegativeNumber);
  uuid->callback([&] {
    dev("uuid",
        v4 ? std::vector<std::string>{"--v4", std::to_string(count)} : std::vector<std::string>{std::to_string(count)});
  });
  std::vector<std::string> probes;
  auto* probe = cli.add_subcommand("probe", "Probe content types and STL statistics")->group("Development");
  probe->add_option("files", probes)->required()->expected(-1);
  probe->callback([&] { dev("probe", probes); });
  std::string vol;
  auto* volumes = cli.add_subcommand("volumes", "List multi-volume candidates")->group("Development");
  volumes->add_option("dir", vol)->required();
  volumes->callback([&] { dev("volumes", {vol}); });
  std::string root, source;
  auto* zip_path = cli.add_subcommand("zip-path", "Map a container path")->group("Development");
  zip_path->add_option("root", root)->required();
  zip_path->add_option("source-path", source)->required();
  zip_path->callback([&] { dev("zip-path", {root, source}); });
  std::string bad_name;
  auto* fix = cli.add_subcommand("fix-name", "Repair CP437 mojibake")->group("Development");
  fix->add_option("text", bad_name)->required();
  fix->callback([&] { dev("fix-name", {bad_name}); });
  std::string excluded_path, excluded_file;
  auto* exclude = cli.add_subcommand("exclude", "Show an exclusion rule verdict")->group("Development");
  exclude->add_option("entry-path", excluded_path)->required();
  auto* excluded = exclude->add_option("file", excluded_file);
  exclude->callback([&] {
    dev("exclude", excluded->count() ? std::vector<std::string>{excluded_path, excluded_file}
                                     : std::vector<std::string>{excluded_path});
  });
  try {
    cli.parse(argc, argv);
    return code;
  } catch (const CLI::ParseError& e) {
    return cli.exit(e) == 0 ? 0 : 2;
  } catch (const std::exception& e) {
    auto error = am::Error::make(am::Errc::internal, "Command failed", e.what()).id("am.app.internal");
    if (json) std::cout << Json{{"command", selected_command}, {"error", error_value(error)}}.dump() << '\n';
    print_error(error, verbose);
    return 4;
  } catch (...) {
    std::cerr << app::message_text("am.app.internal") << '\n';
    return 4;
  }
}
