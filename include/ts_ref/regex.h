#ifndef TS_REF_REGEX_H_
#define TS_REF_REGEX_H_

// A parser for the regex subset that tree-sitter grammars use.
//
// The grammar DSL does not interpret regexes -- a `Pattern` rule carries the
// raw source text and flags straight through from JavaScript -- so something
// has to parse them before an NFA can be built. tree-sitter delegates this to
// the `regex_syntax` crate; lacking that, this is a hand-written parser over
// the JavaScript regex syntax that actually appears in grammars.
//
// The output is deliberately small: a regex here only ever has to be compiled
// into an NFA, so the AST collapses literals and character classes into the
// same node (a set of code points to advance over) and keeps no capture
// information.
//
// Deliberately unsupported, because an NFA-based lexer cannot honor them:
// anchors (^ $ \b \B), lookaround ((?= (?! (?<= (?<!), and backreferences.
// These are rejected rather than ignored.

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"

#include "ts_ref/character_set.h"

namespace ts_ref {

class RegexNode;

// Matches the empty string. Produced by e.g. an empty alternation branch.
struct RegexEmpty {};

// Advances over exactly one code point drawn from this set. Both a literal
// character and a bracketed class compile to this -- a literal is just a
// one-element set.
struct RegexCharClass {
  CharacterSet characters;
};

struct RegexConcat {
  std::vector<RegexNode> members;
};

struct RegexAlternation {
  std::vector<RegexNode> members;
};

// `max` absent means unbounded. Greedy and lazy quantifiers parse to the same
// node: laziness only affects which match a backtracking engine prefers, and
// this compiles to an NFA that explores all alternatives at once.
struct RegexRepetition {
  std::shared_ptr<RegexNode> sub;
  std::uint32_t min = 0;
  std::optional<std::uint32_t> max;
};

using RegexStorage = std::variant<RegexEmpty, RegexCharClass, RegexConcat,
                                  RegexAlternation, RegexRepetition>;

class RegexNode {
 public:
  explicit RegexNode(RegexStorage storage) : storage_{std::move(storage)} {}

  const RegexStorage& storage() const { return storage_; }

 private:
  RegexStorage storage_;
};

// Parses `pattern` with the given JavaScript regex `flags` (only "i" is
// meaningful here; others are accepted and ignored). Returns InvalidArgument
// with a message naming the offending construct for anything unsupported.
absl::StatusOr<RegexNode> ParseRegex(std::string_view pattern,
                                     std::string_view flags);

}  // namespace ts_ref

#endif
