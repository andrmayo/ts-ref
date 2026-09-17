cmake_minimum_required(VERSION 3.20)
project(ts_ref)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Without this CMake passes no optimization flags at all. The table generator
# leans hard on std::map/std::set, which at -O0 inline nothing and run an order
# of magnitude slower -- enough to make a real grammar look unbuildable.
# Debug info is kept so a debugger still works.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_subdirectory(third_party)

add_library(ts_ref_lib STATIC
  src/dfa.cc src/grammar_loader.cc src/lexer_generator.cc src/parser_engine.cc
  src/parser_table.cc src/stack.cc src/table_generator.cc
  src/node.cc src/grammar_ir.cc src/grammar_normalizer.cc
  src/character_set.cc src/regex.cc src/nfa.cc src/external_scanner.cc
)

target_link_libraries(ts_ref_lib PRIVATE
  absl::log
  absl::check
  nlohmann_json
  re2_parser
)

# absl::status/statusor appear in grammar_loader.h, so consumers need them too
target_link_libraries(ts_ref_lib PUBLIC
  absl::status
  absl::statusor
  # dlopen/dlsym, for loading a grammar's compiled external scanner
  ${CMAKE_DL_LIBS}
)

# Lets grammar_loader find js/runner.js without an install step; overridable at
# runtime via the TS_REF_JS_DIR environment variable.
target_compile_definitions(ts_ref_lib PRIVATE
  TS_REF_JS_DIR="${PROJECT_SOURCE_DIR}/js"
  # Where the vendored tree_sitter/*.h live, for compiling a grammar's
  # scanner.c when the grammar does not ship its own copies.
  TS_REF_VENDOR_INCLUDE_DIR="${PROJECT_SOURCE_DIR}/third_party"
)

add_executable(ts_ref src/main.cc)

# The CMake target keeps an underscore, to pair with ts_ref_lib and to stay a
# plain identifier; the binary itself is hyphenated, which is the usual shape
# for a command name and matches the project's own name.
set_target_properties(ts_ref PROPERTIES OUTPUT_NAME ts-ref)

target_include_directories(ts_ref_lib PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_link_libraries(ts_ref PRIVATE ts_ref_lib)

# TODO: uncomment below and modify once tests are added
# add_executable(ts_ref_tests tests/stack_test.cc ...)
# target_link_libraries(ts_ref_tests PRIVATE ts_ref_lib doctest::doctest)
