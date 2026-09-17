#ifndef TS_REF_LEXER_GENERATOR_H_
#define TS_REF_LEXER_GENERATOR_H_

// Phase 3: LexicalGrammar -> NFA -> DFA.
//
// Two steps, separable because the NFA is also what a per-parse-state lexer
// would be built from later:
//
//   BuildLexicalAutomaton  compiles every token rule into one shared NFA,
//                          recording where each token's states begin.
//                          (tree-sitter: prepare_grammar/expand_tokens.rs)
//   BuildLexTable          runs subset construction over a chosen set of
//                          tokens to produce the DFA the scanner executes.
//                          (tree-sitter: build_tables/build_lex_table.rs)
//
// BuildLexTable takes a token subset because that is what Phase 5 will need:
// tree-sitter builds a separate lex state per parse state, covering only the
// tokens valid there, which is what makes context-dependent tokenization work.
// Until the parse table exists, BuildMainLexTable over every token is the
// useful entry point.

#include <cstdint>
#include <vector>

#include "absl/status/statusor.h"

#include "ts_ref/dfa.h"
#include "ts_ref/nfa.h"
#include "ts_ref/prepared_grammar.h"

namespace ts_ref {

// The NFA for a whole grammar, plus the per-token information the DFA builder
// needs to resolve ties between tokens that match the same text.
struct LexicalAutomaton {
  Nfa nfa;
  // Parallel to LexicalGrammar::variables.
  std::vector<NfaStateId> start_states;
  // Precedence implied by a token's shape rather than declared: a string
  // literal outranks a pattern, so a keyword written "if" wins over an
  // identifier /[a-z]+/ that also matches it.
  std::vector<std::int32_t> implicit_precedences;

  // Start state of a chain that matches nothing but a run of separators.
  // Ordinarily separators are skipped by the leading loop built into each
  // token, but a parse state whose only valid lookahead is end-of-input has no
  // tokens at all -- and so, without this, no way to skip trailing whitespace
  // before reaching EOF.
  NfaStateId separator_start_state = 0;
  bool has_separator_chain = false;
};

absl::StatusOr<LexicalAutomaton> BuildLexicalAutomaton(
    const LexicalGrammar& grammar);

// Subset construction over the tokens named by `token_indices`.
LexTable BuildLexTable(const LexicalAutomaton& automaton,
                       const LexicalGrammar& grammar,
                       const std::vector<Symbol>& token_indices,
                       bool eof_valid = true);

// Convenience: every token in the grammar.
LexTable BuildMainLexTable(const LexicalAutomaton& automaton,
                           const LexicalGrammar& grammar);

}  // namespace ts_ref

#endif
