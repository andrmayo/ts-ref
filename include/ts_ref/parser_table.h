#ifndef TS_REF_PARSER_TABLE_H_
#define TS_REF_PARSER_TABLE_H_

// The LR automaton the GLR engine runs: ACTION and GOTO, as one table of
// states.
//
// The one structural difference from an ordinary LR table is the point of the
// whole project: a terminal entry holds a *vector* of actions, not one action.
// Static precedence and associativity still eliminate the conflicts they can,
// but whatever survives is kept rather than resolved arbitrarily or rejected.
// At runtime the engine forks a stack version per surviving action, so a
// genuinely ambiguous grammar is parsed rather than diagnosed.
//
// Corresponds to tree-sitter's ParseTable/ParseState in
// crates/generate/src/tables.rs, minus the error-recovery machinery this
// project leaves out of scope.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ts_ref/grammar_ir.h"
#include "ts_ref/prepared_grammar.h"
#include "ts_ref/types.h"

namespace ts_ref {

using ParseStateId = std::uint32_t;

enum class ParseActionKind {
  // Consume the lookahead token and move to `state`.
  Shift,
  // Pop `child_count` entries, build a node for `symbol`, then goto.
  Reduce,
  // The start symbol has been recognized and the input is exhausted.
  Accept,
};

struct ParseAction {
  ParseActionKind kind = ParseActionKind::Shift;

  // Shift only.
  ParseStateId state = 0;
  // Set when the shift belongs to a generated repetition rule. Such a conflict
  // is the intentional ambiguity of `repeat()`, not a real one, so the engine
  // can prefer it without forking.
  bool is_repetition = false;

  // Reduce only.
  SymbolData symbol{};
  std::uint32_t child_count = 0;
  DynamicPrecedenceType dynamic_precedence = 0;
  // Index into SyntaxVariable::productions, so the engine can recover the
  // aliases and field names of the production being reduced.
  std::uint32_t production_id = 0;

  bool operator==(const ParseAction&) const = default;
};

// The actions available for one lookahead token. More than one means the
// engine forks.
struct ParseTableEntry {
  std::vector<ParseAction> actions;

  bool IsConflict() const { return actions.size() > 1; }
};

struct ParseState {
  // Keyed by terminal or external symbol; ordered so that table dumps and
  // generated output are reproducible.
  std::map<SymbolData, ParseTableEntry> terminal_entries;
  // Keyed by non-terminal symbol: where to go after a reduction.
  std::map<SymbolData, ParseStateId> nonterminal_entries;
  // The lex table to scan with while in this state. This is what makes
  // tokenization context-dependent: only the tokens valid here are considered.
  std::uint32_t lex_state_id = 0;
  // True when this state completes a non-terminal extra rule. Such a state
  // reduces without consulting the lexer at all -- the rule ends wherever it
  // can no longer continue, not at a particular lookahead.
  bool is_end_of_nonterminal_extra = false;
  // Terminals that are valid lookaheads here, which is exactly the token set
  // the state's lex table is built from.
  std::vector<SymbolData> valid_terminals;
  // External token indices valid here. This is the bitset handed to a
  // scanner's scan(), and an empty list means the scanner is not consulted in
  // this state at all.
  std::vector<std::uint32_t> valid_externals;
};

// A conflict that static precedence could not remove. Not an error: these are
// the states where the runtime will fork. Reported so that a grammar author
// can see where ambiguity actually lives.
struct TableConflict {
  ParseStateId state = 0;
  SymbolData lookahead{};
  // The rules whose interpretations compete here.
  std::vector<SymbolData> rule_symbols;
  std::vector<std::string> rule_names;
  // True when the grammar's `conflicts: $ => [...]` declared this exact set of
  // rules. tree-sitter requires that declaration before it will keep a
  // conflict; here it changes nothing operationally, but it does separate the
  // ambiguity an author knew about from the kind that is a surprise.
  bool is_expected = false;
  std::size_t shift_count = 0;
  std::size_t reduce_count = 0;
};

// A non-terminal `extras` rule -- one that spans several tokens, like a macro
// or a block comment with structure -- cannot be shifted as a single token. It
// gets its own entry state, reached by shifting the terminal its productions
// begin with, and every ordinary state gains a shift into it.
struct NonTerminalExtraEntry {
  SymbolData first_terminal;
  ParseStateId state = 0;
};

struct ParseTable {
  std::vector<ParseState> states;
  // Entry points for non-terminal extras, keyed by their first terminal.
  std::vector<NonTerminalExtraEntry> non_terminal_extra_states;
  // The non-terminal symbols declared as extras. A reduction to one of these
  // has no goto anywhere; it is pushed as an extra instead, leaving the state
  // unchanged.
  std::vector<SymbolData> non_terminal_extras;
  // Conflicts left for the runtime to resolve by forking.
  std::vector<TableConflict> conflicts;

  ParseStateId start_state = 0;
};

std::string DescribeParseTable(const ParseTable& table,
                               const PreparedGrammar& grammar);

// A one-line-per-conflict summary, for reporting where a grammar is ambiguous.
std::string DescribeConflicts(const ParseTable& table,
                              const PreparedGrammar& grammar);

}  // namespace ts_ref

#endif
