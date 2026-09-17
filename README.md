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
