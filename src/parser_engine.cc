#include "ts_ref/parser_engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "ts_ref/dfa.h"
#include "ts_ref/grammar_normalizer.h"
#include "ts_ref/lexer_generator.h"
#include "ts_ref/node.h"
#include "ts_ref/parser_table.h"
#include "ts_ref/prepared_grammar.h"
#include "ts_ref/stack.h"
#include "ts_ref/table_generator.h"
#include "ts_ref/types.h"

namespace ts_ref {
namespace {

// Measures a byte range as a Length, whose Point is a *delta* in the form
// Point::Add expects: a row count, plus either the column within the final line
// (when rows > 0) or a column offset to add (when rows == 0).
Length MeasureRange(std::string_view input, std::uint32_t start,
                    std::uint32_t end) {
  start = std::min<std::uint32_t>(start, input.size());
  end = std::min<std::uint32_t>(end, input.size());
  std::uint32_t rows = 0;
  std::uint32_t column = 0;
  for (std::uint32_t i = start; i < end; ++i) {
    if (input[i] == '\n') {
      ++rows;
      column = 0;
    } else {
      ++column;
    }
  }
  return Length(end - start, Point(rows, column));
}

// Orders two subtrees deterministically, so that a tie between equally
// plausible parses always resolves the same way. Compares symbol then child
// count, descending into children; the smaller symbol wins, and since symbols
// are numbered in declaration order that means the earlier-declared rule wins.
// Iterative rather than recursive, like tree-sitter's ts_subtree_compare.
int CompareSubtrees(const std::shared_ptr<CSTNode>& left,
                    const std::shared_ptr<CSTNode>& right) {
  std::vector<std::pair<const CSTNode*, const CSTNode*>> pending;
  pending.emplace_back(left.get(), right.get());

  while (!pending.empty()) {
    const auto [a, b] = pending.back();
    pending.pop_back();
    // Subtrees are shared, so two readings of the same span usually agree on
    // most of their structure by pointer. Stopping there turns what is
    // otherwise a full walk of both trees into a shallow one, which matters
    // because this runs once per resolved ambiguity.
    if (a == b) continue;
    if (a == nullptr || b == nullptr) return a == nullptr ? -1 : 1;

    if (a->symbol != b->symbol) return a->symbol < b->symbol ? -1 : 1;
    if (a->children.size() != b->children.size()) {
      return a->children.size() < b->children.size() ? -1 : 1;
    }
    // Reversed, so that the stack pops children left to right.
    for (std::size_t i = a->children.size(); i > 0; --i) {
      pending.emplace_back(a->children[i - 1].get(), b->children[i - 1].get());
    }
  }
  return 0;
}

// Whether `right` should be preferred over `left`. This is the whole of
// TASKS.md Phase 7's disambiguation: a higher dynamic precedence wins, and
// otherwise the deterministic order above decides. tree-sitter weighs error
// cost first, which does not arise here because there is no error recovery.
bool PreferRightTree(const std::shared_ptr<CSTNode>& left,
                     const std::shared_ptr<CSTNode>& right) {
  if (left == nullptr) return true;
  if (right == nullptr) return false;
  if (right->dynamic_precedence != left->dynamic_precedence) {
    return right->dynamic_precedence > left->dynamic_precedence;
  }
  // On a true tie the incumbent stays, which keeps selection stable.
  return CompareSubtrees(left, right) > 0;
}

class GlrParser {
 public:
  GlrParser(const CompiledGrammar& compiled, std::string_view input)
      : compiled_{&compiled},
        grammar_{&compiled.grammar},
        table_{&compiled.parse_table},
        input_{input},
        stack_{compiled.parse_table.start_state} {}

  absl::StatusOr<ParseResult> Run() {
    // Each iteration advances every live version by one action. A version is
    // not required to stay in step with the others: a reduce leaves its
    // position unchanged while a shift moves it forward, so versions drift and
    // re-converge, and merging is what pulls them back together.
    const std::size_t step_limit = 4096 + 512 * input_.size();
    std::size_t step = 0;
    for (; step < step_limit; ++step) {
      if (stack_.GetVersionCount() == 0) break;

      const StackVersion count = stack_.GetVersionCount();
      bool any_active = false;
      // Only versions present at the start of the round are advanced. Versions
      // appended during it (by a fork or a reduce) wait for the next round,
      // which keeps indices stable while iterating.
      for (StackVersion version = 0; version < count; ++version) {
        if (stack_.GetHead(version)->halted) continue;
        any_active = true;
        AdvanceVersion(version);
      }
      if (!any_active) break;

      SweepHaltedVersions();
      MergeEquivalentVersions();
      result_.max_versions =
          std::max<std::size_t>(result_.max_versions, stack_.GetVersionCount());
      if (stack_.GetVersionCount() == 0) break;
    }
    // Distinguishing "ran out of budget" from "every version died" matters:
    // they look identical in the tree count but mean opposite things.
    result_.hit_step_limit = step >= step_limit;

    // Several versions can still accept -- the same input parsed two genuinely
    // different ways. Every one is kept for inspection, but one is chosen, so
    // that a caller wanting "the" tree gets a stable answer.
    for (const std::shared_ptr<CSTNode>& tree : result_.trees) {
      if (PreferRightTree(result_.tree, tree)) result_.tree = tree;
    }
    return std::move(result_);
  }

 private:
  // What the lexer found at a position: either an ordinary token from the
  // generated DFA, or one produced by the grammar's external scanner.
  struct Lookahead {
    LexResult lex;
    bool from_scanner = false;
    // Only meaningful when from_scanner: the scanner's state afterwards, to be
    // stored on the token so the version can resume from it.
    ScannerState scanner_state;
  };

  // Scans one token at `position` for a version in `state`.
  //
  // The external scanner gets first refusal whenever the state has any valid
  // external tokens, exactly as tree-sitter does: only the scanner can
  // recognize things like a template-literal chunk or an indent, and it must
  // see the position before the DFA consumes anything. If it declines, the
  // generated DFA runs at the same position as though nothing happened.
  Lookahead LookaheadFor(StateId state, std::uint32_t position,
                         const ScannerState& scanner_state) {
    const ParseState& parse_state = table_->states[state];
    const std::uint32_t lex_state = parse_state.lex_state_id;

    if (compiled_->scanner != nullptr && !parse_state.valid_externals.empty()) {
      std::vector<bool> valid(compiled_->grammar.syntax.external_tokens.size(),
                              false);
      for (const std::uint32_t index : parse_state.valid_externals) {
        if (index < valid.size()) valid[index] = true;
      }
      const ExternalScanner::ScanResult scan =
          compiled_->scanner->Scan(input_, position, scanner_state, valid);
      if (scan.found &&
          scan.external_index <
              compiled_->grammar.syntax.external_tokens.size() &&
          !IsUnproductiveEmptyToken(scan, parse_state, position,
                                    scanner_state)) {
        Lookahead result;
        result.from_scanner = true;
        result.scanner_state = scan.state;
        result.lex.padding = ByteRange{position, scan.token_start};
        result.lex.token = Token{static_cast<Symbol>(scan.external_index),
                                 ByteRange{scan.token_start, scan.token_end}};
        return result;
      }
    }

    // Cached because several versions can share a state and position, and the
    // DFA result does not depend on anything else. The scanner path above is
    // not cached: it depends on the version's own scanner state.
    const auto key = std::make_pair(position, lex_state);
    auto existing = lex_cache_.find(key);
    if (existing == lex_cache_.end()) {
      Lexer lexer(compiled_->lex_tables.tables[lex_state], input_);
      lexer.set_position(position);
      existing = lex_cache_.emplace(key, lexer.Next()).first;
    }
    Lookahead result;
    result.lex = existing->second;
    return result;
  }

  // A scanner may legitimately return a zero-width token -- an indent, an
  // automatic semicolon -- but one that consumes nothing, changes no scanner
  // state, and does not move the parse state either would be returned again at
  // the same position forever. That combination is exactly an empty `extras`
  // token, which is shifted without changing state; rejecting it is what stops
  // the loop. Mirrors the `token_is_extra` arm of parser.c's empty-token guard.
  bool IsUnproductiveEmptyToken(const ExternalScanner::ScanResult& scan,
                                const ParseState& parse_state,
                                std::uint32_t position,
                                const ScannerState& previous_state) const {
    if (scan.token_end > position) return false;
    if (scan.state != previous_state) return false;
    const SymbolData symbol = SymbolForExternal(scan.external_index);
    // Anything the state has a real action for does move the parse forward.
    if (parse_state.terminal_entries.count(symbol) != 0) return false;
    return IsExtraSymbol(symbol);
  }

  // The parse symbol an external token stands for. A scanner token that also
  // has an ordinary rule of the same name is fed back in as that terminal, so
  // the table needs no separate entry for it -- this is what
  // `corresponding_internal_token` records.
  SymbolData SymbolForExternal(std::uint32_t index) const {
    const ExternalToken& token =
        compiled_->grammar.syntax.external_tokens[index];
    if (token.corresponding_internal_token.has_value()) {
      return *token.corresponding_internal_token;
    }
    return ExternalSymbol(static_cast<Symbol>(index));
  }

  static SymbolData SymbolForToken(const LexResult& lookahead) {
    if (lookahead.at_eof) return SymbolData{SymbolType::End, 0};
    return TerminalSymbol(lookahead.token.token_kind);
  }

  bool IsExtraSymbol(SymbolData symbol) const {
    const std::vector<SymbolData>& extras = grammar_->syntax.extra_symbols;
    return std::find(extras.begin(), extras.end(), symbol) != extras.end();
  }

  void Halt(StackVersion version) { stack_.GetHead(version)->halted = true; }

  std::string SymbolName(SymbolData symbol) const {
    switch (symbol.kind) {
      case SymbolType::End:
        return "<eof>";
      case SymbolType::Terminal:
        return symbol.index < grammar_->lexical.variables.size()
                   ? grammar_->lexical.variables[symbol.index].name
                   : absl::StrCat("<t", symbol.index, ">");
      case SymbolType::External:
        return symbol.index < grammar_->syntax.external_tokens.size()
                   ? grammar_->syntax.external_tokens[symbol.index].name
                   : absl::StrCat("<x", symbol.index, ">");
      default:
        return absl::StrCat("<?", symbol.index, ">");
    }
  }

  std::shared_ptr<CSTNode> MakeLeaf(SymbolData symbol,
                                    const Lookahead& lookahead, bool extra) {
    auto node = std::make_shared<CSTNode>();
    node->padding = MeasureRange(input_, lookahead.lex.padding.start,
                                 lookahead.lex.padding.end);
    node->size = MeasureRange(input_, lookahead.lex.token.token_span.start,
                              lookahead.lex.token.token_span.end);
    node->extra = extra;

    if (lookahead.from_scanner) {
      node->has_external_token = true;
      node->external_scanner_state = lookahead.scanner_state;
    }

    // An external token with an internal counterpart is named by that
    // terminal; one without has no lexical variable of its own.
    if (symbol.IsExternal()) {
      node->symbol = FlatExternalSymbol(*grammar_, symbol.index);
      const ExternalToken& token =
          grammar_->syntax.external_tokens[symbol.index];
      node->named = token.kind == VariableType::Named;
      node->visible = token.kind == VariableType::Named ||
                      token.kind == VariableType::Anonymous;
      return node;
    }

    node->symbol = FlatTerminalSymbol(*grammar_, symbol.index);
    if (symbol.index < grammar_->lexical.variables.size()) {
      const VariableType kind = grammar_->lexical.variables[symbol.index].kind;
      node->named = kind == VariableType::Named;
      node->visible =
          kind == VariableType::Named || kind == VariableType::Anonymous;
    }
    return node;
  }

  // Builds the node for a completed production. A parent's extent is its
  // children's, except that the first child's padding is hoisted onto the
  // parent -- that is what keeps leading whitespace outside the node's span.
  std::shared_ptr<CSTNode> MakeInterior(
      const ParseAction& action,
      const std::vector<std::shared_ptr<CSTNode>>& children) {
    auto node = std::make_shared<CSTNode>();
    node->symbol = FlatNonTerminalSymbol(*grammar_, action.symbol.index);
    node->children = children;

    // Dynamic precedence is a score for the whole subtree, not a label on one
    // production: a node's score is its production's plus every child's. That
    // is what lets two same-shaped alternatives be compared when the stack
    // merges them, since the difference between them is usually buried
    // somewhere below the root. Mirrors parser.c's
    // `parent.ptr->dynamic_precedence += dynamic_precedence` on top of the
    // per-child accumulation in ts_subtree_summarize_children.
    node->dynamic_precedence = action.dynamic_precedence;
    for (const std::shared_ptr<CSTNode>& child : children) {
      node->dynamic_precedence += child->dynamic_precedence;
    }
    // Needed to recover each child's alias and field name when reading the
    // tree: those belong to the parent's production step, not to the child.
    node->production_id = action.production_id;

    if (!children.empty()) {
      node->padding = children.front()->padding;
      node->size = children.front()->size;
      for (std::size_t i = 1; i < children.size(); ++i) {
        node->size = node->size.Add(children[i]->SubtreeTotalSize());
      }
    }

    if (action.symbol.index < grammar_->syntax.variables.size()) {
      const VariableType kind =
          grammar_->syntax.variables[action.symbol.index].kind;
      node->named = kind == VariableType::Named;
      node->visible = kind == VariableType::Named;
    }
    for (const std::shared_ptr<CSTNode>& child : children) {
      if (child->has_external_token) node->has_external_token = true;
    }
    return node;
  }

  static SymbolData kEndOfExtraSymbol() {
    return SymbolData{SymbolType::EndOfNonTerminalExtra, 0};
  }

  void AdvanceVersion(StackVersion version) {
    const StateId state = stack_.GetState(version);
    const auto position =
        static_cast<std::uint32_t>(stack_.GetPosition(version).bytes);

    // A non-terminal extra ends where it can go no further, not at any
    // particular token, so this state reduces without lexing at all. Asking
    // the lexer here would consume input belonging to whatever follows.
    if (table_->states[state].is_end_of_nonterminal_extra) {
      const ParseState& parse_state = table_->states[state];
      auto entry = parse_state.terminal_entries.find(kEndOfExtraSymbol());
      if (entry != parse_state.terminal_entries.end() &&
          !entry->second.actions.empty()) {
        if (trace_) {
          std::fprintf(stderr, "  v%u state=%u pos=%u -> <end-of-extra>\n",
                       version, state, position);
        }
        ApplyAction(entry->second.actions.front(), version, kEndOfExtraSymbol(),
                    Lookahead{});
        return;
      }
    }

    // Each version resumes the scanner from its own last external token, so a
    // fork cannot leak one branch's scanner state into another.
    ScannerState scanner_state;
    if (const std::shared_ptr<CSTNode> last =
            stack_.GetLastExternalToken(version)) {
      scanner_state = last->external_scanner_state;
    }
    const Lookahead lookahead = LookaheadFor(state, position, scanner_state);

    if (lookahead.lex.error) {
      Halt(version);
      return;
    }

    const SymbolData symbol =
        lookahead.from_scanner
            ? SymbolForExternal(lookahead.lex.token.token_kind)
            : SymbolForToken(lookahead.lex);
    const ParseState& parse_state = table_->states[state];

    if (trace_) {
      std::fprintf(stderr, "  v%u state=%u pos=%u -> %s%s [%u..%u)\n", version,
                   state, position, SymbolName(symbol).c_str(),
                   lookahead.from_scanner ? " (scanner)" : "",
                   lookahead.lex.token.token_span.start,
                   lookahead.lex.token.token_span.end);
    }
    auto entry = parse_state.terminal_entries.find(symbol);

    if (entry == parse_state.terminal_entries.end()) {
      // A token declared in `extras` may appear between any two tokens, so it
      // is shifted without changing state and marked so that the next
      // reduction adopts it as an extra child.
      if (!lookahead.lex.at_eof && IsExtraSymbol(symbol)) {
        stack_.Push(version, MakeLeaf(symbol, lookahead, /*extra=*/true),
                    state);
        return;
      }
      if (trace_) std::fprintf(stderr, "    no action; version dies\n");
      Halt(version);
      return;
    }

    // A shift marked `is_repetition` is the repeat-rule ambiguity that the
    // table generator flagged rather than resolved. Skipping it here -- so the
    // competing reduce wins -- is what stops a long repetition from forking
    // once per element. Mirrors parser.c's `if (action.shift.repetition)
    // break;`.
    std::vector<ParseAction> actions;
    for (const ParseAction& action : entry->second.actions) {
      if (action.kind == ParseActionKind::Shift && action.is_repetition) {
        continue;
      }
      actions.push_back(action);
    }
    if (actions.empty()) {
      Halt(version);
      return;
    }
    if (actions.size() > 1) result_.forks += actions.size() - 1;

    // Every action gets its own version. The last one reuses this version;
    // the rest get copies, made before any of them mutates anything.
    std::vector<StackVersion> targets;
    targets.reserve(actions.size());
    for (std::size_t i = 0; i + 1 < actions.size(); ++i) {
      targets.push_back(stack_.CopyVersion(version));
    }
    targets.push_back(version);

    for (std::size_t i = 0; i < actions.size(); ++i) {
      ApplyAction(actions[i], targets[i], symbol, lookahead);
    }
  }

  void ApplyAction(const ParseAction& action, StackVersion version,
                   SymbolData symbol, const Lookahead& lookahead) {
    switch (action.kind) {
      case ParseActionKind::Shift: {
        std::shared_ptr<CSTNode> leaf =
            MakeLeaf(symbol, lookahead, /*extra=*/false);
        stack_.Push(version, leaf, action.state);
        // Remember the scanner state this token left behind, so the next scan
        // for this version resumes from it.
        if (lookahead.from_scanner) {
          stack_.SetLastExternalToken(version, std::move(leaf));
        }
        return;
      }

      case ParseActionKind::Reduce:
        ApplyReduce(action, version);
        return;

      case ParseActionKind::Accept: {
        // Everything is taken, not just the start symbol: `extras` that sit
        // beside the root -- a comment before the first statement -- are still
        // on the stack, below or above it, and belong in the tree. The root is
        // rebuilt with them alongside its own children, which is what
        // ts_parser__accept does.
        const std::vector<StackSlice> slices = stack_.PopAll(version);
        for (const StackSlice& slice : slices) {
          std::shared_ptr<CSTNode> tree = BuildRoot(slice.subtrees);
          if (tree != nullptr) result_.trees.push_back(std::move(tree));
          Halt(slice.version);
        }
        Halt(version);
        return;
      }
    }
  }

  // Rebuilds the start symbol's node so that extras lying beside it become its
  // children. The last non-extra subtree is the root; it is replaced in the
  // list by its own children, and a fresh node is built over the result.
  std::shared_ptr<CSTNode> BuildRoot(
      const std::vector<std::shared_ptr<CSTNode>>& subtrees) {
    std::size_t root_index = subtrees.size();
    for (std::size_t i = subtrees.size(); i > 0; --i) {
      if (subtrees[i - 1] != nullptr && !subtrees[i - 1]->extra) {
        root_index = i - 1;
        break;
      }
    }
    if (root_index == subtrees.size()) return nullptr;

    const std::shared_ptr<CSTNode>& root = subtrees[root_index];
    // Nothing beside it, so the node already stands on its own.
    if (subtrees.size() == 1) return root;

    std::vector<std::shared_ptr<CSTNode>> children;
    children.reserve(subtrees.size() + root->children.size());
    children.insert(children.end(), subtrees.begin(),
                    subtrees.begin() + static_cast<std::ptrdiff_t>(root_index));
    children.insert(children.end(), root->children.begin(),
                    root->children.end());
    children.insert(
        children.end(),
        subtrees.begin() + static_cast<std::ptrdiff_t>(root_index) + 1,
        subtrees.end());

    auto node = std::make_shared<CSTNode>();
    node->symbol = root->symbol;
    node->production_id = root->production_id;
    node->visible = root->visible;
    node->named = root->named;
    node->dynamic_precedence = root->dynamic_precedence;
    node->has_external_token = root->has_external_token;
    node->children = std::move(children);
    if (!node->children.empty()) {
      node->padding = node->children.front()->padding;
      node->size = node->children.front()->size;
      for (std::size_t i = 1; i < node->children.size(); ++i) {
        node->size = node->size.Add(node->children[i]->SubtreeTotalSize());
      }
    }
    return node;
  }

  void ApplyReduce(const ParseAction& action, StackVersion version) {
    // PopCount does not mutate `version`; it walks back through the graph and
    // appends a head per distinct node it lands on. Several slices can share a
    // version when two paths converge on the same node -- that is a genuine
    // ambiguity, and each needs its own continuation.
    const std::vector<StackSlice> slices =
        stack_.PopCount(version, action.child_count);

    std::map<StackVersion, std::vector<const StackSlice*>> slices_by_version;
    for (const StackSlice& slice : slices) {
      slices_by_version[slice.version].push_back(&slice);
    }

    for (const auto& [base_version, group] : slices_by_version) {
      const StateId base_state = stack_.GetState(base_version);
      const ParseState& base = table_->states[base_state];
      auto goto_entry = base.nonterminal_entries.find(action.symbol);
      const bool is_extra_reduction =
          goto_entry == base.nonterminal_entries.end() &&
          std::find(table_->non_terminal_extras.begin(),
                    table_->non_terminal_extras.end(),
                    action.symbol) != table_->non_terminal_extras.end();
      if (goto_entry == base.nonterminal_entries.end() && !is_extra_reduction) {
        // Reachable after an LALR merge widened a lookahead: the reduction is
        // valid for some other path through this state but not this one.
        // Dropping the version is exactly the pruning GLR relies on.
        Halt(base_version);
        continue;
      }

      // Several slices sharing a version means several paths converged on the
      // same node -- the same span parsed more than one way. Rather than carry
      // every reading forward, pick one here by the Phase 7 rules. Keeping them
      // all is what previously let a real file end with hundreds of equivalent
      // parses and hundreds of live versions.
      std::shared_ptr<CSTNode> best;
      std::vector<std::shared_ptr<CSTNode>> best_extras;
      for (const StackSlice* slice : group) {
        // Extras that trail the production's last real symbol belong after the
        // node, not inside it -- a comment following a statement is not part
        // of it. They come back off the children and are re-pushed above the
        // new parent, so the next reduction can pick them up instead.
        std::vector<std::shared_ptr<CSTNode>> children = slice->subtrees;
        std::vector<std::shared_ptr<CSTNode>> trailing_extras;
        while (!children.empty() && children.back()->extra) {
          trailing_extras.push_back(std::move(children.back()));
          children.pop_back();
        }
        std::reverse(trailing_extras.begin(), trailing_extras.end());

        std::shared_ptr<CSTNode> candidate = MakeInterior(action, children);
        if (PreferRightTree(best, candidate)) {
          best = std::move(candidate);
          best_extras = std::move(trailing_extras);
        } else if (best != nullptr) {
          ++result_.ambiguities_resolved;
        }
      }
      if (group.size() > 1) ++result_.ambiguities_resolved;

      // A reduction to a non-terminal extra has no goto anywhere: it is not
      // part of any production. It is pushed as an extra and the state is left
      // exactly as it was, so the interrupted rule resumes where it left off.
      const StateId next_state =
          is_extra_reduction ? base_state : goto_entry->second;
      if (is_extra_reduction && best != nullptr) best->extra = true;

      stack_.Push(base_version, std::move(best), next_state);
      for (std::shared_ptr<CSTNode>& extra : best_extras) {
        stack_.Push(base_version, std::move(extra), next_state);
      }
    }

    Halt(version);
  }

  void SweepHaltedVersions() {
    // Backwards, because RemoveVersion shifts every higher index down.
    for (StackVersion version = stack_.GetVersionCount(); version > 0;
         --version) {
      const StackVersion index = version - 1;
      if (stack_.GetHead(index)->halted) stack_.RemoveVersion(index);
    }
  }

  // Two versions that agree on state and position have identical futures, so
  // they are folded into one node with several incoming links. This is what
  // keeps a shared prefix from being re-parsed once per alternative, and what
  // makes the stack a graph rather than a forest.
  void MergeEquivalentVersions() {
    for (StackVersion left = 0; left < stack_.GetVersionCount(); ++left) {
      for (StackVersion right = left + 1; right < stack_.GetVersionCount();) {
        if (stack_.Merge(left, right)) {
          ++result_.merges;
          // Merge removed `right`, so the next candidate has slid into place.
          continue;
        }
        ++right;
      }
    }
  }

  const CompiledGrammar* compiled_;
  const PreparedGrammar* grammar_;
  const ParseTable* table_;
  std::string_view input_;
  Stack stack_;
  std::map<std::pair<std::uint32_t, std::uint32_t>, LexResult> lex_cache_;
  const bool trace_ = std::getenv("TS_REF_TRACE_PARSE") != nullptr;
  ParseResult result_;
};

// How a node should appear: its own name, unless an alias renames it. A
// per-step alias from the parent's production wins over the symbol's global
// default alias, and either makes an otherwise hidden node visible.
struct NodeLabel {
  std::string name;
  bool visible = true;
  bool named = false;
};

NodeLabel LabelFor(const PreparedGrammar& grammar,
                   const std::shared_ptr<CSTNode>& node,
                   const std::optional<Alias>& step_alias) {
  if (step_alias.has_value()) {
    return NodeLabel{step_alias->value, true, step_alias->is_named};
  }
  const std::optional<SymbolData> symbol =
      SymbolDataForFlatSymbol(grammar, node->symbol);
  if (symbol.has_value()) {
    auto entry = grammar.default_aliases.find(*symbol);
    if (entry != grammar.default_aliases.end()) {
      return NodeLabel{entry->second.value, true, entry->second.is_named};
    }
  }
  return NodeLabel{FlatSymbolName(grammar, node->symbol), node->visible,
                   node->named};
}

// The production that built `node`, if it was built by a reduction.
const Production* ProductionFor(const PreparedGrammar& grammar,
                                const std::shared_ptr<CSTNode>& node) {
  const std::optional<SymbolData> symbol =
      SymbolDataForFlatSymbol(grammar, node->symbol);
  if (!symbol.has_value() || !symbol->IsNonTerminal()) return nullptr;
  if (symbol->index >= grammar.syntax.variables.size()) return nullptr;
  const SyntaxVariable& variable = grammar.syntax.variables[symbol->index];
  if (node->production_id >= variable.productions.size()) return nullptr;
  return &variable.productions[node->production_id];
}

void AppendSExpression(const PreparedGrammar& grammar,
                       const std::shared_ptr<CSTNode>& node,
                       std::string_view input, int depth, bool is_root,
                       const std::optional<Alias>& alias,
                       bool include_anonymous, std::string& out) {
  if (node == nullptr) return;

  const NodeLabel label = LabelFor(grammar, node, alias);

  // Hidden and auxiliary nodes contribute their children directly, which is
  // how a generated `_repeat` rule disappears from the output tree. An alias
  // makes such a node visible again, which is the whole point of writing
  // alias($._hidden, $.name).
  const Production* production = ProductionFor(grammar, node);
  const auto child_alias = [&](std::size_t step_index) -> std::optional<Alias> {
    if (production == nullptr || step_index >= production->steps.size()) {
      return std::nullopt;
    }
    return production->steps[step_index].alias;
  };

  if (!label.visible && !is_root) {
    std::size_t step_index = 0;
    for (const std::shared_ptr<CSTNode>& child : node->children) {
      AppendSExpression(grammar, child, input, depth, false,
                        child->extra ? std::nullopt : child_alias(step_index),
                        include_anonymous, out);
      if (!child->extra) ++step_index;
    }
    return;
  }

  // tree-sitter's convention: an anonymous node is written as the literal text
  // it matched, and a corpus expectation lists only the named ones. Following
  // it is what makes the output comparable with a grammar's own tests.
  if (!label.named && !include_anonymous && !is_root) {
    return;
  }

  if (!out.empty() && out.back() != '\n') out += "\n";
  out.append(2 * depth, ' ');
  if (!label.named) {
    absl::StrAppend(&out, "\"", label.name, "\"");
    return;
  }
  absl::StrAppend(&out, "(", label.name);

  // Extras are interleaved among the children but are not production steps, so
  // they must not advance the step cursor.
  std::size_t step_index = 0;
  for (const std::shared_ptr<CSTNode>& child : node->children) {
    AppendSExpression(grammar, child, input, depth + 1, false,
                      child->extra ? std::nullopt : child_alias(step_index),
                      include_anonymous, out);
    if (!child->extra) ++step_index;
  }
  out += ")";
}

}  // namespace

Symbol FlatTerminalSymbol(const PreparedGrammar& grammar, Symbol index) {
  return static_cast<Symbol>(1 + index);
}

Symbol FlatNonTerminalSymbol(const PreparedGrammar& grammar, Symbol index) {
  return static_cast<Symbol>(1 + grammar.lexical.variables.size() + index);
}

// Externals without an internal counterpart have no lexical variable, so they
// get their own block after the non-terminals.
Symbol FlatExternalSymbol(const PreparedGrammar& grammar, Symbol index) {
  return static_cast<Symbol>(1 + grammar.lexical.variables.size() +
                             grammar.syntax.variables.size() + index);
}

std::optional<SymbolData> SymbolDataForFlatSymbol(
    const PreparedGrammar& grammar, Symbol symbol) {
  if (symbol == 0) return std::nullopt;
  const std::size_t terminal_count = grammar.lexical.variables.size();
  const std::size_t variable_count = grammar.syntax.variables.size();
  std::size_t index = symbol - 1;
  if (index < terminal_count) {
    return TerminalSymbol(static_cast<Symbol>(index));
  }
  index -= terminal_count;
  if (index < variable_count) {
    return NonTerminalSymbol(static_cast<Symbol>(index));
  }
  index -= variable_count;
  if (index < grammar.syntax.external_tokens.size()) {
    return ExternalSymbol(static_cast<Symbol>(index));
  }
  return std::nullopt;
}

std::string FlatSymbolName(const PreparedGrammar& grammar, Symbol symbol) {
  if (symbol == 0) return "<eof>";
  const std::size_t terminal_count = grammar.lexical.variables.size();
  const std::size_t index = symbol - 1;
  if (index < terminal_count) return grammar.lexical.variables[index].name;
  const std::size_t variable_index = index - terminal_count;
  if (variable_index < grammar.syntax.variables.size()) {
    return grammar.syntax.variables[variable_index].name;
  }
  const std::size_t external_index =
      variable_index - grammar.syntax.variables.size();
  if (external_index < grammar.syntax.external_tokens.size()) {
    return grammar.syntax.external_tokens[external_index].name;
  }
  return absl::StrCat("<symbol ", symbol, ">");
}

absl::StatusOr<CompiledGrammar> CompileGrammar(
    const InputGrammar& grammar, const std::filesystem::path& grammar_path) {
  CompiledGrammar compiled;

  absl::StatusOr<PreparedGrammar> prepared = NormalizeGrammar(grammar);
  if (!prepared.ok()) return prepared.status();
  compiled.grammar = *std::move(prepared);

  absl::StatusOr<LexicalAutomaton> automaton =
      BuildLexicalAutomaton(compiled.grammar.lexical);
  if (!automaton.ok()) return automaton.status();
  compiled.automaton = *std::move(automaton);

  absl::StatusOr<ParseTable> parse_table = BuildParseTable(compiled.grammar);
  if (!parse_table.ok()) return parse_table.status();
  compiled.parse_table = *std::move(parse_table);

  absl::StatusOr<LexTableSet> lex_tables = BuildLexTables(
      compiled.grammar, compiled.automaton, compiled.parse_table);
  if (!lex_tables.ok()) return lex_tables.status();
  compiled.lex_tables = *std::move(lex_tables);

  // A grammar that declares externals but ships no scanner still compiles;
  // it simply cannot produce those tokens, and inputs needing them fail.
  if (!grammar_path.empty() &&
      !compiled.grammar.syntax.external_tokens.empty()) {
    absl::StatusOr<std::unique_ptr<ExternalScanner>> scanner =
        ExternalScanner::Load(grammar_path, grammar.name);
    if (!scanner.ok()) return scanner.status();
    compiled.scanner = std::shared_ptr<ExternalScanner>(*std::move(scanner));
  }

  return compiled;
}

absl::StatusOr<ParseResult> Parse(const CompiledGrammar& compiled,
                                  std::string_view input) {
  GlrParser parser(compiled, input);
  return parser.Run();
}

std::string ToSExpression(const PreparedGrammar& grammar,
                          const std::shared_ptr<CSTNode>& tree,
                          std::string_view input, bool include_anonymous) {
  std::string out;
  AppendSExpression(grammar, tree, input, 0, /*is_root=*/true, std::nullopt,
                    include_anonymous, out);
  return out;
}

}  // namespace ts_ref
