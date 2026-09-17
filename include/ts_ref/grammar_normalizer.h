#ifndef TS_REF_GRAMMAR_NORMALIZER_H_
#define TS_REF_GRAMMAR_NORMALIZER_H_

// Corresponds to tree-sitter's crates/generate/src/prepare_grammar/. Four
// passes run in order, each depending on the last:
//
//   1. InternSymbols  -- resolve every $.rule_name reference to a SymbolData,
//                        erroring on undefined references
//   2. ExtractTokens  -- split the lexical grammar out of the syntactic one:
//                        every string literal and pattern becomes a terminal,
//                        deduplicated, and a rule that is *entirely* a token
//                        collapses into that terminal
//   3. ExpandRepeats  -- rewrite repeat(x) into a generated auxiliary rule, so
//                        that the flattened grammar has no repetition left
//   4. FlattenGrammar -- turn each rule tree into a flat list of Productions,
//                        pushing precedence/associativity/alias/field metadata
//                        down onto the individual steps
//
// Not yet implemented, and not needed for the core GLR pipeline:
// extract_default_aliases and process_inlines. Their absence means
// `supertypes` and `inline` are carried through as symbol lists but not yet
// acted upon.

#include <string>
#include <vector>

#include "absl/status/statusor.h"

#include "ts_ref/grammar_ir.h"
#include "ts_ref/prepared_grammar.h"

namespace ts_ref {

// Runs all four passes. Returns InvalidArgument for a grammar that is
// well-formed JSON but not a well-formed grammar -- an undefined rule
// reference, a hidden start rule, a non-symbol external token.
absl::StatusOr<PreparedGrammar> NormalizeGrammar(const InputGrammar& grammar);

// Names of non-terminals unreachable from the start rule. Not an error: an
// unreachable rule is usually a typo or a leftover, but it is harmless, so
// this is reported rather than rejected.
std::vector<std::string> FindUnreachableVariables(const SyntaxGrammar& grammar);

}  // namespace ts_ref

#endif
