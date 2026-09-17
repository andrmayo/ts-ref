#ifndef TS_REF_PARSER_ENGINE_H_
#define TS_REF_PARSER_ENGINE_H_

// Phase 6: the GLR engine that drives the Graph-Structured Stack.
//
// This is what turns the parse table into an actual parse. The loop is the
// usual LR one -- shift, reduce, accept -- with the one difference that makes
// it GLR: when a (state, lookahead) cell holds several actions, every one of
// them is taken, each on its own stack version. Versions that turn out to be
// wrong hit a cell with no action and are pruned; versions that converge on
// the same (state, position) are merged back together, so the shared prefix is
// parsed once rather than once per alternative.
//
// Corresponds to tree-sitter's ts_parser__advance in lib/src/parser.c, minus
// the incremental-reuse and error-recovery machinery this project leaves out
// of scope.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

#include "ts_ref/external_scanner.h"
#include "ts_ref/lexer_generator.h"
#include "ts_ref/node.h"
#include "ts_ref/parser_table.h"
#include "ts_ref/prepared_grammar.h"
#include "ts_ref/table_generator.h"

namespace ts_ref {

// Everything needed to parse with a grammar, built once and reused per input.
struct CompiledGrammar {
  PreparedGrammar grammar;
  LexicalAutomaton automaton;
  ParseTable parse_table;
  LexTableSet lex_tables;
  // Null unless the grammar ships a scanner.c. Shared because a
  // CompiledGrammar is copied around and the loaded library is not copyable.
  std::shared_ptr<ExternalScanner> scanner;
};

// `grammar_path` is where the grammar was loaded from; a `scanner.c` beside it
// is compiled and loaded. Pass an empty path to skip external scanner support.
absl::StatusOr<CompiledGrammar> CompileGrammar(
    const InputGrammar& grammar,
    const std::filesystem::path& grammar_path = {});

struct ParseResult {
  // The chosen parse: highest dynamic precedence, then the deterministic
  // earlier-declared-rule order. Null if nothing parsed.
  std::shared_ptr<CSTNode> tree;

  // Every stack version that reached acceptance. More than one means the input
  // is genuinely ambiguous under this grammar; `tree` is the one selected.
  // Kept because seeing the alternatives a grammar admits is most of the value
  // of having built a GLR parser.
  std::vector<std::shared_ptr<CSTNode>> trees;

  // How many versions existed at the widest point, and how many were pruned.
  // Purely diagnostic, but it is the most direct evidence that forking is
  // happening at all.
  std::size_t max_versions = 1;
  std::size_t forks = 0;
  std::size_t merges = 0;
  // How many times two readings of the same span were resolved down to one.
  std::size_t ambiguities_resolved = 0;

  // True when parsing stopped because it exhausted its step budget rather
  // than because every version finished or died.
  bool hit_step_limit = false;

  bool Succeeded() const { return !trees.empty(); }
};

// Parses `input` against an already-compiled grammar.
absl::StatusOr<ParseResult> Parse(const CompiledGrammar& compiled,
                                  std::string_view input);

// CSTNode stores a single Symbol, so terminals and non-terminals share one
// numbering: 0 is end-of-input, then terminals, then non-terminals. These
// convert in both directions.
Symbol FlatTerminalSymbol(const PreparedGrammar& grammar, Symbol index);
Symbol FlatNonTerminalSymbol(const PreparedGrammar& grammar, Symbol index);
Symbol FlatExternalSymbol(const PreparedGrammar& grammar, Symbol index);
// The inverse: recovers the kind and index a flat symbol came from.
std::optional<SymbolData> SymbolDataForFlatSymbol(
    const PreparedGrammar& grammar, Symbol symbol);
std::string FlatSymbolName(const PreparedGrammar& grammar, Symbol symbol);

// tree.toString()'s equivalent: the parse tree as an S-expression, with hidden
// and auxiliary nodes elided the way tree-sitter elides them. By default only
// named nodes appear, matching the convention a grammar's own corpus tests are
// written in; `include_anonymous` also prints anonymous tokens, as quoted text.
std::string ToSExpression(const PreparedGrammar& grammar,
                          const std::shared_ptr<CSTNode>& tree,
                          std::string_view input,
                          bool include_anonymous = false);

}  // namespace ts_ref

#endif
