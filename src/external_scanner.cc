#include "ts_ref/external_scanner.h"

#include <dlfcn.h>
#include <sys/wait.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

namespace ts_ref {
namespace {

// Drives a scanner over the input. The TSLexerAbi must be the first member:
// the scanner is handed a TSLexerAbi* and the callbacks cast it back.
struct ScannerLexer {
  TSLexerAbi base{};
  std::string_view input;
  std::uint32_t position = 0;
  std::uint32_t token_start = 0;
  std::uint32_t token_end = 0;
  bool end_marked = false;

  static ScannerLexer* From(TSLexerAbi* base) {
    return reinterpret_cast<ScannerLexer*>(base);
  }
  static const ScannerLexer* From(const TSLexerAbi* base) {
    return reinterpret_cast<const ScannerLexer*>(base);
  }

  // Decodes the code point at the cursor into `base.lookahead`. At end of
  // input the scanner expects to see 0, which is how it detects EOF.
  void RefreshLookahead() {
    if (position >= input.size()) {
      base.lookahead = 0;
      width_ = 0;
      return;
    }
    const auto byte = static_cast<unsigned char>(input[position]);
    if (byte < 0x80) {
      base.lookahead = byte;
      width_ = 1;
      return;
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
      base.lookahead = byte;
      width_ = 1;
      return;
    }
    if (position + extra >= input.size()) {
      base.lookahead = byte;
      width_ = 1;
      return;
    }
    for (std::uint32_t i = 1; i <= extra; ++i) {
      value = (value << 6) |
              (static_cast<unsigned char>(input[position + i]) & 0x3F);
    }
    base.lookahead = static_cast<std::int32_t>(value);
    width_ = extra + 1;
  }

  std::uint32_t width() const { return width_; }

 private:
  std::uint32_t width_ = 0;
};

// `skip` means the character is whitespace before the token rather than part
// of it, so the token's start slides forward past it.
void LexerAdvance(TSLexerAbi* base, bool skip) {
  ScannerLexer* self = ScannerLexer::From(base);
  if (self->position < self->input.size()) {
    self->position += self->width() > 0 ? self->width() : 1;
  }
  self->RefreshLookahead();
  if (skip) self->token_start = self->position;
}

// Records where the token ends. A scanner may call this repeatedly as it finds
// longer matches, and may then read past the mark without extending the token.
void LexerMarkEnd(TSLexerAbi* base) {
  ScannerLexer* self = ScannerLexer::From(base);
  self->token_end = self->position;
  self->end_marked = true;
}

// Column in code points from the start of the line. Indentation-sensitive
// scanners use this to decide indent/dedent.
std::uint32_t LexerGetColumn(TSLexerAbi* base) {
  ScannerLexer* self = ScannerLexer::From(base);
  std::uint32_t line_start = self->position;
  while (line_start > 0 && self->input[line_start - 1] != '\n') --line_start;
  std::uint32_t column = 0;
  for (std::uint32_t i = line_start; i < self->position; ++i) {
    // Count code points, not bytes: continuation bytes do not advance a column.
    if ((static_cast<unsigned char>(self->input[i]) & 0xC0) != 0x80) ++column;
  }
  return column;
}

// This project parses one contiguous range, so a position is never at the
// start of a second included range.
bool LexerIsAtIncludedRangeStart(const TSLexerAbi*) { return false; }

bool LexerEof(const TSLexerAbi* base) {
  const ScannerLexer* self = ScannerLexer::From(base);
  return self->position >= self->input.size();
}

void LexerLog(const TSLexerAbi*, const char*, ...) {}

// Looks for the header a scanner includes as "tree_sitter/parser.h". Published
// grammars vendor it under src/, which is also where scanner.c lives.
std::vector<std::filesystem::path> HeaderSearchPaths(
    const std::filesystem::path& scanner_directory) {
  std::vector<std::filesystem::path> paths;
  paths.push_back(scanner_directory);
  paths.push_back(scanner_directory.parent_path());
  paths.push_back(scanner_directory.parent_path() / "src");
  // The project's vendored copies, so that a grammar which does not ship
  // tree_sitter/*.h of its own still compiles. Last, so a grammar's own
  // headers always win.
  paths.push_back(std::filesystem::path(TS_REF_VENDOR_INCLUDE_DIR));
  if (const char* override_dir = std::getenv("TS_REF_TS_INCLUDE_DIR")) {
    paths.insert(paths.begin(), std::filesystem::path(override_dir));
  }
  return paths;
}

std::string ShellQuote(std::string_view value) {
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
  return quoted;
}

// The scanner's entry points are named after the grammar.
std::string SymbolName(const std::string& grammar_name,
                       std::string_view suffix) {
  return absl::StrCat("tree_sitter_", grammar_name, "_external_scanner_",
                      suffix);
}

}  // namespace

ExternalScanner::~ExternalScanner() {
  if (payload_ != nullptr && destroy_ != nullptr) destroy_(payload_);
  if (handle_ != nullptr) ::dlclose(handle_);
  if (!library_path_.empty()) {
    std::error_code error;
    std::filesystem::remove(library_path_, error);
  }
}

absl::StatusOr<std::unique_ptr<ExternalScanner>> ExternalScanner::Load(
    const std::filesystem::path& grammar_path,
    const std::string& grammar_name) {
  // A scanner lives beside the grammar, usually in src/.
  const std::filesystem::path directory = grammar_path.parent_path();
  std::filesystem::path source;
  for (const std::filesystem::path& candidate :
       {directory / "src" / "scanner.c", directory / "src" / "scanner.cc",
        directory / "scanner.c", directory / "scanner.cc"}) {
    std::error_code error;
    if (std::filesystem::exists(candidate, error)) {
      source = candidate;
      break;
    }
  }
  // Most grammars have no scanner; that is not a failure.
  if (source.empty()) return std::unique_ptr<ExternalScanner>();

  const bool is_cpp = source.extension() == ".cc";
  const char* compiler_env = std::getenv(is_cpp ? "CXX" : "CC");
  const std::string compiler = compiler_env != nullptr && *compiler_env != '\0'
                                   ? compiler_env
                                   : (is_cpp ? "c++" : "cc");

  std::error_code error;
  const std::filesystem::path library =
      std::filesystem::temp_directory_path(error) /
      absl::StrCat("ts_ref_scanner_", grammar_name, "_", ::getpid(), ".so");

  std::string command = absl::StrCat(compiler, " -shared -fPIC -O2 -o ",
                                     ShellQuote(library.string()), " ",
                                     ShellQuote(source.string()));
  for (const std::filesystem::path& include :
       HeaderSearchPaths(source.parent_path())) {
    absl::StrAppend(&command, " -I", ShellQuote(include.string()));
  }

  const int status = std::system(command.c_str());
  if (status != 0) {
    return absl::InvalidArgumentError(
        absl::StrCat("failed to compile the external scanner ", source.string(),
                     " (compiler exited with status ", status,
                     "); if it could not find tree_sitter/parser.h, set "
                     "TS_REF_TS_INCLUDE_DIR to the directory containing it"));
  }

  void* handle = ::dlopen(library.string().c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    return absl::InternalError(
        absl::StrCat("failed to load the compiled scanner: ", ::dlerror()));
  }

  auto scanner = std::unique_ptr<ExternalScanner>(new ExternalScanner());
  scanner->handle_ = handle;
  scanner->library_path_ = library;

  const auto resolve = [&](std::string_view suffix) -> void* {
    return ::dlsym(handle, SymbolName(grammar_name, suffix).c_str());
  };
  scanner->create_ = reinterpret_cast<void* (*)()>(resolve("create"));
  scanner->destroy_ = reinterpret_cast<void (*)(void*)>(resolve("destroy"));
  scanner->scan_ = reinterpret_cast<bool (*)(void*, TSLexerAbi*, const bool*)>(
      resolve("scan"));
  scanner->serialize_ =
      reinterpret_cast<unsigned (*)(void*, char*)>(resolve("serialize"));
  scanner->deserialize_ =
      reinterpret_cast<void (*)(void*, const char*, unsigned)>(
          resolve("deserialize"));

  // `scan` is the only one a scanner cannot do without.
  if (scanner->scan_ == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "the scanner does not export ", SymbolName(grammar_name, "scan"),
        "; the grammar name must match the scanner's function prefix"));
  }
  if (scanner->create_ != nullptr) scanner->payload_ = scanner->create_();
  return scanner;
}

ExternalScanner::ScanResult ExternalScanner::Scan(
    std::string_view input, std::uint32_t position,
    const ScannerState& previous_state,
    const std::vector<bool>& valid_symbols) {
  ScanResult result;

  // Restore this stack version's scanner state before scanning for it. Without
  // this, a fork would leak one branch's state into another.
  if (deserialize_ != nullptr) {
    deserialize_(payload_, previous_state.data(),
                 static_cast<unsigned>(previous_state.size()));
  }

  ScannerLexer lexer;
  lexer.base.advance = &LexerAdvance;
  lexer.base.mark_end = &LexerMarkEnd;
  lexer.base.get_column = &LexerGetColumn;
  lexer.base.is_at_included_range_start = &LexerIsAtIncludedRangeStart;
  lexer.base.eof = &LexerEof;
  lexer.base.log = &LexerLog;
  lexer.base.result_symbol = 0;
  lexer.input = input;
  lexer.position = position;
  lexer.token_start = position;
  lexer.token_end = position;
  lexer.end_marked = false;
  lexer.RefreshLookahead();

  // std::vector<bool> is a bitfield, so it cannot be handed over directly.
  std::vector<char> valid(valid_symbols.size());
  for (std::size_t i = 0; i < valid_symbols.size(); ++i) {
    valid[i] = valid_symbols[i] ? 1 : 0;
  }

  const bool found =
      scan_(payload_, &lexer.base, reinterpret_cast<const bool*>(valid.data()));
  if (!found) return result;

  result.found = true;
  result.external_index = lexer.base.result_symbol;
  result.token_start = lexer.token_start;
  // A scanner that never marked an end means the token runs to wherever it
  // stopped reading.
  result.token_end = lexer.end_marked ? lexer.token_end : lexer.position;

  // A scanner may mark the end *before* skipping ahead -- JavaScript's
  // automatic-semicolon scanner marks a zero-width token at the current
  // position and only then skips the newline that justified it. That leaves
  // the end behind the start, and the token must collapse back onto the end,
  // not be stretched forward to the start: the semicolon belongs where the
  // statement ended, and the skipped whitespace must stay unconsumed for the
  // next scan. Mirrors lexer.c's `if (token_end < token_start) token_start =
  // token_end`.
  if (result.token_end < result.token_start) {
    result.token_start = result.token_end;
  }

  if (serialize_ != nullptr) {
    std::array<char, kSerializationBufferSize> buffer{};
    const unsigned length = serialize_(payload_, buffer.data());
    result.state.assign(buffer.data(), length);
  }
  return result;
}

}  // namespace ts_ref
