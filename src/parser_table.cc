#include "ts_ref/parser_table.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"

#include "ts_ref/grammar_ir.h"
#include "ts_ref/prepared_grammar.h"

namespace ts_ref {
namespace {

std::string SymbolName(SymbolData symbol, const PreparedGrammar& grammar) {
  switch (symbol.kind) {
    case SymbolType::End:
      return "<eof>";
    case SymbolType::EndOfNonTerminalExtra:
      return "<end-of-extra>";
    case SymbolType::NonTerminal:
      if (symbol.index < grammar.syntax.variables.size()) {
        return grammar.syntax.variables[symbol.index].name;
      }
      return absl::StrCat("<non-terminal ", symbol.index, "?>");
    case SymbolType::Terminal:
      if (symbol.index < grammar.lexical.variables.size()) {
        return absl::StrCat("'", grammar.lexical.variables[symbol.index].name,
                            "'");
      }
      return absl::StrCat("<terminal ", symbol.index, "?>");
    case SymbolType::External:
      if (symbol.index < grammar.syntax.external_tokens.size()) {
        return absl::StrCat(
            "<", grammar.syntax.external_tokens[symbol.index].name, ">");
      }
      return absl::StrCat("<external ", symbol.index, "?>");
    default:
      return "<?>";
  }
}

// True when a multi-action entry is just a repeat rule conflicting with
// itself, which the engine settles without forking.
bool EntryIsRepetition(const ParseTableEntry& entry) {
  return std::any_of(entry.actions.begin(), entry.actions.end(),
                     [](const ParseAction& action) {
                       return action.kind == ParseActionKind::Shift &&
                              action.is_repetition;
                     });
}

std::string DescribeAction(const ParseAction& action,
                           const PreparedGrammar& grammar) {
  switch (action.kind) {
    case ParseActionKind::Shift:
      return absl::StrCat("shift ", action.state,
                          action.is_repetition ? " (repeat)" : "");
    case ParseActionKind::Reduce:
      return absl::StrCat(
          "reduce ", SymbolName(action.symbol, grammar), " <- ",
          action.child_count, " child", action.child_count == 1 ? "" : "ren",
          action.dynamic_precedence != 0
              ? absl::StrCat(" dyn_prec=", action.dynamic_precedence)
              : "");
    case ParseActionKind::Accept:
      return "accept";
  }
  return "<?>";
}

}  // namespace

std::string DescribeParseTable(const ParseTable& table,
                               const PreparedGrammar& grammar) {
  std::string out = absl::StrCat("parse table: ", table.states.size(),
                                 " states, start = ", table.start_state, "\n");
  // A multi-action entry marked `is_repetition` is the intentional ambiguity
  // of a repeat rule, which the engine resolves by preferring the shift rather
  // than forking. Counting those separately keeps the interesting number
  // honest.
  std::size_t fork_entries = 0;
  std::size_t repetition_entries = 0;
  for (const ParseState& state : table.states) {
    for (const auto& [symbol, entry] : state.terminal_entries) {
      if (!entry.IsConflict()) continue;
      if (EntryIsRepetition(entry)) {
        ++repetition_entries;
      } else {
        ++fork_entries;
      }
    }
  }
  absl::StrAppend(&out, "  fork sites: ", fork_entries,
                  "   repeat-ambiguity entries: ", repetition_entries, "\n");

  for (std::size_t i = 0; i < table.states.size(); ++i) {
    const ParseState& state = table.states[i];
    absl::StrAppend(&out, "\n  state ", i, "  [lex ", state.lex_state_id,
                    "]\n");
    for (const auto& [symbol, entry] : state.terminal_entries) {
      absl::StrAppend(&out, "    ", SymbolName(symbol, grammar), " -> ");
      for (std::size_t a = 0; a < entry.actions.size(); ++a) {
        if (a > 0) out += " | ";
        out += DescribeAction(entry.actions[a], grammar);
      }
      if (entry.IsConflict()) {
        out +=
            EntryIsRepetition(entry) ? "   (repeat ambiguity)" : "   <-- fork";
      }
      out += "\n";
    }
    for (const auto& [symbol, next_state] : state.nonterminal_entries) {
      absl::StrAppend(&out, "    ", SymbolName(symbol, grammar), " => goto ",
                      next_state, "\n");
    }
  }
  return out;
}

std::string DescribeConflicts(const ParseTable& table,
                              const PreparedGrammar& grammar) {
  if (table.conflicts.empty()) {
    return "no unresolved conflicts: this grammar is LR(1)\n";
  }
  std::size_t expected = 0;
  for (const TableConflict& conflict : table.conflicts) {
    if (conflict.is_expected) ++expected;
  }
  std::string out =
      absl::StrCat(table.conflicts.size(),
                   " unresolved conflict(s); the runtime will fork at these "
                   "points");
  if (expected > 0) {
    absl::StrAppend(&out, " (", expected,
                    " declared in the grammar's "
                    "`conflicts`)");
  }
  out += ":\n";
  for (const TableConflict& conflict : table.conflicts) {
    absl::StrAppend(&out, "  state ", conflict.state, " on ",
                    SymbolName(conflict.lookahead, grammar), ": ",
                    conflict.shift_count, " shift / ", conflict.reduce_count,
                    " reduce");
    if (!conflict.rule_names.empty()) {
      out += "  between";
      for (const std::string& name : conflict.rule_names) {
        absl::StrAppend(&out, " ", name);
      }
    }
    if (conflict.is_expected) out += "  [declared]";
    out += "\n";
  }
  return out;
}

}  // namespace ts_ref
