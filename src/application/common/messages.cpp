#include "am/application/asset_service.hpp"
#include "messages_resource.hpp"
#include <map>
#include <toml++/toml.hpp>
namespace am::application {
std::string_view message_text(std::string_view id) noexcept {
  static const auto messages = []() noexcept {
    std::map<std::string, std::string, std::less<>> out;
    try {
      std::string text;
      for (const auto part : message_parts) text += part;
      const auto parsed = toml::parse(text);
      for (const auto& [key, node] : parsed)
        if (auto value = node.value<std::string>()) out.emplace(std::string(key.str()), std::move(*value));
    } catch (...) {
      out.clear();
    }
    return out;
  }();
  auto found = messages.find(id);
  return found == messages.end() ? id : std::string_view(found->second);
}
std::string_view message_text(const Error& error) noexcept {
  const auto translated = message_text(error.message_id);
  if (translated != error.message_id) return translated;
  return error.message.empty() ? std::string_view(error.message_id) : std::string_view(error.message);
}
}  // namespace am::application
