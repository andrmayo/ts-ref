#include "ts_ref/lexer_generator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "ts_ref/character_set.h"
#include "ts_ref/dfa.h"
#include "ts_ref/grammar_ir.h"
#include "ts_ref/nfa.h"
#include "ts_ref/prepared_grammar.h"
#include "ts_ref/regex.h"

namespace ts_ref {
namespace {

// Precedence implied by a token's shape. A string literal beats a pattern, so
// that a keyword "if" is preferred over an identifier rule that also matches
// "if"; token.immediate() adds one more, so an immediate token beats an
// otherwise identical non-immediate one.
std::int32_t ImplicitPrecedenceFor(const Rule& rule) {
  if (std::holds_alternative<StringLiteral>(rule.storage())) return 2;
  if (const auto* metadata = std::get_if<Metadata>(&rule.storage())) {
    const std::int32_t inner = ImplicitPrecedenceFor(*metadata->rule);
    return metadata->params.is_main_token ? inner + 1 : inner;
  }
  return 0;
}

// Precedence declared with an integer prec() at the top of a token rule,
// applied when the token completes.
std::int32_t CompletionPrecedenceFor(const Rule& rule) {
  if (const auto* metadata = std::get_if<Metadata>(&rule.storage())) {
    if (metadata->params.precedence.IsInteger()) {
      return metadata->params.precedence.AsInteger();
    }
  }
  return 0;
}

bool RuleIsEmpty(const Rule& rule) {
  if (const auto* literal = std::get_if<StringLiteral>(&rule.storage())) {
    return literal->value.empty();
  }
  if (const auto* metadata = std::get_if<Metadata>(&rule.storage())) {
    return RuleIsEmpty(*metadata->rule);
  }
  return false;
}

// Builds the NFA backwards: the accepting state is created first, and each
// construct is emitted so that it *leads into* the state already built. Doing
// it in this direction means a state's successor id is always known when the
// state is created, so no back-patching is needed except for the single split
// in a repetition.
class NfaBuilder {
 public:
  absl::Status ExpandRule(const Rule& rule, NfaStateId next_state) {
    if (const auto* literal = std::get_if<StringLiteral>(&rule.storage())) {
      // Reversed, because construction runs from the accepting end backwards.
      const std::vector<std::uint32_t> code_points = DecodeUtf8(literal->value);
      for (auto iter = code_points.rbegin(); iter != code_points.rend();
           ++iter) {
        PushAdvance(CharacterSet::FromChar(*iter), next_state);
        next_state = nfa.LastStateId();
      }
      pushed_ = !code_points.empty();
      return absl::OkStatus();
    }

    if (const auto* pattern = std::get_if<Pattern>(&rule.storage())) {
      absl::StatusOr<RegexNode> parsed =
          ParseRegex(pattern->pattern, pattern->flags);
      if (!parsed.ok()) return parsed.status();
      return ExpandRegex(*parsed, next_state);
    }

    if (const auto* choice = std::get_if<Choice>(&rule.storage())) {
      return ExpandAlternatives(choice->members, next_state);
    }

    if (const auto* sequence = std::get_if<Seq>(&rule.storage())) {
      bool any_pushed = false;
      for (auto iter = sequence->members.rbegin();
           iter != sequence->members.rend(); ++iter) {
        absl::Status status = ExpandRule(*iter, next_state);
        if (!status.ok()) return status;
        if (pushed_) {
          any_pushed = true;
          next_state = nfa.LastStateId();
        }
      }
      pushed_ = any_pushed;
      return absl::OkStatus();
    }

    if (const auto* repeat = std::get_if<Repeat>(&rule.storage())) {
      return ExpandOneOrMore(*repeat->rule, next_state);
    }

    if (const auto* metadata = std::get_if<Metadata>(&rule.storage())) {
      const bool has_precedence = metadata->params.precedence.IsInteger();
      if (has_precedence) {
        precedence_stack_.push_back(metadata->params.precedence.AsInteger());
      }
      absl::Status status = ExpandRule(*metadata->rule, next_state);
      if (has_precedence) precedence_stack_.pop_back();
      return status;
    }

    if (std::holds_alternative<Blank>(rule.storage())) {
      pushed_ = false;
      return absl::OkStatus();
    }

    return absl::InvalidArgumentError(
        "a token rule may only contain strings, patterns, and combinators");
  }

  Nfa nfa;
  bool is_separator = false;

 private:
  absl::Status ExpandRegex(const RegexNode& node, NfaStateId next_state) {
    if (std::holds_alternative<RegexEmpty>(node.storage())) {
      pushed_ = false;
      return absl::OkStatus();
    }

    if (const auto* character_class =
            std::get_if<RegexCharClass>(&node.storage())) {
      if (character_class->characters.IsEmpty()) {
        pushed_ = false;
        return absl::OkStatus();
      }
      PushAdvance(character_class->characters, next_state);
      pushed_ = true;
      return absl::OkStatus();
    }

    if (const auto* concat = std::get_if<RegexConcat>(&node.storage())) {
      bool any_pushed = false;
      for (auto iter = concat->members.rbegin(); iter != concat->members.rend();
           ++iter) {
        absl::Status status = ExpandRegex(*iter, next_state);
        if (!status.ok()) return status;
        if (pushed_) {
          any_pushed = true;
          next_state = nfa.LastStateId();
        }
      }
      pushed_ = any_pushed;
      return absl::OkStatus();
    }

    if (const auto* alternation =
            std::get_if<RegexAlternation>(&node.storage())) {
      return ExpandRegexAlternatives(alternation->members, next_state);
    }

    const auto& repetition = std::get<RegexRepetition>(node.storage());
    return ExpandRepetition(repetition, next_state);
  }

  absl::Status ExpandRepetition(const RegexRepetition& repetition,
                                NfaStateId next_state) {
    const std::uint32_t min = repetition.min;
    const std::optional<std::uint32_t> max = repetition.max;

    if (min == 0 && !max.has_value()) {
      return ExpandZeroOrMore(*repetition.sub, next_state);
    }
    if (min == 1 && !max.has_value()) {
      return ExpandRegexOneOrMore(*repetition.sub, next_state);
    }
    if (min == 0 && max.has_value() && *max == 1) {
      return ExpandZeroOrOne(*repetition.sub, next_state);
    }
    if (max.has_value() && *max == min) {
      return ExpandCount(*repetition.sub, min, next_state);
    }
    if (!max.has_value()) {
      // {n,} is n copies followed by zero-or-more.
      absl::Status status = ExpandZeroOrMore(*repetition.sub, next_state);
      if (!status.ok()) return status;
      if (pushed_) next_state = nfa.LastStateId();
      return ExpandCount(*repetition.sub, min, next_state);
    }
    // {n,m} is n mandatory copies preceded by (m - n) optional ones.
    bool any_pushed = false;
    for (std::uint32_t i = min; i < *max; ++i) {
      absl::Status status = ExpandZeroOrOne(*repetition.sub, next_state);
      if (!status.ok()) return status;
      if (pushed_) {
        any_pushed = true;
        next_state = nfa.LastStateId();
      }
    }
    absl::Status status = ExpandCount(*repetition.sub, min, next_state);
    if (!status.ok()) return status;
    pushed_ = pushed_ || any_pushed;
    return absl::OkStatus();
  }

  absl::Status ExpandCount(const RegexNode& node, std::uint32_t count,
                           NfaStateId next_state) {
    bool any_pushed = false;
    for (std::uint32_t i = 0; i < count; ++i) {
      absl::Status status = ExpandRegex(node, next_state);
      if (!status.ok()) return status;
      if (pushed_) {
        any_pushed = true;
        next_state = nfa.LastStateId();
      }
    }
    pushed_ = any_pushed;
    return absl::OkStatus();
  }

  absl::Status ExpandZeroOrOne(const RegexNode& node, NfaStateId next_state) {
    absl::Status status = ExpandRegex(node, next_state);
    if (!status.ok()) return status;
    if (pushed_) PushSplit(next_state);
    return absl::OkStatus();
  }

  absl::Status ExpandZeroOrMore(const RegexNode& node, NfaStateId next_state) {
    absl::Status status = ExpandRegexOneOrMore(node, next_state);
    if (!status.ok()) return status;
    // Zero-or-more is one-or-more with a bypass around it.
    if (pushed_) PushSplit(next_state);
    return absl::OkStatus();
  }

  // The one place back-patching is unavoidable: the loop's split has to name
  // the state that begins the repeated content, which does not exist until the
  // content has been built. A placeholder reserves the slot.
  absl::Status ExpandRegexOneOrMore(const RegexNode& node,
                                    NfaStateId next_state) {
    nfa.states.push_back(NfaAccept{});
    const NfaStateId split_state = nfa.LastStateId();
    absl::Status status = ExpandRegex(node, split_state);
    if (!status.ok()) return status;
    if (pushed_) {
      nfa.states[split_state] = NfaSplit{nfa.LastStateId(), next_state};
    } else {
      nfa.states.pop_back();
    }
    return absl::OkStatus();
  }

  absl::Status ExpandOneOrMore(const Rule& rule, NfaStateId next_state) {
    nfa.states.push_back(NfaAccept{});
    const NfaStateId split_state = nfa.LastStateId();
    absl::Status status = ExpandRule(rule, split_state);
    if (!status.ok()) return status;
    if (pushed_) {
      nfa.states[split_state] = NfaSplit{nfa.LastStateId(), next_state};
    } else {
      nfa.states.pop_back();
    }
    return absl::OkStatus();
  }

  absl::Status ExpandAlternatives(const std::vector<Rule>& members,
                                  NfaStateId next_state) {
    std::vector<NfaStateId> branch_states;
    for (const Rule& member : members) {
      absl::Status status = ExpandRule(member, next_state);
      if (!status.ok()) return status;
      branch_states.push_back(pushed_ ? nfa.LastStateId() : next_state);
    }
    JoinBranches(branch_states);
    pushed_ = true;
    return absl::OkStatus();
  }

  absl::Status ExpandRegexAlternatives(const std::vector<RegexNode>& members,
                                       NfaStateId next_state) {
    std::vector<NfaStateId> branch_states;
    for (const RegexNode& member : members) {
      absl::Status status = ExpandRegex(member, next_state);
      if (!status.ok()) return status;
      branch_states.push_back(pushed_ ? nfa.LastStateId() : next_state);
    }
    JoinBranches(branch_states);
    pushed_ = true;
    return absl::OkStatus();
  }

  // Chains a split per branch, so that entering the chain reaches all of them.
  void JoinBranches(std::vector<NfaStateId> branch_states) {
    std::sort(branch_states.begin(), branch_states.end());
    branch_states.erase(std::unique(branch_states.begin(), branch_states.end()),
                        branch_states.end());
    // The most recently built branch is already reachable as the chain's tail,
    // so it needs no split of its own.
    branch_states.erase(std::remove(branch_states.begin(), branch_states.end(),
                                    nfa.LastStateId()),
                        branch_states.end());
    for (const NfaStateId branch_state : branch_states) {
      PushSplit(branch_state);
    }
  }

  void PushAdvance(CharacterSet characters, NfaStateId next_state) {
    nfa.states.push_back(
        NfaAdvance{std::move(characters), next_state, is_separator,
                   precedence_stack_.empty() ? 0 : precedence_stack_.back()});
  }

  void PushSplit(NfaStateId state) {
    nfa.states.push_back(NfaSplit{state, nfa.LastStateId()});
  }

  static std::vector<std::uint32_t> DecodeUtf8(const std::string& text) {
    std::vector<std::uint32_t> code_points;
    for (std::size_t i = 0; i < text.size();) {
      const auto byte = static_cast<unsigned char>(text[i]);
      std::size_t width = 1;
      std::uint32_t value = byte;
      if ((byte & 0xE0) == 0xC0) {
        width = 2;
        value = byte & 0x1F;
      } else if ((byte & 0xF0) == 0xE0) {
        width = 3;
        value = byte & 0x0F;
      } else if ((byte & 0xF8) == 0xF0) {
        width = 4;
        value = byte & 0x07;
      }
      if (width > 1 && i + width <= text.size()) {
        for (std::size_t k = 1; k < width; ++k) {
          value =
              (value << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
      } else {
        width = 1;
        value = byte;
      }
      code_points.push_back(value);
      i += width;
    }
    return code_points;
  }

  // Whether the most recent expansion emitted any state at all. A rule that
  // matches only the empty string emits none, and its caller must then leave
  // `next_state` untouched rather than chaining onto a state never created.
  bool pushed_ = false;
  std::vector<std::int32_t> precedence_stack_;
};

// Between two tokens a grammar may allow any run of `extras`, so each token's
// states are preceded by a loop over the separator rules. An immediate token
// (token.immediate) opts out: it must start exactly where the previous token
// ended.
Rule BuildSeparatorRule(const std::vector<Rule>& separators) {
  if (separators.empty()) return Rule(Blank{});
  std::vector<Rule> alternatives = separators;
  // The blank alternative is what makes the repetition zero-or-more.
  alternatives.push_back(Rule(Blank{}));
  return Rule(Repeat(Rule(Choice(std::move(alternatives)))));
}

bool IsImmediateToken(const Rule& rule) {
  if (const auto* metadata = std::get_if<Metadata>(&rule.storage())) {
    return metadata->params.is_main_token;
  }
  return false;
}

// Decides which of two tokens wins when both match the same text: higher
// declared precedence first, then higher implicit precedence (a literal beats
// a pattern), then the earlier-declared token.
bool PreferToken(const LexicalAutomaton& automaton, std::size_t left_index,
                 std::int32_t left_precedence, std::size_t right_index,
                 std::int32_t right_precedence) {
  if (left_precedence != right_precedence) {
    return left_precedence > right_precedence;
  }
  const std::int32_t left_implicit = automaton.implicit_precedences[left_index];
  const std::int32_t right_implicit =
      automaton.implicit_precedences[right_index];
  if (left_implicit != right_implicit) return left_implicit > right_implicit;
  return left_index < right_index;
}

// Decides whether to keep advancing past a token that has already completed.
// Continuing is what implements longest-match; the exceptions are what stop a
// completed token from being swallowed by trailing separators.
bool PreferTransition(const NfaTransition& transition,
                      std::int32_t completed_precedence,
                      bool has_separator_transitions,
                      const std::vector<std::size_t>& transition_variables,
                      std::size_t completed_index) {
  if (transition.precedence < completed_precedence) return false;
  if (transition.precedence == completed_precedence) {
    if (transition.is_separator) return false;
    if (has_separator_transitions &&
        std::find(transition_variables.begin(), transition_variables.end(),
                  completed_index) == transition_variables.end()) {
      return false;
    }
  }
  return true;
}

// Which tokens the given NFA states could still go on to complete.
std::vector<std::size_t> VariablesForStates(
    const LexicalAutomaton& automaton, const std::vector<NfaStateId>& states) {
  std::vector<std::size_t> variables;
  for (const NfaStateId state : states) {
    // A token owns every state from its start state down to the previous
    // token's start state, because construction appends them contiguously.
    for (std::size_t i = 0; i < automaton.start_states.size(); ++i) {
      const NfaStateId start = automaton.start_states[i];
      const NfaStateId previous_end =
          i == 0 ? 0 : automaton.start_states[i - 1] + 1;
      if (state <= start && state >= previous_end) {
        variables.push_back(i);
        break;
      }
    }
  }
  std::sort(variables.begin(), variables.end());
  variables.erase(std::unique(variables.begin(), variables.end()),
                  variables.end());
  return variables;
}

class LexTableBuilder {
 public:
  LexTableBuilder(const LexicalAutomaton& automaton,
                  const LexicalGrammar& grammar)
      : automaton_{&automaton},
        grammar_{&grammar},
        cursor_{automaton.nfa, {}} {}

  LexTable Build(const std::vector<Symbol>& token_indices, bool eof_valid) {
    std::vector<NfaStateId> start_states;
    for (const Symbol token : token_indices) {
      if (token < automaton_->start_states.size()) {
        start_states.push_back(automaton_->start_states[token]);
      }
    }
    // With no tokens there is nothing to carry the leading separator loop, so
    // splice it in explicitly; otherwise trailing whitespace before EOF cannot
    // be skipped.
    if (start_states.empty() && automaton_->has_separator_chain) {
      start_states.push_back(automaton_->separator_start_state);
    }

    table_.start_state = AddState(std::move(start_states), eof_valid).first;
    while (!queue_.empty()) {
      QueueEntry entry = std::move(queue_.front());
      queue_.pop_front();
      PopulateState(entry.state_id, std::move(entry.nfa_states),
                    entry.eof_valid);
    }
    return std::move(table_);
  }

 private:
  struct QueueEntry {
    DfaStateId state_id;
    std::vector<NfaStateId> nfa_states;
    bool eof_valid;
  };

  // Returns the DFA state for this NFA state set, creating it if new. The set
  // (after epsilon closure) *is* the state's identity, which is what makes
  // subset construction terminate.
  std::pair<DfaStateId, bool> AddState(std::vector<NfaStateId> nfa_states,
                                       bool eof_valid) {
    cursor_.Reset(std::move(nfa_states));
    const auto key = std::make_pair(cursor_.state_ids(), eof_valid);
    auto existing = state_ids_by_nfa_states_.find(key);
    if (existing != state_ids_by_nfa_states_.end()) {
      return {existing->second, false};
    }
    const auto state_id = static_cast<DfaStateId>(table_.states.size());
    table_.states.emplace_back();
    state_ids_by_nfa_states_.emplace(key, state_id);
    queue_.push_back(QueueEntry{state_id, key.first, eof_valid});
    return {state_id, true};
  }

  void PopulateState(DfaStateId state_id, std::vector<NfaStateId> nfa_states,
                     bool eof_valid) {
    cursor_.ForceReset(std::move(nfa_states));

    // Of the tokens complete here, keep only the winner.
    std::optional<std::pair<std::size_t, std::int32_t>> completion;
    for (const auto& [index, precedence] : cursor_.Completions()) {
      // The separator chain's sentinel is not a token and must never be
      // reported as one.
      if (index >= grammar_->variables.size()) continue;
      if (completion.has_value() &&
          PreferToken(*automaton_, completion->first, completion->second, index,
                      precedence)) {
        continue;
      }
      completion = std::make_pair(index, precedence);
    }

    bool has_separator_transitions = false;
    const std::vector<NfaTransition> transitions =
        cursor_.Transitions(&has_separator_transitions);

    for (const NfaTransition& transition : transitions) {
      if (completion.has_value()) {
        const std::vector<std::size_t> variables =
            VariablesForStates(*automaton_, transition.states);
        if (!PreferTransition(transition, completion->second,
                              has_separator_transitions, variables,
                              completion->first)) {
          continue;
        }
      }
      const auto [next_state_id, _] =
          AddState(transition.states, eof_valid && transition.is_separator);
      table_.states[state_id].advance_actions.emplace_back(
          transition.characters,
          AdvanceAction{next_state_id, !transition.is_separator});
    }

    if (completion.has_value()) {
      table_.states[state_id].accept_token =
          static_cast<Symbol>(completion->first);
    }
    // An empty NFA state set means nothing can follow, which at end of input
    // is exactly the end-of-input token.
    table_.states[state_id].accepts_eof =
        eof_valid || cursor_.state_ids().empty();
  }

  const LexicalAutomaton* automaton_;
  const LexicalGrammar* grammar_;
  NfaCursor cursor_;
  LexTable table_;
  std::deque<QueueEntry> queue_;
  std::map<std::pair<std::vector<NfaStateId>, bool>, DfaStateId>
      state_ids_by_nfa_states_;
};

}  // namespace

absl::StatusOr<LexicalAutomaton> BuildLexicalAutomaton(
    const LexicalGrammar& grammar) {
  NfaBuilder builder;
  LexicalAutomaton automaton;
  const Rule separator_rule = BuildSeparatorRule(grammar.separators);

  for (std::size_t i = 0; i < grammar.variables.size(); ++i) {
    const LexicalVariable& variable = grammar.variables[i];
    if (RuleIsEmpty(variable.rule)) {
      return absl::InvalidArgumentError(absl::StrCat(
          "the token `", variable.name, "` matches the empty string"));
    }

    // The accepting state goes down first; everything else is built to lead
    // into it.
    builder.nfa.states.push_back(
        NfaAccept{i, CompletionPrecedenceFor(variable.rule)});
    builder.is_separator = false;
    absl::Status status =
        builder.ExpandRule(variable.rule, builder.nfa.LastStateId());
    if (!status.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("in token `", variable.name, "`: ", status.message()));
    }

    // Then the optional run of leading separators, so that entering this
    // token's start state can skip whitespace before the token proper.
    if (!IsImmediateToken(variable.rule)) {
      builder.is_separator = true;
      status = builder.ExpandRule(separator_rule, builder.nfa.LastStateId());
      if (!status.ok()) return status;
    }

    automaton.start_states.push_back(builder.nfa.LastStateId());
    automaton.implicit_precedences.push_back(
        ImplicitPrecedenceFor(variable.rule));
  }

  // The separator-only chain, accepting a sentinel that is not a real token.
  // BuildLexTable seeds a state with this when it has no tokens of its own.
  if (!grammar.separators.empty()) {
    builder.nfa.states.push_back(NfaAccept{grammar.variables.size(), 0});
    builder.is_separator = true;
    absl::Status status =
        builder.ExpandRule(separator_rule, builder.nfa.LastStateId());
    if (!status.ok()) return status;
    automaton.separator_start_state = builder.nfa.LastStateId();
    automaton.has_separator_chain = true;
  }

  automaton.nfa = std::move(builder.nfa);
  return automaton;
}

LexTable BuildLexTable(const LexicalAutomaton& automaton,
                       const LexicalGrammar& grammar,
                       const std::vector<Symbol>& token_indices,
                       bool eof_valid) {
  LexTableBuilder builder(automaton, grammar);
  return builder.Build(token_indices, eof_valid);
}

LexTable BuildMainLexTable(const LexicalAutomaton& automaton,
                           const LexicalGrammar& grammar) {
  std::vector<Symbol> all_tokens;
  all_tokens.reserve(grammar.variables.size());
  for (std::size_t i = 0; i < grammar.variables.size(); ++i) {
    all_tokens.push_back(static_cast<Symbol>(i));
  }
  return BuildLexTable(automaton, grammar, all_tokens, /*eof_valid=*/true);
}

}  // namespace ts_ref
