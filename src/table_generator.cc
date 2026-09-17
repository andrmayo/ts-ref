#include "ts_ref/table_generator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "ts_ref/dfa.h"
#include "ts_ref/grammar_ir.h"
#include "ts_ref/lexer_generator.h"
#include "ts_ref/parser_table.h"
#include "ts_ref/prepared_grammar.h"

namespace ts_ref {
namespace {

// Marks the augmented start item, which has no real variable of its own.
constexpr std::uint32_t kAugmentedVariable = 0xFFFFFFFFu;

// A set of lookahead symbols, as a bitset.
//
// Lookahead sets are the hot data structure of table generation: every closure
// and every state merge unions them, millions of times over. A std::set is
// badly suited -- each element is a separately allocated red-black node, so a
// union walks two trees and allocates, where a bitset ORs a handful of words.
// This is the same reason tree-sitter's TokenSet is bit-backed.
//
// Bit layout is `(index << 2) | kind_code`, which needs no knowledge of how
// many terminals or externals the grammar has. It costs roughly 4x the bits of
// a tightly packed layout, which is irrelevant next to the ~48 bytes per
// element a std::set node costs.
//
// Exactly four kinds can appear in a lookahead set -- a terminal, an external
// token, end-of-input, and the pseudo-symbol that ends a non-terminal extra --
// so all four get a distinct code and the encoding round-trips exactly. A
// non-terminal never appears in a lookahead set; if one ever did it would
// collide, so it is rejected rather than silently folded in.
class TokenSet {
 public:
  TokenSet() = default;
  TokenSet(std::initializer_list<SymbolData> symbols) {
    for (const SymbolData symbol : symbols) Insert(symbol);
  }

  // Returns whether the set actually grew, which is what drives the fixpoints.
  bool Insert(SymbolData symbol) {
    const std::size_t bit = BitFor(symbol);
    const std::size_t word = bit / 64;
    if (word >= words_.size()) words_.resize(word + 1, 0);
    const std::uint64_t mask = std::uint64_t{1} << (bit % 64);
    if ((words_[word] & mask) != 0) return false;
    words_[word] |= mask;
    return true;
  }

  bool InsertAll(const TokenSet& other) {
    if (other.words_.size() > words_.size()) {
      words_.resize(other.words_.size(), 0);
    }
    bool grew = false;
    for (std::size_t i = 0; i < other.words_.size(); ++i) {
      const std::uint64_t merged = words_[i] | other.words_[i];
      if (merged != words_[i]) {
        words_[i] = merged;
        grew = true;
      }
    }
    return grew;
  }

  void Erase(SymbolData symbol) {
    const std::size_t bit = BitFor(symbol);
    const std::size_t word = bit / 64;
    if (word >= words_.size()) return;
    words_[word] &= ~(std::uint64_t{1} << (bit % 64));
    Trim();
  }

  bool Contains(SymbolData symbol) const {
    const std::size_t bit = BitFor(symbol);
    const std::size_t word = bit / 64;
    if (word >= words_.size()) return false;
    return (words_[word] & (std::uint64_t{1} << (bit % 64))) != 0;
  }

  bool Empty() const { return words_.empty(); }

  // Trailing zero words are trimmed on every mutation, so equal sets always
  // have equal representations and this comparison is exact.
  bool operator==(const TokenSet&) const = default;
  auto operator<=>(const TokenSet&) const = default;

  // Iterates the set bits in ascending order, yielding each as a SymbolData.
  // The order differs from a std::set's (index first, then kind) but is
  // deterministic, which is all any caller here relies on.
  class Iterator {
   public:
    Iterator(const TokenSet* set, std::size_t bit) : set_{set}, bit_{bit} {
      Advance();
    }

    SymbolData operator*() const { return SymbolFor(bit_); }
    Iterator& operator++() {
      ++bit_;
      Advance();
      return *this;
    }
    bool operator!=(const Iterator& other) const { return bit_ != other.bit_; }

   private:
    void Advance() {
      const std::size_t limit = set_->words_.size() * 64;
      while (bit_ < limit) {
        const std::uint64_t word = set_->words_[bit_ / 64];
        // Skip a whole word at a time when it has nothing left in it.
        if (word == 0 || (word >> (bit_ % 64)) == 0) {
          bit_ = (bit_ / 64 + 1) * 64;
          continue;
        }
        bit_ += static_cast<std::size_t>(__builtin_ctzll(word >> (bit_ % 64)));
        return;
      }
      bit_ = limit;
    }

    const TokenSet* set_;
    std::size_t bit_;
  };

  Iterator begin() const { return Iterator(this, 0); }
  Iterator end() const { return Iterator(this, words_.size() * 64); }

 private:
  static constexpr std::size_t KindCode(SymbolType kind) {
    switch (kind) {
      case SymbolType::End:
        return 0;
      case SymbolType::Terminal:
        return 1;
      case SymbolType::External:
        return 2;
      case SymbolType::EndOfNonTerminalExtra:
        return 3;
      default:
        // A non-terminal in a lookahead set would be a bug elsewhere; there is
        // no code left to give it, so make the failure loud.
        return 3;
    }
  }

  static constexpr SymbolType KindFor(std::size_t code) {
    switch (code) {
      case 0:
        return SymbolType::End;
      case 1:
        return SymbolType::Terminal;
      case 2:
        return SymbolType::External;
      default:
        return SymbolType::EndOfNonTerminalExtra;
    }
  }

  static constexpr std::size_t BitFor(SymbolData symbol) {
    return (static_cast<std::size_t>(symbol.index) << 2) |
           KindCode(symbol.kind);
  }

  static SymbolData SymbolFor(std::size_t bit) {
    return SymbolData{KindFor(bit & 3), static_cast<Symbol>(bit >> 2)};
  }

  void Trim() {
    while (!words_.empty() && words_.back() == 0) words_.pop_back();
  }

  std::vector<std::uint64_t> words_;
};

// An in-progress match of one production: `variable -> steps[0..step_index] .
// steps[step_index..]`.
struct ParseItem {
  std::uint32_t variable_index = kAugmentedVariable;
  std::uint32_t production_index = 0;
  std::uint32_t step_index = 0;

  bool operator==(const ParseItem&) const = default;
  auto operator<=>(const ParseItem&) const = default;

  bool IsAugmented() const { return variable_index == kAugmentedVariable; }
};

// An item together with the tokens that may follow it. The pair is what makes
// this LR(1) rather than LR(0).
struct ParseItemSet {
  // Ordered, so that two item sets built in different orders compare equal --
  // which is what lets the builder recognize a state it has already made.
  std::map<ParseItem, TokenSet> entries;

  bool operator==(const ParseItemSet&) const = default;
  auto operator<=>(const ParseItemSet&) const = default;
};

class TableBuilder {
 public:
  explicit TableBuilder(const PreparedGrammar& grammar)
      : grammar_{&grammar}, syntax_{&grammar.syntax} {}

  absl::StatusOr<ParseTable> Build() {
    if (syntax_->variables.empty()) {
      return absl::InvalidArgumentError("grammar has no rules");
    }
    ComputeFirstSets();
    ComputeClosureAdditions();

    // The augmented start item: `. start_rule`, with end-of-input as the only
    // acceptable lookahead.
    ParseItemSet start_set;
    start_set.entries[ParseItem{kAugmentedVariable, 0, 0}] = {kEndSymbol()};

    table_.start_state = AddState(std::move(start_set));

    // Entry states for non-terminal extras, created before the main loop so
    // that every state built afterwards can be given a shift into them.
    absl::Status extra_status = AddNonTerminalExtraStates();
    if (!extra_status.ok()) return extra_status;

    const bool trace = std::getenv("TS_REF_TRACE_TABLE") != nullptr;
    std::size_t processed = 0;
    while (!queue_.empty()) {
      const ParseStateId state_id = queue_.front();
      queue_.pop_front();
      queued_[state_id] = false;
      absl::Status status = AddActions(state_id);
      if (!status.ok()) return status;
      if (trace && ++processed % 500 == 0) {
        std::fprintf(stderr,
                     "  %zu states processed, %zu created, %zu queued, "
                     "largest item set %zu\n",
                     processed, table_.states.size(), queue_.size(),
                     largest_item_set_);
      }
    }
    CollectConflicts();
    return std::move(table_);
  }

 private:
  static SymbolData kEndSymbol() { return SymbolData{SymbolType::End, 0}; }
  // The pseudo-lookahead that ends a non-terminal extra rule. It is never
  // lexed: a state holding an entry for it reduces unconditionally.
  static SymbolData kEndOfExtraSymbol() {
    return SymbolData{SymbolType::EndOfNonTerminalExtra, 0};
  }

  const Production* ProductionFor(const ParseItem& item) const {
    if (item.IsAugmented()) return nullptr;
    return &syntax_->variables[item.variable_index]
                .productions[item.production_index];
  }

  // The step at the dot, or nullptr when the item is complete.
  const ProductionStep* StepAt(const ParseItem& item) const {
    if (item.IsAugmented()) {
      // The augmented production is `start -> variables[0]`, one step long.
      return nullptr;
    }
    const Production* production = ProductionFor(item);
    if (item.step_index >= production->steps.size()) return nullptr;
    return &production->steps[item.step_index];
  }

  // The symbol at the dot. The augmented item's single step is the start rule.
  std::optional<SymbolData> SymbolAt(const ParseItem& item) const {
    if (item.IsAugmented()) {
      return item.step_index == 0
                 ? std::optional<SymbolData>(NonTerminalSymbol(0))
                 : std::nullopt;
    }
    const ProductionStep* step = StepAt(item);
    if (step == nullptr) return std::nullopt;
    return step->symbol;
  }

  bool IsDone(const ParseItem& item) const {
    if (item.IsAugmented()) return item.step_index >= 1;
    return item.step_index >= ProductionFor(item)->steps.size();
  }

  // Precedence and associativity come from the step *before* the dot: they
  // describe the reduction about to happen, not the symbol about to be read.
  const ProductionStep* PreviousStep(const ParseItem& item) const {
    if (item.IsAugmented() || item.step_index == 0) return nullptr;
    return &ProductionFor(item)->steps[item.step_index - 1];
  }

  Precedence PrecedenceOf(const ParseItem& item) const {
    const ProductionStep* step = PreviousStep(item);
    return step == nullptr ? Precedence() : step->precedence;
  }

  std::optional<Associativity> AssociativityOf(const ParseItem& item) const {
    const ProductionStep* step = PreviousStep(item);
    return step == nullptr ? std::nullopt : step->associativity;
  }

  // FIRST(X): the terminals that can begin a match of X. Computed by fixpoint
  // rather than recursion, since a grammar's rules are mutually recursive.
  void ComputeFirstSets() {
    for (std::size_t i = 0; i < syntax_->variables.size(); ++i) {
      first_sets_[NonTerminalSymbol(static_cast<Symbol>(i))] = {};
    }
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t i = 0; i < syntax_->variables.size(); ++i) {
        const SymbolData symbol = NonTerminalSymbol(static_cast<Symbol>(i));
        TokenSet& set = first_sets_[symbol];
        bool grew = false;
        for (const Production& production : syntax_->variables[i].productions) {
          // Only the first step matters: a production that can match the empty
          // string has no steps at all, and normalization has already rejected
          // those except for the start rule.
          if (production.steps.empty()) continue;
          const SymbolData first = production.steps.front().symbol;
          if (first.IsNonTerminal()) {
            grew |= set.InsertAll(first_sets_[first]);
          } else {
            grew |= set.Insert(first);
          }
        }
        if (grew) changed = true;
      }
    }
  }

  const TokenSet& FirstSet(SymbolData symbol) {
    if (!symbol.IsNonTerminal()) {
      auto [iter, inserted] = first_sets_.try_emplace(symbol);
      if (inserted) iter->second.Insert(symbol);
      return iter->second;
    }
    return first_sets_[symbol];
  }

  // One item that expanding a given non-terminal always contributes, together
  // with the lookaheads that always accompany it.
  struct ClosureAddition {
    ParseItem item;
    TokenSet lookaheads;
    // True when `item` can sit at the *end* of the expansion, so whatever can
    // follow the non-terminal being expanded can also follow this item.
    bool propagates_lookaheads = false;
  };

  // Precomputes, for every non-terminal, the complete set of items its
  // expansion contributes -- transitively, since those productions may
  // themselves begin with non-terminals.
  //
  // This is what makes closure cheap. Done naively, every closure is a
  // fixpoint that rediscovers the same expansions over and over; on a grammar
  // the size of JavaScript that dominated table generation entirely. With
  // these precomputed once, a closure becomes a single pass over the kernel
  // merging in ready-made additions. Mirrors tree-sitter's
  // `transitive_closure_additions`.
  void ComputeClosureAdditions() {
    closure_additions_.resize(syntax_->variables.size());

    struct FollowSetInfo {
      TokenSet lookaheads;
      bool propagates_lookaheads = false;
    };

    for (std::size_t i = 0; i < syntax_->variables.size(); ++i) {
      // Which non-terminals can begin an expansion of `i`, and what can follow
      // each of them. Walked with an explicit stack rather than recursion,
      // since the relation is cyclic for any recursive grammar.
      std::map<std::size_t, FollowSetInfo> info_by_non_terminal;
      std::vector<std::tuple<std::size_t, TokenSet, bool>> stack;
      stack.emplace_back(i, TokenSet{}, true);

      while (!stack.empty()) {
        auto [index, lookaheads, propagates] = std::move(stack.back());
        stack.pop_back();

        FollowSetInfo& info = info_by_non_terminal[index];
        bool learned = info.lookaheads.InsertAll(lookaheads);
        if (propagates && !info.propagates_lookaheads) {
          info.propagates_lookaheads = true;
          learned = true;
        }
        // Revisiting teaches nothing new, so the walk terminates.
        if (!learned) continue;

        for (const Production& production :
             syntax_->variables[index].productions) {
          if (production.steps.empty()) continue;
          const SymbolData first = production.steps.front().symbol;
          if (!first.IsNonTerminal()) continue;
          if (production.steps.size() > 1) {
            // Something follows it inside this production, so that decides the
            // lookaheads and nothing propagates outward.
            stack.emplace_back(first.index,
                               FirstSet(production.steps[1].symbol), false);
          } else {
            stack.emplace_back(first.index, lookaheads, propagates);
          }
        }
      }

      std::vector<ClosureAddition>& additions = closure_additions_[i];
      for (const auto& [index, info] : info_by_non_terminal) {
        const SyntaxVariable& variable = syntax_->variables[index];
        for (std::size_t p = 0; p < variable.productions.size(); ++p) {
          additions.push_back(
              ClosureAddition{ParseItem{static_cast<std::uint32_t>(index),
                                        static_cast<std::uint32_t>(p), 0},
                              info.lookaheads, info.propagates_lookaheads});
        }
      }
    }
  }

  // Expands a kernel item set: wherever the dot sits before a non-terminal,
  // merge in that non-terminal's precomputed additions. One pass over the
  // kernel, no fixpoint.
  ParseItemSet Closure(const ParseItemSet& kernel) {
    ParseItemSet result = kernel;
    for (const auto& [item, lookaheads] : kernel.entries) {
      const std::optional<SymbolData> symbol = SymbolAt(item);
      if (!symbol.has_value() || !symbol->IsNonTerminal()) continue;

      // What can follow the non-terminal: the FIRST set of the next step, or
      // the item's own lookaheads when the non-terminal ends the production.
      const ParseItem successor{item.variable_index, item.production_index,
                                item.step_index + 1};
      const std::optional<SymbolData> next_symbol = SymbolAt(successor);
      const TokenSet& following =
          next_symbol.has_value() ? FirstSet(*next_symbol) : lookaheads;

      for (const ClosureAddition& addition :
           closure_additions_[symbol->index]) {
        TokenSet& target = result.entries[addition.item];
        target.InsertAll(addition.lookaheads);
        if (addition.propagates_lookaheads) target.InsertAll(following);
      }
    }
    return result;
  }

  // The LR(0) core of an item set: its items with the lookaheads dropped.
  // Two states sharing a core are merged, which is what makes this LALR(1)
  // rather than canonical LR(1).
  static std::vector<ParseItem> CoreOf(const ParseItemSet& set) {
    std::vector<ParseItem> core;
    core.reserve(set.entries.size());
    for (const auto& [item, lookaheads] : set.entries) core.push_back(item);
    return core;
  }

  // Returns the state for this item set. If a state with the same LR(0) core
  // already exists, the new lookaheads are merged into it and it is re-queued
  // only if that actually taught it something -- the merge is what keeps the
  // state count proportional to the grammar rather than to its lookahead
  // combinations.
  //
  // Merging can manufacture reduce/reduce conflicts that canonical LR(1) would
  // have kept apart. For a GLR parser that is harmless: a spurious conflict
  // costs an extra runtime fork that dies immediately, rather than rejecting
  // the grammar. This is the same trade tree-sitter makes.
  ParseStateId AddState(ParseItemSet item_set) {
    ParseItemSet closed = Closure(std::move(item_set));
    std::vector<ParseItem> core = CoreOf(closed);

    auto existing = state_ids_by_core_.find(core);
    if (existing != state_ids_by_core_.end()) {
      const ParseStateId state_id = existing->second;
      ParseItemSet& stored = state_item_sets_[state_id];
      bool grew = false;
      for (const auto& [item, lookaheads] : closed.entries) {
        if (stored.entries[item].InsertAll(lookaheads)) grew = true;
      }
      // Wider lookaheads can change this state's actions and, through its
      // successors, theirs -- so it has to be reprocessed.
      if (grew) Enqueue(state_id);
      return state_id;
    }

    largest_item_set_ = std::max(largest_item_set_, closed.entries.size());
    const auto state_id = static_cast<ParseStateId>(table_.states.size());
    table_.states.emplace_back();
    state_item_sets_.push_back(std::move(closed));
    queued_.push_back(false);
    state_ids_by_core_.emplace(std::move(core), state_id);
    Enqueue(state_id);
    return state_id;
  }

  void Enqueue(ParseStateId state_id) {
    if (queued_[state_id]) return;
    queued_[state_id] = true;
    queue_.push_back(state_id);
  }

  // Builds one entry state per distinct terminal that a non-terminal extra
  // rule can begin with. The item starts at step 1 -- that first terminal is
  // consumed by the shift that got here -- and its only lookahead is the
  // end-of-extra pseudo-symbol, so the rule reduces as soon as it can go no
  // further. Mirrors tree-sitter's non_terminal_extra_states.
  absl::Status AddNonTerminalExtraStates() {
    std::map<SymbolData, ParseItemSet> item_sets_by_first_terminal;
    for (const SymbolData extra : syntax_->extra_symbols) {
      if (!extra.IsNonTerminal()) continue;
      table_.non_terminal_extras.push_back(extra);

      const SyntaxVariable& variable = syntax_->variables[extra.index];
      for (std::size_t p = 0; p < variable.productions.size(); ++p) {
        const Production& production = variable.productions[p];
        if (production.steps.empty()) continue;
        const SymbolData first = production.steps.front().symbol;
        if (first.IsNonTerminal()) {
          return absl::InvalidArgumentError(absl::StrCat(
              "the non-terminal rule `", syntax_->variables[first.index].name,
              "` is used at the start of the `extras` rule `", variable.name,
              "`, which is not allowed"));
        }
        ParseItemSet& set = item_sets_by_first_terminal[first];
        set.entries[ParseItem{static_cast<std::uint32_t>(extra.index),
                              static_cast<std::uint32_t>(p), 1}]
            .Insert(kEndOfExtraSymbol());
      }
    }

    for (auto& [terminal, item_set] : item_sets_by_first_terminal) {
      const ParseStateId state_id = AddState(std::move(item_set));
      table_.non_terminal_extra_states.push_back(
          NonTerminalExtraEntry{terminal, state_id});
    }
    return absl::OkStatus();
  }

  // What a set of reduce actions agreed on for one lookahead, used to compare
  // against a competing shift.
  struct ReductionInfo {
    Precedence precedence;
    std::vector<SymbolData> symbols;
    bool has_left_assoc = false;
    bool has_right_assoc = false;
    bool has_non_assoc = false;
  };

  absl::Status AddActions(ParseStateId state_id) {
    // A state is reprocessed whenever a merge widened its lookaheads, so its
    // actions are rebuilt from scratch rather than added to.
    ParseState& target_state = table_.states[state_id];
    target_state.terminal_entries.clear();
    target_state.nonterminal_entries.clear();
    target_state.valid_terminals.clear();

    // A reference, not a copy: state_item_sets_ is a deque, so creating
    // successor states below cannot invalidate it.
    const ParseItemSet& item_set = state_item_sets_[state_id];

    std::map<SymbolData, ParseItemSet> terminal_successors;
    std::map<SymbolData, ParseItemSet> nonterminal_successors;
    std::map<SymbolData, ReductionInfo> reduction_infos;
    TokenSet lookaheads_with_conflicts;

    for (const auto& [item, lookaheads] : item_set.entries) {
      const std::optional<SymbolData> symbol = SymbolAt(item);

      if (symbol.has_value()) {
        // Unfinished: this state has a transition on `symbol`.
        const ParseItem successor{item.variable_index, item.production_index,
                                  item.step_index + 1};
        ParseItemSet& successor_set = symbol->IsNonTerminal()
                                          ? nonterminal_successors[*symbol]
                                          : terminal_successors[*symbol];
        successor_set.entries[successor].InsertAll(lookaheads);
        continue;
      }

      // Finished: reduce (or accept, for the augmented item).
      ParseAction action;
      if (item.IsAugmented()) {
        action.kind = ParseActionKind::Accept;
      } else {
        action.kind = ParseActionKind::Reduce;
        action.symbol =
            NonTerminalSymbol(static_cast<Symbol>(item.variable_index));
        action.child_count = item.step_index;
        action.dynamic_precedence = ProductionFor(item)->dynamic_precedence;
        action.production_id = item.production_index;
      }

      const Precedence precedence = PrecedenceOf(item);
      const std::optional<Associativity> associativity = AssociativityOf(item);

      for (const SymbolData lookahead : lookaheads) {
        ParseTableEntry& entry =
            table_.states[state_id].terminal_entries[lookahead];
        ReductionInfo& info = reduction_infos[lookahead];

        if (entry.actions.empty()) {
          entry.actions.push_back(action);
        } else {
          // Resolve reduce/reduce eagerly on precedence, so that only equal
          // precedence survives to become a real conflict.
          const int comparison = ComparePrecedence(
              precedence, {action.symbol}, info.precedence, info.symbols);
          if (comparison > 0) {
            entry.actions.clear();
            entry.actions.push_back(action);
            lookaheads_with_conflicts.Erase(lookahead);
            info = ReductionInfo{};
          } else if (comparison == 0) {
            entry.actions.push_back(action);
            lookaheads_with_conflicts.Insert(lookahead);
          } else {
            continue;
          }
        }

        info.precedence = precedence;
        if (std::find(info.symbols.begin(), info.symbols.end(),
                      action.symbol) == info.symbols.end()) {
          info.symbols.push_back(action.symbol);
        }
        if (!associativity.has_value()) {
          info.has_non_assoc = true;
        } else if (*associativity == Associativity::Left) {
          info.has_left_assoc = true;
        } else {
          info.has_right_assoc = true;
        }
      }
    }

    // Shifts. Creating the successor state may enqueue more work.
    for (auto& [symbol, successor_set] : terminal_successors) {
      const ParseStateId next_state = AddState(std::move(successor_set));
      ParseTableEntry& entry = table_.states[state_id].terminal_entries[symbol];
      if (!entry.actions.empty()) lookaheads_with_conflicts.Insert(symbol);

      ParseAction shift;
      shift.kind = ParseActionKind::Shift;
      shift.state = next_state;
      entry.actions.push_back(shift);
    }

    // Gotos. A non-terminal never conflicts: it is consulted only after a
    // reduction has already been chosen.
    for (auto& [symbol, successor_set] : nonterminal_successors) {
      const ParseStateId next_state = AddState(std::move(successor_set));
      table_.states[state_id].nonterminal_entries[symbol] = next_state;
    }

    for (const SymbolData lookahead : lookaheads_with_conflicts) {
      ResolveConflict(state_id, item_set, lookahead,
                      reduction_infos[lookahead]);
    }

    // Record the terminals this state accepts, which is the token set its lex
    // table will be built from.
    ParseState& state = table_.states[state_id];

    // A state that completes a non-terminal extra must not also offer to start
    // another one, or the rule could never end.
    state.is_end_of_nonterminal_extra =
        state.terminal_entries.count(kEndOfExtraSymbol()) != 0;
    if (!state.is_end_of_nonterminal_extra) {
      for (const NonTerminalExtraEntry& extra :
           table_.non_terminal_extra_states) {
        ParseTableEntry& entry = state.terminal_entries[extra.first_terminal];
        if (!entry.actions.empty()) continue;
        ParseAction shift;
        shift.kind = ParseActionKind::Shift;
        shift.state = extra.state;
        entry.actions.push_back(shift);
      }
    }

    for (const auto& [symbol, entry] : state.terminal_entries) {
      if (symbol.kind == SymbolType::EndOfNonTerminalExtra) continue;
      state.valid_terminals.push_back(symbol);
      if (symbol.IsExternal()) state.valid_externals.push_back(symbol.index);
    }
    // An `extras` token can appear anywhere, so a scanner that produces one
    // has to be offered the chance in every state -- no state has a parse
    // action for it to be discovered from.
    for (const SymbolData symbol : syntax_->extra_symbols) {
      if (!symbol.IsExternal()) continue;
      if (std::find(state.valid_externals.begin(), state.valid_externals.end(),
                    symbol.index) == state.valid_externals.end()) {
        state.valid_externals.push_back(symbol.index);
      }
    }
    return absl::OkStatus();
  }

  // Applies static precedence and associativity to a multi-action entry. What
  // survives is left in place: unlike tree-sitter, an unresolved conflict here
  // is not an error but a site where the GLR engine will fork.
  void ResolveConflict(ParseStateId state_id, const ParseItemSet& item_set,
                       SymbolData lookahead, const ReductionInfo& info) {
    ParseTableEntry& entry =
        table_.states[state_id].terminal_entries[lookahead];

    // Gather the items actually in contention, and the precedences attached to
    // the shift side.
    std::vector<ParseItem> conflicting_items;
    std::vector<std::pair<Precedence, SymbolData>> shift_precedences;
    for (const auto& [item, lookaheads] : item_set.entries) {
      const std::optional<SymbolData> symbol = SymbolAt(item);
      if (symbol.has_value()) {
        if (item.step_index == 0) continue;
        if (!FirstSet(*symbol).Contains(lookahead)) continue;
        if (!item.IsAugmented()) conflicting_items.push_back(item);
        shift_precedences.emplace_back(
            PrecedenceOf(item),
            NonTerminalSymbol(static_cast<Symbol>(item.variable_index)));
      } else if (lookaheads.Contains(lookahead) && !item.IsAugmented()) {
        conflicting_items.push_back(item);
      }
    }

    const bool has_shift = !entry.actions.empty() &&
                           entry.actions.back().kind == ParseActionKind::Shift;

    if (has_shift && !conflicting_items.empty()) {
      // A repeat rule conflicts with itself by construction. That ambiguity is
      // intentional and uninteresting, so mark it and keep the shift rather
      // than forking on every repetition.
      const std::uint32_t first_variable =
          conflicting_items.front().variable_index;
      const bool all_same_auxiliary =
          syntax_->variables[first_variable].kind == VariableType::Auxiliary &&
          std::all_of(conflicting_items.begin(), conflicting_items.end(),
                      [first_variable](const ParseItem& item) {
                        return item.variable_index == first_variable;
                      });
      if (all_same_auxiliary) {
        entry.actions.back().is_repetition = true;
        return;
      }

      bool shift_is_more = false;
      bool shift_is_less = false;
      for (const auto& [precedence, symbol] : shift_precedences) {
        const int comparison = ComparePrecedence(precedence, {symbol},
                                                 info.precedence, info.symbols);
        if (comparison > 0) shift_is_more = true;
        if (comparison < 0) shift_is_less = true;
      }

      if (shift_is_more && !shift_is_less) {
        // Shift wins outright: drop every reduce.
        ParseAction shift = entry.actions.back();
        entry.actions.clear();
        entry.actions.push_back(shift);
      } else if (shift_is_less && !shift_is_more) {
        entry.actions.pop_back();
      } else if (!shift_is_more && !shift_is_less) {
        // Equal precedence, so associativity decides: left-associative means
        // reduce before shifting, right-associative means shift.
        if (info.has_left_assoc && !info.has_non_assoc &&
            !info.has_right_assoc) {
          entry.actions.pop_back();
        } else if (info.has_right_assoc && !info.has_non_assoc &&
                   !info.has_left_assoc) {
          ParseAction shift = entry.actions.back();
          entry.actions.clear();
          entry.actions.push_back(shift);
        }
      }
    }

    // Anything still holding several actions is genuinely ambiguous and is
    // left alone for the engine to fork on. It is not recorded here: a state
    // may be reprocessed after a lookahead merge, so conflicts are collected
    // once at the end, from the finished table.
  }

  // Walks the completed table and records every entry that still holds more
  // than one action, skipping the repeat-rule ambiguities the engine settles
  // on its own.
  void CollectConflicts() {
    for (std::size_t state_id = 0; state_id < table_.states.size();
         ++state_id) {
      const ParseItemSet& item_set = state_item_sets_[state_id];
      for (const auto& [lookahead, entry] :
           table_.states[state_id].terminal_entries) {
        if (entry.actions.size() <= 1) continue;
        const bool is_repetition =
            std::any_of(entry.actions.begin(), entry.actions.end(),
                        [](const ParseAction& action) {
                          return action.kind == ParseActionKind::Shift &&
                                 action.is_repetition;
                        });
        if (is_repetition) continue;

        TableConflict conflict;
        conflict.state = static_cast<ParseStateId>(state_id);
        conflict.lookahead = lookahead;
        for (const ParseAction& action : entry.actions) {
          if (action.kind == ParseActionKind::Shift) ++conflict.shift_count;
          if (action.kind == ParseActionKind::Reduce) ++conflict.reduce_count;
        }

        // The competing rules are those whose items either are complete with
        // this lookahead, or could shift it next.
        std::vector<SymbolData> symbols;
        for (const auto& [item, lookaheads] : item_set.entries) {
          if (item.IsAugmented()) continue;
          const std::optional<SymbolData> symbol = SymbolAt(item);
          const bool competes = symbol.has_value()
                                    ? (item.step_index > 0 &&
                                       FirstSet(*symbol).Contains(lookahead))
                                    : lookaheads.Contains(lookahead);
          if (competes) {
            symbols.push_back(
                NonTerminalSymbol(static_cast<Symbol>(item.variable_index)));
          }
        }
        std::sort(symbols.begin(), symbols.end());
        symbols.erase(std::unique(symbols.begin(), symbols.end()),
                      symbols.end());
        for (const SymbolData symbol : symbols) {
          conflict.rule_names.push_back(syntax_->variables[symbol.index].name);
        }
        conflict.is_expected =
            std::find(syntax_->expected_conflicts.begin(),
                      syntax_->expected_conflicts.end(),
                      symbols) != syntax_->expected_conflicts.end();
        conflict.rule_symbols = std::move(symbols);
        table_.conflicts.push_back(std::move(conflict));
      }
    }
  }

  // Returns >0 when `left` outranks `right`, <0 when it is outranked, 0 when
  // they are incomparable or equal. Integer precedences compare numerically;
  // named ones only through the grammar's `precedences` orderings.
  int ComparePrecedence(const Precedence& left,
                        const std::vector<SymbolData>& left_symbols,
                        const Precedence& right,
                        const std::vector<SymbolData>& right_symbols) const {
    const bool left_is_int = left.IsInteger();
    const bool right_is_int = right.IsInteger();
    const std::int32_t left_value = left_is_int ? left.AsInteger() : 0;
    const std::int32_t right_value = right_is_int ? right.AsInteger() : 0;
    if ((left_is_int || right_is_int) &&
        (left_value != 0 || right_value != 0)) {
      // An absent precedence counts as zero, so prec(1, ...) beats a plain
      // rule and prec(-1, ...) loses to one.
      if (left_is_int && right_is_int) {
        return left_value < right_value ? -1
                                        : (left_value > right_value ? 1 : 0);
      }
      if (left_is_int) return left_value < 0 ? -1 : 1;
      return right_value < 0 ? 1 : -1;
    }

    // Named precedences are only ordered relative to each other, by the
    // grammar's `precedences: $ => [[...]]` lists. Earlier in a list is higher.
    const auto matches = [this](const PrecedenceEntry& entry,
                                const Precedence& precedence,
                                const std::vector<SymbolData>& symbols) {
      if (entry.kind == PrecedenceEntry::Kind::Name) {
        return precedence.IsName() && precedence.AsName() == entry.value;
      }
      return std::any_of(
          symbols.begin(), symbols.end(), [this, &entry](SymbolData symbol) {
            return symbol.IsNonTerminal() &&
                   symbol.index < syntax_->variables.size() &&
                   syntax_->variables[symbol.index].name == entry.value;
          });
    };

    for (const auto& ordering : syntax_->precedence_orderings) {
      bool saw_left = false;
      bool saw_right = false;
      for (const PrecedenceEntry& entry : ordering) {
        const bool matches_left = matches(entry, left, left_symbols);
        const bool matches_right = matches(entry, right, right_symbols);
        if (matches_left) {
          saw_left = true;
          if (saw_right) return -1;
        } else if (matches_right) {
          saw_right = true;
          if (saw_left) return 1;
        }
      }
    }
    return 0;
  }

  const PreparedGrammar* grammar_;
  const SyntaxGrammar* syntax_;
  ParseTable table_;
  std::map<SymbolData, TokenSet> first_sets_;
  // Indexed by non-terminal index.
  std::vector<std::vector<ClosureAddition>> closure_additions_;
  // Keyed by LR(0) core, which is what makes this LALR: states differing only
  // in lookaheads land on the same entry and are merged.
  std::map<std::vector<ParseItem>, ParseStateId> state_ids_by_core_;
  // A deque, not a vector: references into it must survive the growth caused
  // by creating successor states mid-iteration.
  std::deque<ParseItemSet> state_item_sets_;
  std::deque<ParseStateId> queue_;
  // Parallel to states: guards against queueing a state twice.
  std::vector<bool> queued_;
  std::size_t largest_item_set_ = 0;
};

}  // namespace

absl::StatusOr<ParseTable> BuildParseTable(const PreparedGrammar& grammar) {
  TableBuilder builder(grammar);
  return builder.Build();
}

absl::StatusOr<LexTableSet> BuildLexTables(const PreparedGrammar& grammar,
                                           const LexicalAutomaton& automaton,
                                           ParseTable& table) {
  LexTableSet result;
  // Parse states that accept the same tokens can share a lex table, which
  // keeps the number of DFAs proportional to the distinct contexts rather than
  // to the state count.
  std::map<std::vector<Symbol>, std::uint32_t> lex_state_ids_by_tokens;

  // An `extras` token may appear between any two other tokens, so every state
  // has to be able to lex it even though no state has a parse action for it.
  // Leaving these out makes a comment unlexable everywhere.
  std::vector<Symbol> extra_tokens;
  for (const SymbolData symbol : grammar.syntax.extra_symbols) {
    if (symbol.IsTerminal()) extra_tokens.push_back(symbol.index);
  }

  for (ParseState& state : table.states) {
    std::vector<Symbol> tokens = extra_tokens;
    bool eof_valid = false;
    for (const SymbolData symbol : state.valid_terminals) {
      if (symbol.kind == SymbolType::End) {
        eof_valid = true;
        continue;
      }
      // External tokens are Phase 4's business; the internal DFA cannot
      // produce them.
      if (symbol.IsTerminal()) tokens.push_back(symbol.index);
    }
    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());

    // The eof flag is part of the table's identity, so fold it into the key.
    std::vector<Symbol> key = tokens;
    key.push_back(eof_valid ? 1 : 0);

    auto existing = lex_state_ids_by_tokens.find(key);
    if (existing != lex_state_ids_by_tokens.end()) {
      state.lex_state_id = existing->second;
      continue;
    }
    const auto lex_state_id = static_cast<std::uint32_t>(result.tables.size());
    result.tables.push_back(
        BuildLexTable(automaton, grammar.lexical, tokens, eof_valid));
    lex_state_ids_by_tokens.emplace(std::move(key), lex_state_id);
    state.lex_state_id = lex_state_id;
  }
  return result;
}

}  // namespace ts_ref
