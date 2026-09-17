# Vendored tree-sitter scanner headers

`parser.h`, `alloc.h` and `array.h`, copied verbatim from the tree-sitter
repository (`crates/generate/src/parser.h.inc` and
`crates/generate/src/templates/`). MIT licensed, Copyright (c) 2018 Max
Brunsfeld.

A grammar's hand-written `scanner.c` includes these, and `tree-sitter generate`
normally vendors them into the grammar's own `src/tree_sitter/`. Grammars that
do not ship them -- tree-sitter's own test fixtures, for one -- would otherwise
fail to compile here, so a copy lives in the project and is added to the
compiler's include path by `ExternalScanner::Load`.

`parser.h` also fixes the `TSLexer` ABI that `external_scanner.h` mirrors: if
these are ever updated, check that struct still matches.
