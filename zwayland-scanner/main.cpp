#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Arg {
  std::string name;
  std::string type;
  std::string interface;
  bool nullable = false;
};
struct Message {
  std::string name;
  std::string type;
  std::uint32_t since = 1;
  std::vector<Arg> args;
};
struct Entry { std::string name; std::string value; std::uint32_t since = 1; };
struct Enum { std::string name; bool bitfield = false; std::vector<Entry> entries; };
struct Interface {
  std::string name;
  std::uint32_t version = 1;
  std::vector<Message> requests;
  std::vector<Message> events;
  std::vector<Enum> enums;
};
struct Protocol { std::string name; std::vector<Interface> interfaces; };

std::string signature_type(const Arg& arg);

std::string property(xmlNode* node, const char* name, std::string fallback = {}) {
  xmlChar* value = xmlGetProp(node, reinterpret_cast<const xmlChar*>(name));
  if (value == nullptr) return fallback;
  std::string result(reinterpret_cast<const char*>(value));
  xmlFree(value);
  return result;
}

std::uint32_t number(xmlNode* node, const char* name, std::uint32_t fallback = 1) {
  const std::string text = property(node, name);
  if (text.empty()) return fallback;
  char* end = nullptr;
  const auto value = std::strtoul(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || value > UINT32_MAX) throw std::runtime_error("invalid numeric XML attribute");
  return static_cast<std::uint32_t>(value);
}

bool element(xmlNode* node, std::string_view name) {
  return node->type == XML_ELEMENT_NODE && name == reinterpret_cast<const char*>(node->name);
}

Message parse_message(xmlNode* node) {
  Message result{property(node, "name"), property(node, "type"), number(node, "since"), {}};
  for (xmlNode* child = node->children; child != nullptr; child = child->next) if (element(child, "arg")) {
    Arg arg{property(child, "name"), property(child, "type"), property(child, "interface"),
            property(child, "allow-null") == "true"};
    if (arg.name.empty()) throw std::runtime_error("argument without a name");
    (void)signature_type(arg);
    if (arg.nullable && arg.type != "string" && arg.type != "object")
      throw std::runtime_error("allow-null is only valid for string and object arguments");
    result.args.push_back(std::move(arg));
  }
  if (std::count_if(result.args.begin(), result.args.end(), [](const Arg& arg) {
        return arg.type == "new_id";
      }) > 1)
    throw std::runtime_error("message has more than one new_id argument");
  if (result.name.empty()) throw std::runtime_error("message without a name");
  return result;
}

Protocol parse(const char* path) {
  xmlDoc* document = xmlReadFile(path, nullptr, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
  if (document == nullptr) throw std::runtime_error("could not parse protocol XML");
  Protocol result;
  xmlNode* root = xmlDocGetRootElement(document);
  if (root == nullptr || !element(root, "protocol")) { xmlFreeDoc(document); throw std::runtime_error("root must be protocol"); }
  result.name = property(root, "name");
  for (xmlNode* node = root->children; node != nullptr; node = node->next) if (element(node, "interface")) {
    Interface interface{property(node, "name"), number(node, "version"), {}, {}, {}};
    for (xmlNode* child = node->children; child != nullptr; child = child->next) {
      if (element(child, "request")) {
        Message message = parse_message(child);
        if (message.since > interface.version)
          throw std::runtime_error("request since version exceeds interface version");
        interface.requests.push_back(std::move(message));
      } else if (element(child, "event")) {
        Message message = parse_message(child);
        if (message.since > interface.version)
          throw std::runtime_error("event since version exceeds interface version");
        interface.events.push_back(std::move(message));
      }
      else if (element(child, "enum")) {
        Enum enumeration{property(child, "name"), property(child, "bitfield") == "true", {}};
        for (xmlNode* entry = child->children; entry != nullptr; entry = entry->next) if (element(entry, "entry"))
          enumeration.entries.push_back({property(entry, "name"), property(entry, "value"), number(entry, "since")});
        enumeration.name.empty() ? throw std::runtime_error("enum without a name") : void();
        interface.enums.push_back(std::move(enumeration));
      }
    }
    if (interface.name.empty()) { xmlFreeDoc(document); throw std::runtime_error("interface without a name"); }
    result.interfaces.push_back(std::move(interface));
  }
  xmlFreeDoc(document);
  if (result.name.empty() || result.interfaces.empty()) throw std::runtime_error("empty protocol");
  return result;
}

std::string upper(std::string value) {
  for (char& character : value) character = character == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  return value;
}

std::string opcode(const Interface& interface, const Message& message,
                   std::string_view direction) {
  return "ZWAYLAND_" + upper(interface.name + "_" + std::string(direction) +
                              "_" + message.name) + "_OPCODE";
}

std::string parameter(std::string value) {
  static const std::set<std::string> keywords{"alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "nullptr", "operator", "or", "private", "protected", "public", "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor"};
  if (keywords.contains(value)) value += '_';
  return value;
}

std::string signature_type(const Arg& arg) {
  const char code = arg.type == "int" ? 'i' : arg.type == "uint" ? 'u' : arg.type == "fixed" ? 'f' :
                    arg.type == "string" ? 's' : arg.type == "object" ? 'o' : arg.type == "new_id" ? 'n' :
                    arg.type == "array" ? 'a' : arg.type == "fd" ? 'h' : '\0';
  if (code == '\0') throw std::runtime_error("unsupported argument type " + arg.type);
  return std::string(arg.nullable ? "?" : "") + code;
}

void zwayland_enums(std::ostream& out, const Protocol& protocol) {
  for (const auto& interface : protocol.interfaces)
    for (const auto& enumeration : interface.enums)
      for (const auto& entry : enumeration.entries)
        out << "inline constexpr std::uint32_t "
            << upper(interface.name + "_" + enumeration.name + "_" + entry.name)
            << " = static_cast<std::uint32_t>(" << entry.value << ");\n";
}

const Arg* new_id(const Message& message) {
  const auto found = std::find_if(message.args.begin(), message.args.end(), [](const Arg& arg) { return arg.type == "new_id"; });
  return found == message.args.end() ? nullptr : &*found;
}

std::string clean_signature(const Message& message) {
  std::string result;
  for (const auto& arg : message.args)
    result += arg.type == "new_id" && arg.interface.empty()
                  ? "sun"
                  : signature_type(arg);
  return result;
}

// Reads an argument off a MessageParser into the given C++ variable name.
std::string read_expr(const Arg& arg, const std::string& var) {
  if (arg.type == "int") return var + " = p.read_int()";
  if (arg.type == "uint") return var + " = p.read_uint()";
  if (arg.type == "new_id") return var + " = p.read_new_id()";
  if (arg.type == "object") return var + " = p.read_object()";
  if (arg.type == "fixed") return var + " = p.read_fixed()";
  if (arg.type == "fd") return var + " = p.read_fd()";
  if (arg.type == "array") return var + " = p.read_array()";
  if (arg.type == "string")
    return arg.nullable
        ? var + " = p.is_null_string() ? ((void)p.read_string(), std::nullopt) : std::optional<std::string>(std::string(p.read_string()))"
        : var + " = std::string(p.read_string())";
  throw std::runtime_error("unsupported argument type " + arg.type);
}

// C++ value type used when reading an argument in a dispatch/observer binder.
std::string value_type(const Arg& arg) {
  if (arg.type == "int") return "std::int32_t";
  if (arg.type == "uint" || arg.type == "new_id" || arg.type == "object") return "std::uint32_t";
  if (arg.type == "fixed") return "double";
  if (arg.type == "string")
    return arg.nullable ? "std::optional<std::string>" : "std::string";
  if (arg.type == "array") return "std::span<const std::byte>";
  if (arg.type == "fd") return "int";
  throw std::runtime_error("unsupported argument type " + arg.type);
}

std::string server_value_type(const Arg& arg) {
  if (arg.type == "object" || arg.type == "new_id")
    return arg.type == "object" ? "zwayland::server::Resource*" : "std::uint32_t";
  return value_type(arg);
}

std::string server_event_type(const Arg& arg) {
  if (arg.type == "object" || arg.type == "new_id") return "zwayland::server::Resource*";
  if (arg.type == "string")
    return arg.nullable ? "wire::NullableString" : "std::string_view";
  return value_type(arg);
}

std::string client_event_type(const Arg& arg) {
  if (arg.type == "object" || arg.type == "new_id") return "zwayland::client::Proxy*";
  return value_type(arg);
}

// Emits one append_* call for a builder variable against a value expression.
void emit_append(std::ostream& out, const std::string& builder, const Arg& arg,
                 const std::string& value) {
  if (arg.type == "int") out << builder << ".append_int(" << value << ");\n";
  else if (arg.type == "uint") out << builder << ".append_uint(" << value << ");\n";
  else if (arg.type == "new_id") out << builder << ".append_new_id(" << value << ");\n";
  else if (arg.type == "object") out << builder << ".append_object(" << value << ");\n";
  else if (arg.type == "fixed") out << builder << ".append_fixed(" << value << ");\n";
  else if (arg.type == "string") {
    if (arg.nullable)
      out << "if (" << value << ") " << builder << ".append_string(*" << value
          << "); else " << builder << ".append_null_string();\n";
    else
      out << builder << ".append_string(" << value << ");\n";
  }
  else if (arg.type == "array") out << builder << ".append_array(" << value << ");\n";
  else if (arg.type == "fd") out << builder << ".add_fd(" << value << ");\n";
}

// C++ parameter type for client request/event code.
std::string client_param_type(const Arg& arg) {
  if (arg.type == "object") return "zwayland::client::Proxy*";
  if (arg.type == "new_id") return "zwayland::client::Proxy*";
  if (arg.type == "string")
    return arg.nullable ? "wire::NullableString" : "std::string_view";
  return value_type(arg);
}

// Returns a parameter name unique within `used`, inserting it for later calls.
std::string unique_name(std::string base, std::set<std::string>& used) {
  if (base.empty()) base = "arg";
  while (used.count(base)) base += '_';
  used.insert(base);
  return base;
}

void zwayland_server_header(std::ostream& out, const Protocol& protocol) {
  const std::string guard = "ZWAYLAND_GENERATED_" + upper(protocol.name) + "_SERVER_H";
  out << "/* Generated by zwayland-scanner (zwayland server metadata). */\n"
          "#ifndef " << guard << "\n#define " << guard
      << "\n#include <cstdint>\n#include <optional>\n#include <stdexcept>\n#include <string>\n#include <string_view>\n#include <span>\n#include <utility>\n"
         "#include <zwayland/server/display.hpp>\n#include <zwayland/wire/message.hpp>\n"
          "namespace zwayland { namespace generated {\n";
  zwayland_enums(out, protocol);
  for (const auto& interface : protocol.interfaces)
    out << "extern const server::Interface " << interface.name << "_interface;\n";
  for (const auto& interface : protocol.interfaces) {
    if (!interface.requests.empty()) {
      out << "\ninline const server::Message " << interface.name << "_requests[] = {";
      for (const auto& request : interface.requests)
        out << "\n  {\"" << request.name << "\", \"" << clean_signature(request) << "\", "
            << request.since << "},";
      out << "\n};\n";
    } else {
      out << "\ninline const server::Message* " << interface.name << "_requests = nullptr;\n";
    }

    if (!interface.events.empty()) {
      out << "inline const server::Message " << interface.name << "_events[] = {";
      for (const auto& event : interface.events)
        out << "\n  {\"" << event.name << "\", \"" << clean_signature(event) << "\", "
            << event.since << "},";
      out << "\n};\n";
    } else {
      out << "inline const server::Message* " << interface.name << "_events = nullptr;\n";
    }

    out << "inline const server::Interface " << interface.name << "_interface{\n"
        << "  \"" << interface.name << "\", " << interface.version << ",\n"
        << "  " << interface.name << "_requests, " << interface.requests.size() << ",\n"
        << "  " << interface.name << "_events, " << interface.events.size() << "};\n";

    for (std::size_t i = 0; i < interface.requests.size(); ++i)
      out << "constexpr std::uint32_t " << opcode(interface, interface.requests[i], "request")
          << " = " << i << ";\n";
    for (std::size_t i = 0; i < interface.events.size(); ++i)
      out << "constexpr std::uint32_t " << opcode(interface, interface.events[i], "event")
          << " = " << i << ";\n";

    // Typed event senders (compositor -> client).
    for (const auto& event : interface.events) {
      std::set<std::string> used{"id", "client"};
      out << "inline void " << interface.name << "_send_" << event.name
          << "(server::Resource& resource";
      std::vector<std::string> names;
      for (const auto& arg : event.args) {
        const std::string n = unique_name(parameter(arg.name), used);
        names.push_back(n);
        out << ", " << server_event_type(arg) << ' ' << n;
      }
      out << ") {\n  if (resource.version < " << event.since << ") return;\n"
          << "  wire::MessageBuilder b(resource.id); b.set_opcode("
          << opcode(interface, event, "event") << ");\n";
      for (std::size_t i = 0; i < event.args.size(); ++i) {
        if (event.args[i].type == "object" || event.args[i].type == "new_id") {
          if (!event.args[i].nullable)
            out << "  if (" << names[i] << " == nullptr) throw std::invalid_argument(\"event requires an object\");\n";
          out << "  if (" << names[i] << " != nullptr && " << names[i]
              << "->client != resource.client) throw std::invalid_argument(\"event object belongs to another client\");\n";
          out << "b.append_" << (event.args[i].type == "object" ? "object" : "new_id")
              << "(" << names[i] << " == nullptr ? 0 : " << names[i] << "->id);\n";
        } else {
          emit_append(out, "b", event.args[i], names[i]);
        }
      }
      out << "  resource.client->send_event(resource.id, " << opcode(interface, event, "event")
          << ", b.finish(), b.take_fds());\n}\n";
    }

    // Typed request handler: maps opcodes to handler.<request>(client, resource, args...).
    out << "template <typename H>\ninline server::RequestHandler " << interface.name
        << "_handler(H&& handler) {\n"
        << "  return [handler = std::forward<H>(handler)](server::Client& client, server::Resource& resource,\n"
        << "                                     std::uint32_t opcode, wire::MessageParser& p) mutable {\n"
        << "    (void)p; (void)client; (void)resource;\n"
        << "    switch (opcode) {\n";
    for (const auto& request : interface.requests) {
      out << "      case " << opcode(interface, request, "request") << ": {\n";
      std::set<std::string> used{"client", "resource", "opcode", "p"};
      std::vector<std::string> call_values;
      for (const auto& arg : request.args) {
        const std::string var = unique_name(parameter(arg.name), used);
        if (arg.type == "object") {
          out << "        const std::uint32_t " << var << "_id = p.read_object();\n"
              << "        server::Resource* " << var << " = " << var << "_id == 0 ? nullptr : client.find_resource("
              << var << "_id);\n";
          if (!arg.nullable)
            out << "        if (" << var << " == nullptr) throw std::runtime_error(\"invalid object argument\");\n";
          if (!arg.interface.empty())
            out << "        if (" << var << " != nullptr && std::string_view(" << var
                << "->interface->name) != \"" << arg.interface
                << "\") throw std::runtime_error(\"object has wrong interface\");\n";
        } else if (arg.type == "new_id") {
          if (arg.interface.empty()) {
            out << "        if (p.is_null_string()) throw std::runtime_error(\"generic new_id has no interface name\");\n"
                << "        std::string " << var << "_interface(p.read_string());\n"
                << "        std::uint32_t " << var << "_version = p.read_uint();\n";
            call_values.push_back(var + "_interface");
            call_values.push_back(var + "_version");
          }
          out << "        std::uint32_t " << var << " = p.read_new_id();\n"
              << "        if (!client.valid_new_id(" << var
              << ")) throw std::runtime_error(\"invalid new object id\");\n";
        } else if (arg.type == "fd") {
          out << "        wire::FileDescriptor " << var << "(p.read_fd());\n";
        } else if (arg.type == "string" && !arg.nullable) {
          out << "        if (p.is_null_string()) throw std::runtime_error(\"null string in non-nullable argument\");\n"
              << "        std::string " << var << "; " << read_expr(arg, var) << ";\n";
        } else {
          out << "        " << server_value_type(arg) << ' ' << var << "; " << read_expr(arg, var) << ";\n";
        }
        call_values.push_back(arg.type == "fd" ? var + ".release()" : var);
      }
      out << "        handler." << request.name << "(client, resource";
      for (const auto& value : call_values) out << ", " << value;
      out << ");\n        break;\n      }\n";
    }
    out << "    }\n  };\n}\n";
  }
  out << "} }  // namespace zwayland::generated\n#endif /* " << guard << " */\n";
}

void zwayland_client_header(std::ostream& out, const Protocol& protocol) {
  const std::string guard = "ZWAYLAND_GENERATED_" + upper(protocol.name) + "_CLIENT_H";
  out << "/* Generated by zwayland-scanner (zwayland client metadata). */\n"
         "#ifndef " << guard << "\n#define " << guard
      << "\n#include <cstdint>\n#include <optional>\n#include <stdexcept>\n#include <string>\n#include <string_view>\n#include <span>\n#include <utility>\n"
         "#include <zwayland/client/display.hpp>\n#include <zwayland/wire/message.hpp>\n"
          "namespace zwayland { namespace generated {\n";
  zwayland_enums(out, protocol);
  {
    std::set<std::string> declared;
    for (const auto& interface : protocol.interfaces) {
      out << "extern const client::Interface " << interface.name << "_interface;\n";
      declared.insert(interface.name);
    }
    for (const auto& interface : protocol.interfaces)
      for (const auto* messages : {&interface.requests, &interface.events})
        for (const auto& message : *messages)
          if (const Arg* created = new_id(message);
              created != nullptr && !created->interface.empty() &&
              declared.insert(created->interface).second)
            out << "extern const client::Interface " << created->interface << "_interface;\n";
  }
  for (const auto& interface : protocol.interfaces) {
    if (!interface.requests.empty()) {
      out << "\ninline const client::Message " << interface.name << "_requests[] = {";
      for (const auto& request : interface.requests)
        out << "\n  {\"" << request.name << "\", \"" << clean_signature(request) << "\", "
            << request.since << "},";
      out << "\n};\n";
    } else {
      out << "\ninline const client::Message* " << interface.name << "_requests = nullptr;\n";
    }

    if (!interface.events.empty()) {
      out << "inline const client::Message " << interface.name << "_events[] = {";
      for (const auto& event : interface.events)
        out << "\n  {\"" << event.name << "\", \"" << clean_signature(event) << "\", "
            << event.since << "},";
      out << "\n};\n";
    } else {
      out << "inline const client::Message* " << interface.name << "_events = nullptr;\n";
    }

    out << "inline const client::Interface " << interface.name << "_interface{\n"
        << "  \"" << interface.name << "\", " << interface.version << ",\n"
        << "  " << interface.name << "_requests, " << interface.requests.size() << ",\n"
        << "  " << interface.name << "_events, " << interface.events.size() << "};\n";

    for (std::size_t i = 0; i < interface.requests.size(); ++i)
      out << "constexpr std::uint32_t " << opcode(interface, interface.requests[i], "request")
          << " = " << i << ";\n";
    for (std::size_t i = 0; i < interface.events.size(); ++i)
      out << "constexpr std::uint32_t " << opcode(interface, interface.events[i], "event")
          << " = " << i << ";\n";

    // Typed request senders (client -> compositor).
    for (const auto& request : interface.requests) {
      const Arg* created = new_id(request);
      std::set<std::string> used{"id", "display", "new_id", "new_interface"};
      out << "inline " << (created ? "std::uint32_t" : "void") << ' ' << interface.name << '_' << request.name
          << "(client::Display& display, std::uint32_t id";
      if (created != nullptr && created->interface.empty())
        out << ", const client::Interface& new_interface, std::uint32_t new_version";
      std::vector<std::string> names;
      for (const auto& arg : request.args) {
        if (&arg == created) continue;
        const std::string n = unique_name(parameter(arg.name), used);
        names.push_back(n);
        out << ", " << client_param_type(arg) << ' ' << n;
      }
      out << ") {\n";
      std::size_t validation_index = 0;
      for (const auto& arg : request.args) {
        if (&arg == created) continue;
        if (arg.type == "object") {
          if (!arg.nullable)
            out << "  if (" << names[validation_index]
                << " == nullptr) throw std::invalid_argument(\"request requires an object\");\n";
          out << "  if (" << names[validation_index] << " != nullptr && "
              << names[validation_index]
              << "->display != &display) throw std::invalid_argument(\"request object belongs to another display\");\n";
        }
        ++validation_index;
      }
      if (created != nullptr && created->interface.empty())
        out << "  if (new_version == 0 || new_version > new_interface.version) throw std::invalid_argument(\"invalid generic new_id version\");\n";
      out << "  wire::MessageBuilder b(id); b.set_opcode("
          << opcode(interface, request, "request") << ");\n";
      if (created != nullptr) {
        out << "  client::Proxy* sender = display.find_proxy(id);\n"
            << "  if (sender == nullptr) throw std::invalid_argument(\"constructor targets an invalid proxy\");\n"
            << "  std::uint32_t new_id = display.alloc_id();\n"
            << "  if (new_id == 0) throw std::runtime_error(\"could not allocate new proxy id\");\n";
      }
      std::size_t k = 0;
      for (const auto& arg : request.args) {
        if (&arg == created) {
          if (created->interface.empty()) {
            out << "b.append_string(new_interface.name);\n"
                << "b.append_uint(new_version);\n";
          }
          emit_append(out, "b", arg, "new_id");
        } else {
          const std::string value = arg.type == "object"
                                        ? (arg.nullable ? names[k] + " == nullptr ? 0 : " + names[k] + "->id"
                                                        : names[k] + "->id")
                                        : names[k];
          emit_append(out, "b", arg, value);
          ++k;
        }
      }
      if (created != nullptr) {
        out << "  if (display.create_proxy(";
        if (created->interface.empty()) out << "&new_interface";
        else out << "&" << created->interface << "_interface";
        out << ", new_id";
        if (created->interface.empty()) out << ", new_version";
        else out << ", sender->version";
        out << ") == nullptr) throw std::runtime_error(\"could not allocate new proxy\");\n"
            << "  try {\n"
            << "    display.send_request(id, " << opcode(interface, request, "request")
            << ", b.finish(), b.take_fds());\n"
            << "  } catch (...) {\n"
            << "    display.discard_proxy(new_id);\n"
            << "    throw;\n"
            << "  }\n";
      } else {
        out << "  display.send_request(id, " << opcode(interface, request, "request")
            << ", b.finish(), b.take_fds());\n";
      }
      if (request.type == "destructor") out << "  display.destroy_proxy(id);\n";
      if (created != nullptr) out << "  return new_id;\n";
      out << "}\n";
    }

    // Typed event observer factory.
    out << "template <typename H>\ninline client::EventObserver " << interface.name
        << "_observer(H&& handler) {\n"
        << "  return [handler = std::forward<H>(handler)](client::Proxy& proxy, std::uint32_t opcode,\n"
        << "                                     wire::MessageParser& p) mutable {\n"
        << "    (void)p; (void)proxy;\n"
        << "    switch (opcode) {\n";
    for (const auto& event : interface.events) {
      out << "      case " << opcode(interface, event, "event") << ": {\n";
      std::set<std::string> used{"proxy", "opcode", "p"};
      std::vector<std::string> names;
      for (const auto& arg : event.args) {
        const std::string var = unique_name(parameter(arg.name), used);
        names.push_back(var);
        if (arg.type == "object" || arg.type == "new_id") {
          out << "        const std::uint32_t " << var << "_id = p.read_"
              << (arg.type == "object" ? "object" : "new_id") << "();\n"
              << "        client::Proxy* " << var << " = " << var << "_id == 0 ? nullptr : proxy.display->find_proxy("
              << var << "_id);\n";
          if (arg.type == "new_id" && !arg.interface.empty())
            out << "        if (" << var << "_id == 0 || " << var
                << " != nullptr) throw std::runtime_error(\"invalid new object id in event\");\n"
                << "        " << var << " = proxy.display->create_proxy(&" << arg.interface
                << "_interface, " << var << "_id, proxy.version);\n";
          if (!arg.nullable)
            out << "        if (" << var << " == nullptr) throw std::runtime_error(\"event references an invalid object\");\n";
          if (arg.type == "object" && !arg.interface.empty())
            out << "        if (" << var << " != nullptr && std::string_view(" << var
                << "->interface->name) != \"" << arg.interface
                << "\") throw std::runtime_error(\"event object has wrong interface\");\n";
        } else if (arg.type == "fd") {
          out << "        wire::FileDescriptor " << var << "(p.read_fd());\n";
        } else if (arg.type == "string" && !arg.nullable) {
          out << "        if (p.is_null_string()) throw std::runtime_error(\"null string in non-nullable event\");\n"
              << "        std::string " << var << "; " << read_expr(arg, var) << ";\n";
        } else {
          out << "        " << client_event_type(arg) << ' ' << var << "; " << read_expr(arg, var) << ";\n";
        }
      }
      out << "        handler." << event.name << "(proxy";
      for (std::size_t i = 0; i < names.size(); ++i)
        out << ", " << names[i] << (event.args[i].type == "fd" ? ".release()" : "");
      out << ");\n        break;\n      }\n";
    }
    out << "    }\n  };\n}\n"
        << "template <typename H>\ninline void " << interface.name
        << "_observe(client::Display& display, H&& handler) {\n"
        << "  display.set_observer(&" << interface.name << "_interface, "
        << interface.name << "_observer(std::forward<H>(handler)));\n}\n";
  }
  out << "} }  // namespace zwayland::generated\n#endif /* " << guard << " */\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: zwayland-scanner "
                 "{zwayland-server-header|zwayland-client-header} INPUT.xml OUTPUT\n";
    return EXIT_FAILURE;
  }
  try {
    const Protocol protocol = parse(argv[2]);
    std::ofstream output(argv[3]);
    if (!output) throw std::runtime_error("could not open output");
    const std::string_view mode(argv[1]);
    if (mode == "zwayland-server-header") zwayland_server_header(output, protocol);
    else if (mode == "zwayland-client-header") zwayland_client_header(output, protocol);
    else throw std::runtime_error("unknown generation mode");
  } catch (const std::exception& exception) {
    std::cerr << "zwayland-scanner: " << exception.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
