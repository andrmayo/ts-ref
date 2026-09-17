#include "ts_ref/grammar_loader.h"

#include <sys/wait.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "nlohmann/json.hpp"

#include "ts_ref/grammar_ir.h"

namespace ts_ref {
namespace {

// Rule order is load-bearing -- InputGrammar::variables[0] is the start rule,
// and `externals` order sets external-scanner priority -- so this must be the
// insertion-order-preserving flavor. Plain nlohmann::json is backed by
// std::map and would silently sort the rule table alphabetically.
using Json = nlohmann::ordered_json;

// Propagates a non-ok StatusOr, otherwise binds its value. The two-step
// concatenation is what forces __LINE__ to expand before being pasted, so that
// several uses in one scope get distinct temporaries.
#define TS_REF_CONCAT_INNER(a, b) a##b
#define TS_REF_CONCAT(a, b) TS_REF_CONCAT_INNER(a, b)
#define TS_REF_ASSIGN_OR_RETURN_IMPL(temp, lhs, rexpr) \
  auto temp = (rexpr);                                 \
  if (!temp.ok()) return temp.status();                \
  lhs = std::move(temp).value()
#define TS_REF_ASSIGN_OR_RETURN(lhs, rexpr)                                 \
  TS_REF_ASSIGN_OR_RETURN_IMPL(TS_REF_CONCAT(toy_statusor_, __LINE__), lhs, \
                               rexpr)

absl::Status TypeError(std::string_view path, std::string_view expected,
                       const Json& value) {
  return absl::InvalidArgumentError(
      absl::StrCat(path, ": expected ", expected, ", got ", value.type_name()));
}

absl::StatusOr<const Json*> GetMember(const Json& object, std::string_view path,
                                      std::string_view key) {
  // ordered_json's key type is std::string, which a string_view will not
  // convert to implicitly.
  auto iter = object.find(std::string(key));
  if (iter == object.end()) {
    return absl::InvalidArgumentError(
        absl::StrCat(path, ": missing required field '", key, "'"));
  }
  return &*iter;
}

absl::StatusOr<std::string> GetString(const Json& object, std::string_view path,
                                      std::string_view key) {
  TS_REF_ASSIGN_OR_RETURN(const Json* member, GetMember(object, path, key));
  if (!member->is_string()) {
    return TypeError(absl::StrCat(path, ".", key), "a string", *member);
  }
  return member->get<std::string>();
}

// Reads an optional string field. Absent, null, and JS `undefined` (which
// JSON.stringify drops entirely) all map to nullopt.
std::optional<std::string> GetOptionalString(const Json& object,
                                             std::string_view key) {
  auto iter = object.find(std::string(key));
  if (iter == object.end() || !iter->is_string()) return std::nullopt;
  return iter->get<std::string>();
}

absl::StatusOr<Rule> DeserializeRule(const Json& value, std::string_view path);

absl::StatusOr<std::vector<Rule>> DeserializeRuleArray(const Json& value,
                                                       std::string_view path) {
  if (!value.is_array()) return TypeError(path, "an array of rules", value);
  std::vector<Rule> rules;
  rules.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    TS_REF_ASSIGN_OR_RETURN(
        Rule rule, DeserializeRule(value[i], absl::StrCat(path, "[", i, "]")));
    rules.push_back(std::move(rule));
  }
  return rules;
}

// The `content` field carried by every single-child rule tag.
absl::StatusOr<Rule> DeserializeContent(const Json& value,
                                        std::string_view path) {
  TS_REF_ASSIGN_OR_RETURN(const Json* content,
                          GetMember(value, path, "content"));
  return DeserializeRule(*content, absl::StrCat(path, ".content"));
}

// prec(...) and friends accept either a number or the name of a level declared
// in the grammar's `precedences` list.
absl::StatusOr<Precedence> DeserializePrecedence(const Json& value,
                                                 std::string_view path) {
  TS_REF_ASSIGN_OR_RETURN(const Json* member, GetMember(value, path, "value"));
  if (member->is_string()) return Precedence(member->get<std::string>());
  if (member->is_number_integer()) {
    return Precedence(member->get<std::int32_t>());
  }
  return TypeError(absl::StrCat(path, ".value"), "a precedence number or name",
                   *member);
}

absl::StatusOr<Rule> DeserializeRule(const Json& value, std::string_view path) {
  if (!value.is_object()) return TypeError(path, "a rule object", value);
  TS_REF_ASSIGN_OR_RETURN(std::string type, GetString(value, path, "type"));

  if (type == "BLANK") return Rule(Blank{});

  if (type == "STRING") {
    TS_REF_ASSIGN_OR_RETURN(std::string literal,
                            GetString(value, path, "value"));
    return Rule(StringLiteral{std::move(literal)});
  }

  if (type == "PATTERN") {
    TS_REF_ASSIGN_OR_RETURN(std::string pattern,
                            GetString(value, path, "value"));
    // dsl.js omits `flags` entirely for a regex with none set
    std::string flags = GetOptionalString(value, "flags").value_or("");
    return Rule(Pattern{std::move(pattern), std::move(flags)});
  }

  if (type == "SYMBOL") {
    TS_REF_ASSIGN_OR_RETURN(std::string name, GetString(value, path, "name"));
    return Rule(NamedSymbolRef{std::move(name)});
  }

  if (type == "CHOICE" || type == "SEQ") {
    TS_REF_ASSIGN_OR_RETURN(const Json* members,
                            GetMember(value, path, "members"));
    TS_REF_ASSIGN_OR_RETURN(
        std::vector<Rule> rules,
        DeserializeRuleArray(*members, absl::StrCat(path, ".members")));
    // Choice's constructor flattens nested choices; Seq keeps its shape
    if (type == "CHOICE") return Rule(Choice(std::move(rules)));
    return Rule(Seq{std::move(rules)});
  }

  if (type == "REPEAT1") {
    // The IR's Repeat node is one-or-more (see grammar_ir.h), so REPEAT1 maps
    // onto it directly.
    TS_REF_ASSIGN_OR_RETURN(Rule content, DeserializeContent(value, path));
    return Rule(Repeat(std::move(content)));
  }

  if (type == "REPEAT") {
    // Zero-or-more is optional(one-or-more). Desugaring here rather than in
    // the IR keeps a single repetition node for expand_repeats to rewrite.
    TS_REF_ASSIGN_OR_RETURN(Rule content, DeserializeContent(value, path));
    std::vector<Rule> alternatives;
    alternatives.push_back(Rule(Repeat(std::move(content))));
    alternatives.push_back(Rule(Blank{}));
    return Rule(Choice(std::move(alternatives)));
  }

  // Everything below decorates its content rule with metadata rather than
  // adding a node of its own, so each reuses Rule's static setters, which
  // fold into an existing Metadata wrapper instead of nesting a second one.

  if (type == "TOKEN" || type == "IMMEDIATE_TOKEN") {
    TS_REF_ASSIGN_OR_RETURN(Rule content, DeserializeContent(value, path));
    if (type == "TOKEN") {
      Rule::Token(content);
    } else {
      Rule::TokenImmediate(content);
    }
    return content;
  }

  if (type == "FIELD") {
    TS_REF_ASSIGN_OR_RETURN(std::string name, GetString(value, path, "name"));
    TS_REF_ASSIGN_OR_RETURN(Rule content, DeserializeContent(value, path));
    Rule::Field(std::move(name), content);
    return content;
  }

  if (type == "ALIAS") {
    TS_REF_ASSIGN_OR_RETURN(std::string alias_value,
                            GetString(value, path, "value"));
    auto named_iter = value.find("named");
    if (named_iter == value.end() || !named_iter->is_boolean()) {
      return absl::InvalidArgumentError(
          absl::StrCat(path, ".named: expected a boolean"));
    }
    TS_REF_ASSIGN_OR_RETURN(Rule content, DeserializeContent(value, path));
    Rule::Alias(Alias{std::move(alias_value), named_iter->get<bool>()},
                content);
    return content;
  }

  if (type == "PREC" || type == "PREC_LEFT" || type == "PREC_RIGHT") {
    TS_REF_ASSIGN_OR_RETURN(Precedence precedence,
                            DeserializePrecedence(value, path));
    TS_REF_ASSIGN_OR_RETURN(Rule content, DeserializeContent(value, path));
    if (type == "PREC") {
      Rule::Prec(std::move(precedence), content);
    } else if (type == "PREC_LEFT") {
      Rule::PrecLeft(std::move(precedence), content);
    } else {
      Rule::PrecRight(std::move(precedence), content);
    }
    return content;
  }

  if (type == "PREC_DYNAMIC") {
    TS_REF_ASSIGN_OR_RETURN(const Json* member,
                            GetMember(value, path, "value"));
    if (!member->is_number_integer()) {
      return TypeError(absl::StrCat(path, ".value"), "an integer", *member);
    }
    TS_REF_ASSIGN_OR_RETURN(Rule content, DeserializeContent(value, path));
    Rule::PrecDynamic(member->get<DynamicPrecedenceType>(), content);
    return content;
  }

  if (type == "RESERVED") {
    TS_REF_ASSIGN_OR_RETURN(std::string context_name,
                            GetString(value, path, "context_name"));
    TS_REF_ASSIGN_OR_RETURN(Rule content, DeserializeContent(value, path));
    Rule::Reserved(std::move(context_name), content);
    return content;
  }

  return absl::InvalidArgumentError(
      absl::StrCat(path, ": unknown rule type '", type, "'"));
}

// A `precedences` entry is a normalized rule, but only two shapes are
// meaningful there: a bare string naming a precedence level, or a symbol
// reference naming a rule.
absl::StatusOr<PrecedenceEntry> DeserializePrecedenceEntry(
    const Json& value, std::string_view path) {
  if (!value.is_object()) return TypeError(path, "a precedence entry", value);
  TS_REF_ASSIGN_OR_RETURN(std::string type, GetString(value, path, "type"));
  if (type == "STRING") {
    TS_REF_ASSIGN_OR_RETURN(std::string name, GetString(value, path, "value"));
    return PrecedenceEntry{PrecedenceEntry::Kind::Name, std::move(name)};
  }
  if (type == "SYMBOL") {
    TS_REF_ASSIGN_OR_RETURN(std::string name, GetString(value, path, "name"));
    return PrecedenceEntry{PrecedenceEntry::Kind::Symbol, std::move(name)};
  }
  return absl::InvalidArgumentError(absl::StrCat(
      path, ": a precedences entry must be a string or a rule reference, got '",
      type, "'"));
}

// tree-sitter's convention: a rule whose name begins with an underscore is
// hidden, i.e. it does not appear as a named node in the output tree. The
// Auxiliary and Anonymous kinds are not assigned here -- they are produced
// later, by the normalization passes that generate repeat helpers and intern
// string literals.
VariableType VariableKindForName(std::string_view name) {
  return name.starts_with('_') ? VariableType::Hidden : VariableType::Named;
}

std::string ShellQuote(std::string_view value) {
  std::string quoted = "'";
  for (char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
  return quoted;
}

std::filesystem::path RunnerScriptPath() {
  if (const char* override_dir = std::getenv("TS_REF_JS_DIR");
      override_dir != nullptr && *override_dir != '\0') {
    return std::filesystem::path(override_dir) / "runner.js";
  }
  return std::filesystem::path(TS_REF_JS_DIR) / "runner.js";
}

}  // namespace

absl::StatusOr<std::string> RunGrammarJs(
    const std::filesystem::path& grammar_js_path) {
  std::error_code error;
  if (!std::filesystem::exists(grammar_js_path, error)) {
    return absl::NotFoundError(
        absl::StrCat("grammar file not found: ", grammar_js_path.string()));
  }

  const std::filesystem::path runner = RunnerScriptPath();
  if (!std::filesystem::exists(runner, error)) {
    return absl::NotFoundError(absl::StrCat(
        "grammar DSL runner not found: ", runner.string(),
        " (set TS_REF_JS_DIR to the directory holding runner.js)"));
  }

  // stderr is deliberately left attached to ours, so that a syntax error or a
  // thrown DSL validation error from the grammar shows up verbatim.
  const std::string command =
      absl::StrCat("node ", ShellQuote(runner.string()), " ",
                   ShellQuote(grammar_js_path.string()));

  std::FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return absl::InternalError(absl::StrCat("failed to start node: ", command));
  }

  std::string output;
  std::array<char, 8192> buffer{};
  while (std::size_t read_count =
             std::fread(buffer.data(), 1, buffer.size(), pipe)) {
    output.append(buffer.data(), read_count);
  }

  const int close_status = ::pclose(pipe);
  if (close_status == -1) {
    return absl::InternalError("failed to close the node subprocess");
  }
  if (!WIFEXITED(close_status) || WEXITSTATUS(close_status) != 0) {
    return absl::InvalidArgumentError(
        absl::StrCat("node exited with status ", WEXITSTATUS(close_status),
                     " while ", "evaluating ", grammar_js_path.string(),
                     "; see the error above for details"));
  }
  if (output.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("node produced no output for ", grammar_js_path.string()));
  }
  return output;
}

absl::StatusOr<InputGrammar> ParseGrammarJson(std::string_view json_text) {
  Json root = Json::parse(json_text, /*cb=*/nullptr,
                          /*allow_exceptions=*/false);
  if (root.is_discarded()) {
    return absl::InvalidArgumentError("grammar JSON is malformed");
  }
  if (!root.is_object()) {
    return TypeError("grammar", "an object", root);
  }

  InputGrammar grammar;
  TS_REF_ASSIGN_OR_RETURN(grammar.name, GetString(root, "grammar", "name"));
  grammar.inherits = GetOptionalString(root, "inherits");
  grammar.word_token = GetOptionalString(root, "word");

  TS_REF_ASSIGN_OR_RETURN(const Json* rules,
                          GetMember(root, "grammar", "rules"));
  if (!rules->is_object()) {
    return TypeError("grammar.rules", "an object", *rules);
  }
  if (rules->empty()) {
    return absl::InvalidArgumentError("grammar.rules: grammar has no rules");
  }
  // Insertion order is preserved by ordered_json, so variables[0] is the
  // grammar's first-declared rule, i.e. the start rule.
  grammar.variables.reserve(rules->size());
  for (const auto& [name, rule_json] : rules->items()) {
    TS_REF_ASSIGN_OR_RETURN(
        Rule rule,
        DeserializeRule(rule_json, absl::StrCat("grammar.rules.", name)));
    grammar.variables.push_back(
        Variable{name, VariableKindForName(name), std::move(rule)});
  }

  if (auto extras = root.find("extras"); extras != root.end()) {
    TS_REF_ASSIGN_OR_RETURN(grammar.extra_symbols,
                            DeserializeRuleArray(*extras, "grammar.extras"));
  }

  if (auto externals = root.find("externals"); externals != root.end()) {
    TS_REF_ASSIGN_OR_RETURN(
        grammar.external_tokens,
        DeserializeRuleArray(*externals, "grammar.externals"));
  }

  if (auto conflicts = root.find("conflicts"); conflicts != root.end()) {
    if (!conflicts->is_array()) {
      return TypeError("grammar.conflicts", "an array", *conflicts);
    }
    for (std::size_t i = 0; i < conflicts->size(); ++i) {
      const Json& conflict_set = (*conflicts)[i];
      const std::string path = absl::StrCat("grammar.conflicts[", i, "]");
      if (!conflict_set.is_array()) {
        return TypeError(path, "an array of rule names", conflict_set);
      }
      std::vector<std::string> names;
      names.reserve(conflict_set.size());
      for (std::size_t j = 0; j < conflict_set.size(); ++j) {
        if (!conflict_set[j].is_string()) {
          return TypeError(absl::StrCat(path, "[", j, "]"), "a rule name",
                           conflict_set[j]);
        }
        names.push_back(conflict_set[j].get<std::string>());
      }
      grammar.expected_conflicts.push_back(std::move(names));
    }
  }

  if (auto precedences = root.find("precedences"); precedences != root.end()) {
    if (!precedences->is_array()) {
      return TypeError("grammar.precedences", "an array", *precedences);
    }
    for (std::size_t i = 0; i < precedences->size(); ++i) {
      const Json& level = (*precedences)[i];
      const std::string path = absl::StrCat("grammar.precedences[", i, "]");
      if (!level.is_array()) {
        return TypeError(path, "an array of precedence entries", level);
      }
      std::vector<PrecedenceEntry> entries;
      entries.reserve(level.size());
      for (std::size_t j = 0; j < level.size(); ++j) {
        TS_REF_ASSIGN_OR_RETURN(PrecedenceEntry entry,
                                DeserializePrecedenceEntry(
                                    level[j], absl::StrCat(path, "[", j, "]")));
        entries.push_back(std::move(entry));
      }
      grammar.precedences.push_back(std::move(entries));
    }
  }

  const auto read_name_list =
      [&root](std::string_view key,
              std::vector<std::string>& out) -> absl::Status {
    auto iter = root.find(std::string(key));
    if (iter == root.end()) return absl::OkStatus();
    if (!iter->is_array()) {
      return TypeError(absl::StrCat("grammar.", key), "an array", *iter);
    }
    for (std::size_t i = 0; i < iter->size(); ++i) {
      if (!(*iter)[i].is_string()) {
        return TypeError(absl::StrCat("grammar.", key, "[", i, "]"),
                         "a rule name", (*iter)[i]);
      }
      out.push_back((*iter)[i].get<std::string>());
    }
    return absl::OkStatus();
  };
  if (auto status = read_name_list("inline", grammar.inline_variables);
      !status.ok()) {
    return status;
  }
  if (auto status = read_name_list("supertypes", grammar.supertypes);
      !status.ok()) {
    return status;
  }

  if (auto reserved = root.find("reserved"); reserved != root.end()) {
    if (!reserved->is_object()) {
      return TypeError("grammar.reserved", "an object", *reserved);
    }
    for (const auto& [set_name, tokens] : reserved->items()) {
      TS_REF_ASSIGN_OR_RETURN(
          std::vector<Rule> rules,
          DeserializeRuleArray(tokens,
                               absl::StrCat("grammar.reserved.", set_name)));
      grammar.reserved_word_sets.push_back(
          ReservedWordSet{set_name, std::move(rules)});
    }
  }

  return grammar;
}

absl::StatusOr<InputGrammar> LoadGrammar(
    const std::filesystem::path& grammar_path) {
  if (grammar_path.extension() == ".json") {
    std::ifstream file(grammar_path);
    if (!file) {
      return absl::NotFoundError(
          absl::StrCat("could not open ", grammar_path.string()));
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return ParseGrammarJson(contents.str());
  }
  TS_REF_ASSIGN_OR_RETURN(std::string json_text, RunGrammarJs(grammar_path));
  return ParseGrammarJson(json_text);
}

#undef TS_REF_ASSIGN_OR_RETURN
#undef TS_REF_ASSIGN_OR_RETURN_IMPL
#undef TS_REF_CONCAT
#undef TS_REF_CONCAT_INNER

}  // namespace ts_ref
