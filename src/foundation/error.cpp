#include "am/foundation/result.hpp"

namespace am {

std::string_view errc_name(Errc c) noexcept {
  switch (c) {
    case Errc::ok: return "am.ok";
    case Errc::invalid_argument: return "am.invalid_argument";
    case Errc::not_found: return "am.not_found";
    case Errc::io_error: return "am.io_error";
    case Errc::corrupt_data: return "am.corrupt_data";
    case Errc::unsupported: return "am.unsupported";
    case Errc::conflict: return "am.conflict";
    case Errc::cancelled: return "am.cancelled";
    case Errc::needs_decision: return "am.needs_decision";
    case Errc::permission_denied: return "am.permission_denied";
    case Errc::internal: return "am.internal";
    case Errc::resource_exhausted: return "am.resource_exhausted";
    case Errc::insufficient_space: return "am.insufficient_space";
    case Errc::read_only: return "am.read_only";
    case Errc::locked: return "am.locked";
    case Errc::incomplete: return "am.incomplete";
  }
  return "am.unknown";
}

std::string to_log_string(const Error& e) {
  std::string out;
  for (const Error* cur = &e; cur; cur = cur->cause.get()) {
    if (cur != &e) out += " <- ";
    out += errc_name(cur->code);
    if (!cur->component.empty()) { out += " ["; out += cur->component; out += ']'; }
    if (!cur->ctx.operation.empty()) { out += ' '; out += cur->ctx.operation; }
    out += ": ";
    out += cur->message;
    if (!cur->ctx.detail.empty()) { out += " ("; out += cur->ctx.detail; out += ')'; }
  }
  return out;
}

}  // namespace am
