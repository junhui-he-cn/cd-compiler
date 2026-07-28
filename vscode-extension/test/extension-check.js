const assert = require("assert");
const fs = require("fs");
const path = require("path");

const extensionRoot = path.resolve(__dirname, "..");
const manifest = JSON.parse(
  fs.readFileSync(path.join(extensionRoot, "package.json"), "utf8"),
);
const languageConfiguration = JSON.parse(
  fs.readFileSync(path.join(extensionRoot, "language-configuration.json"), "utf8"),
);
const grammar = JSON.parse(
  fs.readFileSync(
    path.join(extensionRoot, "syntaxes", "compiler-design.tmLanguage.json"),
    "utf8",
  ),
);
const extensionSource = fs.readFileSync(
  path.join(extensionRoot, "src", "extension.js"),
  "utf8",
);

assert.strictEqual(manifest.main, "./src/extension.js");
assert.deepStrictEqual(manifest.activationEvents, ["onLanguage:compiler-design"]);
assert.strictEqual(manifest.contributes.languages[0].id, "compiler-design");
assert.deepStrictEqual(manifest.contributes.languages[0].extensions, [".cd"]);
assert.deepStrictEqual(manifest.contributes.grammars, [
  {
    language: "compiler-design",
    path: "./syntaxes/compiler-design.tmLanguage.json",
    scopeName: "source.compiler-design",
  },
]);
assert.strictEqual(grammar.scopeName, "source.compiler-design");
assert.ok(grammar.repository.comments);
assert.ok(grammar.repository.keywords);
assert.ok(grammar.repository.strings);
assert.ok(grammar.repository.types);
assert.ok(grammar.repository.functions);
assert.strictEqual(languageConfiguration.comments.lineComment, "//");
assert.ok(manifest.dependencies["vscode-languageclient"]);
assert.ok(extensionSource.includes('get("serverArgs", ["--lsp"])'));
assert.ok(extensionSource.includes('language: "compiler-design"'));
assert.ok(extensionSource.includes("documentSelector"));

console.log("VS Code extension manifest and client configuration: passed");
