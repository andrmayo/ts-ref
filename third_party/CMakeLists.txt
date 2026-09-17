include(FetchContent)

# RE2 exports an install set that names its abseil dependencies, which fails
# unless abseil also generates install rules. Harmless for a build that never
# installs, and required for the two to coexist under FetchContent.
set(ABSL_ENABLE_INSTALL ON CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  absl
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG 20260526.0
)
FetchContent_MakeAvailable(absl)

# RE2 supplies the regex *parser* (re2::Regexp::Parse), not its matching
# engine: the lexer needs a regex AST it can compile into the combined NFA, and
# it needs a real Unicode character database so that \p{...} classes are exact
# rather than approximated. This fills the same slot the regex_syntax crate
# fills for tree-sitter. RE2 depends on abseil, which is already fetched above.
set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  re2
  GIT_REPOSITORY https://github.com/google/re2.git
  GIT_TAG 2024-07-02
)
FetchContent_MakeAvailable(re2)

# re2/regexp.h (the AST) is part of the source tree but is not among the
# headers RE2 installs, so reach it through the fetched source directory.
add_library(re2_parser INTERFACE)
target_link_libraries(re2_parser INTERFACE re2::re2)
target_include_directories(re2_parser INTERFACE ${re2_SOURCE_DIR})

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
