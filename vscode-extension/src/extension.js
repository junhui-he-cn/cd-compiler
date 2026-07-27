const fs = require("fs");
const path = require("path");
const vscode = require("vscode");
const { LanguageClient } = require("vscode-languageclient/node");

let client;

function executableNames() {
  return process.platform === "win32"
    ? ["compiler_design.exe", "compiler_design"]
    : ["compiler_design"];
}

function workspaceRoot() {
  return vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
}

function configuredServerPath() {
  const configured = vscode.workspace
    .getConfiguration("compilerDesign")
    .get("serverPath");
  return typeof configured === "string" && configured.trim() ? configured.trim() : "";
}

function defaultServerCandidates(context) {
  const candidates = [];
  const root = workspaceRoot();
  for (const name of executableNames()) {
    if (root) {
      candidates.push(path.join(root, "build", name));
    }
    candidates.push(path.resolve(context.extensionPath, "..", "build", name));
  }
  return candidates;
}

function resolveServerCommand(context) {
  const configured = configuredServerPath();
  if (configured) {
    return configured;
  }
  return defaultServerCandidates(context).find((candidate) => fs.existsSync(candidate));
}

function serverArguments() {
  const configured = vscode.workspace
    .getConfiguration("compilerDesign")
    .get("serverArgs", ["--lsp"]);
  const args = Array.isArray(configured)
    ? configured.filter((argument) => typeof argument === "string")
    : [];
  if (!args.includes("--lsp")) {
    args.unshift("--lsp");
  }
  return args;
}

function showMissingServerMessage() {
  const action = "Open Settings";
  vscode.window
    .showErrorMessage(
      "Compiler Design language server was not found. Build the project or set compilerDesign.serverPath.",
      action,
    )
    .then((selected) => {
      if (selected === action) {
        vscode.commands.executeCommand(
          "workbench.action.openSettings",
          "compilerDesign.serverPath",
        );
      }
    });
}

function activate(context) {
  const command = resolveServerCommand(context);
  if (!command) {
    showMissingServerMessage();
    return;
  }

  const options = workspaceRoot() ? { cwd: workspaceRoot() } : undefined;
  const serverOptions = {
    run: { command, args: serverArguments(), options },
    debug: { command, args: serverArguments(), options },
  };
  const clientOptions = {
    documentSelector: [
      { scheme: "file", language: "compiler-design" },
      { scheme: "untitled", language: "compiler-design" },
    ],
    outputChannelName: "Compiler Design Language Server",
  };

  client = new LanguageClient(
    "compilerDesignLanguageServer",
    "Compiler Design Language Server",
    serverOptions,
    clientOptions,
  );
  context.subscriptions.push(client);
  client.start().catch((error) => {
    console.error("Compiler Design language server failed to start", error);
    vscode.window.showErrorMessage(
      `Compiler Design language server failed to start: ${error.message || error}`,
    );
  });
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = {
  activate,
  deactivate,
};
