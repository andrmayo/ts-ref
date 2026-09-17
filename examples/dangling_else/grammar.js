// Toy grammar #4 from TASKS.md Phase 8: the classic dangling-else ambiguity,
// written *without* the prec.right that would normally resolve it.
//
// In `if a then if b then x else y`, the `else` can attach to either `if`.
// An LR generator reports this as a shift/reduce conflict and refuses to
// proceed; this project keeps both actions and forks at runtime, then picks a
// winner by dynamic precedence. The `prec.dynamic` below says the parse that
// binds `else` to the *nearer* `if` wins, which is what every real language
// specifies.

module.exports = grammar({
  name: "dangling_else",

  extras: ($) => [/\s/],

  rules: {
    program: ($) => repeat($.statement),

    statement: ($) => choice($.if_statement, $.expression_statement),

    if_statement: ($) =>
      choice(
        prec.dynamic(1, seq("if", $.expression, "then", $.statement, "else", $.statement)),
        seq("if", $.expression, "then", $.statement),
      ),

    expression_statement: ($) => seq($.expression, ";"),

    expression: ($) => $.identifier,

    identifier: ($) => /[a-z_]\w*/,
  },
});
