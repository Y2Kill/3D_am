#include <CLI/CLI.hpp>
#include <array>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "am/application/asset_service.hpp"
#include "am/application/development.hpp"
#include "am/application/import_service.hpp"
#include "am/application/export_service.hpp"
#include "am/application/job_service.hpp"
#include "am/application/library_service.hpp"
#include "am/application/logging.hpp"
#include "am/foundation/version.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef AM_DIAGNOSTIC_CLI_TEST_HOOK
void install_diagnostic_cli_test_hook(am::application::DiagnosticOptions&);
#endif
namespace {
namespace app = am::application;
using app::Json;
std::filesystem::path path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
std::mutex job_console_mutex;
#ifdef _WIN32
std::mutex job_signal_mutex;
HANDLE job_signal_event = nullptr;
BOOL WINAPI jobs_console_handler(DWORD signal) {
  if (signal != CTRL_C_EVENT && signal != CTRL_BREAK_EVENT) return FALSE;
  try {
    const std::lock_guard lock(job_signal_mutex);
    return job_signal_event && SetEvent(job_signal_event) ? TRUE : FALSE;
  } catch (...) {
    return FALSE;
  }
}
#else
volatile std::sig_atomic_t job_signal_fd = -1;
void jobs_signal_handler(int) {
  const int saved = errno;
  const int fd = job_signal_fd;
  if (fd >= 0) {
    const char value = 1;
    (void)::write(fd, &value, 1);
  }
  errno = saved;
}
#endif
class JobSignals {
 public:
  JobSignals() {
#ifdef _WIN32
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event_) throw std::runtime_error("Signal event could not be created");
    {
      const std::lock_guard lock(job_signal_mutex);
      job_signal_event = event_;
    }
    if (!SetConsoleCtrlHandler(jobs_console_handler, TRUE)) {
      close();
      throw std::runtime_error("Console signal handler could not be installed");
    }
    registered_ = true;
#else
    if (::pipe(pipe_) != 0) throw std::runtime_error("Signal pipe could not be created");
    const int flags = fcntl(pipe_[1], F_GETFL);
    if (flags < 0 || fcntl(pipe_[1], F_SETFL, flags | O_NONBLOCK) < 0) {
      close();
      throw std::runtime_error("Signal pipe could not be configured");
    }
    job_signal_fd = pipe_[1];
    struct sigaction action{};
    action.sa_handler = jobs_signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &previous_int_) != 0) {
      close();
      throw std::runtime_error("Interrupt handler could not be installed");
    }
    int_registered_ = true;
    if (sigaction(SIGTERM, &action, &previous_term_) != 0) {
      close();
      throw std::runtime_error("Termination handler could not be installed");
    }
    term_registered_ = true;
#endif
    try {
      listener_ = std::thread([this] {
#ifdef _WIN32
        if (WaitForSingleObject(event_, INFINITE) != WAIT_OBJECT_0) return;
#else
        char value = 0;
        ssize_t count = 0;
        do {
          count = ::read(pipe_[0], &value, 1);
        } while (count < 0 && errno == EINTR);
        if (count != 1 || value == 0) return;
#endif
        if (disposing_.load()) return;
        source_.request_stop();
        const std::lock_guard lock(job_console_mutex);
        std::cerr << app::message_text("am.jobs.waiting_safe_point") << '\n';
      });
    } catch (...) {
      close();
      throw;
    }
  }
  ~JobSignals() { close(); }
  JobSignals(const JobSignals&) = delete;
  JobSignals& operator=(const JobSignals&) = delete;
  std::stop_token token() const noexcept { return source_.get_token(); }

 private:
  std::stop_source source_;
  std::thread listener_;
  std::atomic<bool> disposing_{false};
#ifdef _WIN32
  HANDLE event_ = nullptr;
  bool registered_ = false;
#else
  int pipe_[2]{-1, -1};
  struct sigaction previous_int_{}, previous_term_{};
  bool int_registered_ = false, term_registered_ = false;
#endif
  void close() noexcept {
    disposing_ = true;
#ifdef _WIN32
    if (registered_) {
      SetConsoleCtrlHandler(jobs_console_handler, FALSE);
      registered_ = false;
    }
    {
      const std::lock_guard lock(job_signal_mutex);
      job_signal_event = nullptr;
    }
    if (event_) SetEvent(event_);
#else
    if (int_registered_) {
      sigaction(SIGINT, &previous_int_, nullptr);
      int_registered_ = false;
    }
    if (term_registered_) {
      sigaction(SIGTERM, &previous_term_, nullptr);
      term_registered_ = false;
    }
    job_signal_fd = -1;
    if (pipe_[1] >= 0) {
      const char value = 0;
      (void)::write(pipe_[1], &value, 1);
    }
#endif
    if (listener_.joinable()) listener_.join();
#ifdef _WIN32
    if (event_) {
      CloseHandle(event_);
      event_ = nullptr;
    }
#else
    for (auto& fd : pipe_)
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
#endif
  }
};
void print_job_event(const am::jobs::JobEvent& event) {
  const char* key = event.kind == am::jobs::EventKind::started    ? "am.jobs.event.started"
                    : event.kind == am::jobs::EventKind::progress ? "am.jobs.event.progress"
                    : event.kind == am::jobs::EventKind::finished ? "am.jobs.event.finished"
                                                                  : "am.jobs.event.state_changed";
  const std::lock_guard lock(job_console_mutex);
  std::cerr << app::message_text(key) << ": " << event.job.job_id << ' ' << event.job.type << ' '
            << am::contracts::job_state_name(event.job.state);
  if (event.kind == am::jobs::EventKind::progress) {
    std::cerr << ' ';
    if (event.job.progress.done)
      std::cerr << *event.job.progress.done;
    else
      std::cerr << '?';
    std::cerr << '/';
    if (event.job.progress.total)
      std::cerr << *event.job.progress.total;
    else
      std::cerr << '?';
    std::cerr << ' ' << event.job.progress.unit;
    if (event.rate) std::cerr << " (" << *event.rate << "/s)";
  }
  std::cerr << '\n';
}
void print_error(const am::Error& e, bool verbose) {
  if(e.severity>=am::Severity::error)AM_LOG(error,"cli","cli.error","Command error",{},&e);
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
  if (command == "export") {
    std::cout << app::message_text("am.export.summary") << '\n';
    for (const auto& asset : j.at("assets")) {
      std::cout << asset.at("asset_id").get<std::string>() << ' ' << asset.at("status").get<std::string>();
      if (asset.contains("target_name")) std::cout << ' ' << asset.at("target_name").get<std::string>();
      if (asset.contains("error") && asset.at("error").is_object())
        std::cout << ' ' << app::message_text(asset.at("error").value("message_id",std::string{}));
      std::cout << '\n';
    }
  } else if (command == "import scan") {
    std::cout << app::message_text("am.import.scan_summary") << ": " << j.dump() << '\n';
  } else if (command.starts_with("library ")) {
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
  struct LogLifetime {
    int& code;
    std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    ~LogLifetime(){AM_LOG(info,"cli","app.exit","CLI exited",{{"exit_code",static_cast<std::int64_t>(code)},{"duration_ms",static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count())}});am::log::set_sink(nullptr);}
  } log_lifetime{code};
  std::string log_directory,log_level="info";
  auto* log_dir_option=cli.add_option("--log-dir",log_directory,"Enable a process log in this directory")->envname("AM_LOG_DIR");
  cli.add_option("--log-level",log_level,"Minimum process log level")->envname("AM_LOG_LEVEL")->check(CLI::IsMember({"trace","debug","info","warning","error","critical"}));
  bool logging_initialized=false,diagnostic_dry_run=false;
  const bool explicit_log_directory=std::any_of(argv+1,argv+argc,[](const char* value){const auto option=std::string_view(value);return option=="--log-dir" || option.starts_with("--log-dir=");});
  auto initialize_logging=[&]{
    if(logging_initialized)return;
    logging_initialized=true;
    if(!diagnostic_dry_run && (log_dir_option->count() || !log_directory.empty())){
      app::LoggingOptions options;options.directory=path(log_directory);
      for(unsigned i=0;i<6;++i)if(am::log::level_name(static_cast<am::log::Level>(i))==log_level)options.level=static_cast<am::log::Level>(i);
      auto opened=app::open_file_log(options);if(!opened)print_error(opened.error(),verbose);
    }
    std::string command;auto* current=&cli;
    while(!current->get_subcommands().empty()){current=current->get_subcommands().front();if(!command.empty())command+=' ';command+=current->get_name();}
    selected_command=command;
#ifdef _WIN32
    const std::string os="windows";
#else
    const std::string os="linux";
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
    const std::string arch="arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    const std::string arch="x86_64";
#else
    const std::string arch="other";
#endif
    AM_LOG(info,"cli","app.start","CLI started",{{"version",std::string(am::kAppVersion)},{"command",command},{"os",os},{"arch",arch}});
  };
  cli.parse_complete_callback(initialize_logging);
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
      Json state_warnings = Json::array();
      for (const auto& diagnostic : result->diagnostics)
        if (diagnostic.message_id == "am.library.state_recreated") state_warnings.push_back(error_value(diagnostic));
      if (!state_warnings.empty()) j["warnings"] = std::move(state_warnings);
      if(command!="validate"&&command!="verify"&&command!="library verify"&&!result->diagnostics.empty()&&
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
  auto render_jobs = [&](const std::string& command, am::Result<app::JobCommandOutcome> result) {
    selected_command=command;
    if(!result){
      render(command, result.error());
      return;
    }
    code=static_cast<int>(result->status);
    if(json){
      auto encoded = result->json.empty() ? result->value.dump() : result->json;
      encoded.pop_back();
      std::cout << encoded << ",\"command\":" << Json(command).dump() << "}\n";
    } else {
      for (const auto& [key, value] : result->value.items()) {
        std::cout << app::message_text("am.jobs.label." + key) << ": ";
        if (value.is_string())
          std::cout << value.get<std::string>();
        else
          std::cout << value.dump();
        std::cout << '\n';
      }
      if (command == "jobs show" && !result->records.empty()) {
        const auto& record = result->records.front();
        for (const auto& [key, value] :
             std::array<std::pair<std::string, std::optional<std::string>>, 4>{{{"params", record.params_json},
                                                                                {"decision", record.decision_json},
                                                                                {"result", record.result_json},
                                                                                {"error", record.error_json}}})
          std::cout << app::message_text("am.jobs.label." + key) << ": " << value.value_or("null") << '\n';
      }
    }
    for (const auto& diagnostic : result->diagnostics) print_error(diagnostic, verbose);
  };
  auto* jobs = cli.add_subcommand("jobs", "Run and manage persistent background jobs");
  jobs->require_subcommand(1);
  std::string jobs_list_root, jobs_parent;
  std::vector<std::string> jobs_states;
  bool jobs_all = false;
  auto* job_list = jobs->add_subcommand("list", "List queued jobs and parent progress");
  job_list->add_option("dir", jobs_list_root)->required();
  std::vector<std::string> state_names;
  for (auto name : am::contracts::job_state_names) state_names.emplace_back(name);
  job_list->add_option("--state", jobs_states)->check(CLI::IsMember(state_names));
  job_list->add_option("--parent", jobs_parent);
  job_list->add_flag("--all", jobs_all, "Include finished jobs and children");
  presentation(job_list);
  job_list->callback([&] {
    am::contracts::JobFilter filter;
    for (const auto& state : jobs_states) filter.states.push_back(*am::contracts::parse_job_state(state));
    if (!jobs_parent.empty()) filter.parent_id = jobs_parent;
    render_jobs("jobs list", app::list_jobs(path(jobs_list_root), filter, jobs_all));
  });
  std::string jobs_show_root, jobs_show_id;
  auto* job_show = jobs->add_subcommand("show", "Show a complete job record");
  job_show->add_option("dir", jobs_show_root)->required();
  job_show->add_option("id", jobs_show_id)->required();
  presentation(job_show);
  job_show->callback([&] { render_jobs("jobs show", app::show_job(path(jobs_show_root), jobs_show_id)); });
  std::string jobs_run_root, jobs_profile = "balanced";
  unsigned jobs_parallel = 0;
  auto* job_run = jobs->add_subcommand("run", "Run queued jobs until idle or interrupted");
  job_run->add_option("dir", jobs_run_root)->required();
  job_run->add_option("--profile", jobs_profile)->check(CLI::IsMember({"economy", "balanced", "performance"}));
  auto* parallel = job_run->add_option("--jobs", jobs_parallel)->check(CLI::PositiveNumber);
  presentation(job_run);
  job_run->callback([&] {
    JobSignals signals;
    app::JobServiceOptions options;
    options.profile = jobs_profile == "economy"       ? am::jobs::Profile::economy
                      : jobs_profile == "performance" ? am::jobs::Profile::performance
                                                      : am::jobs::Profile::balanced;
    if (parallel->count()) options.parallel_jobs = jobs_parallel;
    options.on_event = print_job_event;
    render_jobs("jobs run", app::run_jobs(path(jobs_run_root), options, signals.token()));
  });
  struct JobControlArguments {
    std::string root;
    std::vector<std::string> ids;
  };
  std::map<std::string, JobControlArguments> jobs_controls;
  for (const auto* verb : {"pause", "resume", "cancel", "retry"}) {
    auto& args = jobs_controls[verb];
    auto* command = jobs->add_subcommand(verb, std::string("Apply ") + verb + " to jobs at safe points");
    command->add_option("dir", args.root)->required();
    command->add_option("id", args.ids)->required()->expected(-1);
    presentation(command);
    command->callback([&, name = std::string(verb)] {
      const auto& selected = jobs_controls.at(name);
      render_jobs("jobs " + name, app::control_jobs(path(selected.root), name, selected.ids));
    });
  }
  std::string jobs_answer_root, jobs_answer_id, jobs_answer_option;
  auto* job_answer = jobs->add_subcommand("answer", "Answer a pending job decision");
  job_answer->add_option("dir", jobs_answer_root)->required();
  job_answer->add_option("id", jobs_answer_id)->required();
  job_answer->add_option("option", jobs_answer_option)->required();
  presentation(job_answer);
  job_answer->callback(
      [&] { render_jobs("jobs answer", app::answer_job(path(jobs_answer_root), jobs_answer_id, jobs_answer_option)); });
  std::string jobs_move_root, jobs_move_id, jobs_before;
  bool jobs_last = false;
  auto* job_move = jobs->add_subcommand("move", "Move a queued job within its queue group");
  job_move->add_option("dir", jobs_move_root)->required();
  job_move->add_option("id", jobs_move_id)->required();
  auto* before = job_move->add_option("--before", jobs_before);
  job_move->add_flag("--last", jobs_last);
  presentation(job_move);
  job_move->callback([&] {
    if ((before->count() != 0) == jobs_last)
      throw CLI::ValidationError("destination", std::string(app::message_text("am.jobs.destination_invalid")));
    render_jobs("jobs move", app::move_job(path(jobs_move_root), jobs_move_id,
                                           jobs_last ? std::nullopt : std::optional<std::string_view>(jobs_before)));
  });
  std::string jobs_probe_root, jobs_probe_fail;
  std::uint64_t jobs_probe_steps = 1;
  bool jobs_probe_ask = false;
  auto* job_probe = jobs->add_subcommand("enqueue-dev", "Enqueue a development probe")->group("Development");
  job_probe->add_option("dir", jobs_probe_root)->required();
  job_probe->add_option("--steps", jobs_probe_steps);
  job_probe->add_flag("--ask", jobs_probe_ask);
  auto* fail = job_probe->add_option("--fail", jobs_probe_fail);
  presentation(job_probe);
  job_probe->callback([&] {
    render_jobs("jobs enqueue-dev", app::enqueue_probe(path(jobs_probe_root), jobs_probe_steps, jobs_probe_ask,
                                                       fail->count() ? std::optional(jobs_probe_fail) : std::nullopt));
  });
  std::string media_path;
  auto* media = cli.add_subcommand("media", "Detect a local storage device and media class")->group("Development");
  media->add_option("path", media_path)->required();
  presentation(media);
  media->callback([&] { render_jobs("media", app::inspect_media(path(media_path))); });
  auto* export_assets_command = cli.add_subcommand("export", "Export assets or selected current files without replacing existing files");
  std::string export_library, export_target, export_name, export_conflict = "fail", export_verify = "quick";
  std::vector<std::string> export_assets, export_presets, export_files;
  bool export_enqueue = false;
  export_assets_command->add_option("library",export_library)->required();
  export_assets_command->add_option("asset-id",export_assets)->required()->expected(-1);
  export_assets_command->add_option("--to",export_target)->required();
  export_assets_command->add_option("--files",export_presets)->check(CLI::IsMember({"3d","3d_preview","slicer","all"}));
  export_assets_command->add_option("--file",export_files);
  auto* export_name_option = export_assets_command->add_option("--name",export_name);
  export_assets_command->add_option("--on-conflict",export_conflict)->check(CLI::IsMember({"fail","rename"}));
  export_assets_command->add_option("--verify",export_verify)->check(CLI::IsMember({"quick","full"}));
  export_assets_command->add_flag("--enqueue-only",export_enqueue);
  presentation(export_assets_command);
  export_assets_command->callback([&] {
    JobSignals signals;
    app::ExportOptions options;
    options.request.asset_ids = export_assets; options.request.target_dir = path(export_target);
    options.request.file_ids = export_files; options.enqueue_only = export_enqueue;
    for (const auto& value : export_presets) {
      const auto preset = am::exporting::parse_preset(value);
      if (!preset) { render("export",preset.error()); return; }
      options.request.presets.push_back(*preset);
    }
    if (export_name_option->count()) options.request.name = export_name;
    options.request.on_conflict = export_conflict == "fail" ? am::exporting::ConflictPolicy::fail : am::exporting::ConflictPolicy::rename;
    options.request.verify_full = export_verify == "full";
    options.jobs.on_event = print_job_event;
    auto result = app::export_assets(path(export_library),options,signals.token());
    const bool usage_error = !result && result.error().code == am::Errc::invalid_argument;
    render("export",std::move(result));
    if (usage_error) code = 2;
  });
  auto* import_command = cli.add_subcommand("import", "Scan sources and propose an editable import plan");
  import_command->require_subcommand(1);
  auto* import_scan = import_command->add_subcommand("scan", "Read sources without modifying them or creating assets");
  auto* import_run = import_command->add_subcommand("run", "Execute an edited import plan through durable jobs");
  std::string run_plan,run_library,run_profile="balanced";
  bool enqueue_only=false;
  unsigned run_parallel=0;
  import_run->add_option("--plan",run_plan)->required();
  import_run->add_option("--library",run_library)->required();
  import_run->add_flag("--enqueue-only",enqueue_only);
  import_run->add_option("--profile",run_profile)->check(CLI::IsMember({"economy","balanced","performance"}));
  auto* run_jobs_option=import_run->add_option("--jobs",run_parallel)->check(CLI::PositiveNumber);
  presentation(import_run);
  import_run->callback([&] {
    JobSignals signals;
    app::JobServiceOptions options;
    options.profile=run_profile=="economy" ? am::jobs::Profile::economy : run_profile=="performance" ? am::jobs::Profile::performance : am::jobs::Profile::balanced;
    if(run_jobs_option->count()) options.parallel_jobs=run_parallel;
    options.on_event=print_job_event;
    auto result=app::run_import(path(run_plan),path(run_library),enqueue_only,options,signals.token());
    if(!result) {
      const auto& error=result.error();
      code=error.code==am::Errc::cancelled ? 3 : error.code==am::Errc::invalid_argument ? 2 : 4;
      if(json) std::cout<<Json{{"command","import run"},{"error",error_value(error)}}.dump()<<'\n';
      print_error(error,verbose);
      return;
    }
    render("import run",std::move(result));
  });
  std::vector<std::string> import_sources;
  std::string import_profile = "default", import_library, import_plan;
  bool import_hash = false, import_skip = false, import_single = false;
  import_scan->add_option("paths", import_sources)->required()->expected(-1);
  import_scan->add_option("--profile", import_profile, "Built-in profile ID or TOML file");
  auto* scan_library = import_scan->add_option("--library", import_library, "Match inputs against a read-only library");
  auto* scan_hash = import_scan->add_flag("--hash", import_hash, "Compute SHA-256 for source files");
  import_scan->add_flag("--skip-identical", import_skip, "Skip candidates with exact matches for every input object")
      ->needs(scan_hash);
  import_scan->add_flag("--single", import_single, "Combine remaining inputs; ready assets stay separate");
  import_scan->add_option("--plan", import_plan, "Publish a new import plan without replacing an existing file")
      ->required();
  presentation(import_scan);
  import_scan->callback([&] {
    JobSignals signals;
    auto run = [&]() -> am::Result<app::AssetOutcome> {
      auto profile = app::load_import_profile(import_profile);
      if (!profile) return profile.error();
      am::import::ScanRequest request;
      request.profile = std::move(*profile);
      for (const auto& source : import_sources) request.roots.push_back(path(source));
      request.hash_inputs = import_hash;
      request.skip_identical = import_skip;
      request.single_candidate = import_single;
      if (scan_library->count()) request.library = path(import_library);
      request.cancelled = [&] { return signals.token().stop_requested(); };
      std::cerr << app::message_text("am.import.scanning") << '\n';
      return app::scan_import(std::move(request), path(import_plan));
    };
    auto result = run();
    if (!result) {
      const auto& error = result.error();
      code = error.code == am::Errc::cancelled ? 3 : error.code == am::Errc::invalid_argument ? 2 : 4;
      if (json) std::cout << Json{{"command", "import scan"}, {"error", error_value(error)}}.dump() << '\n';
      print_error(error, verbose);
      return;
    }
    render("import scan", std::move(result));
  });
  auto* diag=cli.add_subcommand("diag","Export local diagnostics without personal data");diag->require_subcommand(1);
  auto* export_command=diag->add_subcommand("export","Export redacted diagnostics as a ZIP");
  std::string diagnostic_output,diagnostic_logs,diagnostic_library,diagnostic_since;
  export_command->add_option("out.zip",diagnostic_output)->required();
  auto* diagnostic_source=export_command->add_option("--log-dir",diagnostic_logs,"Source log directory");
  auto* diagnostic_lib=export_command->add_option("--library",diagnostic_library,"Include a read-only library summary");
  auto* diagnostic_time=export_command->add_option("--since",diagnostic_since,"RFC3339 lower timestamp boundary");
  export_command->add_flag("--dry-run",diagnostic_dry_run,"Show entries and replacement counts without writing files");
  presentation(export_command);
  export_command->callback([&]{
    app::DiagnosticOptions options;options.output=path(diagnostic_output);options.dry_run=diagnostic_dry_run;
    if(diagnostic_source->count())options.log_directory=path(diagnostic_logs);else if(explicit_log_directory && !log_directory.empty())options.log_directory=path(log_directory);
    if(diagnostic_lib->count())options.library=path(diagnostic_library);
    if(diagnostic_time->count())options.since=diagnostic_since;
#ifdef AM_DIAGNOSTIC_CLI_TEST_HOOK
    install_diagnostic_cli_test_hook(options);
#endif
    auto result=app::export_diagnostics(options);
    if(!result){code=result.error().code==am::Errc::invalid_argument?2:4;if(json)std::cout<<Json{{"command","diag export"},{"error",error_value(result.error())}}.dump()<<'\n';print_error(result.error(),verbose);return;}
    Json value{{"command","diag export"},{"entries",result->entries},{"logs_available",result->logs_available},{"dry_run",diagnostic_dry_run},{"replacements",{{"path",result->path_replacements},{"name",result->name_replacements},{"user",result->user_replacements},{"host",result->host_replacements}}}};
    if (json) {
      if (!result->unavailable.empty()) {
        value["unavailable"] = Json::array();
        for (const auto& source : result->unavailable)
          value["unavailable"].push_back(
              {{"source_kind", source.source_kind}, {"name", source.name}, {"reason", source.reason}});
      }
      std::cout << value.dump() << '\n';
    } else {
      std::cout << app::message_text("am.diag.exported") << ": " << value.dump() << '\n';
      if (!result->skipped_summary.empty()) std::cout << result->skipped_summary << '\n';
    }
    code=0;
  });
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
  std::string trash_root, restore_root, purge_root;
  std::vector<std::string> trash_ids, restore_ids, purge_ids;
  bool purge_all = false, purge_yes = false;
  auto* trash = library->add_subcommand("trash", "Move assets to the library trash without changing ZIP bytes");
  trash->add_option("dir", trash_root)->required();
  trash->add_option("asset-id", trash_ids)->required()->expected(-1);
  presentation(trash);
  trash->callback([&] { render("library trash", app::trash_library(path(trash_root), trash_ids)); });
  auto* restore = library->add_subcommand("restore", "Restore assets from the library trash");
  restore->add_option("dir", restore_root)->required();
  restore->add_option("asset-id", restore_ids)->required()->expected(-1);
  presentation(restore);
  restore->callback([&] { render("library restore", app::restore_library(path(restore_root), restore_ids)); });
  auto* purge = library->add_subcommand("purge", "Permanently delete assets from trash; requires --yes");
  purge->add_option("dir", purge_root)->required();
  auto* purge_selection = purge->add_option("asset-id", purge_ids)->expected(-1);
  purge->add_flag("--all", purge_all, "Select every correctly named regular ZIP in trash")->excludes(purge_selection);
  purge->add_flag("--yes", purge_yes, "Confirm permanent deletion");
  presentation(purge);
  purge->callback(
      [&] { render("library purge", app::purge_library(path(purge_root), purge_ids, purge_all, purge_yes)); });
  std::string add_ready_root, add_ready_replace;
  std::vector<std::string> add_ready_sources;
  auto* add_ready = library->add_subcommand("add-ready", "Import complete asset ZIPs without changing their bytes");
  add_ready->add_option("dir", add_ready_root)->required();
  add_ready->add_option("zip", add_ready_sources)->required()->expected(-1);
  auto* replace_ready =
      add_ready->add_option("--replace", add_ready_replace, "Expected current manifest SHA-256; exactly one source");
  presentation(add_ready);
  add_ready->callback([&] {
    std::vector<std::filesystem::path> sources;
    for (const auto& s : add_ready_sources) sources.push_back(path(s));
    render("library add-ready",
           app::add_ready_library(path(add_ready_root), sources,
                                  replace_ready->count() ? std::optional(add_ready_replace) : std::nullopt, verbose));
  });
  std::string copy_root, copy_id;
  std::string log_root, log_asset, log_job; std::int64_t log_after=0; int log_limit=100;
  auto* library_log=library->add_subcommand("log","Read the library operation log");
  library_log->add_option("dir",log_root)->required();
  auto* log_asset_option=library_log->add_option("--asset",log_asset);
  auto* log_job_option=library_log->add_option("--job",log_job);
  auto* log_after_option=library_log->add_option("--after",log_after);
  library_log->add_option("--limit",log_limit)->check(CLI::Range(1,1000));
  presentation(library_log);
  library_log->callback([&] {
    selected_command="library log"; am::db::LogQuery query; query.limit=static_cast<std::size_t>(log_limit);
    if(log_asset_option->count()) query.asset_id=log_asset;
    if(log_job_option->count()) query.job_id=log_job;
    if(log_after_option->count()) query.after_seq=log_after;
    auto result=app::library_log(path(log_root),query);
    if(!result) { render("library log",result.error()); return; }
    if(json) std::cout << result->json << '\n';
    else for(const auto& entry:result->entries) {
      std::cout << entry.seq << ' ' << entry.at << ' ' << app::message_text(entry.code) << '\n';
      if(verbose) std::cout << entry.message << ' ' << entry.detail_json.value_or("null") << '\n';
    }
    for(const auto& diagnostic:result->diagnostics) print_error(diagnostic,verbose);
  });
  std::string library_verify_root; bool library_verify_full = false, library_verify_no_trash = false, library_verify_enqueue = false;
  auto* library_verify = library->add_subcommand("verify", "Check library databases and ZIPs; record detected corruption in the index");
  library_verify->add_option("dir",library_verify_root)->required();
  library_verify->add_flag("--full",library_verify_full,"Read and verify every payload stream");
  library_verify->add_flag("--no-trash",library_verify_no_trash,"Exclude assets in trash");
  library_verify->add_flag("--enqueue",library_verify_enqueue,"Enqueue verification as a persistent background job");
  presentation(library_verify);
  library_verify->callback([&] { if(library_verify_enqueue)render_jobs("library verify",app::enqueue_verify(path(library_verify_root),library_verify_full,!library_verify_no_trash));else render("library verify",app::verify_library(path(library_verify_root),library_verify_full,!library_verify_no_trash,verbose)); });
  auto* copy = library->add_subcommand("copy", "Create an independent asset with new instance IDs");
  copy->add_option("dir", copy_root)->required();
  copy->add_option("asset-id", copy_id)->required();
  presentation(copy);
  copy->callback([&] { render("library copy", app::copy_library(path(copy_root),copy_id,verbose)); });
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
  std::function<void(CLI::App*)> inherit_options=[&](CLI::App* app){for(auto* child:app->get_subcommands([](CLI::App*){return true;})){child->fallthrough();inherit_options(child);}};
  inherit_options(&cli);
  try {
    cli.parse(argc, argv);
    return code;
  } catch (const CLI::ParseError& e) {
    code=cli.exit(e)==0?0:2;
    if(code){try{initialize_logging();auto error=am::Error::make(am::Errc::invalid_argument,"Invalid command arguments",e.what()).id("am.diag.arguments_invalid");AM_LOG(error,"cli","cli.error","Invalid command arguments",{},&error);}catch(...) {}}
    return code;
  } catch (const std::exception& e) {
    auto error = am::Error::make(am::Errc::internal, "Command failed", e.what()).id("am.app.internal");
    if (json) std::cout << Json{{"command", selected_command}, {"error", error_value(error)}}.dump() << '\n';
    print_error(error, verbose);
    return code=4;
  } catch (...) {
    std::cerr << app::message_text("am.app.internal") << '\n';
    return code=4;
  }
}
