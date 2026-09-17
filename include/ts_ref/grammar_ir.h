#ifndef TS_REF_GRAMMAR_IR_H_
#define TS_REF_GRAMMAR_IR_H_

// this corresponds to crates/generate/src/rules.rs and parts of grammars.rs
// in the tree-sitter codebase, and UI-wise the tree-sitter generate command

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "ts_ref/types.h"

namespace ts_ref {

enum SymbolType {
  External = 0,
  End = 1,
  EndOfNonTerminalExtra = 2,
  Terminal = 3,
  NonTerminal = 4,
};

enum Associativity {
  Left,
  Right,
};

struct Alias {
  std::string value;
  bool is_named;

  bool operator==(const Alias&) const = default;
};

// static precedence used at compile-time during table generation to resulve
// shift/reduce and reduce/reduce conflicts, while dynamic precedence resolves
// runtime conflicts
class Precedence {
 public:
  Precedence() : value_{std::monostate{}} {}
  // Signed, not unsigned: real grammars use negative precedence to push a
  // rule *below* the default, e.g. prec(-1, ...) in tree-sitter-javascript.
  explicit Precedence(std::int32_t value) : value_{value} {}
  explicit Precedence(std::string name) : value_{std::move(name)} {}

  bool IsNone() const { return std::holds_alternative<std::monostate>(value_); }
  bool IsInteger() const {
    return std::holds_alternative<std::int32_t>(value_);
  }
  bool IsName() const { return std::holds_alternative<std::string>(value_); }

  std::int32_t AsInteger() const { return std::get<std::int32_t>(value_); }
  const std::string& AsName() const { return std::get<std::string>(value_); }

  bool operator==(const Precedence&) const = default;

 private:
  std::variant<std::monostate, std::int32_t, std::string> value_;
};

// One entry in grammar.js's `precedences: $ => [[...], ...]`, the ordered
// lists that give *relative* precedence to named precedence levels and
// symbols without assigning each one a number. Mirrors tree-sitter's
// PrecedenceEntry.
struct PrecedenceEntry {
  enum class Kind {
    // a named level, referred to by prec("name", ...)
    Name,
    // a rule referred to by $.rule_name
    Symbol,
  };

  Kind kind;
  std::string value;

  bool operator==(const PrecedenceEntry&) const = default;
};

// A symbol is namespaced by kind: a Terminal's index refers to the lexical
// grammar's variables, a NonTerminal's to the syntax grammar's, and an
// External's to the syntax grammar's external token list.
struct SymbolData {
  SymbolType kind;
  Symbol index;

  bool operator==(const SymbolData&) const = default;
  auto operator<=>(const SymbolData&) const = default;

  bool IsTerminal() const { return kind == SymbolType::Terminal; }
  bool IsNonTerminal() const { return kind == SymbolType::NonTerminal; }
  bool IsExternal() const { return kind == SymbolType::External; }
};

inline SymbolData NonTerminalSymbol(Symbol index) {
  return SymbolData{SymbolType::NonTerminal, index};
}
inline SymbolData TerminalSymbol(Symbol index) {
  return SymbolData{SymbolType::Terminal, index};
}
inline SymbolData ExternalSymbol(Symbol index) {
  return SymbolData{SymbolType::External, index};
}

using AliasMap = std::unordered_map<Symbol, Alias>;

enum VariableType {
  Hidden,
  Auxiliary,
  Anonymous,
  Named,
};

struct MetadataParams {
  // i.e. static precedence
  Precedence precedence;
  DynamicPrecedenceType dynamic_precedence = 0;
  std::optional<Associativity> associativity;
  bool is_token = false;
  // most rules are not aliased
  std::optional<Alias> alias;
  std::optional<std::string> field_name;
  // true when created by token.immediate(), "the token at this exact lexer
  // position, no separator", as opposed to just token(). Basically, anywhere
  // where things like whitespace / comments aren't allowed (e.g. between the
  // contents of a string literal and a quotation mark)
  bool is_main_token = false;
  // set by reserved(word_set_name, rule): within this rule, the named reserved
  // word set overrides the grammar-wide default. Names a key of
  // InputGrammar::reserved_word_sets
  std::optional<std::string> reserved_word_set_name;

  bool operator==(const MetadataParams&) const = default;
};

class Rule;

// empty rule, matching nothing
// produced by the DSL's blank()
struct Blank {};

// a string literal token written directly into rule
struct StringLiteral {
  std::string value;
};

// a regex token from DSL's token(...), e.g. token(/[a-z+]/)
// note that DSL doesn't parse or interpret regex, hence we need the flags
struct Pattern {
  std::string pattern;
  std::string flags;
};

// A reference to another rule by name; this is a transient rule,
// for instance what $.expression compiles to
struct NamedSymbolRef {
  std::string name;
};

// a reference to a rule by index (SymbolData)
struct ResolvedSymbol {
  SymbolData symbol_data;
};

// ordered sequence, seq(a, b, c)
struct Seq {
  std::vector<Rule> members;
};

// ONE or more of a sub-rule. This matches tree-sitter's internal convention,
// which is the inverse of the DSL's: the JSON tag REPEAT1 maps directly onto
// this node, while the zero-or-more REPEAT desugars to choice(Repeat(x),
// blank). Keeping tree-sitter's convention is what lets expand_repeats replace
// a Repeat with a single auxiliary symbol, since the auxiliary rule it
// generates (aux -> aux aux | x) is itself one-or-more.
class Repeat {
 public:
  Repeat(const Rule& target_rule) : rule{std::make_shared<Rule>(target_rule)} {}
  Repeat(Rule&& target_rule)
      : rule{std::make_shared<Rule>(std::move(target_rule))} {}
  Repeat(std::shared_ptr<Rule> old_ptr) : rule{std::move(old_ptr)} {}
  std::shared_ptr<Rule> rule;
};

// annotation carrier that decorates another rule
class Metadata {
 public:
  Metadata(const Rule& target_rule, MetadataParams param_agg)
      : rule{std::make_shared<Rule>(target_rule)},
        params{std::move(param_agg)} {}
  Metadata(Rule&& target_rule, MetadataParams param_agg)
      : rule{std::make_shared<Rule>(std::move(target_rule))},
        params{std::move(param_agg)} {}
  Metadata(std::shared_ptr<Rule> rule_ptr, MetadataParams param_agg)
      : rule{std::move(rule_ptr)}, params{std::move(param_agg)} {}
  std::shared_ptr<Rule> rule;
  MetadataParams params;
};

// one of n alternatives, from choice(a, b, c) or DSL array passing
// A class rather than struct, because flattening logic makes it more
// complex than a simple aggregate
class Choice {
 public:
  Choice(std::vector<Rule> rules);
  std::vector<Rule> members;
};

using StorageType = std::variant<Blank, StringLiteral, Pattern, NamedSymbolRef,
                                 ResolvedSymbol, Choice, Seq, Repeat, Metadata>;

class Rule {
 public:
  explicit Rule(StorageType rule_struct) : storage_{std::move(rule_struct)} {}

  // Read-only access to the underlying variant, so that visitors (dumping,
  // validation, the normalization passes) can live outside this class rather
  // than accreting as member functions.
  const StorageType& storage() const { return storage_; }

  // Structural (deep) equality. Not defaulted: Repeat and Metadata hold
  // shared_ptr<Rule>, so a defaulted comparison would compare pointer identity
  // rather than the rules pointed to. The normalization passes depend on this
  // being structural -- deduplicating identical extracted tokens and identical
  // repeat sub-rules is what keeps the symbol space from exploding.
  bool operator==(const Rule& other) const;

  bool IsChoice() const { return std::holds_alternative<Choice>(storage_); }
  // Any Rule where storage_ is not a Choice returns an empty vector
  std::vector<Rule> GetChoices() const;

  // Metadata static setter functions
  // needed to handle nesting rules within rules so that
  // metadata pointers get reused, instead of wrapping metadata in metadata
  // these will create a Metadata object if the rule doesn't already have a
  // Metadata object

  // sets field_name in MetadataParams
  static void Field(std::string name, Rule& rule);
  // sets is_token = true in MetadataParams
  static void Token(Rule& rule);
  // sets is_token = true and is_main_token = true in MetadataParams
  static void TokenImmediate(Rule& rule);
  // sets precedence, leaving associativity alone
  static void Prec(Precedence value, Rule& rule);
  // sets associativity = left
  static void PrecLeft(Rule& rule);
  // sets precedence as well as associativity = left
  static void PrecLeft(Precedence value, Rule& rule);
  // sets associativity = right
  static void PrecRight(Rule& rule);
  // sets precedence as well as sets associativity = right
  static void PrecRight(Precedence value, Rule& rule);
  // sets dynamic_precedence
  static void PrecDynamic(DynamicPrecedenceType value, Rule& rule);
  // sets alias, from the DSL's alias(rule, name)
  static void Alias(struct Alias value, Rule& rule);
  // sets reserved_word_set_name, from the DSL's reserved(word_set, rule)
  static void Reserved(std::string word_set_name, Rule& rule);

 private:
  StorageType storage_;
};

struct Variable {
  std::string name;
  VariableType kind;
  Rule rule;
};

// One named set from grammar.js's `reserved: { name: $ => [...] }`.
struct ReservedWordSet {
  std::string name;
  std::vector<Rule> rules;
};

// JSON deserialization target

struct InputGrammar {
  // the grammar's name, from grammar.js's grammar({ name: "...", ...})
  std::string name;
  // the rule table, each named rule becomes one Variable{name, kind, rule}
  // order of variables matters, index 0 is the start rule, therefore entry
  // point for the parser
  std::vector<Variable> variables;
  // set of rules e.g. whitespace and comments allowed to appear anywhere
  // between tokens without being written into the rules consumed in lexer
  // generation to splice in separator-skip between ordinary tokens
  std::vector<Rule> extra_symbols;
  // ordered list of tokens a hand-written scanner.c can produce instead of
  // generated DFA lexer, irrelevant in absence of external scanner
  // order determines scan priority when multiple external tokens are valid
  std::vector<Rule> external_tokens;
  // keywords naming which token rule represents "a word" for
  // keyword-extraction optimization, so that e.g. "if" gets parsed right-away
  // as a keyword rather than as an identifier, greatly reducing the lexer state
  // count
  std::optional<std::string> word_token;
  // name of the grammar this one extends, from grammar(base, {...}); absent
  // for a standalone grammar
  std::optional<std::string> inherits;
  // sets of rule names the grammar author declared as *expected* to conflict.
  // Recorded for fidelity and diagnostics only: a GLR parser forks on any
  // conflict, so unlike tree-sitter we never need this to decide whether a
  // conflict is permissible
  std::vector<std::vector<std::string>> expected_conflicts;
  // ordered lists giving relative precedence without explicit numbers; each
  // inner vector runs highest precedence first
  std::vector<std::vector<PrecedenceEntry>> precedences;
  // names of rules to be inlined into their referents during normalization,
  // rather than becoming states of their own
  std::vector<std::string> inline_variables;
  // names of rules that are abstract parents of a set of other rules; affects
  // node-type output and default aliasing
  std::vector<std::string> supertypes;
  // named sets of reserved words, in the order grammar.js declared them. A
  // vector rather than a map because the order is part of the grammar's
  // identity: normalization assigns ids by position, and an unordered_map
  // would make the resulting symbol space vary between runs
  std::vector<ReservedWordSet> reserved_word_sets;
};

}  // namespace ts_ref
#endif
