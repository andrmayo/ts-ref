#include "ts_ref/grammar_normalizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "ts_ref/grammar_ir.h"
#include "ts_ref/prepared_grammar.h"

namespace ts_ref {
namespace {

#define TS_REF_CONCAT_INNER(a, b) a##b
#define TS_REF_CONCAT(a, b) TS_REF_CONCAT_INNER(a, b)
#define TS_REF_ASSIGN_OR_RETURN_IMPL(temp, lhs, rexpr) \
  auto temp = (rexpr);                                 \
  if (!temp.ok()) return temp.status();                \
  lhs = std::move(temp).value()
#define TS_REF_ASSIGN_OR_RETURN(lhs, rexpr)                                 \
  TS_REF_ASSIGN_OR_RETURN_IMPL(TS_REF_CONCAT(toy_statusor_, __LINE__), lhs, \
                               rexpr)

// ---------------------------------------------------------------------------
// Intermediate grammar shapes
//
// These sit between InputGrammar and PreparedGrammar and are private to this
// file, because nothing outside the pass pipeline has any use for a
// half-normalized grammar.
// ---------------------------------------------------------------------------

// After pass 1: every symbol reference is resolved, but tokens have not been
// separated out and rules are still trees.
struct InternedGrammar {
  std::vector<Variable> variables;
  std::vector<Rule> extra_symbols;
  std::vector<std::vector<SymbolData>> expected_conflicts;
  std::vector<std::vector<PrecedenceEntry>> precedence_orderings;
  // one entry per `externals` element; the rule is a resolved symbol
  std::vector<Variable> external_tokens;
  std::vector<SymbolData> variables_to_inline;
  std::vector<SymbolData> supertype_symbols;
  std::optional<SymbolData> word_token;
  std::vector<ReservedWordSet> reserved_word_sets;
};

// After pass 2: terminals live in a separate lexical grammar, and what remains
// here is purely syntactic.
struct ExtractedSyntaxGrammar {
  std::vector<Variable> variables;
  std::vector<SymbolData> extra_symbols;
  std::vector<std::vector<SymbolData>> expected_conflicts;
  std::vector<std::vector<PrecedenceEntry>> precedence_orderings;
  std::vector<ExternalToken> external_tokens;
  std::vector<SymbolData> variables_to_inline;
  std::vector<SymbolData> supertype_symbols;
  std::optional<SymbolData> word_token;
  std::vector<ReservedWordSymbolSet> reserved_word_sets;
};

struct ExtractedLexicalGrammar {
  std::vector<Variable> variables;
  std::vector<Rule> separators;
};

VariableType VariableKindForName(std::string_view name) {
  return name.starts_with('_') ? VariableType::Hidden : VariableType::Named;
}

Symbol ToSymbolIndex(std::size_t index) { return static_cast<Symbol>(index); }

// Convenience for the many places that rebuild a decorated rule.
Rule MakeMetadata(Rule inner, MetadataParams params) {
  return Rule(Metadata(std::move(inner), std::move(params)));
}

// ---------------------------------------------------------------------------
// Pass 1: intern symbols
// ---------------------------------------------------------------------------

class Interner {
 public:
  explicit Interner(const InputGrammar& grammar) : grammar_{grammar} {}

  // Resolution order matters and is not arbitrary: ordinary rules are checked
  // before external tokens, so a grammar that declares `externals: $ =>
  // [$.foo]` *and* defines a rule named `foo` resolves references to the rule.
  // extract_tokens later turns that into an external token with a
  // corresponding internal token, which is how a scanner gets a DFA fallback.
  std::optional<SymbolData> InternName(std::string_view name) const {
    for (std::size_t i = 0; i < grammar_.variables.size(); ++i) {
      if (grammar_.variables[i].name == name) {
        return NonTerminalSymbol(ToSymbolIndex(i));
      }
    }
    for (std::size_t i = 0; i < grammar_.external_tokens.size(); ++i) {
      const auto* reference =
          std::get_if<NamedSymbolRef>(&grammar_.external_tokens[i].storage());
      if (reference != nullptr && reference->name == name) {
        return ExternalSymbol(ToSymbolIndex(i));
      }
    }
    return std::nullopt;
  }

  absl::StatusOr<Rule> InternRule(const Rule& rule) const {
    if (const auto* reference = std::get_if<NamedSymbolRef>(&rule.storage())) {
      std::optional<SymbolData> symbol = InternName(reference->name);
      if (!symbol.has_value()) {
        return absl::InvalidArgumentError(
            absl::StrCat("undefined symbol `", reference->name, "`"));
      }
      return Rule(ResolvedSymbol{*symbol});
    }
    if (const auto* choice = std::get_if<Choice>(&rule.storage())) {
      TS_REF_ASSIGN_OR_RETURN(std::vector<Rule> members,
                              InternRules(choice->members));
      return Rule(Choice(std::move(members)));
    }
    if (const auto* sequence = std::get_if<Seq>(&rule.storage())) {
      TS_REF_ASSIGN_OR_RETURN(std::vector<Rule> members,
                              InternRules(sequence->members));
      return Rule(Seq{std::move(members)});
    }
    if (const auto* repeat = std::get_if<Repeat>(&rule.storage())) {
      TS_REF_ASSIGN_OR_RETURN(Rule inner, InternRule(*repeat->rule));
      return Rule(Repeat(std::move(inner)));
    }
    if (const auto* metadata = std::get_if<Metadata>(&rule.storage())) {
      TS_REF_ASSIGN_OR_RETURN(Rule inner, InternRule(*metadata->rule));
      return MakeMetadata(std::move(inner), metadata->params);
    }
    return rule;
  }

  absl::StatusOr<std::vector<Rule>> InternRules(
      const std::vector<Rule>& rules) const {
    std::vector<Rule> interned;
    interned.reserve(rules.size());
    for (const Rule& rule : rules) {
      TS_REF_ASSIGN_OR_RETURN(Rule value, InternRule(rule));
      interned.push_back(std::move(value));
    }
    return interned;
  }

 private:
  const InputGrammar& grammar_;
};

absl::StatusOr<InternedGrammar> InternSymbols(const InputGrammar& input) {
  if (input.variables.empty()) {
    return absl::InvalidArgumentError("grammar has no rules");
  }
  if (VariableKindForName(input.variables[0].name) == VariableType::Hidden) {
    return absl::InvalidArgumentError(
        absl::StrCat("a grammar's start rule must be visible, but `",
                     input.variables[0].name, "` is hidden"));
  }

  const Interner interner(input);
  InternedGrammar grammar;

  grammar.variables.reserve(input.variables.size());
  for (const Variable& variable : input.variables) {
    TS_REF_ASSIGN_OR_RETURN(Rule rule, interner.InternRule(variable.rule));
    grammar.variables.push_back(Variable{
        variable.name, VariableKindForName(variable.name), std::move(rule)});
  }

  grammar.external_tokens.reserve(input.external_tokens.size());
  for (const Rule& external : input.external_tokens) {
    TS_REF_ASSIGN_OR_RETURN(Rule rule, interner.InternRule(external));
    std::string name;
    VariableType kind = VariableType::Anonymous;
    if (const auto* reference =
            std::get_if<NamedSymbolRef>(&external.storage())) {
      name = reference->name;
      kind = VariableKindForName(name);
    }
    grammar.external_tokens.push_back(
        Variable{std::move(name), kind, std::move(rule)});
  }

  TS_REF_ASSIGN_OR_RETURN(grammar.extra_symbols,
                          interner.InternRules(input.extra_symbols));

  grammar.supertype_symbols.reserve(input.supertypes.size());
  for (const std::string& name : input.supertypes) {
    std::optional<SymbolData> symbol = interner.InternName(name);
    if (!symbol.has_value()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "undefined symbol `", name, "` in the grammar's supertypes"));
    }
    grammar.supertype_symbols.push_back(*symbol);
  }

  grammar.reserved_word_sets.reserve(input.reserved_word_sets.size());
  for (const ReservedWordSet& word_set : input.reserved_word_sets) {
    TS_REF_ASSIGN_OR_RETURN(std::vector<Rule> rules,
                            interner.InternRules(word_set.rules));
    grammar.reserved_word_sets.push_back(
        ReservedWordSet{word_set.name, std::move(rules)});
  }

  grammar.expected_conflicts.reserve(input.expected_conflicts.size());
  for (const auto& conflict : input.expected_conflicts) {
    std::vector<SymbolData> symbols;
    symbols.reserve(conflict.size());
    for (const std::string& name : conflict) {
      std::optional<SymbolData> symbol = interner.InternName(name);
      if (!symbol.has_value()) {
        return absl::InvalidArgumentError(absl::StrCat(
            "undefined symbol `", name, "` in the grammar's conflicts"));
      }
      symbols.push_back(*symbol);
    }
    grammar.expected_conflicts.push_back(std::move(symbols));
  }

  // Unlike the lists above, an unresolvable inline name is skipped rather than
  // rejected -- it only ever suppresses an optimization.
  for (const std::string& name : input.inline_variables) {
    if (std::optional<SymbolData> symbol = interner.InternName(name)) {
      grammar.variables_to_inline.push_back(*symbol);
    }
  }

  if (input.word_token.has_value()) {
    std::optional<SymbolData> symbol = interner.InternName(*input.word_token);
    if (!symbol.has_value()) {
      return absl::InvalidArgumentError(absl::StrCat("undefined symbol `",
                                                     *input.word_token,
                                                     "` as the grammar's word "
                                                     "token"));
    }
    grammar.word_token = *symbol;
  }

  // A supertype is never itself a node in the tree, so it is forced hidden.
  for (std::size_t i = 0; i < grammar.variables.size(); ++i) {
    const SymbolData symbol = NonTerminalSymbol(ToSymbolIndex(i));
    if (std::find(grammar.supertype_symbols.begin(),
                  grammar.supertype_symbols.end(),
                  symbol) != grammar.supertype_symbols.end()) {
      grammar.variables[i].kind = VariableType::Hidden;
    }
  }

  grammar.precedence_orderings = input.precedences;
  return grammar;
}

// ---------------------------------------------------------------------------
// Pass 2: extract tokens
// ---------------------------------------------------------------------------

class TokenExtractor {
 public:
  absl::Status ExtractTokensInVariable(bool is_first, Variable& variable) {
    current_variable_name_ = variable.name;
    current_variable_token_count_ = 0;
    is_first_rule_ = is_first;
    TS_REF_ASSIGN_OR_RETURN(variable.rule, ExtractTokensInRule(variable.rule));
    return absl::OkStatus();
  }

  std::vector<Variable>& extracted_variables() { return extracted_variables_; }
  const std::vector<std::size_t>& usage_counts() const {
    return extracted_usage_counts_;
  }

 private:
  absl::StatusOr<Rule> ExtractTokensInRule(const Rule& input) {
    if (const auto* literal = std::get_if<StringLiteral>(&input.storage())) {
      TS_REF_ASSIGN_OR_RETURN(SymbolData symbol,
                              ExtractToken(input, &literal->value));
      return Rule(ResolvedSymbol{symbol});
    }
    if (std::holds_alternative<Pattern>(input.storage())) {
      TS_REF_ASSIGN_OR_RETURN(SymbolData symbol, ExtractToken(input, nullptr));
      return Rule(ResolvedSymbol{symbol});
    }
    if (const auto* metadata = std::get_if<Metadata>(&input.storage())) {
      if (!metadata->params.is_token) {
        TS_REF_ASSIGN_OR_RETURN(Rule inner,
                                ExtractTokensInRule(*metadata->rule));
        return MakeMetadata(std::move(inner), metadata->params);
      }
      // A token() wrapper marks the boundary between the syntactic and lexical
      // grammars: everything beneath it is lexed as a single unit, so the
      // whole subtree is handed to the lexical grammar without descending.
      //
      // An alias or field name is the exception. Those name the *node* the
      // token produces, not the text it matches, so they stay behind on the
      // syntactic side; folding them into the token's identity would both lose
      // them and stop two otherwise identical tokens from sharing a terminal.
      MetadataParams stripped = metadata->params;
      stripped.is_token = false;
      MetadataParams carried;
      carried.alias = stripped.alias;
      carried.field_name = stripped.field_name;
      stripped.alias.reset();
      stripped.field_name.reset();

      const auto* literal =
          std::get_if<StringLiteral>(&metadata->rule->storage());
      // If token() carried nothing else, the wrapper itself is redundant and
      // only the inner rule needs extracting; otherwise the remaining
      // annotations are part of the token's identity and must come along.
      const bool wrapper_is_redundant = stripped == MetadataParams{};
      Rule rule_to_extract = wrapper_is_redundant
                                 ? *metadata->rule
                                 : MakeMetadata(*metadata->rule, stripped);
      TS_REF_ASSIGN_OR_RETURN(
          SymbolData symbol,
          ExtractToken(rule_to_extract,
                       literal != nullptr ? &literal->value : nullptr));
      Rule result = Rule(ResolvedSymbol{symbol});
      if (carried.alias.has_value() || carried.field_name.has_value()) {
        result = MakeMetadata(std::move(result), carried);
      }
      return result;
    }
    if (const auto* repeat = std::get_if<Repeat>(&input.storage())) {
      TS_REF_ASSIGN_OR_RETURN(Rule inner, ExtractTokensInRule(*repeat->rule));
      return Rule(Repeat(std::move(inner)));
    }
    if (const auto* sequence = std::get_if<Seq>(&input.storage())) {
      TS_REF_ASSIGN_OR_RETURN(std::vector<Rule> members,
                              ExtractTokensInRules(sequence->members));
      return Rule(Seq{std::move(members)});
    }
    if (const auto* choice = std::get_if<Choice>(&input.storage())) {
      TS_REF_ASSIGN_OR_RETURN(std::vector<Rule> members,
                              ExtractTokensInRules(choice->members));
      return Rule(Choice(std::move(members)));
    }
    return input;
  }

  absl::StatusOr<std::vector<Rule>> ExtractTokensInRules(
      const std::vector<Rule>& rules) {
    std::vector<Rule> result;
    result.reserve(rules.size());
    for (const Rule& rule : rules) {
      TS_REF_ASSIGN_OR_RETURN(Rule value, ExtractTokensInRule(rule));
      result.push_back(std::move(value));
    }
    return result;
  }

  absl::StatusOr<SymbolData> ExtractToken(const Rule& rule,
                                          const std::string* string_value) {
    // Deduplication by structural equality: every occurrence of "(" across a
    // grammar collapses to one terminal. This is what keeps the terminal
    // count proportional to the distinct tokens rather than their uses.
    for (std::size_t i = 0; i < extracted_variables_.size(); ++i) {
      if (extracted_variables_[i].rule == rule) {
        ++extracted_usage_counts_[i];
        return TerminalSymbol(ToSymbolIndex(i));
      }
    }

    const std::size_t index = extracted_variables_.size();
    if (string_value != nullptr) {
      if (string_value->empty() && !is_first_rule_) {
        return absl::InvalidArgumentError(absl::StrCat(
            "the rule `", current_variable_name_,
            "` contains an empty string; only the start rule may do so"));
      }
      extracted_variables_.push_back(
          Variable{*string_value, VariableType::Anonymous, rule});
    } else {
      ++current_variable_token_count_;
      extracted_variables_.push_back(
          Variable{absl::StrCat(current_variable_name_, "_token",
                                current_variable_token_count_),
                   VariableType::Auxiliary, rule});
    }
    extracted_usage_counts_.push_back(1);
    return TerminalSymbol(ToSymbolIndex(index));
  }

  std::string current_variable_name_;
  int current_variable_token_count_ = 0;
  bool is_first_rule_ = false;
  std::vector<Variable> extracted_variables_;
  std::vector<std::size_t> extracted_usage_counts_;
};

// Rewrites non-terminal symbols after some variables have been removed from
// the syntax grammar: a removed variable's references become the terminal that
// replaced it, and everything after it shifts down.
class SymbolReplacer {
 public:
  explicit SymbolReplacer(
      std::vector<std::pair<std::size_t, std::size_t>> replacements)
      : replacements_{std::move(replacements)} {}

  SymbolData ReplaceSymbol(SymbolData symbol) const {
    if (!symbol.IsNonTerminal()) return symbol;
    std::size_t adjusted = symbol.index;
    for (const auto& [removed_index, terminal_index] : replacements_) {
      if (removed_index == symbol.index) {
        return TerminalSymbol(ToSymbolIndex(terminal_index));
      }
      if (removed_index < symbol.index) --adjusted;
    }
    return NonTerminalSymbol(ToSymbolIndex(adjusted));
  }

  Rule ReplaceSymbolsInRule(const Rule& rule) const {
    if (const auto* symbol = std::get_if<ResolvedSymbol>(&rule.storage())) {
      return Rule(ResolvedSymbol{ReplaceSymbol(symbol->symbol_data)});
    }
    if (const auto* choice = std::get_if<Choice>(&rule.storage())) {
      return Rule(Choice(ReplaceSymbolsInRules(choice->members)));
    }
    if (const auto* sequence = std::get_if<Seq>(&rule.storage())) {
      return Rule(Seq{ReplaceSymbolsInRules(sequence->members)});
    }
    if (const auto* repeat = std::get_if<Repeat>(&rule.storage())) {
      return Rule(Repeat(ReplaceSymbolsInRule(*repeat->rule)));
    }
    if (const auto* metadata = std::get_if<Metadata>(&rule.storage())) {
      return MakeMetadata(ReplaceSymbolsInRule(*metadata->rule),
                          metadata->params);
    }
    return rule;
  }

  std::vector<Rule> ReplaceSymbolsInRules(
      const std::vector<Rule>& rules) const {
    std::vector<Rule> result;
    result.reserve(rules.size());
    for (const Rule& rule : rules) {
      result.push_back(ReplaceSymbolsInRule(rule));
    }
    return result;
  }

 private:
  // (index removed from the syntax grammar, index it became in the lexical one)
  std::vector<std::pair<std::size_t, std::size_t>> replacements_;
};

absl::StatusOr<std::pair<ExtractedSyntaxGrammar, ExtractedLexicalGrammar>>
ExtractTokens(InternedGrammar grammar) {
  TokenExtractor extractor;
  for (std::size_t i = 0; i < grammar.variables.size(); ++i) {
    absl::Status status =
        extractor.ExtractTokensInVariable(i == 0, grammar.variables[i]);
    if (!status.ok()) return status;
  }
  for (Variable& external : grammar.external_tokens) {
    absl::Status status = extractor.ExtractTokensInVariable(false, external);
    if (!status.ok()) return status;
  }

  std::vector<Variable> lexical_variables =
      std::move(extractor.extracted_variables());
  const std::vector<std::size_t>& usage_counts = extractor.usage_counts();

  // If a variable's whole rule turned out to be a single token that is used
  // nowhere else, the variable is redundant with that token: drop it and give
  // the token its name. This is why `number: $ => /\d+/` yields one terminal
  // called `number` rather than a non-terminal wrapping an anonymous token.
  std::vector<Variable> variables;
  variables.reserve(grammar.variables.size());
  std::vector<std::pair<std::size_t, std::size_t>> replacements;
  for (std::size_t i = 0; i < grammar.variables.size(); ++i) {
    Variable& variable = grammar.variables[i];
    const auto* symbol = std::get_if<ResolvedSymbol>(&variable.rule.storage());
    if (symbol != nullptr && symbol->symbol_data.IsTerminal() && i > 0 &&
        usage_counts[symbol->symbol_data.index] == 1) {
      Variable& lexical = lexical_variables[symbol->symbol_data.index];
      // A hidden variable does not get to rename a token that already has a
      // real name of its own.
      if (lexical.kind == VariableType::Auxiliary ||
          variable.kind != VariableType::Hidden) {
        lexical.kind = variable.kind;
        lexical.name = variable.name;
        replacements.emplace_back(i, symbol->symbol_data.index);
        continue;
      }
    }
    variables.push_back(std::move(variable));
  }

  const SymbolReplacer replacer(std::move(replacements));
  for (Variable& variable : variables) {
    variable.rule = replacer.ReplaceSymbolsInRule(variable.rule);
  }

  ExtractedSyntaxGrammar syntax;
  syntax.variables = std::move(variables);
  syntax.precedence_orderings = std::move(grammar.precedence_orderings);

  syntax.expected_conflicts.reserve(grammar.expected_conflicts.size());
  for (const auto& conflict : grammar.expected_conflicts) {
    std::vector<SymbolData> symbols;
    symbols.reserve(conflict.size());
    for (SymbolData symbol : conflict) {
      symbols.push_back(replacer.ReplaceSymbol(symbol));
    }
    std::sort(symbols.begin(), symbols.end());
    symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
    syntax.expected_conflicts.push_back(std::move(symbols));
  }

  syntax.supertype_symbols.reserve(grammar.supertype_symbols.size());
  for (SymbolData symbol : grammar.supertype_symbols) {
    const SymbolData replaced = replacer.ReplaceSymbol(symbol);
    if (replaced.IsTerminal()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "terminal rule `", lexical_variables[replaced.index].name,
          "` cannot be used as a supertype"));
    }
    syntax.supertype_symbols.push_back(replaced);
  }

  syntax.variables_to_inline.reserve(grammar.variables_to_inline.size());
  for (SymbolData symbol : grammar.variables_to_inline) {
    syntax.variables_to_inline.push_back(replacer.ReplaceSymbol(symbol));
  }

  // An `extras` entry is either a symbol (becomes an extra token) or a bare
  // pattern with no rule of its own (becomes a separator the lexer skips).
  std::vector<Rule> separators;
  for (const Rule& rule : grammar.extra_symbols) {
    if (const auto* symbol = std::get_if<ResolvedSymbol>(&rule.storage())) {
      syntax.extra_symbols.push_back(
          replacer.ReplaceSymbol(symbol->symbol_data));
      continue;
    }
    auto match = std::find_if(
        lexical_variables.begin(), lexical_variables.end(),
        [&rule](const Variable& variable) { return variable.rule == rule; });
    if (match != lexical_variables.end()) {
      syntax.extra_symbols.push_back(
          TerminalSymbol(ToSymbolIndex(match - lexical_variables.begin())));
    } else {
      separators.push_back(rule);
    }
  }

  syntax.external_tokens.reserve(grammar.external_tokens.size());
  for (Variable& external : grammar.external_tokens) {
    const Rule rule = replacer.ReplaceSymbolsInRule(external.rule);
    const auto* symbol = std::get_if<ResolvedSymbol>(&rule.storage());
    if (symbol == nullptr) {
      return absl::InvalidArgumentError(
          "non-symbol rules cannot be used as external tokens");
    }
    if (symbol->symbol_data.IsNonTerminal()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "rule `", syntax.variables[symbol->symbol_data.index].name,
          "` cannot be used as both an external token and a non-terminal"));
    }
    if (symbol->symbol_data.IsExternal()) {
      syntax.external_tokens.push_back(
          ExternalToken{external.name, external.kind, std::nullopt});
    } else {
      // The grammar defines an ordinary rule of the same name, which becomes
      // the DFA fallback for when the scanner declines to produce this token.
      syntax.external_tokens.push_back(
          ExternalToken{lexical_variables[symbol->symbol_data.index].name,
                        external.kind, symbol->symbol_data});
    }
  }

  if (grammar.word_token.has_value()) {
    const SymbolData replaced = replacer.ReplaceSymbol(*grammar.word_token);
    if (replaced.IsNonTerminal()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "non-terminal symbol `", syntax.variables[replaced.index].name,
          "` cannot be used as the word token"));
    }
    syntax.word_token = replaced;
  }

  syntax.reserved_word_sets.reserve(grammar.reserved_word_sets.size());
  for (const ReservedWordSet& word_set : grammar.reserved_word_sets) {
    std::vector<SymbolData> symbols;
    symbols.reserve(word_set.rules.size());
    for (const Rule& rule : word_set.rules) {
      if (const auto* symbol = std::get_if<ResolvedSymbol>(&rule.storage())) {
        symbols.push_back(replacer.ReplaceSymbol(symbol->symbol_data));
        continue;
      }
      auto match = std::find_if(
          lexical_variables.begin(), lexical_variables.end(),
          [&rule](const Variable& variable) { return variable.rule == rule; });
      if (match == lexical_variables.end()) {
        return absl::InvalidArgumentError(absl::StrCat(
            "reserved word in set `", word_set.name, "` must be a token"));
      }
      symbols.push_back(
          TerminalSymbol(ToSymbolIndex(match - lexical_variables.begin())));
    }
    syntax.reserved_word_sets.push_back(
        ReservedWordSymbolSet{word_set.name, std::move(symbols)});
  }

  ExtractedLexicalGrammar lexical;
  lexical.variables = std::move(lexical_variables);
  lexical.separators = std::move(separators);
  return std::make_pair(std::move(syntax), std::move(lexical));
}

// ---------------------------------------------------------------------------
// Pass 3: expand repeats
// ---------------------------------------------------------------------------

class RepeatExpander {
 public:
  explicit RepeatExpander(std::size_t preceding_symbol_count)
      : preceding_symbol_count_{preceding_symbol_count} {}

  // Returns true if the variable itself was turned into the recursive rule,
  // which means it can no longer be inlined.
  bool ExpandVariable(std::size_t index, Variable& variable) {
    current_variable_name_ = variable.name;
    repeat_count_in_variable_ = 0;

    // A hidden rule that is nothing but a repetition can become the recursive
    // rule itself, rather than delegating to a fresh auxiliary rule.
    if (variable.kind == VariableType::Hidden) {
      if (const auto* repeat = std::get_if<Repeat>(&variable.rule.storage())) {
        Rule inner = ExpandRule(*repeat->rule);
        variable.rule = WrapInBinaryTree(
            NonTerminalSymbol(ToSymbolIndex(index)), std::move(inner));
        variable.kind = VariableType::Auxiliary;
        return true;
      }
    }

    variable.rule = ExpandRule(variable.rule);
    return false;
  }

  std::vector<Variable>& auxiliary_variables() { return auxiliary_variables_; }

 private:
  // Produces `aux -> aux aux | content`, tree-sitter's shape for a repetition.
  // The self-referential pair makes the resulting parse tree a balanced binary
  // tree rather than a left- or right-leaning list.
  static Rule WrapInBinaryTree(SymbolData symbol, Rule content) {
    std::vector<Rule> pair;
    pair.push_back(Rule(ResolvedSymbol{symbol}));
    pair.push_back(Rule(ResolvedSymbol{symbol}));

    std::vector<Rule> alternatives;
    alternatives.push_back(Rule(Seq{std::move(pair)}));
    alternatives.push_back(std::move(content));
    return Rule(Choice(std::move(alternatives)));
  }

  Rule ExpandRule(const Rule& rule) {
    if (const auto* choice = std::get_if<Choice>(&rule.storage())) {
      return Rule(Choice(ExpandRules(choice->members)));
    }
    if (const auto* sequence = std::get_if<Seq>(&rule.storage())) {
      return Rule(Seq{ExpandRules(sequence->members)});
    }
    if (const auto* metadata = std::get_if<Metadata>(&rule.storage())) {
      return MakeMetadata(ExpandRule(*metadata->rule), metadata->params);
    }
    if (const auto* repeat = std::get_if<Repeat>(&rule.storage())) {
      Rule inner = ExpandRule(*repeat->rule);
      // Identical repetitions within one variable share an auxiliary rule.
      for (const auto& [existing_rule, symbol] : existing_repeats_) {
        if (existing_rule == inner) return Rule(ResolvedSymbol{symbol});
      }

      ++repeat_count_in_variable_;
      const SymbolData repeat_symbol = NonTerminalSymbol(
          ToSymbolIndex(preceding_symbol_count_ + auxiliary_variables_.size()));
      existing_repeats_.emplace_back(inner, repeat_symbol);
      auxiliary_variables_.push_back(
          Variable{absl::StrCat(current_variable_name_, "_repeat",
                                repeat_count_in_variable_),
                   VariableType::Auxiliary,
                   WrapInBinaryTree(repeat_symbol, std::move(inner))});
      return Rule(ResolvedSymbol{repeat_symbol});
    }
    return rule;
  }

  std::vector<Rule> ExpandRules(const std::vector<Rule>& rules) {
    std::vector<Rule> result;
    result.reserve(rules.size());
    for (const Rule& rule : rules) result.push_back(ExpandRule(rule));
    return result;
  }

  std::string current_variable_name_;
  int repeat_count_in_variable_ = 0;
  std::size_t preceding_symbol_count_;
  std::vector<Variable> auxiliary_variables_;
  std::vector<std::pair<Rule, SymbolData>> existing_repeats_;
};

ExtractedSyntaxGrammar ExpandRepeats(ExtractedSyntaxGrammar grammar) {
  RepeatExpander expander(grammar.variables.size());
  for (std::size_t i = 0; i < grammar.variables.size(); ++i) {
    if (expander.ExpandVariable(i, grammar.variables[i])) {
      const SymbolData symbol = NonTerminalSymbol(ToSymbolIndex(i));
      grammar.variables_to_inline.erase(
          std::remove(grammar.variables_to_inline.begin(),
                      grammar.variables_to_inline.end(), symbol),
          grammar.variables_to_inline.end());
    }
  }
  std::vector<Variable>& auxiliaries = expander.auxiliary_variables();
  grammar.variables.insert(grammar.variables.end(),
                           std::make_move_iterator(auxiliaries.begin()),
                           std::make_move_iterator(auxiliaries.end()));
  return grammar;
}

// ---------------------------------------------------------------------------
// Pass 4: flatten
// ---------------------------------------------------------------------------

// Expands a rule tree into the list of alternatives it denotes, distributing
// sequences over choices. seq(choice(a, b), c) becomes [seq(a, c), seq(b, c)].
std::vector<Rule> ExtractChoices(const Rule& rule) {
  if (const auto* sequence = std::get_if<Seq>(&rule.storage())) {
    std::vector<Rule> result;
    result.push_back(Rule(Blank{}));
    for (const Rule& member : sequence->members) {
      const std::vector<Rule> extraction = ExtractChoices(member);
      std::vector<Rule> next;
      next.reserve(result.size() * extraction.size());
      for (const Rule& entry : result) {
        for (const Rule& extracted : extraction) {
          std::vector<Rule> pair;
          pair.push_back(entry);
          pair.push_back(extracted);
          next.push_back(Rule(Seq{std::move(pair)}));
        }
      }
      result = std::move(next);
    }
    return result;
  }
  if (const auto* choice = std::get_if<Choice>(&rule.storage())) {
    std::vector<Rule> result;
    for (const Rule& member : choice->members) {
      for (Rule& extracted : ExtractChoices(member)) {
        result.push_back(std::move(extracted));
      }
    }
    return result;
  }
  if (const auto* metadata = std::get_if<Metadata>(&rule.storage())) {
    std::vector<Rule> result;
    for (Rule& extracted : ExtractChoices(*metadata->rule)) {
      result.push_back(MakeMetadata(std::move(extracted), metadata->params));
    }
    return result;
  }
  return std::vector<Rule>{rule};
}

class RuleFlattener {
 public:
  explicit RuleFlattener(std::vector<std::string> reserved_word_set_names)
      : reserved_word_set_names_{std::move(reserved_word_set_names)} {}

  absl::StatusOr<SyntaxVariable> FlattenVariable(const Variable& variable) {
    std::vector<Production> productions;
    for (const Rule& alternative : ExtractChoices(variable.rule)) {
      TS_REF_ASSIGN_OR_RETURN(Production production, FlattenRule(alternative));
      // Distinct rule trees can denote the same production; keep one.
      if (std::find(productions.begin(), productions.end(), production) ==
          productions.end()) {
        productions.push_back(std::move(production));
      }
    }
    return SyntaxVariable{variable.name, variable.kind, std::move(productions)};
  }

 private:
  absl::StatusOr<Production> FlattenRule(const Rule& rule) {
    production_ = Production{};
    precedence_stack_.clear();
    associativity_stack_.clear();
    alias_stack_.clear();
    field_name_stack_.clear();
    reserved_word_stack_.clear();
    absl::StatusOr<bool> pushed = Apply(rule, /*at_end=*/true);
    if (!pushed.ok()) return pushed.status();
    return production_;
  }

  // Returns whether this subtree contributed any step. `at_end` tracks whether
  // the subtree sits at the very end of the production, which decides how a
  // precedence annotation is attached: an annotation that ends with the
  // production applies to the reduction as a whole, so it stays on the last
  // step, while one that closes mid-production applies only to the steps it
  // spans, and the following step reverts to the enclosing precedence.
  absl::StatusOr<bool> Apply(const Rule& rule, bool at_end) {
    if (const auto* sequence = std::get_if<Seq>(&rule.storage())) {
      if (sequence->members.empty()) return false;
      bool result = false;
      const std::size_t last_index = sequence->members.size() - 1;
      for (std::size_t i = 0; i < sequence->members.size(); ++i) {
        TS_REF_ASSIGN_OR_RETURN(
            const bool pushed,
            Apply(sequence->members[i], i == last_index && at_end));
        result = result || pushed;
      }
      return result;
    }

    if (const auto* metadata = std::get_if<Metadata>(&rule.storage())) {
      const MetadataParams& params = metadata->params;

      const bool has_precedence = !params.precedence.IsNone();
      if (has_precedence) precedence_stack_.push_back(params.precedence);
      const bool has_associativity = params.associativity.has_value();
      if (has_associativity) {
        associativity_stack_.push_back(*params.associativity);
      }
      const bool has_alias = params.alias.has_value();
      if (has_alias) alias_stack_.push_back(*params.alias);
      const bool has_field_name = params.field_name.has_value();
      if (has_field_name) field_name_stack_.push_back(*params.field_name);

      const bool has_reserved = params.reserved_word_set_name.has_value();
      if (has_reserved) {
        if (std::find(reserved_word_set_names_.begin(),
                      reserved_word_set_names_.end(),
                      *params.reserved_word_set_name) ==
            reserved_word_set_names_.end()) {
          return absl::InvalidArgumentError(absl::StrCat(
              "no such reserved word set: ", *params.reserved_word_set_name));
        }
        reserved_word_stack_.push_back(*params.reserved_word_set_name);
      }

      // The largest-magnitude dynamic precedence anywhere in the production
      // wins, rather than the innermost one.
      if (std::abs(params.dynamic_precedence) >
          std::abs(production_.dynamic_precedence)) {
        production_.dynamic_precedence = params.dynamic_precedence;
      }

      TS_REF_ASSIGN_OR_RETURN(const bool did_push,
                              Apply(*metadata->rule, at_end));

      if (has_precedence) {
        precedence_stack_.pop_back();
        if (did_push && !at_end) {
          production_.steps.back().precedence = precedence_stack_.empty()
                                                    ? Precedence()
                                                    : precedence_stack_.back();
        }
      }
      if (has_associativity) {
        associativity_stack_.pop_back();
        if (did_push && !at_end) {
          production_.steps.back().associativity =
              associativity_stack_.empty()
                  ? std::nullopt
                  : std::optional<Associativity>(associativity_stack_.back());
        }
      }
      if (has_alias) alias_stack_.pop_back();
      if (has_field_name) field_name_stack_.pop_back();
      if (has_reserved) reserved_word_stack_.pop_back();
      return did_push;
    }

    if (const auto* symbol = std::get_if<ResolvedSymbol>(&rule.storage())) {
      production_.steps.push_back(ProductionStep{
          symbol->symbol_data,
          precedence_stack_.empty() ? Precedence() : precedence_stack_.back(),
          associativity_stack_.empty()
              ? std::nullopt
              : std::optional<Associativity>(associativity_stack_.back()),
          alias_stack_.empty() ? std::nullopt
                               : std::optional<Alias>(alias_stack_.back()),
          field_name_stack_.empty()
              ? std::nullopt
              : std::optional<std::string>(field_name_stack_.back()),
          reserved_word_stack_.empty()
              ? std::nullopt
              : std::optional<std::string>(reserved_word_stack_.back()),
      });
      return true;
    }

    // Blank contributes nothing, and no other rule shape can survive to here.
    return false;
  }

  Production production_;
  std::vector<std::string> reserved_word_set_names_;
  std::vector<Precedence> precedence_stack_;
  std::vector<Associativity> associativity_stack_;
  std::vector<Alias> alias_stack_;
  std::vector<std::string> field_name_stack_;
  std::vector<std::string> reserved_word_stack_;
};

bool SymbolIsUsed(const std::vector<SyntaxVariable>& variables,
                  SymbolData symbol) {
  for (const SyntaxVariable& variable : variables) {
    for (const Production& production : variable.productions) {
      for (const ProductionStep& step : production.steps) {
        if (step.symbol == symbol) return true;
      }
    }
  }
  return false;
}

absl::StatusOr<SyntaxGrammar> FlattenGrammar(ExtractedSyntaxGrammar grammar) {
  std::vector<std::string> set_names;
  set_names.reserve(grammar.reserved_word_sets.size());
  for (const ReservedWordSymbolSet& set : grammar.reserved_word_sets) {
    set_names.push_back(set.name);
  }

  RuleFlattener flattener(std::move(set_names));
  std::vector<SyntaxVariable> variables;
  variables.reserve(grammar.variables.size());
  for (const Variable& variable : grammar.variables) {
    TS_REF_ASSIGN_OR_RETURN(SyntaxVariable flattened,
                            flattener.FlattenVariable(variable));
    variables.push_back(std::move(flattened));
  }

  for (std::size_t i = 0; i < variables.size(); ++i) {
    const SymbolData symbol = NonTerminalSymbol(ToSymbolIndex(i));
    const bool used = SymbolIsUsed(variables, symbol);
    for (const Production& production : variables[i].productions) {
      if (used && production.steps.empty()) {
        return absl::InvalidArgumentError(absl::StrCat(
            "the rule `", variables[i].name,
            "` matches the empty string, which is only allowed for the start "
            "rule"));
      }
      const bool inlined =
          std::find(grammar.variables_to_inline.begin(),
                    grammar.variables_to_inline.end(),
                    symbol) != grammar.variables_to_inline.end();
      if (inlined) {
        for (const ProductionStep& step : production.steps) {
          if (step.symbol == symbol) {
            return absl::InvalidArgumentError(absl::StrCat(
                "rule `", variables[i].name,
                "` cannot be inlined because it refers to itself"));
          }
        }
      }
    }
  }

  SyntaxGrammar result;
  result.variables = std::move(variables);
  result.extra_symbols = std::move(grammar.extra_symbols);
  result.expected_conflicts = std::move(grammar.expected_conflicts);
  result.external_tokens = std::move(grammar.external_tokens);
  result.supertype_symbols = std::move(grammar.supertype_symbols);
  result.variables_to_inline = std::move(grammar.variables_to_inline);
  result.word_token = grammar.word_token;
  result.precedence_orderings = std::move(grammar.precedence_orderings);
  result.reserved_word_sets = std::move(grammar.reserved_word_sets);
  return result;
}

LexicalGrammar BuildLexicalGrammar(ExtractedLexicalGrammar extracted) {
  LexicalGrammar lexical;
  lexical.variables.reserve(extracted.variables.size());
  for (Variable& variable : extracted.variables) {
    lexical.variables.push_back(LexicalVariable{
        std::move(variable.name), variable.kind, std::move(variable.rule)});
  }
  lexical.separators = std::move(extracted.separators);
  return lexical;
}

// Substitutes each `inline` rule's productions into every step that references
// it, so the rule contributes no node of its own.
//
// tree-sitter does this lazily, keyed by (production, step), to keep the stored
// grammar small while still getting the parse-table effect. Doing it eagerly
// here yields the same language and the same trees for a fraction of the code;
// the cost is that a production count can multiply, which is measured rather
// than assumed. The inlined variables are left in place but unreferenced,
// which avoids renumbering every symbol.
absl::Status ProcessInlines(SyntaxGrammar& grammar) {
  const auto is_inlined = [&grammar](SymbolData symbol) {
    return symbol.IsNonTerminal() &&
           std::find(grammar.variables_to_inline.begin(),
                     grammar.variables_to_inline.end(),
                     symbol) != grammar.variables_to_inline.end();
  };
  if (grammar.variables_to_inline.empty()) return absl::OkStatus();

  for (std::size_t i = 0; i < grammar.variables.size(); ++i) {
    // An inlined variable's own productions are left alone; only references to
    // it are expanded.
    if (is_inlined(NonTerminalSymbol(ToSymbolIndex(i)))) continue;

    std::vector<Production> productions = grammar.variables[i].productions;
    // Bounded because a rule that refers to itself is rejected below, so each
    // substitution strictly reduces the number of inlined references left.
    std::size_t guard = 0;
    for (std::size_t p = 0; p < productions.size();) {
      std::size_t step = 0;
      while (step < productions[p].steps.size() &&
             !is_inlined(productions[p].steps[step].symbol)) {
        ++step;
      }
      if (step == productions[p].steps.size()) {
        ++p;
        continue;
      }
      if (++guard > 1000000) {
        return absl::InvalidArgumentError(absl::StrCat(
            "inlining `", grammar.variables[i].name, "` did not terminate"));
      }

      const Production original = productions[p];
      const ProductionStep replaced = original.steps[step];
      const SyntaxVariable& inlined = grammar.variables[replaced.symbol.index];

      std::vector<Production> expanded;
      expanded.reserve(inlined.productions.size());
      for (const Production& inner : inlined.productions) {
        Production copy = original;
        copy.steps.erase(copy.steps.begin() +
                         static_cast<std::ptrdiff_t>(step));
        copy.steps.insert(
            copy.steps.begin() + static_cast<std::ptrdiff_t>(step),
            inner.steps.begin(), inner.steps.end());

        // The reference's alias and field name apply to everything that
        // replaced it, and its precedence lands on the last inserted step.
        for (std::size_t k = 0; k < inner.steps.size(); ++k) {
          ProductionStep& inserted = copy.steps[step + k];
          if (replaced.alias.has_value()) inserted.alias = replaced.alias;
          if (replaced.field_name.has_value()) {
            inserted.field_name = replaced.field_name;
          }
        }
        if (!inner.steps.empty()) {
          ProductionStep& last = copy.steps[step + inner.steps.size() - 1];
          if (last.precedence.IsNone()) last.precedence = replaced.precedence;
          if (!last.associativity.has_value()) {
            last.associativity = replaced.associativity;
          }
        }
        if (std::abs(inner.dynamic_precedence) >
            std::abs(copy.dynamic_precedence)) {
          copy.dynamic_precedence = inner.dynamic_precedence;
        }
        expanded.push_back(std::move(copy));
      }

      productions.erase(productions.begin() + static_cast<std::ptrdiff_t>(p));
      productions.insert(productions.begin() + static_cast<std::ptrdiff_t>(p),
                         expanded.begin(), expanded.end());
      // Deliberately does not advance p: the substituted productions may
      // themselves reference an inlined rule.
    }

    // Distinct rule trees can collapse onto the same production once inlined.
    std::vector<Production> deduplicated;
    for (Production& production : productions) {
      if (std::find(deduplicated.begin(), deduplicated.end(), production) ==
          deduplicated.end()) {
        deduplicated.push_back(std::move(production));
      }
    }
    grammar.variables[i].productions = std::move(deduplicated);
  }
  return absl::OkStatus();
}

// Finds every symbol that *only* ever appears aliased and promotes its most
// common alias to a global default, then drops the now-redundant per-step
// aliases. Mirrors tree-sitter's extract_default_aliases.
DefaultAliasMap ExtractDefaultAliases(SyntaxGrammar& grammar) {
  struct SymbolStatus {
    // (alias, how many times it was seen), in first-seen order
    std::vector<std::pair<Alias, std::size_t>> aliases;
    bool appears_unaliased = false;
  };
  std::map<SymbolData, SymbolStatus> statuses;

  for (const SyntaxVariable& variable : grammar.variables) {
    for (const Production& production : variable.productions) {
      for (const ProductionStep& step : production.steps) {
        // An inlined symbol has no node of its own to alias.
        if (std::find(grammar.variables_to_inline.begin(),
                      grammar.variables_to_inline.end(),
                      step.symbol) != grammar.variables_to_inline.end()) {
          continue;
        }
        SymbolStatus& status = statuses[step.symbol];
        if (!step.alias.has_value()) {
          status.appears_unaliased = true;
          continue;
        }
        auto existing = std::find_if(
            status.aliases.begin(), status.aliases.end(),
            [&step](const auto& entry) { return entry.first == *step.alias; });
        if (existing != status.aliases.end()) {
          ++existing->second;
        } else {
          status.aliases.emplace_back(*step.alias, 1);
        }
      }
    }
  }
  // An extra appears on its own, outside any production, so it is never
  // aliased there.
  for (const SymbolData symbol : grammar.extra_symbols) {
    statuses[symbol].appears_unaliased = true;
  }

  DefaultAliasMap defaults;
  for (auto& [symbol, status] : statuses) {
    if (status.appears_unaliased || status.aliases.empty()) {
      status.aliases.clear();
      continue;
    }
    // Most frequent wins; ties go to the first seen.
    auto best = status.aliases.begin();
    for (auto iter = status.aliases.begin(); iter != status.aliases.end();
         ++iter) {
      if (iter->second > best->second) best = iter;
    }
    const Alias winner = best->first;
    status.aliases.assign(1, std::make_pair(winner, best->second));
    defaults.emplace(symbol, winner);
  }

  // A step aliased as its symbol's default no longer needs to say so.
  for (SyntaxVariable& variable : grammar.variables) {
    for (Production& production : variable.productions) {
      for (ProductionStep& step : production.steps) {
        if (!step.alias.has_value()) continue;
        auto entry = defaults.find(step.symbol);
        if (entry != defaults.end() && entry->second == *step.alias) {
          step.alias.reset();
        }
      }
    }
  }
  return defaults;
}

}  // namespace

absl::StatusOr<PreparedGrammar> NormalizeGrammar(const InputGrammar& input) {
  TS_REF_ASSIGN_OR_RETURN(InternedGrammar interned, InternSymbols(input));
  TS_REF_ASSIGN_OR_RETURN(auto extracted, ExtractTokens(std::move(interned)));
  ExtractedSyntaxGrammar expanded = ExpandRepeats(std::move(extracted.first));
  TS_REF_ASSIGN_OR_RETURN(SyntaxGrammar syntax,
                          FlattenGrammar(std::move(expanded)));

  absl::Status inline_status = ProcessInlines(syntax);
  if (!inline_status.ok()) return inline_status;

  PreparedGrammar prepared;
  prepared.name = input.name;
  prepared.default_aliases = ExtractDefaultAliases(syntax);
  prepared.syntax = std::move(syntax);
  prepared.lexical = BuildLexicalGrammar(std::move(extracted.second));
  return prepared;
}

std::vector<std::string> FindUnreachableVariables(
    const SyntaxGrammar& grammar) {
  if (grammar.variables.empty()) return {};

  std::vector<bool> reachable(grammar.variables.size(), false);
  std::vector<std::size_t> pending;

  const auto visit = [&](SymbolData symbol) {
    if (!symbol.IsNonTerminal() || symbol.index >= reachable.size()) return;
    if (reachable[symbol.index]) return;
    reachable[symbol.index] = true;
    pending.push_back(symbol.index);
  };

  // The start rule is a root, and so is anything reachable only via `extras`.
  visit(NonTerminalSymbol(0));
  for (SymbolData symbol : grammar.extra_symbols) visit(symbol);

  while (!pending.empty()) {
    const std::size_t index = pending.back();
    pending.pop_back();
    for (const Production& production : grammar.variables[index].productions) {
      for (const ProductionStep& step : production.steps) {
        visit(step.symbol);
      }
    }
  }

  std::vector<std::string> unreachable;
  for (std::size_t i = 0; i < grammar.variables.size(); ++i) {
    if (!reachable[i]) unreachable.push_back(grammar.variables[i].name);
  }
  return unreachable;
}

#undef TS_REF_ASSIGN_OR_RETURN
#undef TS_REF_ASSIGN_OR_RETURN_IMPL
#undef TS_REF_CONCAT
#undef TS_REF_CONCAT_INNER

}  // namespace ts_ref
