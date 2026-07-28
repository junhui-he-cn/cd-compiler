# Compiler Design VS Code extension

This extension connects VS Code to the repository's stdio language server.
It registers `.cd` files as `compiler-design` documents and enables the LSP
features advertised by `compiler_design --lsp`: diagnostics, formatting,
definition and reference navigation, hover, rename, completion, document
symbols, and workspace symbols. It also provides TextMate syntax highlighting
for comments, keywords, declarations, types, functions, literals, operators,
and punctuation.

## Local installation

Build the compiler from the repository root first:

```sh
cmake -S . -B build
cmake --build build
```

Then package and install the extension:

```sh
cd vscode-extension
npm install
npm run check
npm run package
code --install-extension compiler-design-language-support-0.1.2.vsix
```

After installing or updating the extension, run `Developer: Reload Window` so
VS Code reloads both the grammar and the language-server client.

When the repository is the first VS Code workspace folder, the extension
automatically uses `build/compiler_design`. For another checkout or a
packaged extension, set the executable explicitly in VS Code settings:

```json
{
  "compilerDesign.serverPath": "/home/junhe/compiler/build/compiler_design"
}
```

The server uses stdio JSON-RPC. The extension owns the process lifecycle and
the VS Code client handles standard LSP `Content-Length` framing and full
document synchronization.
