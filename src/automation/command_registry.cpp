#include "am/automation/command_registry.hpp"

namespace am::automation {

Result<void> CommandRegistry::add(Command cmd) {
  if (cmd.id.empty() || !cmd.run) return Error::make(Errc::invalid_argument, "command needs id and handler");
  if (cmd.origin == CommandOrigin::plugin && cmd.id.rfind("x.", 0) != 0)
    return Error::make(Errc::invalid_argument, "plugin command ids must start with 'x.'", cmd.id);
  std::lock_guard lock(m_);
  auto it = commands_.find(cmd.id);
  if (it != commands_.end()) {
    return Error::make(Errc::conflict, "command already registered", cmd.id);
  }
  auto id = cmd.id;
  commands_.emplace(std::move(id), std::make_unique<Command>(std::move(cmd)));
  return {};
}

const Command* CommandRegistry::find(std::string_view id) const {
  std::lock_guard lock(m_);
  auto it = commands_.find(id);
  return it == commands_.end() ? nullptr : it->second.get();
}

std::vector<const Command*> CommandRegistry::for_context(CommandContext ctx) const {
  std::lock_guard lock(m_);
  std::vector<const Command*> out;
  for (const auto& [id, c] : commands_)
    if (contracts::any(c->contexts, ctx)) out.push_back(c.get());
  return out;
}

std::vector<const Command*> CommandRegistry::all() const {
  std::lock_guard lock(m_);
  std::vector<const Command*> out;
  out.reserve(commands_.size());
  for (const auto& [id, c] : commands_) out.push_back(c.get());
  return out;
}

Result<std::string> CommandRegistry::invoke(std::string_view id, const CommandInvocation& inv) const {
  return invoke_as(id, inv, CommandPermission::modify);
}

Result<std::string> CommandRegistry::invoke_as(std::string_view id, const CommandInvocation& inv,
                                               CommandPermission granted) const {
  const Command* c = find(id);
  if (!c) return Error::make(Errc::not_found, "unknown command", std::string(id));
  if (!contracts::allows(granted, c->permission))
    return Error::make(Errc::permission_denied, "command requires a higher permission level", c->id);
  if (c->idempotency == contracts::Idempotency::by_op_id && inv.op_id.empty())
    return Error::make(Errc::invalid_argument, "command requires op_id", c->id);
  return c->run(inv);
}

}  // namespace am::automation
