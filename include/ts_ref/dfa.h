#ifndef TS_REF_DFA_H_
#define TS_REF_DFA_H_

// The deterministic automaton the scanner actually runs, and the scanner
// itself.
//
// A LexTable is the compiled form of every token rule in the grammar: each
// state carries a set of character-indexed transitions, and optionally an
// accept action naming the token that has matched by the time the state is
// reached. Recognizing a token means walking this table from the start state,
// remembering the last accepting position seen, and backing up to it when no
// further transition applies -- the usual longest-match rule.
//
// Corresponds to tree-sitter's LexTable/LexState in
// crates/generate/src/tables.rs, and the runtime lexer in lib/src/lexer.c.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ts_ref/character_set.h"
#include "ts_ref/token.h"
#include "ts_ref/types.h"

namespace ts_ref {

using DfaStateId = std::uint32_t;

struct AdvanceAction {
  DfaStateId state = 0;
  // False when the character being consumed belongs to the leading run of
  // `extras` rather than to the token's own text, which is what lets a token's
  // span exclude the whitespace skipped before it.
  bool in_main_token = true;
};

struct LexState {
  // Pairwise disjoint character sets, so at most one matches any character.
  std::vector<std::pair<CharacterSet, AdvanceAction>> advance_actions;
  // The terminal recognized on reaching this state, if any. Index into the
  // LexicalGrammar's variables.
  std::optional<Symbol> accept_token;
  // Set when this state can also accept end-of-input.
  bool accepts_eof = false;
};

struct LexTable {
  std::vector<LexState> states;
  DfaStateId start_state = 0;
};

// The result of one scan.
struct LexResult {
  // The token recognized. Its span excludes any leading `extras`.
  Token token;
  // Bytes of `extras` skipped before the token began.
  ByteRange padding;
  // Set when no token could be recognized at this position.
  bool error = false;
  // Set at end of input. A separate flag because kEndOfInput is 0, which is
  // also a perfectly good terminal index -- the two are only distinguishable
  // out of band.
  bool at_eof = false;
};

// Runs a LexTable over a UTF-8 input buffer.
//
// This is the internal lexer of TASKS.md Phase 3. Phase 4's external scanner
// will sit in front of it: when a grammar declares externals that are valid in
// the current parse state, the hand-written scanner gets first refusal and this
// runs only as the fallback.
class Lexer {
 public:
  Lexer(const LexTable& table, std::string_view input)
      : table_{&table}, input_{input} {}

  // Byte offset of the next character to be scanned.
  std::uint32_t position() const { return position_; }
  void set_position(std::uint32_t position) { position_ = position; }

  bool AtEnd() const { return position_ >= input_.size(); }

  // Scans one token starting at the current position, advancing past it.
  // At end of input, returns a zero-width token with kind kEndOfInput.
  LexResult Next();

  // Row/column of a byte offset, for diagnostics.
  Point PointAt(std::uint32_t offset) const;

 private:
  // Decodes the code point at `offset`, reporting how many bytes it occupied.
  std::uint32_t DecodeAt(std::uint32_t offset, std::uint32_t* width) const;

  const LexTable* table_;
  std::string_view input_;
  std::uint32_t position_ = 0;
};

// Renders a LexTable for debugging, with `token_names` supplying the name of
// each terminal.
std::string DescribeLexTable(const LexTable& table,
                             const std::vector<std::string>& token_names);

}  // namespace ts_ref

#endif
