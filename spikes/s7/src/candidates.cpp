#include "candidates.hpp"
#include "reference.hpp"
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>
#include <rapidjson/reader.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <nlohmann/json-schema.hpp>

namespace s7 {
using Ordered = nlohmann::ordered_json;
Ordered history_entry(std::uint64_t revision) {
  return {{"revision", revision}, {"operation_id", "12345678-1234-4123-8123-000000000004"},
          {"timestamp", "2026-11-11T00:00:00Z"}, {"operation", "metadata_changed"},
          {"changes", Ordered::array({{{"kind", "metadata_set"}, {"target", "asset"}}})}};
}
void edit_dom(Ordered& value, bool structural) {
  value["asset"]["metadata"]["title"] = "S7 edited title";
  if (!structural) return;
  auto& values = value.at("asset").at("metadata").at("custom_values");
  if (values.size() < 2 || values[0].at("field_id").get<std::string>().rfind("user:", 0) != 0 ||
      values[1].at("field_id").get<std::string>().rfind("plugin:", 0) != 0)
    throw std::runtime_error("structural fixture lacks core value before plugin value");
  values.erase(values.begin());
  const auto revision = value["asset"]["revision"].get<std::uint64_t>() + 1;
  value["asset"]["revision"] = revision;
  value["asset"]["modified_at"] = "2026-11-11T00:00:00Z";
  value["history"]["entries"].push_back(history_entry(revision));
}
std::string write_hybrid(const Ordered& value, const std::vector<std::string>& fragments) {
  std::string output;
  auto write = [&](auto&& self, const Ordered& node) -> void {
    if (node.is_binary()) {
      const auto& bytes = node.get_binary();
      if (bytes.size() != sizeof(std::uint64_t)) throw std::runtime_error("invalid fragment handle");
      std::uint64_t id = 0;
      for (std::size_t i = 0; i < bytes.size(); ++i) id |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
      if (id >= fragments.size()) throw std::runtime_error("unknown fragment handle");
      const auto& fragment = fragments[static_cast<std::size_t>(id)];
      const Document checked(fragment);
      if (checked.duplicates) throw std::runtime_error("duplicate fragment keys");
      output += fragment;
    } else if (node.is_object()) {
      output += '{';
      bool comma = false;
      for (auto it = node.begin(); it != node.end(); ++it) {
        if (comma) output += ',';
        comma = true;
        output += Ordered(it.key()).dump(); output += ':';
        self(self, it.value());
      }
      output += '}';
    } else if (node.is_array()) {
      output += '[';
      bool comma = false;
      for (const auto& item : node) {
        if (comma) output += ',';
        comma = true;
        self(self, item);
      }
      output += ']';
    } else output += node.dump();
  };
  write(write, value);
  return output;
}
namespace {
using Json = nlohmann::ordered_json;
struct Dom final : Candidate {
  Json value;
  void parse(std::string_view text) override { value = Json::parse(text); }
  std::string serialize(bool structural) override { edit_dom(value, structural); return value.dump(); }
  void validate_native(std::string_view) override {}
};
enum class EventKind { object, end_object, array, end_array, key, string, number, boolean, null };
struct Event { EventKind kind; std::string text; };
struct EventTree {
  std::vector<Event> events;
  void add(EventKind kind, std::string text = {}) { events.push_back({kind, std::move(text)}); }
  std::string serialize(bool rapid) const {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    std::string own;
    struct Frame { bool object; std::size_t count; };
    std::vector<Frame> stack;
    auto prefix = [&] { if (!stack.empty() && !stack.back().object && stack.back().count++) own += ','; };
    for (const auto& e : events) {
      if (rapid) {
        switch (e.kind) {
          case EventKind::object: writer.StartObject(); break;
          case EventKind::end_object: writer.EndObject(); break;
          case EventKind::array: writer.StartArray(); break;
          case EventKind::end_array: writer.EndArray(); break;
          case EventKind::key: writer.Key(e.text.data(), static_cast<rapidjson::SizeType>(e.text.size())); break;
          case EventKind::string: writer.String(e.text.data(), static_cast<rapidjson::SizeType>(e.text.size())); break;
          case EventKind::number: {
            const auto raw = checked_number(e.text);
            writer.RawValue(raw.data(), raw.size(), rapidjson::kNumberType); break;
          }
          case EventKind::boolean: writer.Bool(e.text == "true"); break;
          case EventKind::null: writer.Null(); break;
        }
      } else {
        switch (e.kind) {
          case EventKind::object: prefix(); own += '{'; stack.push_back({true, 0}); break;
          case EventKind::array: prefix(); own += '['; stack.push_back({false, 0}); break;
          case EventKind::end_object: own += '}'; stack.pop_back(); break;
          case EventKind::end_array: own += ']'; stack.pop_back(); break;
          case EventKind::key:
            if (stack.back().count++) own += ',';
            own += Json(e.text).dump() + ':'; break;
          case EventKind::string: prefix(); own += Json(e.text).dump(); break;
          case EventKind::number: prefix(); own += checked_number(e.text); break;
          case EventKind::boolean: prefix(); own += e.text; break;
          case EventKind::null: prefix(); own += "null"; break;
        }
      }
    }
    return rapid ? std::string(buffer.GetString(), buffer.GetSize()) : own;
  }
};
struct Sax : nlohmann::json_sax<Json> {
  EventTree tree;
  std::string error;
  bool null() override { tree.add(EventKind::null); return true; }
  bool boolean(bool v) override { tree.add(EventKind::boolean, v ? "true" : "false"); return true; }
  bool number_integer(number_integer_t v) override { tree.add(EventKind::number, std::to_string(v)); return true; }
  bool number_unsigned(number_unsigned_t v) override { tree.add(EventKind::number, std::to_string(v)); return true; }
  bool number_float(number_float_t, const string_t& raw) override { tree.add(EventKind::number, raw); return true; }
  bool string(string_t& s) override { tree.add(EventKind::string, s); return true; }
  bool binary(binary_t&) override { return false; }
  bool start_object(std::size_t) override { tree.add(EventKind::object); return true; }
  bool key(string_t& s) override { tree.add(EventKind::key, s); return true; }
  bool end_object() override { tree.add(EventKind::end_object); return true; }
  bool start_array(std::size_t) override { tree.add(EventKind::array); return true; }
  bool end_array() override { tree.add(EventKind::end_array); return true; }
  bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception& e) override { error = e.what(); return false; }
};
struct SaxCandidate final : Candidate {
  Sax sax;
  void parse(std::string_view text) override {
    if (!Json::sax_parse(text, &sax)) throw std::runtime_error(sax.error);
  }
  std::string serialize(bool structural) override {
    const auto text = sax.tree.serialize(false);
    return Document(text).edit_known(structural);
  }
  void validate_native(std::string_view) override {}
};
struct RapidHandler : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, RapidHandler> {
  EventTree tree;
  bool Null() { tree.add(EventKind::null); return true; }
  bool Bool(bool v) { tree.add(EventKind::boolean, v ? "true" : "false"); return true; }
  bool RawNumber(const char* p, rapidjson::SizeType n, bool) { tree.add(EventKind::number, std::string(p, n)); return true; }
  bool String(const char* p, rapidjson::SizeType n, bool) { tree.add(EventKind::string, std::string(p, n)); return true; }
  bool Key(const char* p, rapidjson::SizeType n, bool) { tree.add(EventKind::key, std::string(p, n)); return true; }
  bool StartObject() { tree.add(EventKind::object); return true; }
  bool EndObject(rapidjson::SizeType) { tree.add(EventKind::end_object); return true; }
  bool StartArray() { tree.add(EventKind::array); return true; }
  bool EndArray(rapidjson::SizeType) { tree.add(EventKind::end_array); return true; }
};
struct RapidCandidate final : Candidate {
  RapidHandler handler;
  void parse(std::string_view text) override {
    rapidjson::MemoryStream input(text.data(), text.size());
    rapidjson::Reader reader;
    const auto result = reader.Parse<rapidjson::kParseNumbersAsStringsFlag | rapidjson::kParseIterativeFlag |
                                     rapidjson::kParseValidateEncodingFlag>(input, handler);
    if (!result) throw std::runtime_error("RapidJSON parse error " + std::to_string(result.Code()) + " at " + std::to_string(result.Offset()));
  }
  std::string serialize(bool structural) override {
    const auto text = handler.tree.serialize(true);
    return Document(text).edit_known(structural);
  }
  void validate_native(std::string_view) override {}
};
struct RawCandidate final : Candidate {
  std::string text;
  void parse(std::string_view input) override {
    Document checked(input);
    if (checked.duplicates) throw std::runtime_error("D1 duplicate key rejected");
    text = input;
  }
  std::string serialize(bool structural) override { return Document(text).edit_known(structural); }
  void validate_native(std::string_view) override {}
};
struct HybridCandidate final : Candidate {
  Json value;
  std::vector<std::string> fragments;
  void parse(std::string_view input) override {
    const Document checked(input);
    if (checked.duplicates) throw std::runtime_error("D2 duplicate key rejected");
    value = Json::parse(checked.shadow());
    fragments.clear();
    for (const auto root : checked.opaque_roots()) {
      const auto& token = checked.tokens[root];
      const auto id = static_cast<std::uint64_t>(fragments.size());
      fragments.emplace_back(input.substr(token.begin, token.end - token.begin));
      std::vector<std::uint8_t> bytes(sizeof(id));
      for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(id >> (8 * i));
      // JSON has no binary value: user objects/strings cannot forge this handle.
      value[Json::json_pointer(checked.path(root))] = Json::binary(std::move(bytes));
    }
  }
  std::string serialize(bool structural) override { edit_dom(value, structural); return write_hybrid(value, fragments); }
  void validate_native(std::string_view) override {}
};
}  // namespace
std::unique_ptr<Candidate> candidate(std::string_view id) {
  if (id == "A") return std::make_unique<Dom>();
  if (id == "B") return std::make_unique<SaxCandidate>();
  if (id == "C") return std::make_unique<RapidCandidate>();
  if (id == "D1") return std::make_unique<RawCandidate>();
  if (id == "D2") return std::make_unique<HybridCandidate>();
  if (id == "E") return jsoncons_candidate();
  throw std::runtime_error("unknown candidate");
}
nlohmann::ordered_json validator_probe(std::string_view text, std::string_view schema) {
  nlohmann::ordered_json result;
  try {
    nlohmann::json_schema::json_validator validator(nullptr, nlohmann::json_schema::default_string_format_check);
    validator.set_root_schema(nlohmann::json::parse(schema));
    result["schema_loaded"] = true;
    validator.validate(nlohmann::json::parse(text));
    result["accepted"] = true;
  } catch (const std::exception& e) { result["accepted"] = false; result["error"] = e.what(); }
  return result;
}
}  // namespace s7
