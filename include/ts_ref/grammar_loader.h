#ifndef TS_REF_GRAMMAR_LOADER_H_
#define TS_REF_GRAMMAR_LOADER_H_

// Phase 2: turn a tree-sitter grammar.js into an in-memory InputGrammar.
//
// Mirrors how tree-sitter itself loads a grammar: shell out to `node` to
// evaluate the JS DSL down to JSON (js/dsl.js + js/runner.js), then
// hand-deserialize that JSON into the IR. The JSON library is responsible only
// for turning text into a generic value tree; the mapping onto Rule is written
// out by hand, because Rule's JSON form is a tagged union whose payload fields
// differ per tag and so doesn't fit struct-reflection-based deserialization.

#include <filesystem>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

#include "ts_ref/grammar_ir.h"

namespace ts_ref {

// Runs `node <runner.js> <grammar_js_path>` and returns the JSON text it
// writes to stdout. The runner's stderr is inherited, so JS-level grammar
// errors surface directly to the caller's terminal.
//
// The runner script is located via the TS_REF_JS_DIR environment variable
// when set, and otherwise via the source directory baked in at build time.
absl::StatusOr<std::string> RunGrammarJs(
    const std::filesystem::path& grammar_js_path);

// Deserializes the JSON produced by RunGrammarJs into the grammar IR.
// Exposed separately so a pre-generated grammar.json can be loaded without
// requiring node on the system.
absl::StatusOr<InputGrammar> ParseGrammarJson(std::string_view json_text);

// Convenience wrapper: evaluates a .js grammar through node, or reads a .json
// grammar directly, based on the file extension.
absl::StatusOr<InputGrammar> LoadGrammar(
    const std::filesystem::path& grammar_path);

}  // namespace ts_ref

#endif
