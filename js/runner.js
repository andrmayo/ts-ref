// thin harness for dsl.js

import * as path from "node:path";
import { pathToFileURL } from "node:url";

import "./dsl.js";

const grammarPath = process.argv[2];
if (!grammarPath) {
  process.stderr.write("usage: node runner.js <path-to-grammar.js>\n");
  process.exit(1);
}

// A bare relative specifier would resolve against this module's directory
// rather than the caller's cwd, so resolve to an absolute file URL first.
const grammarUrl = pathToFileURL(path.resolve(process.cwd(), grammarPath));

// this import has to be dynamic, so that globalThis.* has already been set by static import
const result = await import(grammarUrl);
const grammarObj = result.default?.grammar ?? result.grammar;
if (!grammarObj) {
  process.stderr.write(
    `${grammarPath} did not export a grammar; expected 'module.exports = grammar({...})'\n`,
  );
  process.exit(1);
}
process.stdout.write(JSON.stringify(grammarObj));
