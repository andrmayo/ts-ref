#ifndef TS_REF_NFA_H_
#define TS_REF_NFA_H_

// The nondeterministic automaton that all of a grammar's token rules compile
// into, and the cursor used to run subset construction over it.
//
// One NFA holds every token in the grammar, not one per token: each token
// contributes its own chain of states plus an Accept state naming it, and a
// DFA state is then just a set of NFA states drawn from any number of tokens.
// That is what lets a single pass over the input decide which of many possible
// tokens matched.
//
// Corresponds to tree-sitter's crates/generate/src/nfa.rs.

#include <cstdint>
#include <variant>
#include <vector>

#include "ts_ref/character_set.h"

namespace ts_ref {

using NfaStateId = std::uint32_t;

// Consumes one code point from `characters` and moves to `next_state`.
struct NfaAdvance {
  CharacterSet characters;
  NfaStateId next_state = 0;
  // True when this transition belongs to the leading run of `extras` a token
  // may skip over, rather than to the token's own text. The distinction
  // matters when deciding whether to keep advancing past a completed token.
  bool is_separator = false;
  // Static token precedence in force here, from an integer prec() on the rule.
  std::int32_t precedence = 0;
};

// An epsilon branch to two states at once.
struct NfaSplit {
  NfaStateId left = 0;
  NfaStateId right = 0;
};

// Reaching this state means the token named by `variable_index` matched.
struct NfaAccept {
  std::size_t variable_index = 0;
  std::int32_t precedence = 0;
};

using NfaState = std::variant<NfaAdvance, NfaSplit, NfaAccept>;

struct Nfa {
  std::vector<NfaState> states;

  NfaStateId LastStateId() const {
    return static_cast<NfaStateId>(states.size() - 1);
  }
};

// One outgoing transition of a DFA state under construction: a set of
// characters, and the set of NFA states reached by consuming one of them.
struct NfaTransition {
  CharacterSet characters;
  bool is_separator = false;
  std::int32_t precedence = 0;
  // sorted and deduplicated
  std::vector<NfaStateId> states;
};

// A set of NFA states, closed under epsilon (Split) transitions. This is
// exactly one state of the DFA being built.
class NfaCursor {
 public:
  NfaCursor(const Nfa& nfa, std::vector<NfaStateId> states);

  // Recomputes the epsilon closure from a fresh set of states.
  void Reset(std::vector<NfaStateId> states);
  // Adopts an already-closed set, skipping the closure computation.
  void ForceReset(std::vector<NfaStateId> states);

  const std::vector<NfaStateId>& state_ids() const { return state_ids_; }

  // Every token that has matched by the time this set is reached, as
  // (variable index, precedence) pairs.
  std::vector<std::pair<std::size_t, std::int32_t>> Completions() const;

  // The outgoing transitions, split so that their character sets are pairwise
  // disjoint -- the DFA needs exactly one destination per character. Also
  // reports whether any underlying transition was a separator.
  std::vector<NfaTransition> Transitions(bool* any_separator = nullptr) const;

 private:
  void AddStates(std::vector<NfaStateId> new_states);

  const Nfa* nfa_;
  std::vector<NfaStateId> state_ids_;
};

}  // namespace ts_ref

#endif
