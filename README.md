# C++ port of tree-sitter

## Purpose

This has mostly just been an exercise for me both to get a better grasp of
tree-sitter and C++. As a streamlined version of tree-sitter that mostly works
in the same way (there are various minor algorithmic divergences), it could be
handy for prototyping architectural and algorithmic variants, and isolating bugs
and performance issues in tree-sitter itself by providing a differential
implementation.

## Limitations

This version of tree-sitter should work with any valid tree-sitter grammar, as
well as external scanners. It is, however, missing incremental parsing.

## Commands

| Command                                      | Description                 |
| -------------------------------------------- | --------------------------- |
| `make`                                       | Build the project           |
| `./ts-ref --parse <input-file> <grammar.js>` | Parse a file                |
| `./ts-ref --conflicts <grammar.js>`          | Fork points                 |
| `./ts-ref --lex <file>`                      | Token stream (no parsing)   |
| `./ts-ref --lex-table <grammar.js>`          | The generated DFA           |
| `./ts-ref --normalization <grammar.js>`      | Grammar after normalization |

Note that these will pick up a scanner.c sitting next to the grammar
automatically.

`make` will create a symlink to `build/ts-ref` in the project root.

The statistics line after running `--parse` tells you about forks, merges, and
ambiguities resolved, as well as peak stack versions, which tells you how hard
the grammar made the parser work.

The `--conflicts` command is useful for showing where the grammar is ambiguous.

## Output modifiers

| Command        | Description                             |
| -------------- | --------------------------------------- |
| `--anonymous`  | Include anonymous tokens as quoted text |
| `--all-parses` | Print all surviving readings            |

## TODO

The big one is adding unit tests, which should be fairly straightforward
(fixtures have been pulled over from `tree-sitter`).
