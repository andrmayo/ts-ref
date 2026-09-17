#include "ts_ref/grammar_ir.h"

#include <utility>
#include <variant>
#include <vector>

namespace ts_ref {

namespace {
// note that there's no deduplication here
std::vector<Rule> FlattenChoice(std::vector<Rule> input_vec) {
  std::vector<Rule> output_vec;
  for (auto& rule : input_vec) {
    if (!rule.IsChoice()) {
      output_vec.push_back(std::move(rule));
      continue;
    }
    std::vector<Rule> rule_stack{std::move(rule)};
    while (rule_stack.size() > 0) {
      auto cur_rule = std::move(rule_stack.back());
      rule_stack.pop_back();
      if (!cur_rule.IsChoice()) {
        output_vec.push_back(std::move(cur_rule));
        continue;
      }
      // insert into rule_stack in reverse order
      auto cur_choices = cur_rule.GetChoices();
      rule_stack.insert(rule_stack.end(), cur_choices.rbegin(),
                        cur_choices.rend());
    }
  }
  return output_vec;
}
}  // namespace

Choice::Choice(std::vector<Rule> rules) {
  members = FlattenChoice(std::move(rules));
}

bool Rule::operator==(const Rule& other) const {
  if (storage_.index() != other.storage_.index()) return false;
  return std::visit(
      [](const auto& left, const auto& right) -> bool {
        using LeftType = std::decay_t<decltype(left)>;
        using RightType = std::decay_t<decltype(right)>;
        if constexpr (!std::is_same_v<LeftType, RightType>) {
          // unreachable given the index() check above, but std::visit still
          // instantiates every pairing
          return false;
        } else if constexpr (std::is_same_v<LeftType, Blank>) {
          return true;
        } else if constexpr (std::is_same_v<LeftType, StringLiteral>) {
          return left.value == right.value;
        } else if constexpr (std::is_same_v<LeftType, Pattern>) {
          return left.pattern == right.pattern && left.flags == right.flags;
        } else if constexpr (std::is_same_v<LeftType, NamedSymbolRef>) {
          return left.name == right.name;
        } else if constexpr (std::is_same_v<LeftType, ResolvedSymbol>) {
          return left.symbol_data == right.symbol_data;
        } else if constexpr (std::is_same_v<LeftType, Choice> ||
                             std::is_same_v<LeftType, Seq>) {
          // recurses through vector's operator==
          return left.members == right.members;
        } else if constexpr (std::is_same_v<LeftType, Repeat>) {
          return *left.rule == *right.rule;
        } else {
          static_assert(std::is_same_v<LeftType, Metadata>);
          return left.params == right.params && *left.rule == *right.rule;
        }
      },
      storage_, other.storage_);
}

std::vector<Rule> Rule::GetChoices() const {
  if (!std::holds_alternative<Choice>(storage_)) {
    return std::vector<Rule>{};
  }
  return std::get<Choice>(storage_).members;
}

// Metadata static setter functions
void Rule::Field(std::string name, Rule& rule) {
  if (std::holds_alternative<Metadata>(rule.storage_)) {
    std::get<Metadata>(rule.storage_).params.field_name = name;
    return;
  }
  auto params = MetadataParams{.field_name = name};
  auto metadata = Metadata(std::move(rule), std::move(params));
  rule = Rule(std::move(metadata));
}

void Rule::Token(Rule& rule) {
  if (std::holds_alternative<Metadata>(rule.storage_)) {
    std::get<Metadata>(rule.storage_).params.is_token = true;
    return;
  }
  auto params = MetadataParams{.is_token = true};
  auto metadata = Metadata(std::move(rule), std::move(params));
  rule = Rule(std::move(metadata));
}

void Rule::TokenImmediate(Rule& rule) {
  if (std::holds_alternative<Metadata>(rule.storage_)) {
    std::get<Metadata>(rule.storage_).params.is_token = true;
    std::get<Metadata>(rule.storage_).params.is_main_token = true;
    return;
  }
  auto params = MetadataParams{.is_token = true, .is_main_token = true};
  auto metadata = Metadata(std::move(rule), std::move(params));
  rule = Rule(std::move(metadata));
}

void Rule::Prec(Precedence value, Rule& rule) {
  if (std::holds_alternative<Metadata>(rule.storage_)) {
    std::get<Metadata>(rule.storage_).params.precedence = value;
    return;
  }
  auto params = MetadataParams{.precedence = value};
  auto metadata = Metadata(std::move(rule), std::move(params));
  rule = Rule(std::move(metadata));
}

void Rule::PrecLeft(Rule& rule) {
  if (std::holds_alternative<Metadata>(rule.storage_)) {
    std::get<Metadata>(rule.storage_).params.associativity =
        Associativity::Left;
    return;
  }
  auto params = MetadataParams{.associativity = Associativity::Left};
  auto metadata = Metadata(std::move(rule), std::move(params));
  rule = Rule(std::move(metadata));
}

void Rule::PrecLeft(Precedence value, Rule& rule) {
  if (std::holds_alternative<Metadata>(rule.storage_)) {
    std::get<Metadata>(rule.storage_).params.precedence = value;
    std::get<Metadata>(rule.storage_).params.associativity =
        Associativity::Left;
    return;
  }
  auto params =
      MetadataParams{.precedence = value, .associativity = Associativity::Left};
  auto metadata = Metadata(std::move(rule), std::move(params));
  rule = Rule(std::move(metadata));
}

void Rule::PrecRight(Rule& rule) {
  if (std::holds_alternative<Metadata>(rule.storage_)) {
    std::get<Metadata>(rule.storage_).params.associativity =
        Associativity::Right;
    return;
  }
  auto params = MetadataParams{.associativity = Associativity::Right};
  auto metadata = Metadata(std::move(rule), std::move(params));
  rule = Rule(std::move(metadata));
}

void Rule::PrecRight(Precedence value, Rule& rule) {
  if (std::holds_alternative<Metadata>(rule.storage_)) {
    std::get<Metadata>(rule.storage_).params.precedence = value;
    std::get<Metadata>(rule.storage_).params.associativity =
        Associativity::Right;
    return;
  }
  auto params = MetadataParams{.precedence = value,
                               .associativity = Associativity::Right};
  auto metadata = Metadata(std::move(rule), std::move(params));
  rule = Rule(std::move(metadata));
}

void Rule::PrecDynamic(DynamicPrecedenceType value, Rule& rule) {
  if (std::holds_alternative<Metadata>(rule.storage_)) {
    std::get<Metadata>(rule.storage_).params.dynamic_precedence = value;
    return;
  }
  auto params = MetadataParams{.dynamic_precedence = value};
  auto metadata = Metadata(std::move(rule), std::move(params));
  rule = Rule(std::move(metadata));
}

void Rule::Alias(struct Alias value, Rule& rule) {
  if (std::holds_alternative<Metadata>(rule.storage_)) {
    std::get<Metadata>(rule.storage_).params.alias = std::move(value);
    return;
  }
  auto params = MetadataParams{.alias = std::move(value)};
  auto metadata = Metadata(std::move(rule), std::move(params));
  rule = Rule(std::move(metadata));
}

void Rule::Reserved(std::string word_set_name, Rule& rule) {
  if (std::holds_alternative<Metadata>(rule.storage_)) {
    std::get<Metadata>(rule.storage_).params.reserved_word_set_name =
        std::move(word_set_name);
    return;
  }
  auto params =
      MetadataParams{.reserved_word_set_name = std::move(word_set_name)};
  auto metadata = Metadata(std::move(rule), std::move(params));
  rule = Rule(std::move(metadata));
}

}  // namespace ts_ref
