#ifndef TS_REF_TABLE_GENERATOR_H_
#define TS_REF_TABLE_GENERATOR_H_

// Phase 5: PreparedGrammar -> ParseTable.
//
// Builds the LR(1) item-set automaton, then turns each item set into a parse
// state. Static `prec`/`prec.left`/`prec.right` are applied at this point to
// eliminate the conflicts they can; everything left over is *retained* as a
// multi-action entry, which is what the GLR engine forks on.
//
// Corresponds to tree-sitter's build_tables/build_parse_table.rs and
// item_set_builder.rs. The deliberate divergence is at the end: where
// tree-sitter raises "Unresolved conflict" and stops unless the grammar
// whitelisted it, this records the conflict and carries on.

#include <vector>

#include "absl/status/statusor.h"

#include "ts_ref/dfa.h"
#include "ts_ref/lexer_generator.h"
#include "ts_ref/parser_table.h"
#include "ts_ref/prepared_grammar.h"

namespace ts_ref {

absl::StatusOr<ParseTable> BuildParseTable(const PreparedGrammar& grammar);

// The lex tables a parse table needs: one per distinct set of valid terminals
// across the parse states, with each state's `lex_state_id` pointing into
// `tables`. This is what makes tokenization context-dependent, and what stops
// a token like a string-body fragment from matching where it is not valid.
struct LexTableSet {
  std::vector<LexTable> tables;
};

absl::StatusOr<LexTableSet> BuildLexTables(const PreparedGrammar& grammar,
                                           const LexicalAutomaton& automaton,
                                           ParseTable& table);

}  // namespace ts_ref

#endif
