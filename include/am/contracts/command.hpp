#pragma once
// Command contract (ТЗ §12.4, §11.3.3; ADR-0024, ADR-0027).
//
// Lives in the low `contracts` level so that services and plugins can declare
// commands without depending on the automation layer or the application facade
// (ТЗ §12.4.4: no cycles between services, application and automation).

#include <functional>
#include <string>

#include "am/foundation/result.hpp"

namespace am::contracts {

enum class CommandContext : unsigned {
  none = 0,
  library = 1u << 0,
  asset = 1u << 1,
  file = 1u << 2,
  selection = 1u << 3,   // several assets
  match_result = 1u << 4,
  global = 1u << 5,
};
constexpr CommandContext operator|(CommandContext a, CommandContext b) {
  return static_cast<CommandContext>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr bool any(CommandContext set, CommandContext c) {
  return (static_cast<unsigned>(set) & static_cast<unsigned>(c)) != 0;
}

// Three permission levels (ТЗ §11.1.3). Ordered: a grant of `prepare`
// also allows `read`; `modify` allows everything.
enum class CommandPermission : unsigned char {
  read = 0,     // search/analysis, no writes
  prepare = 1,  // proposals, plans, jobs that do not change data yet
  modify = 2,   // changes assets/library data
};
constexpr bool allows(CommandPermission granted, CommandPermission required) {
  return static_cast<unsigned char>(granted) >= static_cast<unsigned char>(required);
}

// Idempotency is declared per operation, never assumed (ТЗ §11.3.6).
enum class Idempotency : unsigned char {
  none,      // re-running may duplicate results
  by_op_id,  // re-running with the same op_id is a no-op (ADR-0015)
  natural,   // repeated execution yields the same state
};

enum class CommandOrigin { builtin, plugin };

struct CommandInvocation {
  std::string args_json = "{}";
  std::string op_id;  // optional; required for Idempotency::by_op_id
};

struct Command {
  std::string id;                 // "am.asset.export" | "x.<plugin>.<name>"
  std::string title;              // UI text (translated by GUI)
  std::string group;              // menu path, e.g. "Asset/Export"
  CommandContext contexts = CommandContext::global;
  std::string default_shortcut;   // e.g. "Ctrl+E"
  CommandPermission permission = CommandPermission::read;
  bool dangerous = false;         // needs explicit confirmation even when granted
  bool scriptable = true;         // usable from scenarios (ТЗ §11.3.3: UI-only commands may opt out)
  Idempotency idempotency = Idempotency::none;
  CommandOrigin origin = CommandOrigin::builtin;
  std::function<Result<std::string>(const CommandInvocation&)> run;  // returns JSON
};

// Narrow registration interface handed to services and plugins.
class ICommandSink {
 public:
  virtual ~ICommandSink() = default;
  virtual Result<void> add(Command cmd) = 0;
};

}  // namespace am::contracts
