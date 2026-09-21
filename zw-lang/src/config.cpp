#include "zwwm/lang/config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <charconv>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace zwwm::lang {
namespace {

class Parser {
 public:
  explicit Parser(std::string_view input) : input_(input) {}

  ParseResult parse() {
    Config config;
    skip_ignored();
    while (!at_end()) {
      const SourceLocation location = location_;
      const auto key = parse_identifier();
      if (!key.has_value()) {
        recover();
        skip_ignored();
        continue;
      }
      if (!consume('=')) {
        error("expected '=' after assignment name");
        recover();
        skip_ignored();
        continue;
      }
      const auto value = parse_value_list();
      if (!value.has_value()) {
        recover();
        skip_ignored();
        continue;
      }
      config.assignments.push_back({location, *key, *value});
      skip_ignored();
    }
    return {std::move(config), std::move(diagnostics_)};
  }

 private:
  [[nodiscard]] bool at_end() const { return position_ >= input_.size(); }

  [[nodiscard]] char peek() const { return at_end() ? '\0' : input_[position_]; }

  char take() {
    const char character = peek();
    if (!at_end()) {
      ++position_;
      if (character == '\n') {
        ++location_.line;
        location_.column = 1;
      } else {
        ++location_.column;
      }
    }
    return character;
  }

  void skip_ignored() {
    while (!at_end()) {
      if (std::isspace(static_cast<unsigned char>(peek()))) {
        take();
        continue;
      }
      if (peek() == '-' && position_ + 1 < input_.size() && input_[position_ + 1] == '-') {
        while (!at_end() && take() != '\n') {
        }
        continue;
      }
      break;
    }
  }

  bool consume(char expected) {
    skip_ignored();
    if (peek() != expected) {
      return false;
    }
    take();
    skip_ignored();
    return true;
  }

  [[nodiscard]] bool is_identifier_start(char character) const {
    return std::isalpha(static_cast<unsigned char>(character)) || character == '_';
  }

  [[nodiscard]] bool is_identifier_character(char character) const {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_' || character == '-';
  }

  std::optional<std::string> parse_identifier() {
    skip_ignored();
    if (!is_identifier_start(peek())) {
      error("expected identifier");
      return std::nullopt;
    }
    std::string identifier;
    while (is_identifier_character(peek())) {
      identifier += take();
    }
    skip_ignored();
    return identifier;
  }

  std::optional<Value> parse_value_list() {
    Value::List list;
    if (peek() == ',') {
      list.values.emplace_back(std::string{});
    } else {
      const auto first = parse_value();
      if (!first.has_value()) return std::nullopt;
      list.values.push_back(*first);
    }
    while (consume(',')) {
      const auto next = parse_value();
      if (!next.has_value()) {
        return std::nullopt;
      }
      list.values.push_back(*next);
    }
    if (list.values.size() == 1) {
      return list.values.front();
    }
    return Value{std::move(list)};
  }

  std::optional<Value> parse_value() {
    skip_ignored();
    if (peek() == '"') {
      return parse_string();
    }
    if (peek() == '$') {
      take();
      const auto name = parse_identifier();
      if (!name.has_value()) {
        return std::nullopt;
      }
      return Value{VariableReference{*name}};
    }
    if (peek() == '[') {
      return parse_array();
    }
    if (peek() == '{') {
      return parse_object();
    }
    if (std::isdigit(static_cast<unsigned char>(peek())) ||
        (peek() == '-' && position_ + 1 < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_ + 1])))) {
      return parse_integer();
    }
    const auto identifier = parse_identifier();
    if (!identifier.has_value()) {
      return std::nullopt;
    }
    if (*identifier == "true") {
      return Value{true};
    }
    if (*identifier == "false") {
      return Value{false};
    }
    if (*identifier == "null") {
      return Value{std::monostate{}};
    }
    return Value{*identifier};
  }

  std::optional<Value> parse_string() {
    take();
    std::string value;
    while (!at_end() && peek() != '"') {
      if (peek() == '\\') {
        take();
        if (at_end()) {
          break;
        }
        const char escaped = take();
        if (escaped == 'n') {
          value += '\n';
        } else {
          value += escaped;
        }
      } else {
        value += take();
      }
    }
    if (at_end()) {
      error("unterminated string");
      return std::nullopt;
    }
    take();
    skip_ignored();
    return Value{std::move(value)};
  }

  std::optional<Value> parse_integer() {
    const bool negative = peek() == '-';
    if (negative) {
      take();
    }
    std::int64_t value = 0;
    while (std::isdigit(static_cast<unsigned char>(peek()))) {
      value = value * 10 + (take() - '0');
    }
    skip_ignored();
    return Value{negative ? -value : value};
  }

  std::optional<Value> parse_array() {
    take();
    skip_ignored();
    Value::Array values;
    if (consume(']')) {
      return Value{std::move(values)};
    }
    while (true) {
      const auto value = parse_value();
      if (!value.has_value()) {
        return std::nullopt;
      }
      values.values.push_back(*value);
      if (consume(']')) {
        return Value{std::move(values)};
      }
      if (!consume(',')) {
        error("expected ',' or ']' in array");
        return std::nullopt;
      }
      if (consume(']')) {
        return Value{std::move(values)};
      }
    }
  }

  std::optional<Value> parse_object() {
    take();
    skip_ignored();
    Value::Object fields;
    while (!consume('}')) {
      if (at_end()) {
        error("unterminated object");
        return std::nullopt;
      }
      const auto key = parse_identifier();
      if (!key.has_value()) {
        return std::nullopt;
      }
      if (!consume('=')) {
        error("expected '=' after object field name");
        return std::nullopt;
      }
      const auto value = parse_value_list();
      if (!value.has_value()) {
        return std::nullopt;
      }
      fields.add(*key, *value);
    }
    return Value{std::move(fields)};
  }

  void error(std::string message) { diagnostics_.push_back({DiagnosticLevel::error, location_, std::move(message)}); }

  void recover() {
    while (!at_end() && take() != '\n') {
    }
  }

  std::string_view input_;
  std::size_t position_ = 0;
  SourceLocation location_;
  std::vector<Diagnostic> diagnostics_;
};

bool has_errors(const std::vector<Diagnostic>& diagnostics) {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
    return diagnostic.level == DiagnosticLevel::error;
  });
}

const Value* lookup_field(const Value::Object& object, std::string_view name) {
  const auto entry = std::find(object.names.begin(), object.names.end(), name);
  if (entry == object.names.end()) return nullptr;
  return &object.values[static_cast<std::size_t>(entry - object.names.begin())];
}

bool is_bool(const Value& value) { return std::holds_alternative<bool>(value.data); }
bool is_object(const Value& value) { return std::holds_alternative<Value::Object>(value.data); }

bool is_nonnegative_integer(const Value& value) {
  const auto* integer = std::get_if<std::int64_t>(&value.data);
  return integer != nullptr && *integer >= 0 && *integer <= std::numeric_limits<std::int32_t>::max();
}

bool is_output_mode(const Value& value) {
  const auto* text = std::get_if<std::string>(&value.data);
  if (text == nullptr) return false;
  if (*text == "preferred") return true;
  const auto x = text->find('x');
  const auto at = text->find('@', x == std::string::npos ? 0 : x + 1);
  if (x == std::string::npos || at == std::string::npos || x == 0 || at <= x + 1 || at + 1 == text->size()) return false;
  std::uint32_t width = 0, height = 0;
  double refresh = 0.0;
  const auto width_result = std::from_chars(text->data(), text->data() + x, width);
  const auto height_result = std::from_chars(text->data() + x + 1, text->data() + at, height);
  const auto refresh_result = std::from_chars(text->data() + at + 1, text->data() + text->size(), refresh);
  return width_result.ec == std::errc{} && width_result.ptr == text->data() + x && width > 0 &&
         height_result.ec == std::errc{} && height_result.ptr == text->data() + at && height > 0 &&
         refresh_result.ec == std::errc{} && refresh_result.ptr == text->data() + text->size() && refresh > 0.0;
}

void warn_unknown_fields(const Value::Object& object, std::span<const std::string_view> known,
                         std::vector<Diagnostic>& diagnostics, SourceLocation location,
                         std::string_view object_name) {
  for (const auto& name : object.names) {
    if (std::find(known.begin(), known.end(), name) == known.end()) {
      diagnostics.push_back({DiagnosticLevel::warning, location,
                             "unknown " + std::string(object_name) + " setting: " + name});
    }
  }
}

bool is_uppercase_name(const std::string& name) {
  return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char character) {
    return std::isupper(character) || std::isdigit(character) || character == '_';
  });
}

void check_variables(const Value& value, const std::unordered_set<std::string>& variables,
                     std::vector<Diagnostic>& diagnostics, SourceLocation location) {
  std::visit(
      [&](const auto& item) {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, VariableReference>) {
          if (!variables.contains(item.name)) {
            diagnostics.push_back({DiagnosticLevel::error, location, "undefined variable $" + item.name});
          }
        } else if constexpr (std::is_same_v<Item, Value::Array> || std::is_same_v<Item, Value::List>) {
          for (const Value& nested : item.values) {
            check_variables(nested, variables, diagnostics, location);
          }
        } else if constexpr (std::is_same_v<Item, Value::Object>) {
          for (const auto& nested : item.values) {
            check_variables(nested, variables, diagnostics, location);
          }
        }
      },
      value.data);
}

void validation_error(std::vector<Diagnostic>& diagnostics, SourceLocation location, std::string message) {
  diagnostics.push_back({DiagnosticLevel::error, location, std::move(message)});
}

std::optional<std::string> expand_environment(std::string_view value, std::string& error) {
  std::string expanded;
  for (std::size_t index = 0; index < value.size();) {
    if (value[index] != '$') {
      expanded += value[index++];
      continue;
    }

    const std::size_t marker = index++;
    std::string name;
    if (index < value.size() && value[index] == '{') {
      const auto end = value.find('}', ++index);
      if (end == std::string_view::npos || end == index) {
        error = "invalid environment variable in import path";
        return std::nullopt;
      }
      name = value.substr(index, end - index);
      index = end + 1;
    } else {
      const std::size_t begin = index;
      while (index < value.size() &&
             (std::isalnum(static_cast<unsigned char>(value[index])) || value[index] == '_')) ++index;
      if (begin == index) {
        expanded += value[marker];
        continue;
      }
      name = value.substr(begin, index - begin);
    }

    const char* environment = std::getenv(name.c_str());
    if (environment == nullptr) {
      error = "undefined environment variable $" + name + " in import path";
      return std::nullopt;
    }
    expanded += environment;
  }
  return expanded;
}

std::optional<std::string> import_path(const Value& value, std::string& error) {
  if (const auto* text = std::get_if<std::string>(&value.data)) return expand_environment(*text, error);
  if (const auto* reference = std::get_if<VariableReference>(&value.data)) {
    const char* environment = std::getenv(reference->name.c_str());
    if (environment != nullptr) return std::string(environment);
    error = "undefined environment variable $" + reference->name + " in import path";
    return std::nullopt;
  }
  error = "import paths must be strings or environment variable references";
  return std::nullopt;
}

ParseResult parse_config_file_recursive(const std::filesystem::path& requested,
                                        std::vector<std::filesystem::path>& stack,
                                        SourceLocation location = {}) {
  std::error_code error_code;
  auto path = std::filesystem::weakly_canonical(requested, error_code);
  if (error_code) path = std::filesystem::absolute(requested).lexically_normal();
  if (std::find(stack.begin(), stack.end(), path) != stack.end()) {
    return {{}, {{DiagnosticLevel::error, location, "configuration import cycle at " + path.string()}}};
  }

  std::ifstream file(path);
  if (!file) {
    return {{}, {{DiagnosticLevel::error, location, "could not open configuration file: " + path.string()}}};
  }
  const std::string input{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  auto parsed = Parser(input).parse();
  if (!parsed.ok()) return parsed;

  stack.push_back(path);
  Config merged;
  for (auto& assignment : parsed.config.assignments) {
    if (assignment.key != "import") {
      merged.assignments.push_back(std::move(assignment));
      continue;
    }

    std::vector<const Value*> values;
    if (const auto* array = std::get_if<Value::Array>(&assignment.value.data)) {
      for (const auto& value : array->values) values.push_back(&value);
    } else {
      values.push_back(&assignment.value);
    }
    for (const Value* value : values) {
      std::string import_error;
      const auto imported_path = import_path(*value, import_error);
      if (!imported_path) {
        parsed.diagnostics.push_back({DiagnosticLevel::error, assignment.location, std::move(import_error)});
        continue;
      }
      std::filesystem::path child(*imported_path);
      if (child.is_relative()) child = path.parent_path() / child;
      auto imported = parse_config_file_recursive(child, stack, assignment.location);
      parsed.diagnostics.insert(parsed.diagnostics.end(),
                                std::make_move_iterator(imported.diagnostics.begin()),
                                std::make_move_iterator(imported.diagnostics.end()));
      merged.assignments.insert(merged.assignments.end(),
                                std::make_move_iterator(imported.config.assignments.begin()),
                                std::make_move_iterator(imported.config.assignments.end()));
    }
  }
  stack.pop_back();
  parsed.config = std::move(merged);
  return parsed;
}

}  // namespace

bool ParseResult::ok() const { return !has_errors(diagnostics); }
bool ValidationResult::ok() const { return !has_errors(diagnostics); }

ValidationContext::ValidationContext(std::vector<Diagnostic>& diagnostics) : diagnostics_(diagnostics) {}

void ValidationContext::warning(SourceLocation location, std::string message) {
  diagnostics_.push_back({DiagnosticLevel::warning, location, std::move(message)});
}

void ValidationContext::error(SourceLocation location, std::string message) {
  diagnostics_.push_back({DiagnosticLevel::error, location, std::move(message)});
}

ParseResult parse_config(const std::string& input) { return Parser(input).parse(); }

ParseResult parse_config_file(const std::string& path) {
  std::vector<std::filesystem::path> stack;
  return parse_config_file_recursive(path, stack);
}

ValidationResult validate_config(const Config& config) { return validate_config(config, {}); }

ValidationResult validate_config(const Config& config, std::span<const ValidationRule> rules) {
  ValidationResult result;
  std::unordered_set<std::string> variables;
  for (const Assignment& assignment : config.assignments) {
    if (is_uppercase_name(assignment.key)) {
      variables.insert(assignment.key);
    }
  }

  ValidationContext context(result.diagnostics);
  const auto validate_output = [&](const Value::Object& object, SourceLocation location,
                                   std::string_view prefix) {
    if (const Value* value = lookup_field(object, "mode"); value != nullptr && !is_output_mode(*value))
      validation_error(result.diagnostics, location,
                       std::string(prefix) + "mode must be \"preferred\" or WIDTHxHEIGHT@HZ");
    if (const Value* value = lookup_field(object, "scale-per-mille"); value != nullptr) {
      const auto* scale = std::get_if<std::int64_t>(&value->data);
      if (scale == nullptr || *scale < 250 || *scale > 8000)
        validation_error(result.diagnostics, location,
                         std::string(prefix) + "scale-per-mille must be an integer from 250 to 8000");
    }
    if (const Value* value = lookup_field(object, "bit-depth"); value != nullptr) {
      const auto* depth = std::get_if<std::int64_t>(&value->data);
      if (depth == nullptr || (*depth != 8 && *depth != 10))
        validation_error(result.diagnostics, location,
                         std::string(prefix) + "bit-depth must be 8 or 10");
    }
    if (const Value* value = lookup_field(object, "transform"); value != nullptr) {
      const auto* transform = std::get_if<std::string>(&value->data);
      if (transform == nullptr || (*transform != "normal" && *transform != "90" &&
                                   *transform != "180" && *transform != "270"))
        validation_error(result.diagnostics, location,
                         std::string(prefix) +
                             "transform must be \"normal\", \"90\", \"180\", or \"270\"");
    }
    constexpr std::array<std::string_view, 4> fields = {
        "mode", "scale-per-mille", "bit-depth", "transform"};
    warn_unknown_fields(object, fields, result.diagnostics, location,
                        prefix.substr(0, prefix.size() - 1));
  };
  for (const Assignment& assignment : config.assignments) {
    if (assignment.key != "import") {
      check_variables(assignment.value, variables, result.diagnostics, assignment.location);
    }
    for (const ValidationRule& rule : rules) {
      rule(assignment, context);
    }
    if (is_uppercase_name(assignment.key)) {
      continue;
    }
    if (assignment.key == "import") {
      const auto valid_path = [](const Value& value) {
        return std::holds_alternative<std::string>(value.data) ||
               std::holds_alternative<VariableReference>(value.data);
      };
      if (const auto* array = std::get_if<Value::Array>(&assignment.value.data)) {
        if (!std::all_of(array->values.begin(), array->values.end(), valid_path)) {
          validation_error(result.diagnostics, assignment.location,
                           "import paths must be strings or environment variable references");
        }
      } else if (!valid_path(assignment.value)) {
        validation_error(result.diagnostics, assignment.location,
                         "import must be a path or an array of paths");
      }
      continue;
    }
    if (assignment.key == "exec-sh-on-startup") {
      if (!std::holds_alternative<Value::Array>(assignment.value.data)) {
        validation_error(result.diagnostics, assignment.location, "exec-sh-on-startup must be an array");
      }
      continue;
    }
    if (assignment.key == "bind") {
      const auto* list = std::get_if<Value::List>(&assignment.value.data);
      if (list == nullptr || list->values.size() != 4) {
        validation_error(result.diagnostics, assignment.location, "bind must contain modifier, key, action, and argument");
      }
      continue;
    }
    if (assignment.key == "shaders") {
      const auto* object = std::get_if<Value::Object>(&assignment.value.data);
      if (object == nullptr) {
        validation_error(result.diagnostics, assignment.location, "shaders must be an object");
        continue;
      }
      for (const auto& value : object->values) {
        if (!is_object(value))
          validation_error(result.diagnostics, assignment.location, "each shader must be an object");
      }
      continue;
    }
    if (assignment.key == "layout") {
      const auto* object = std::get_if<Value::Object>(&assignment.value.data);
      if (object == nullptr) {
        validation_error(result.diagnostics, assignment.location, "layout must be an object");
        continue;
      }
      if (const Value* value = lookup_field(*object, "default"); value != nullptr) {
        const auto* name = std::get_if<std::string>(&value->data);
        if (name == nullptr || (*name != "master-stack" && *name != "focus-fibonacci" &&
                                *name != "endless-canvas"))
          validation_error(result.diagnostics, assignment.location,
                            "layout.default must be \"master-stack\", \"focus-fibonacci\", or \"endless-canvas\"");
      }
      if (const Value* value = lookup_field(*object, "master-count"); value != nullptr) {
        const auto* count = std::get_if<std::int64_t>(&value->data);
        if (count == nullptr || *count <= 0 || *count > std::numeric_limits<std::int32_t>::max()) {
          validation_error(result.diagnostics, assignment.location, "layout.master-count must be a positive integer");
        }
      }
      if (const Value* value = lookup_field(*object, "master-ratio"); value != nullptr) {
        const auto* ratio = std::get_if<std::int64_t>(&value->data);
        if (ratio == nullptr || *ratio < 10 || *ratio > 90) {
          validation_error(result.diagnostics, assignment.location,
                           "layout.master-ratio must be an integer percent from 10 to 90");
        }
      }
      for (const char* name : {"outer-gap", "inner-gap", "gaps", "min-zoom-per-mille",
                               "max-zoom-per-mille"}) {
        if (const Value* value = lookup_field(*object, name); value != nullptr && !is_nonnegative_integer(*value)) {
          validation_error(result.diagnostics, assignment.location,
                           std::string("layout.") + name + " must be a nonnegative integer");
        }
      }
      if (const Value* value = lookup_field(*object, "smart-gaps"); value != nullptr && !is_bool(*value)) {
        validation_error(result.diagnostics, assignment.location, "layout.smart-gaps must be a boolean");
      }
      constexpr std::array<std::string_view, 9> fields = {
          "default", "master-count", "master-ratio", "outer-gap", "inner-gap", "gaps",
          "min-zoom-per-mille", "max-zoom-per-mille", "smart-gaps"};
      warn_unknown_fields(*object, fields, result.diagnostics, assignment.location, "layout");
      continue;
    }
    if (assignment.key == "input") {
      const auto* object = std::get_if<Value::Object>(&assignment.value.data);
      if (object == nullptr) {
        validation_error(result.diagnostics, assignment.location, "input must be an object");
        continue;
      }
      if (const Value* value = lookup_field(*object, "focus-mode"); value != nullptr) {
        const auto* mode = std::get_if<std::string>(&value->data);
        if (mode == nullptr || (*mode != "hover" && *mode != "by-click" && *mode != "click"))
          validation_error(result.diagnostics, assignment.location,
                           "input.focus-mode must be \"hover\" or \"by-click\"");
      }
      if (const Value* value = lookup_field(*object, "reverse-mouse-scrolling");
          value != nullptr && !is_bool(*value))
        validation_error(result.diagnostics, assignment.location,
                         "input.reverse-mouse-scrolling must be a boolean");
      constexpr std::array<std::string_view, 2> input_fields = {
          "focus-mode", "reverse-mouse-scrolling"};
      warn_unknown_fields(*object, input_fields, result.diagnostics, assignment.location, "input");
      continue;
    }
    if (assignment.key == "keyboard") {
      const auto* object = std::get_if<Value::Object>(&assignment.value.data);
      if (object == nullptr) {
        validation_error(result.diagnostics, assignment.location, "keyboard must be an object");
        continue;
      }
      for (const char* name : {"rules", "model", "layout", "variant", "options"}) {
        if (const Value* value = lookup_field(*object, name);
            value != nullptr && !std::holds_alternative<std::string>(value->data)) {
          validation_error(result.diagnostics, assignment.location,
                           std::string("keyboard.") + name + " must be a string");
        }
      }
      constexpr std::array<std::string_view, 5> keyboard_fields = {
          "rules", "model", "layout", "variant", "options"};
      warn_unknown_fields(*object, keyboard_fields, result.diagnostics, assignment.location, "keyboard");
      continue;
    }
    if (assignment.key == "animations") {
      const auto* object = std::get_if<Value::Object>(&assignment.value.data);
      if (object == nullptr) {
        validation_error(result.diagnostics, assignment.location, "animations must be an object");
        continue;
      }
      for (const char* name : {"enabled", "open-window", "resize", "close"}) {
        if (const Value* value = lookup_field(*object, name); value != nullptr && !is_bool(*value)) {
          validation_error(result.diagnostics, assignment.location,
                           std::string("animations.") + name + " must be a boolean");
        }
      }
      if (const Value* value = lookup_field(*object, "duration-ms"); value != nullptr && !is_nonnegative_integer(*value)) {
        validation_error(result.diagnostics, assignment.location,
                         "animations.duration-ms must be a nonnegative integer");
      }
      const auto ranged_animation = [&](const char* name, std::int64_t minimum, std::int64_t maximum) {
        const Value* value = lookup_field(*object, name);
        if (value == nullptr) return;
        const auto* integer = std::get_if<std::int64_t>(&value->data);
        if (integer == nullptr || *integer < minimum || *integer > maximum) {
          validation_error(result.diagnostics, assignment.location,
                           std::string("animations.") + name + " must be an integer from " +
                               std::to_string(minimum) + " to " + std::to_string(maximum));
        }
      };
      ranged_animation("open-scale-per-mille", 500, 1000);
      ranged_animation("close-scale-per-mille", 500, 1000);
      ranged_animation("open-offset-px", 0, 4096);
      ranged_animation("close-offset-px", 0, 4096);
      ranged_animation("tag-scale-per-mille", 500, 1000);
      ranged_animation("tag-parallax-per-mille", 0, 1000);
      ranged_animation("tag-fade-per-mille", 0, 1000);
      ranged_animation("tag-duration-ms", 0, 10000);
      if (const Value* value = lookup_field(*object, "spring"); value != nullptr) {
        const auto* spring = std::get_if<Value::Object>(&value->data);
        if (spring == nullptr) {
          validation_error(result.diagnostics, assignment.location, "animations.spring must be an object");
        } else {
          for (const char* name : {"stiffness", "damping", "mass-per-mille"}) {
            const Value* parameter = lookup_field(*spring, name);
            const auto* integer = parameter == nullptr ? nullptr : std::get_if<std::int64_t>(&parameter->data);
            const bool invalid = integer == nullptr || *integer < (std::string_view(name) == "damping" ? 0 : 1) ||
                                 *integer > std::numeric_limits<std::int32_t>::max();
            if (parameter != nullptr && invalid) {
              validation_error(result.diagnostics, assignment.location,
                               std::string("animations.spring.") + name +
                                   (std::string_view(name) == "damping" ? " must be a nonnegative integer"
                                                                        : " must be a positive integer"));
            }
          }
          constexpr std::array<std::string_view, 3> spring_fields = {"stiffness", "damping", "mass-per-mille"};
          warn_unknown_fields(*spring, spring_fields, result.diagnostics, assignment.location, "animations.spring");
        }
      }
      if (const Value* value = lookup_field(*object, "cubic-bezier"); value != nullptr) {
        const auto* bezier = std::get_if<Value::Object>(&value->data);
        if (bezier == nullptr) {
          validation_error(result.diagnostics, assignment.location, "animations.cubic-bezier must be an object");
        } else {
          for (const char* name : {"x1", "y1", "x2", "y2"}) {
            const Value* point = lookup_field(*bezier, name);
            const auto* integer = point == nullptr ? nullptr : std::get_if<std::int64_t>(&point->data);
            if (point != nullptr && (integer == nullptr || *integer < 0 || *integer > 1000)) {
              validation_error(result.diagnostics, assignment.location,
                               std::string("animations.cubic-bezier.") + name + " must be an integer from 0 to 1000");
            }
          }
          constexpr std::array<std::string_view, 4> bezier_fields = {"x1", "y1", "x2", "y2"};
          warn_unknown_fields(*bezier, bezier_fields, result.diagnostics, assignment.location,
                              "animations.cubic-bezier");
        }
      }
      constexpr std::array<std::string_view, 15> animation_fields = {
          "enabled", "duration-ms", "tag-duration-ms", "spring", "cubic-bezier", "open-window",
          "resize", "close", "open-scale-per-mille", "close-scale-per-mille", "open-offset-px",
          "close-offset-px", "tag-scale-per-mille", "tag-parallax-per-mille", "tag-fade-per-mille"};
      warn_unknown_fields(*object, animation_fields, result.diagnostics, assignment.location, "animations");
      continue;
    }
    if (assignment.key == "output") {
      const auto* object = std::get_if<Value::Object>(&assignment.value.data);
      if (object == nullptr) {
        validation_error(result.diagnostics, assignment.location, "output must be an object");
        continue;
      }
      validate_output(*object, assignment.location, "output.");
      continue;
    }
    if (assignment.key == "outputs") {
      const auto* object = std::get_if<Value::Object>(&assignment.value.data);
      if (object == nullptr) {
        validation_error(result.diagnostics, assignment.location, "outputs must be an object");
        continue;
      }
      for (std::size_t index = 0; index < object->names.size(); ++index) {
        const auto* configured = std::get_if<Value::Object>(&object->values[index].data);
        if (configured == nullptr) {
          validation_error(result.diagnostics, assignment.location,
                           "outputs." + object->names[index] + " must be an object");
          continue;
        }
        validate_output(*configured, assignment.location,
                        "outputs." + object->names[index] + ".");
      }
      continue;
    }
    if (assignment.key == "environment") {
      const auto* object = std::get_if<Value::Object>(&assignment.value.data);
      if (object == nullptr) {
        validation_error(result.diagnostics, assignment.location, "environment must be an object");
        continue;
      }
      for (std::size_t index = 0; index < object->names.size(); ++index) {
        const auto& name = object->names[index];
        const auto& value = object->values[index];
        const bool valid_name = !name.empty() && (std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_') &&
            std::all_of(std::next(name.begin()), name.end(), [](unsigned char character) {
              return std::isalnum(character) || character == '_';
            });
        if (!valid_name) validation_error(result.diagnostics, assignment.location, "environment variable names must be shell identifiers");
        if (!std::holds_alternative<std::string>(value.data) && !std::holds_alternative<VariableReference>(value.data))
          validation_error(result.diagnostics, assignment.location, "environment values must be strings or variable references");
      }
      continue;
    }
    if (assignment.key == "rule") {
      const auto* rule = std::get_if<Value::Object>(&assignment.value.data);
      if (rule == nullptr) {
        validation_error(result.diagnostics, assignment.location, "rule must be an object");
        continue;
      }
      const Value* match = lookup_field(*rule, "match");
      const Value* set = lookup_field(*rule, "set");
      if (match == nullptr || !is_object(*match)) {
        validation_error(result.diagnostics, assignment.location, "rule.match must be an object");
      }
      if (set == nullptr || !is_object(*set)) {
        validation_error(result.diagnostics, assignment.location, "rule.set must be an object");
        continue;
      }
      const auto& actions = std::get<Value::Object>(set->data);
      for (const char* name : {"floating", "centered", "always-on-top", "glass"}) {
        if (const Value* value = lookup_field(actions, name); value != nullptr && !is_bool(*value)) {
          validation_error(result.diagnostics, assignment.location, std::string("rule.set.") + name + " must be a boolean");
        }
      }
      continue;
    }
    result.diagnostics.push_back({DiagnosticLevel::warning, assignment.location,
                                  "unknown top-level setting: " + assignment.key});
  }
  return result;
}

const Value* find_assignment(const Config& config, std::string_view key) {
  const auto assignment = std::find_if(config.assignments.begin(), config.assignments.end(), [key](const Assignment& item) {
    return item.key == key;
  });
  return assignment == config.assignments.end() ? nullptr : &assignment->value;
}

const Value* find_field(const Value::Object& object, std::string_view key) { return lookup_field(object, key); }

const std::string* as_string(const Value& value) { return std::get_if<std::string>(&value.data); }
const bool* as_boolean(const Value& value) { return std::get_if<bool>(&value.data); }
const std::int64_t* as_integer(const Value& value) { return std::get_if<std::int64_t>(&value.data); }
const Value::Array* as_array(const Value& value) { return std::get_if<Value::Array>(&value.data); }
const Value::List* as_list(const Value& value) { return std::get_if<Value::List>(&value.data); }
const Value::Object* as_object(const Value& value) { return std::get_if<Value::Object>(&value.data); }

}  // namespace zwwm::lang
