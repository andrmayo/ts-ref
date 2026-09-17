#include "ts_ref/dfa.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"

#include "ts_ref/character_set.h"
#include "ts_ref/token.h"
#include "ts_ref/types.h"

namespace ts_ref {

std::uint32_t Lexer::DecodeAt(std::uint32_t offset,
                              std::uint32_t* width) const {
  const auto byte = static_cast<unsigned char>(input_[offset]);
  if (byte < 0x80) {
    *width = 1;
    return byte;
  }
  std::uint32_t extra = 0;
  std::uint32_t value = 0;
  if ((byte & 0xE0) == 0xC0) {
    extra = 1;
    value = byte & 0x1F;
  } else if ((byte & 0xF0) == 0xE0) {
    extra = 2;
    value = byte & 0x0F;
  } else if ((byte & 0xF8) == 0xF0) {
    extra = 3;
    value = byte & 0x07;
  } else {
    // Malformed leading byte. Consume it alone so that scanning can make
    // progress and report an error rather than looping.
    *width = 1;
    return byte;
  }
  if (offset + extra >= input_.size()) {
    *width = 1;
    return byte;
  }
  for (std::uint32_t i = 1; i <= extra; ++i) {
    value =
        (value << 6) | (static_cast<unsigned char>(input_[offset + i]) & 0x3F);
  }
  *width = extra + 1;
  return value;
}

LexResult Lexer::Next() {
  LexResult result;
  const std::uint32_t start = position_;

  DfaStateId state = table_->start_state;
  // Where the token's own text begins, as opposed to the leading extras. Set
  // the first time a transition marked in_main_token is taken.
  std::optional<std::uint32_t> token_start;
  // The most recent accepting position, which is what longest-match backs up
  // to when the walk gets stuck.
  std::optional<std::uint32_t> last_accept_end;
  std::optional<Symbol> last_accept_token;
  std::optional<std::uint32_t> last_accept_token_start;
  bool at_eof = false;

  std::uint32_t offset = position_;
  while (true) {
    const LexState& lex_state = table_->states[state];
    if (lex_state.accept_token.has_value()) {
      last_accept_end = offset;
      last_accept_token = lex_state.accept_token;
      last_accept_token_start = token_start;
    }

    if (offset >= input_.size()) {
      // End of input can itself be the accepting condition, for a state whose
      // token is complete exactly at EOF.
      if (lex_state.accepts_eof && !last_accept_end.has_value()) {
        last_accept_end = offset;
        last_accept_token = kEndOfInput;
        last_accept_token_start = token_start;
        at_eof = true;
      }
      break;
    }

    std::uint32_t width = 0;
    const std::uint32_t character = DecodeAt(offset, &width);

    const AdvanceAction* action = nullptr;
    for (const auto& [characters, advance] : lex_state.advance_actions) {
      if (characters.Contains(character)) {
        action = &advance;
        break;
      }
    }
    if (action == nullptr) break;

    if (action->in_main_token && !token_start.has_value()) {
      token_start = offset;
    }
    offset += width;
    state = action->state;
  }

  if (!last_accept_token.has_value()) {
    // Nothing matched. Report the failure and consume one character, so a
    // caller that keeps scanning cannot spin.
    result.error = true;
    std::uint32_t width = 1;
    if (start < input_.size()) DecodeAt(start, &width);
    position_ = start + width;
    result.padding = ByteRange{start, start};
    result.token = Token{kEndOfInput, ByteRange{start, position_}};
    return result;
  }

  // A token made entirely of extras (or one matching empty) has no main text,
  // so its span starts where the scan ended.
  const std::uint32_t text_start =
      last_accept_token_start.value_or(*last_accept_end);
  result.padding = ByteRange{start, text_start};
  result.at_eof = at_eof;
  result.token =
      Token{*last_accept_token, ByteRange{text_start, *last_accept_end}};
  position_ = *last_accept_end;

  // A zero-width match that consumed nothing would leave the caller looping.
  // The one legitimate case is the end-of-input token.
  if (position_ == start && *last_accept_token != kEndOfInput) {
    result.error = true;
    std::uint32_t width = 1;
    if (start < input_.size()) DecodeAt(start, &width);
    position_ = start + width;
  }
  return result;
}

Point Lexer::PointAt(std::uint32_t offset) const {
  std::uint32_t row = 0;
  std::uint32_t column = 0;
  for (std::uint32_t i = 0; i < offset && i < input_.size(); ++i) {
    if (input_[i] == '\n') {
      ++row;
      column = 0;
    } else {
      ++column;
    }
  }
  return Point(row, column);
}

std::string DescribeLexTable(const LexTable& table,
                             const std::vector<std::string>& token_names) {
  const auto name_of = [&token_names](Symbol symbol) -> std::string {
    if (symbol == kEndOfInput) return "<eof>";
    if (symbol < token_names.size()) return token_names[symbol];
    return absl::StrCat("<token ", symbol, ">");
  };

  std::string out = absl::StrCat("lex table: ", table.states.size(),
                                 " states, start = ", table.start_state, "\n");
  for (std::size_t i = 0; i < table.states.size(); ++i) {
    const LexState& state = table.states[i];
    absl::StrAppend(&out, "\n  state ", i);
    if (state.accept_token.has_value()) {
      absl::StrAppend(&out, "  accept: ", name_of(*state.accept_token));
    }
    if (state.accepts_eof) out += "  [eof ok]";
    out += "\n";
    for (const auto& [characters, action] : state.advance_actions) {
      absl::StrAppend(&out, "    ", characters.ToString(), " -> ", action.state,
                      action.in_main_token ? "" : " (sep)", "\n");
    }
  }
  return out;
}

}  // namespace ts_ref
