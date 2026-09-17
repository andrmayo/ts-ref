#include "ts_ref/regex.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "re2/regexp.h"

#include "ts_ref/character_set.h"

namespace ts_ref {
namespace {

// RE2 supplies the parser, not the matcher: re2::Regexp::Parse hands back an
// AST, and everything below converts that AST into the small RegexNode form the
// NFA builder consumes. The reason to depend on RE2 rather than hand-roll this
// is its Unicode character database -- \p{Ll}, \p{Sm} and friends have to be
// exact, and approximating them fails silently rather than loudly.

// The flags to parse a grammar pattern under. LikePerl is the closest RE2 gets
// to JavaScript regex syntax, which is what grammar.js patterns are written in.
// Assertions and backreferences that an NFA cannot honor are rejected below,
// either by RE2 itself (it has no backreferences) or by ConvertNode.
constexpr int kBaseParseFlags = re2::Regexp::LikePerl;

// RE2's Perl classes (\d, \w, \s) are ASCII-only, which is what a lexer
// wants: a Unicode \w spans tens of thousands of code points and would balloon
// the DFA. tree-sitter substitutes ASCII forms before parsing for the same
// reason, so no special handling is needed here.

// JavaScript accepts the long-form Unicode general category names, and the
// `General_Category=`/`Script=` prefixes; RE2 accepts only the short category
// abbreviations and bare script names. Rewriting the pattern before handing it
// over is what lets /\p{Letter_Number}/ work.
std::string ShortCategoryName(std::string_view name) {
  static const auto* const kAliases =
      new std::vector<std::pair<std::string_view, std::string_view>>{
          {"Letter", "L"},
          {"Cased_Letter", "LC"},
          {"Lowercase_Letter", "Ll"},
          {"Uppercase_Letter", "Lu"},
          {"Titlecase_Letter", "Lt"},
          {"Modifier_Letter", "Lm"},
          {"Other_Letter", "Lo"},
          {"Mark", "M"},
          {"Nonspacing_Mark", "Mn"},
          {"Spacing_Mark", "Mc"},
          {"Enclosing_Mark", "Me"},
          {"Number", "N"},
          {"Decimal_Number", "Nd"},
          {"Letter_Number", "Nl"},
          {"Other_Number", "No"},
          {"Punctuation", "P"},
          {"Connector_Punctuation", "Pc"},
          {"Dash_Punctuation", "Pd"},
          {"Open_Punctuation", "Ps"},
          {"Close_Punctuation", "Pe"},
          {"Initial_Punctuation", "Pi"},
          {"Final_Punctuation", "Pf"},
          {"Other_Punctuation", "Po"},
          {"Symbol", "S"},
          {"Math_Symbol", "Sm"},
          {"Currency_Symbol", "Sc"},
          {"Modifier_Symbol", "Sk"},
          {"Other_Symbol", "So"},
          {"Separator", "Z"},
          {"Space_Separator", "Zs"},
          {"Line_Separator", "Zl"},
          {"Paragraph_Separator", "Zp"},
          {"Other", "C"},
          {"Control", "Cc"},
          {"Format", "Cf"},
          {"Surrogate", "Cs"},
          {"Private_Use", "Co"},
          {"Unassigned", "Cn"},
      };
  for (const auto& [long_name, short_name] : *kAliases) {
    if (name == long_name) return std::string(short_name);
  }
  return std::string(name);
}

bool IsHexDigit(char character) {
  return (character >= '0' && character <= '9') ||
         (character >= 'a' && character <= 'f') ||
         (character >= 'A' && character <= 'F');
}

// Rewrites the handful of places where JavaScript regex syntax -- which is what
// grammar.js patterns are written in -- differs from what RE2 accepts:
//
//   \uXXXX and \u{XXXXX}   become RE2's \x{XXXX}
//   \p{Long_Category_Name} becomes RE2's short \p{Xx} abbreviation
//   \p{General_Category=X} loses the qualifier RE2 does not take
//
// Everything else is passed through untouched.
std::string TranslateJsRegex(std::string_view pattern) {
  std::string out;
  out.reserve(pattern.size());
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] != '\\' || i + 1 >= pattern.size()) {
      out += pattern[i];
      continue;
    }

    const char escaped = pattern[i + 1];

    if (escaped == 'u') {
      if (i + 2 < pattern.size() && pattern[i + 2] == '{') {
        // \u{1F600} -> \x{1F600}
        const std::size_t close = pattern.find('}', i + 3);
        if (close != std::string_view::npos) {
          absl::StrAppend(&out, "\\x{", pattern.substr(i + 3, close - (i + 3)),
                          "}");
          i = close;
          continue;
        }
      } else if (i + 5 < pattern.size() && IsHexDigit(pattern[i + 2]) &&
                 IsHexDigit(pattern[i + 3]) && IsHexDigit(pattern[i + 4]) &&
                 IsHexDigit(pattern[i + 5])) {
        // a 4-hex-digit escape such as \uFEFF -> \x{FEFF}
        absl::StrAppend(&out, "\\x{", pattern.substr(i + 2, 4), "}");
        i += 5;
        continue;
      }
    }

    if ((escaped == 'p' || escaped == 'P') && i + 2 < pattern.size() &&
        pattern[i + 2] == '{') {
      const std::size_t close = pattern.find('}', i + 3);
      if (close != std::string_view::npos) {
        std::string_view name = pattern.substr(i + 3, close - (i + 3));
        // Strip a `General_Category=` or `Script=` qualifier; RE2 resolves
        // both kinds of name out of the same namespace.
        const std::size_t equals = name.find('=');
        if (equals != std::string_view::npos) name = name.substr(equals + 1);
        absl::StrAppend(&out, "\\", std::string(1, escaped), "{",
                        ShortCategoryName(name), "}");
        i = close;
        continue;
      }
    }

    // An ordinary escape: copy both characters so that the escaped one is
    // never mistaken for the start of another escape.
    out += pattern[i];
    out += escaped;
    ++i;
  }
  return out;
}

class RegexConverter {
 public:
  explicit RegexConverter(std::string_view pattern) : pattern_{pattern} {}

  absl::StatusOr<RegexNode> Convert(re2::Regexp* regexp) {
    switch (regexp->op()) {
      case re2::kRegexpNoMatch:
        // Matches nothing at all, which no token rule should contain.
        return Error("subexpression matches nothing");

      case re2::kRegexpEmptyMatch:
        return RegexNode(RegexEmpty{});

      case re2::kRegexpLiteral:
        return RegexNode(
            RegexCharClass{RuneSet(regexp->rune(), regexp->parse_flags())});

      case re2::kRegexpLiteralString: {
        // A run of adjacent literal characters, which becomes a concatenation
        // of one-character classes.
        std::vector<RegexNode> members;
        members.reserve(static_cast<std::size_t>(regexp->nrunes()));
        for (int i = 0; i < regexp->nrunes(); ++i) {
          members.push_back(RegexNode(RegexCharClass{
              RuneSet(regexp->runes()[i], regexp->parse_flags())}));
        }
        if (members.empty()) return RegexNode(RegexEmpty{});
        if (members.size() == 1) return std::move(members.front());
        return RegexNode(RegexConcat{std::move(members)});
      }

      case re2::kRegexpCharClass: {
        CharacterSet set;
        re2::CharClass* character_class = regexp->cc();
        for (const re2::RuneRange& range : *character_class) {
          set.AddRange(static_cast<std::uint32_t>(range.lo),
                       static_cast<std::uint32_t>(range.hi));
        }
        // RE2 has already applied case folding to the class at parse time, and
        // has already resolved any \p{...} against its Unicode tables.
        return RegexNode(RegexCharClass{std::move(set)});
      }

      case re2::kRegexpAnyChar:
        return RegexNode(RegexCharClass{CharacterSet::Any()});

      case re2::kRegexpAnyByte:
        // \C, a single byte rather than a code point. The lexer works in code
        // points, so this cannot be represented faithfully.
        return Error("\\C (match any byte) is not supported");

      case re2::kRegexpConcat: {
        std::vector<RegexNode> members;
        members.reserve(static_cast<std::size_t>(regexp->nsub()));
        for (int i = 0; i < regexp->nsub(); ++i) {
          absl::StatusOr<RegexNode> member = Convert(regexp->sub()[i]);
          if (!member.ok()) return member;
          members.push_back(*std::move(member));
        }
        if (members.empty()) return RegexNode(RegexEmpty{});
        if (members.size() == 1) return std::move(members.front());
        return RegexNode(RegexConcat{std::move(members)});
      }

      case re2::kRegexpAlternate: {
        std::vector<RegexNode> members;
        members.reserve(static_cast<std::size_t>(regexp->nsub()));
        for (int i = 0; i < regexp->nsub(); ++i) {
          absl::StatusOr<RegexNode> member = Convert(regexp->sub()[i]);
          if (!member.ok()) return member;
          members.push_back(*std::move(member));
        }
        if (members.empty()) return RegexNode(RegexEmpty{});
        if (members.size() == 1) return std::move(members.front());
        return RegexNode(RegexAlternation{std::move(members)});
      }

      case re2::kRegexpStar:
        return ConvertRepetition(regexp->sub()[0], 0, std::nullopt);
      case re2::kRegexpPlus:
        return ConvertRepetition(regexp->sub()[0], 1, std::nullopt);
      case re2::kRegexpQuest:
        return ConvertRepetition(regexp->sub()[0], 0, 1);
      case re2::kRegexpRepeat: {
        const int min = regexp->min();
        const int max = regexp->max();
        return ConvertRepetition(
            regexp->sub()[0], static_cast<std::uint32_t>(min < 0 ? 0 : min),
            max < 0 ? std::nullopt
                    : std::optional<std::uint32_t>(
                          static_cast<std::uint32_t>(max)));
      }

      case re2::kRegexpCapture:
        // Captures carry no meaning for a lexer; keep only the body.
        return Convert(regexp->sub()[0]);

      // Everything below is a zero-width assertion. An NFA-based lexer decides
      // what to do purely from the character at hand, so it cannot express any
      // of these -- rejecting is honest, ignoring would silently mis-lex.
      case re2::kRegexpBeginLine:
      case re2::kRegexpBeginText:
        return Error("anchor '^' is not supported by an NFA-based lexer");
      case re2::kRegexpEndLine:
      case re2::kRegexpEndText:
        return Error("anchor '$' is not supported by an NFA-based lexer");
      case re2::kRegexpWordBoundary:
        return Error("\\b is not supported by an NFA-based lexer");
      case re2::kRegexpNoWordBoundary:
        return Error("\\B is not supported by an NFA-based lexer");

      default:
        return Error("unsupported regex construct");
    }
  }

 private:
  absl::Status Error(std::string_view message) const {
    return absl::InvalidArgumentError(
        absl::StrCat("regex /", pattern_, "/: ", message));
  }

  absl::StatusOr<RegexNode> ConvertRepetition(
      re2::Regexp* sub, std::uint32_t min, std::optional<std::uint32_t> max) {
    absl::StatusOr<RegexNode> inner = Convert(sub);
    if (!inner.ok()) return inner;
    return RegexNode(RegexRepetition{
        std::make_shared<RegexNode>(*std::move(inner)), min, max});
  }

  // A single literal character, expanded to its case-fold set when the
  // subexpression was parsed case-insensitively. RE2 folds character classes
  // during parsing but leaves literals flagged, so the expansion happens here.
  CharacterSet RuneSet(re2::Rune rune, int parse_flags) const {
    const auto code_point = static_cast<std::uint32_t>(rune);
    if ((parse_flags & re2::Regexp::FoldCase) == 0) {
      return CharacterSet::FromChar(code_point);
    }
    return FoldRune(code_point);
  }

  // Expands one code point to every code point that case-folds to it. Done by
  // handing the single character back to RE2 as a case-insensitive class, so
  // that RE2's fold tables -- not a hand-written approximation -- decide the
  // answer for non-ASCII characters.
  static CharacterSet FoldRune(std::uint32_t code_point) {
    CharacterSet set = CharacterSet::FromChar(code_point);
    if (code_point >= 'a' && code_point <= 'z') {
      set.AddChar(code_point - 'a' + 'A');
      return set;
    }
    if (code_point >= 'A' && code_point <= 'Z') {
      set.AddChar(code_point - 'A' + 'a');
      return set;
    }
    if (code_point < 0x80) return set;

    const std::string folded_pattern =
        absl::StrCat("(?i)[\\x{", absl::Hex(code_point), "}]");
    re2::RegexpStatus status;
    re2::Regexp* folded = re2::Regexp::Parse(
        folded_pattern, static_cast<re2::Regexp::ParseFlags>(kBaseParseFlags),
        &status);
    if (folded == nullptr) return set;
    if (folded->op() == re2::kRegexpCharClass) {
      for (const re2::RuneRange& range : *folded->cc()) {
        set.AddRange(static_cast<std::uint32_t>(range.lo),
                     static_cast<std::uint32_t>(range.hi));
      }
    }
    folded->Decref();
    return set;
  }

  std::string_view pattern_;
};

// Owns a parsed re2::Regexp so that every exit path releases it.
class RegexpHandle {
 public:
  explicit RegexpHandle(re2::Regexp* regexp) : regexp_{regexp} {}
  ~RegexpHandle() {
    if (regexp_ != nullptr) regexp_->Decref();
  }

  RegexpHandle(const RegexpHandle&) = delete;
  RegexpHandle& operator=(const RegexpHandle&) = delete;

  re2::Regexp* get() const { return regexp_; }

 private:
  re2::Regexp* regexp_;
};

}  // namespace

absl::StatusOr<RegexNode> ParseRegex(std::string_view pattern,
                                     std::string_view flags) {
  int parse_flags = kBaseParseFlags;
  if (flags.find('i') != std::string_view::npos) {
    parse_flags |= re2::Regexp::FoldCase;
  }
  // JavaScript's `s` flag (dotAll) makes `.` match newlines; without it, RE2's
  // DotNL stays off, which already matches JavaScript's default.
  if (flags.find('s') != std::string_view::npos) {
    parse_flags |= re2::Regexp::DotNL;
  }

  const std::string normalized = TranslateJsRegex(pattern);

  re2::RegexpStatus status;
  RegexpHandle handle(re2::Regexp::Parse(
      normalized, static_cast<re2::Regexp::ParseFlags>(parse_flags), &status));
  if (handle.get() == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("regex /", pattern, "/: ", status.Text()));
  }

  RegexConverter converter(pattern);
  return converter.Convert(handle.get());
}

}  // namespace ts_ref
