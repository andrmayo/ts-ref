// Toy grammar #1 from TASKS.md Phase 8: arithmetic with precedence and
// associativity. Exercises static prec, with little or no actual forking.

module.exports = grammar({
  name: "arithmetic",

  extras: ($) => [/\s/],

  rules: {
    source_file: ($) => $.expression,

    expression: ($) =>
      choice($.number, $.identifier, $.binary_expression, $.parenthesized),

    binary_expression: ($) =>
      choice(
        prec.left(1, seq($.expression, field("op", "+"), $.expression)),
        prec.left(1, seq($.expression, field("op", "-"), $.expression)),
        prec.left(2, seq($.expression, field("op", "*"), $.expression)),
        prec.left(2, seq($.expression, field("op", "/"), $.expression)),
        prec.right(3, seq($.expression, field("op", "^"), $.expression)),
      ),

    parenthesized: ($) => seq("(", $.expression, ")"),

    number: ($) => /\d+(\.\d+)?/,

    identifier: ($) => /[a-zA-Z_]\w*/,
  },
});
