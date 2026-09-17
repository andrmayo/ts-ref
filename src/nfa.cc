#include "ts_ref/nfa.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

#include "ts_ref/character_set.h"

namespace ts_ref {

NfaCursor::NfaCursor(const Nfa& nfa, std::vector<NfaStateId> states)
    : nfa_{&nfa} {
  AddStates(std::move(states));
}

void NfaCursor::Reset(std::vector<NfaStateId> states) {
  state_ids_.clear();
  AddStates(std::move(states));
}

void NfaCursor::ForceReset(std::vector<NfaStateId> states) {
  state_ids_ = std::move(states);
}

// Computes the epsilon closure: a Split is not a real state, so it is replaced
// by the states it branches to, transitively. Everything else is kept, in
// sorted order, so that two closures of the same set compare equal -- which is
// what lets the DFA builder recognize a state it has already created.
void NfaCursor::AddStates(std::vector<NfaStateId> new_states) {
  // Index-based rather than iterator-based because the loop appends to the
  // very vector it walks, as each Split expands into its branches.
  for (std::size_t i = 0; i < new_states.size(); ++i) {
    const NfaStateId state_id = new_states[i];
    const NfaState& state = nfa_->states[state_id];
    if (const auto* split = std::get_if<NfaSplit>(&state)) {
      const bool has_left = std::find(new_states.begin(), new_states.end(),
                                      split->left) != new_states.end();
      const bool has_right = std::find(new_states.begin(), new_states.end(),
                                       split->right) != new_states.end();
      if (!has_left) new_states.push_back(split->left);
      if (!has_right) new_states.push_back(split->right);
      continue;
    }
    auto position =
        std::lower_bound(state_ids_.begin(), state_ids_.end(), state_id);
    if (position == state_ids_.end() || *position != state_id) {
      state_ids_.insert(position, state_id);
    }
  }
}

std::vector<std::pair<std::size_t, std::int32_t>> NfaCursor::Completions()
    const {
  std::vector<std::pair<std::size_t, std::int32_t>> completions;
  for (const NfaStateId state_id : state_ids_) {
    if (const auto* accept = std::get_if<NfaAccept>(&nfa_->states[state_id])) {
      completions.emplace_back(accept->variable_index, accept->precedence);
    }
  }
  return completions;
}

std::vector<NfaTransition> NfaCursor::Transitions(bool* any_separator) const {
  if (any_separator != nullptr) *any_separator = false;
  std::vector<NfaTransition> result;

  for (const NfaStateId state_id : state_ids_) {
    const auto* advance = std::get_if<NfaAdvance>(&nfa_->states[state_id]);
    if (advance == nullptr) continue;
    if (any_separator != nullptr && advance->is_separator) {
      *any_separator = true;
    }

    // Fold this transition into the accumulated set, splitting wherever it
    // overlaps an existing one so that everything stays pairwise disjoint. A
    // character shared by two transitions must lead to *both* destination
    // states, which is precisely how nondeterminism becomes a DFA state.
    CharacterSet characters = advance->characters;
    for (std::size_t i = 0; i < result.size() && !characters.IsEmpty(); ++i) {
      CharacterSet intersection =
          result[i].characters.RemoveIntersection(characters);
      if (intersection.IsEmpty()) continue;

      const bool existing_is_now_empty = result[i].characters.IsEmpty();
      std::vector<NfaStateId> states = result[i].states;
      auto position =
          std::lower_bound(states.begin(), states.end(), advance->next_state);
      if (position == states.end() || *position != advance->next_state) {
        states.insert(position, advance->next_state);
      }

      NfaTransition split{std::move(intersection),
                          result[i].is_separator && advance->is_separator,
                          std::max(result[i].precedence, advance->precedence),
                          std::move(states)};
      if (existing_is_now_empty) {
        // The existing entry was wholly consumed by the overlap, so replace it
        // rather than leaving an empty transition behind.
        result[i] = std::move(split);
      } else {
        // Appending is safe: the remainder left in `characters` is disjoint
        // from the piece just split off, so revisiting it later is a no-op.
        result.push_back(std::move(split));
      }
    }

    if (!characters.IsEmpty()) {
      result.push_back(NfaTransition{std::move(characters),
                                     advance->is_separator,
                                     advance->precedence,
                                     {advance->next_state}});
    }
  }

  // Transitions that agree on destination, separator status and precedence are
  // indistinguishable, so merge their character sets to keep the DFA small.
  for (std::size_t i = 0; i < result.size();) {
    bool merged = false;
    for (std::size_t j = 0; j < i; ++j) {
      if (result[j].states == result[i].states &&
          result[j].is_separator == result[i].is_separator &&
          result[j].precedence == result[i].precedence) {
        result[j].characters.Add(result[i].characters);
        result.erase(result.begin() + static_cast<std::ptrdiff_t>(i));
        merged = true;
        break;
      }
    }
    if (!merged) ++i;
  }

  // A deterministic order keeps generated tables reproducible across runs.
  std::sort(result.begin(), result.end(),
            [](const NfaTransition& left, const NfaTransition& right) {
              return left.characters < right.characters;
            });
  return result;
}

}  // namespace ts_ref
