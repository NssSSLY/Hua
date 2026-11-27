"use strict";
const vscode = require("vscode");
const { execFile } = require("node:child_process");
const path = require("node:path");
const fs = require("node:fs");
const { parseDiagnostics, utf16Column } = require("./diagnostics.cjs");
const { HuaLspClient } = require("./lsp_client.cjs");

const SEMANTIC_TOKEN_TYPES = [
    "namespace", "type", "class", "enum", "interface", "struct",
    "typeParameter", "parameter", "variable", "property", "enumMember",
    "function", "method", "keyword", "comment", "string", "number", "operator"
];

const SEMANTIC_TOKEN_MODIFIERS = [
    "declaration", "definition", "readonly", "static", "deprecated", "defaultLibrary", "modification"
];

function lspRangeToVsRange(r) {
    return new vscode.Range(
        new vscode.Position(r.start.line, r.start.character),
        new vscode.Position(r.end.line, r.end.character)
    );
}

function lspSeverityToVsSeverity(sev) {
    switch (sev) {
    case 1: return vscode.DiagnosticSeverity.Error;
    case 2: return vscode.DiagnosticSeverity.Warning;
    case 3: return vscode.DiagnosticSeverity.Information;
    case 4: return vscode.DiagnosticSeverity.Hint;
    default: return vscode.DiagnosticSeverity.Error;
    }
}

function lspSymbolToVsSymbol(sym) {
    const item = new vscode.DocumentSymbol(
        sym.name,
        sym.detail || "",
        sym.kind,
        lspRangeToVsRange(sym.range),
        lspRangeToVsRange(sym.selectionRange || sym.range)
    );
    if (Array.isArray(sym.children) && sym.children.length) {
        item.children = sym.children.map(lspSymbolToVsSymbol);
    }
    return item;
}

function activate(context) {
    const collection = vscode.languages.createDiagnosticCollection("hua");
    const output = vscode.window.createOutputChannel("Hua");
    const jobs = new Map();
    const owners = new Map();
    const changeTimers = new Map();
    const openedUris = new Set();

    let lspClient = null;
    let lspFailed = false;

    function getWorkspaceContext(doc) {
        const folder = doc ? vscode.workspace.getWorkspaceFolder(doc.uri) : (vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0]);
        const cwd = folder ? folder.uri.fsPath : (doc && doc.uri.scheme === "file" ? path.dirname(doc.uri.fsPath) : process.cwd());
        const config = vscode.workspace.getConfiguration("hua", doc ? doc.uri : undefined);
        const rawExec = config.get("compilerPath", "hua").replace(/\$\{workspaceFolder\}/g, cwd);
        const executable = rawExec.includes(path.sep) && !path.isAbsolute(rawExec) ? path.resolve(cwd, rawExec) : rawExec;
        return { folder, cwd, config, executable };
    }

    const lspSupported = Boolean(
        typeof vscode.SemanticTokensLegend === "function" &&
        vscode.languages &&
        typeof vscode.languages.registerDocumentSemanticTokensProvider === "function"
    );

    function lspEnabled(doc) {
        if (!lspSupported || !vscode.workspace.isTrusted) return false;
        const { config } = getWorkspaceContext(doc);
        return config.get("enableLsp", true);
    }

    async function ensureLsp(doc) {
        if (!lspEnabled(doc) || lspFailed) return null;
        if (lspClient && lspClient.isRunning()) return lspClient;
        const { folder, cwd, config, executable } = getWorkspaceContext(doc);
        const trace = config.get("lspTrace", false);
        const client = new HuaLspClient(executable, cwd, output, trace);
        client.onNotification("textDocument/publishDiagnostics", params => {
            if (!params || !params.uri) return;
            const uri = vscode.Uri.parse(params.uri);
            const items = (params.diagnostics || []).map(d => {
                const diag = new vscode.Diagnostic(lspRangeToVsRange(d.range), d.message || "", lspSeverityToVsSeverity(d.severity));
                if (d.code !== undefined) diag.code = d.code;
                diag.source = d.source || "Hua";
                if (Array.isArray(d.relatedInformation) && d.relatedInformation.length) {
                    diag.relatedInformation = d.relatedInformation.map(ri => new vscode.DiagnosticRelatedInformation(
                        new vscode.Location(vscode.Uri.parse(ri.location.uri), lspRangeToVsRange(ri.location.range)),
                        ri.message || ""
                    ));
                }
                return diag;
            });
            collection.set(uri, items);
        });
        try {
            await client.start(folder ? folder.uri.toString() : null);
            lspClient = client;
            openedUris.clear();
            for (const openDoc of vscode.workspace.textDocuments) {
                if (openDoc.languageId === "hua") sendDidOpen(openDoc);
            }
            return lspClient;
        } catch (err) {
            lspFailed = true;
            output.appendLine("[hua lsp] failed to start, falling back to check-on-save: " + err.message);
            return null;
        }
    }

    function sendDidOpen(doc) {
        if (!lspClient || !lspClient.isRunning() || doc.languageId !== "hua") return;
        const uri = doc.uri.toString();
        if (openedUris.has(uri)) return;
        openedUris.add(uri);
        lspClient.notify("textDocument/didOpen", {
            textDocument: {
                uri,
                languageId: "hua",
                version: doc.version,
                text: doc.getText()
            }
        });
    }

    function sendDidChange(doc) {
        if (!lspClient || !lspClient.isRunning() || doc.languageId !== "hua") return;
        const uri = doc.uri.toString();
        if (!openedUris.has(uri)) {
            sendDidOpen(doc);
            return;
        }
        lspClient.notify("textDocument/didChange", {
            textDocument: { uri, version: doc.version },
            contentChanges: [{ text: doc.getText() }]
        });
    }

    function sendDidSave(doc) {
        if (!lspClient || !lspClient.isRunning() || doc.languageId !== "hua") return;
        const uri = doc.uri.toString();
        if (!openedUris.has(uri)) {
            sendDidOpen(doc);
            return;
        }
        lspClient.notify("textDocument/didSave", {
            textDocument: { uri },
            text: doc.getText()
        });
    }

    function sendDidClose(doc) {
        if (!lspClient || !lspClient.isRunning() || doc.languageId !== "hua") return;
        const uri = doc.uri.toString();
        if (!openedUris.has(uri)) return;
        openedUris.delete(uri);
        lspClient.notify("textDocument/didClose", {
            textDocument: { uri }
        });
    }

    async function restartLsp() {
        lspFailed = false;
        if (lspClient) {
            await lspClient.stop();
            lspClient = null;
        }
        openedUris.clear();
        collection.clear();
        const active = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
        const client = await ensureLsp(active);
        if (client) vscode.window.showInformationMessage("Hua Language Server restarted.");
    }

    // Legacy CLI fallback check (used when LSP is disabled or unavailable, or explicit hua.check)
    function refreshLegacy() {
        collection.clear();
        const merged = new Map();
        for (const items of owners.values()) for (const [file, diagnostics] of items) {
            const values = merged.get(file) || [];
            merged.set(file, values.concat(diagnostics));
        }
        for (const [file, diagnostics] of merged) collection.set(vscode.Uri.file(file), diagnostics);
    }

    function invalidateLegacy(doc) {
        const key = doc.uri.toString();
        const job = jobs.get(key);
        jobs.delete(key);
        if (job && job.child) job.child.kill();
        owners.delete(key);
        if (!lspClient || !lspClient.isRunning()) refreshLegacy();
    }

    async function checkLegacy(doc, manual) {
        if (!vscode.workspace.isTrusted || doc.languageId !== "hua" || doc.uri.scheme !== "file" || doc.isDirty) return;
        invalidateLegacy(doc);
        const key = doc.uri.toString();
        const job = {};
        jobs.set(key, job);
        const { cwd, executable } = getWorkspaceContext(doc);
        job.child = execFile(executable, ["check", doc.uri.fsPath], { cwd, timeout: 30000, maxBuffer: 4 * 1024 * 1024, windowsHide: true }, (error, stdout, stderr) => {
            if (jobs.get(key) !== job) return;
            jobs.delete(key);
            const diagnostics = new Map();
            for (const item of parseDiagnostics(stderr)) {
                const file = path.resolve(cwd, item.file);
                let lines = [];
                try { lines = fs.readFileSync(file, "utf8").replace(/^\uFEFF/, "").replace(/\r\n?/g, "\n").split("\n"); } catch {}
                const column = utf16Column(lines[item.line] || "", item.byteColumn);
                const diagnostic = new vscode.Diagnostic(new vscode.Range(item.line, column, item.line, column + 1), item.message, vscode.DiagnosticSeverity.Error);
                diagnostic.code = item.code;
                diagnostic.source = "Hua";
                const values = diagnostics.get(file) || [];
                values.push(diagnostic);
                diagnostics.set(file, values);
            }
            owners.set(key, diagnostics);
            refreshLegacy();
            if (error && !diagnostics.size) {
                output.appendLine(stderr || error.message);
                if (manual) {
                    output.show();
                    vscode.window.showErrorMessage("Hua compiler check failed. See the Hua output panel and hua.compilerPath.");
                }
            } else if (manual && !error) {
                vscode.window.showInformationMessage("Hua: check passed.");
            }
        });
    }

    async function current(command) {
        if (!vscode.workspace.isTrusted) {
            vscode.window.showWarningMessage("Trust this workspace before running the Hua compiler.");
            return;
        }
        const editor = vscode.window.activeTextEditor;
        if (!editor || editor.document.languageId !== "hua" || editor.document.uri.scheme !== "file") {
            vscode.window.showWarningMessage("Open a local .hua file first.");
            return;
        }
        const doc = editor.document;
        if (doc.isDirty && !await doc.save()) return;
        if (command === "check") {
            checkLegacy(doc, true);
            return;
        }
        const { cwd, config, executable } = getWorkspaceContext(doc);
        const args = [command, doc.uri.fsPath];
        if (command === "run") {
            const extra = config.get("runArguments", []);
            if (extra.length) args.push("--", ...extra);
        }
        const folder = vscode.workspace.getWorkspaceFolder(doc.uri);
        const task = new vscode.Task(
            { type: "hua", command },
            folder || vscode.TaskScope.Global,
            command + " " + path.basename(doc.uri.fsPath),
            "Hua",
            new vscode.ProcessExecution(executable, args, { cwd }),
            []
        );
        await vscode.tasks.executeTask(task);
    }

    const disposables = [collection, output];

    if (lspSupported) {
        const legend = new vscode.SemanticTokensLegend(SEMANTIC_TOKEN_TYPES, SEMANTIC_TOKEN_MODIFIERS);
        disposables.push(vscode.languages.registerDocumentSemanticTokensProvider(
            { language: "hua" },
            {
                async provideDocumentSemanticTokens(doc) {
                    const client = await ensureLsp(doc);
                    if (!client) return null;
                    sendDidOpen(doc);
                    try {
                        const result = await client.request("textDocument/semanticTokens/full", {
                            textDocument: { uri: doc.uri.toString() }
                        });
                        if (result && Array.isArray(result.data)) {
                            return new vscode.SemanticTokens(new Uint32Array(result.data));
                        }
                    } catch {}
                    return null;
                }
            },
            legend
        ));
    }

    if (vscode.languages && typeof vscode.languages.registerHoverProvider === "function") {
        disposables.push(vscode.languages.registerHoverProvider(
            { language: "hua" },
            {
                async provideHover(doc, position) {
                    const client = await ensureLsp(doc);
                    if (!client) return null;
                    sendDidOpen(doc);
                    try {
                        const result = await client.request("textDocument/hover", {
                            textDocument: { uri: doc.uri.toString() },
                            position: { line: position.line, character: position.character }
                        });
                        if (result && result.contents && result.contents.value) {
                            const md = new vscode.MarkdownString(result.contents.value);
                            return new vscode.Hover(md, result.range ? lspRangeToVsRange(result.range) : undefined);
                        }
                    } catch {}
                    return null;
                }
            }
        ));
    }

    if (vscode.languages && typeof vscode.languages.registerDocumentSymbolProvider === "function") {
        disposables.push(vscode.languages.registerDocumentSymbolProvider(
            { language: "hua" },
            {
                async provideDocumentSymbols(doc) {
                    const client = await ensureLsp(doc);
                    if (!client) return [];
                    sendDidOpen(doc);
                    try {
                        const result = await client.request("textDocument/documentSymbol", {
                            textDocument: { uri: doc.uri.toString() }
                        });
                        if (Array.isArray(result)) return result.map(lspSymbolToVsSymbol);
                    } catch {}
                    return [];
                }
            }
        ));
    }

    if (vscode.languages && typeof vscode.languages.registerDefinitionProvider === "function") {
        disposables.push(vscode.languages.registerDefinitionProvider(
            { language: "hua" },
            {
                async provideDefinition(doc, position) {
                    const client = await ensureLsp(doc);
                    if (!client) return null;
                    sendDidChange(doc);
                    try {
                        const result = await client.request("textDocument/definition", {
                            textDocument: { uri: doc.uri.toString() },
                            position: { line: position.line, character: position.character }
                        });
                        if (!result) return null;
                        const toLoc = item => new vscode.Location(vscode.Uri.parse(item.uri), lspRangeToVsRange(item.range));
                        if (Array.isArray(result)) return result.map(toLoc);
                        if (result.uri && result.range) return toLoc(result);
                    } catch {}
                    return null;
                }
            }
        ));
    }

    if (vscode.languages && typeof vscode.languages.registerCompletionItemProvider === "function") {
        disposables.push(vscode.languages.registerCompletionItemProvider(
            { language: "hua" },
            {
                async provideCompletionItems(doc, position) {
                    const client = await ensureLsp(doc);
                    if (!client) return [];
                    // Flush latest unsaved text immediately so `.` completion sees current line
                    sendDidChange(doc);
                    try {
                        const result = await client.request("textDocument/completion", {
                            textDocument: { uri: doc.uri.toString() },
                            position: { line: position.line, character: position.character }
                        });
                        const list = Array.isArray(result) ? result : (result && Array.isArray(result.items) ? result.items : []);
                        return list.map(entry => {
                            const kind = typeof entry.kind === "number" ? Math.max(0, entry.kind - 1) : vscode.CompletionItemKind.Text;
                            const item = new vscode.CompletionItem(entry.label, kind);
                            if (entry.detail) item.detail = entry.detail;
                            return item;
                        });
                    } catch {}
                    return [];
                }
            },
            "."
        ));
    }

    if (typeof vscode.workspace.onDidOpenTextDocument === "function") {
        disposables.push(vscode.workspace.onDidOpenTextDocument(async doc => {
            if (doc.languageId !== "hua") return;
            const client = await ensureLsp(doc);
            if (client) sendDidOpen(doc);
        }));
    }

    disposables.push(
        vscode.workspace.onDidChangeTextDocument(event => {
            const doc = event.document;
            if (doc.languageId !== "hua") return;
            if (lspEnabled(doc) && !lspFailed) {
                const key = doc.uri.toString();
                if (jobs.has(key)) invalidateLegacy(doc);
                if (changeTimers.has(key)) clearTimeout(changeTimers.get(key));
                changeTimers.set(key, setTimeout(async () => {
                    changeTimers.delete(key);
                    const client = await ensureLsp(doc);
                    if (client) sendDidChange(doc);
                }, 150));
            } else {
                invalidateLegacy(doc);
            }
        }),
        vscode.workspace.onDidSaveTextDocument(async doc => {
            if (doc.languageId !== "hua") return;
            if (lspEnabled(doc) && !lspFailed) {
                const client = await ensureLsp(doc);
                if (client) {
                    sendDidSave(doc);
                    return;
                }
            }
            if (vscode.workspace.getConfiguration("hua", doc.uri).get("checkOnSave", true)) {
                checkLegacy(doc, false);
            }
        }),
        vscode.workspace.onDidCloseTextDocument(doc => {
            if (doc.languageId !== "hua") return;
            const key = doc.uri.toString();
            if (changeTimers.has(key)) {
                clearTimeout(changeTimers.get(key));
                changeTimers.delete(key);
            }
            sendDidClose(doc);
            invalidateLegacy(doc);
        })
    );

    if (typeof vscode.workspace.onDidChangeConfiguration === "function") {
        disposables.push(vscode.workspace.onDidChangeConfiguration(event => {
            if (event.affectsConfiguration("hua.compilerPath") ||
                event.affectsConfiguration("hua.enableLsp") ||
                event.affectsConfiguration("hua.lspTrace")) {
                restartLsp();
            }
        }));
    }

    disposables.push(
        vscode.commands.registerCommand("hua.restartLsp", restartLsp),
        ...["check", "run", "build"].map(command => vscode.commands.registerCommand("hua." + command, () => current(command))),
        {
            dispose() {
                for (const t of changeTimers.values()) clearTimeout(t);
                changeTimers.clear();
                for (const job of jobs.values()) if (job.child) job.child.kill();
                jobs.clear();
                if (lspClient) lspClient.stop();
            }
        }
    );

    context.subscriptions.push(...disposables);

    // Eagerly start LSP if a Hua document is already open
    if (lspSupported && Array.isArray(vscode.workspace.textDocuments)) {
        const initialHua = vscode.workspace.textDocuments.find(d => d.languageId === "hua");
        if (initialHua) ensureLsp(initialHua);
    }
}

module.exports = { activate };
