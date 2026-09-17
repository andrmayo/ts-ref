#ifndef TS_REF_PREPARED_GRAMMAR_H_
#define TS_REF_PREPARED_GRAMMAR_H_

// Phase 2.5 output types: the normalized form of a grammar.
//
// grammar_ir.h holds the *input* shape -- a recursive Rule tree that mirrors
// grammar.js. Those types are convenient to write grammars in and useless to
// generate tables from. This header holds the *output* shape that the table
// generator (Phase 5) and lexer generator (Phase 3) actually consume:
//
//   - a SyntaxGrammar of non-terminals, each a flat list of Productions, each
//     a flat vector of ProductionSteps. No nesting, no choice, no repeat.
//   - a LexicalGrammar of terminals, each a single token rule awaiting NFA
//     construction.
//
// Corresponds to the SyntaxGrammar / LexicalGrammar types in tree-sitter's
// crates/generate/src/grammars.rs.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ts_ref/grammar_ir.h"

namespace ts_ref {

// One symbol occurrence on the right-hand side of a production. The metadata
// that decorated the rule tree has been pushed down onto the individual steps
// here, so the table generator never has to walk a Rule again.
struct ProductionStep {
  SymbolData symbol;
  // Static precedence in force at this step, used to resolve shift/reduce
  // conflicts at table-generation time
  Precedence precedence;
  std::optional<Associativity> associativity;
  std::optional<Alias> alias;
  std::optional<std::string> field_name;
  // names a set in SyntaxGrammar::reserved_word_sets
  std::optional<std::string> reserved_word_set_name;

  bool operator==(const ProductionStep&) const = default;
};

// A single alternative for a non-terminal: A -> steps[0] steps[1] ...
struct Production {
  std::vector<ProductionStep> steps;
  // Runtime disambiguation hint, from prec.dynamic. Unlike the static
  // precedence on a step, this is carried on the whole production and compared
  // between surviving stack versions at acceptance (Phase 7)
  DynamicPrecedenceType dynamic_precedence = 0;

  bool operator==(const Production&) const = default;
};

struct SyntaxVariable {
  std::string name;
  VariableType kind;
  std::vector<Production> productions;
};

struct ExternalToken {
  std::string name;
  VariableType kind;
  // set when the grammar also defines an ordinary rule of the same name, which
  // the generated DFA lexer uses as a fallback when the external scanner
  // declines to produce this token
  std::optional<SymbolData> corresponding_internal_token;
};

// A reserved word set after interning: the tokens are terminal symbols now.
struct ReservedWordSymbolSet {
  std::string name;
  std::vector<SymbolData> symbols;
};

struct SyntaxGrammar {
  // variables[0] is the start rule
  std::vector<SyntaxVariable> variables;
  // tokens permitted between any two other tokens (whitespace, comments)
  std::vector<SymbolData> extra_symbols;
  // Recorded for diagnostics only. tree-sitter uses these to decide whether a
  // conflict is permissible; a GLR parser forks on any conflict, so it never
  // needs the author's permission
  std::vector<std::vector<SymbolData>> expected_conflicts;
  // in scan-priority order
  std::vector<ExternalToken> external_tokens;
  std::vector<SymbolData> supertype_symbols;
  std::vector<SymbolData> variables_to_inline;
  std::optional<SymbolData> word_token;
  std::vector<std::vector<PrecedenceEntry>> precedence_orderings;
  std::vector<ReservedWordSymbolSet> reserved_word_sets;
};

// A terminal. The rule here is always a token-level rule (a string literal, a
// pattern, or one of those wrapped in metadata) -- Phase 3 compiles it to an
// NFA and then a DFA.
struct LexicalVariable {
  std::string name;
  VariableType kind;
  Rule rule;
};

struct LexicalGrammar {
  std::vector<LexicalVariable> variables;
  // `extras` entries that were not plain symbol references, e.g. a bare
  // /\s/ pattern. These are skipped between tokens without producing a node
  std::vector<Rule> separators;
};

// A symbol that *always* appears under the same alias gets that alias applied
// globally rather than per-production. tree-sitter does this to shrink the
// parse table, but it also makes the alias survive in places that have no
// production context to consult.
using DefaultAliasMap = std::map<SymbolData, Alias>;

struct PreparedGrammar {
  std::string name;
  SyntaxGrammar syntax;
  LexicalGrammar lexical;
  DefaultAliasMap default_aliases;
};

}  // namespace ts_ref

#endif
