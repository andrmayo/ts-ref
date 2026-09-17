#ifndef TS_REF_EXTERNAL_SCANNER_H_
#define TS_REF_EXTERNAL_SCANNER_H_

// Phase 4: hand-written external scanners.
//
// Several widely-used grammars declare `externals: $ => [...]` and ship a
// `scanner.c` to recognize tokens a regular language cannot describe --
// Python's indent/dedent, heredocs, JavaScript's template-literal chunks and
// automatic semicolons. The generated DFA lexer simply cannot produce those, so
// supporting real grammars means running the grammar's own C code.
//
// This does what tree-sitter does: compile the grammar's `scanner.c` with the
// system C compiler into a shared library, `dlopen` it, and call into it
// through the `TSLexer` function-pointer ABI that tree-sitter's
// `tree_sitter/parser.h` defines.
//
// The serialize/deserialize half of that ABI is not optional here, even though
// this project has no incremental reparsing. Under GLR forking, two stack
// versions can disagree about scanner state -- one inside an indented block,
// one not -- so a single live scanner instance cannot be correct for both.
// Each version carries its own serialized state, restored before every scan.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

#include "ts_ref/types.h"

namespace ts_ref {

// The ABI tree-sitter's parser.h declares. The layout must match exactly: a
// compiled scanner reaches through this pointer by offset. Field order is
// tree-sitter's, and `log` is last so that a scanner built against an older
// header (which lacked it) still agrees on everything it does touch.
extern "C" {
using TSSymbolAbi = std::uint16_t;
struct TSLexerAbi;
struct TSLexerAbi {
  std::int32_t lookahead;
  TSSymbolAbi result_symbol;
  void (*advance)(TSLexerAbi*, bool);
  void (*mark_end)(TSLexerAbi*);
  std::uint32_t (*get_column)(TSLexerAbi*);
  bool (*is_at_included_range_start)(const TSLexerAbi*);
  bool (*eof)(const TSLexerAbi*);
  void (*log)(const TSLexerAbi*, const char*, ...);
};
}

// tree-sitter's TREE_SITTER_SERIALIZATION_BUFFER_SIZE. Scanners assume they may
// write this much.
inline constexpr std::size_t kSerializationBufferSize = 1024;

// A scanner's serialized state, small enough to copy onto every token.
using ScannerState = std::string;

// A compiled, loaded `scanner.c`.
class ExternalScanner {
 public:
  ~ExternalScanner();

  ExternalScanner(const ExternalScanner&) = delete;
  ExternalScanner& operator=(const ExternalScanner&) = delete;

  // Compiles and loads the scanner beside `grammar_path`, if one exists.
  // Returns nullptr (not an error) when the grammar has no scanner, since most
  // grammars do not.
  static absl::StatusOr<std::unique_ptr<ExternalScanner>> Load(
      const std::filesystem::path& grammar_path,
      const std::string& grammar_name);

  // The outcome of one scan attempt.
  struct ScanResult {
    bool found = false;
    // Index into the grammar's `externals` list.
    std::uint32_t external_index = 0;
    // Byte offsets: [padding_start, token_start) is what the scanner skipped,
    // [token_start, token_end) the token itself.
    std::uint32_t token_start = 0;
    std::uint32_t token_end = 0;
    // The scanner's state after the scan, to be stored on the new token.
    ScannerState state;
  };

  // Runs the scanner at `position`, with `previous_state` restored first.
  // `valid_symbols` is indexed by external token index.
  ScanResult Scan(std::string_view input, std::uint32_t position,
                  const ScannerState& previous_state,
                  const std::vector<bool>& valid_symbols);

 private:
  ExternalScanner() = default;

  void* handle_ = nullptr;
  void* payload_ = nullptr;
  std::filesystem::path library_path_;

  void* (*create_)() = nullptr;
  void (*destroy_)(void*) = nullptr;
  bool (*scan_)(void*, TSLexerAbi*, const bool*) = nullptr;
  unsigned (*serialize_)(void*, char*) = nullptr;
  void (*deserialize_)(void*, const char*, unsigned) = nullptr;
};

}  // namespace ts_ref

#endif
