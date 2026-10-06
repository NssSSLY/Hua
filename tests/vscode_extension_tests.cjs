"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");
const Module = require("node:module");
const { parseDiagnostics, utf16Column } = require("../editors/vscode/diagnostics.cjs");
test("diagnostics preserve Windows paths and imported source locations", () => {
    const parsed = parseDiagnostics("error[E3004]: unknown name\r\n --> C:\\目录 with spaces\\module.hua:3:9\r\nerror[E1001]: invalid\n --> relative.hua:1:1\n");
    assert.equal(parsed[0].file, "C:\\目录 with spaces\\module.hua");
    assert.deepEqual(parsed[1], { code: "E1001", message: "invalid", file: "relative.hua", line: 0, byteColumn: 0 });
});
test("byte offsets become UTF16 columns including surrogate pairs and tabs", () => {
    assert.equal(utf16Column('"中😀"\tbad', 10), 6);
    assert.equal(utf16Column('中x', 3), 1);
    assert.equal(utf16Column('中x', 1), 0);
});
function harness(trusted = true) {
    const calls = [], commands = new Map(), events = {}, diagnostics = new Map();
    const doc = { languageId: "hua", isDirty: false, uri: { scheme: "file", fsPath: path.resolve("examples/hello.hua"), toString() { return this.fsPath; } }, async save() { this.isDirty = false; return true; } };
    const config = { get(name, fallback) { return { compilerPath: "hua", runArguments: ["a b", "$(no-shell)", '"quoted"'], checkOnSave: true }[name] ?? fallback; } };
    const disposable = { dispose() {} };
    const vscode = {
        workspace: { isTrusted: trusted, getWorkspaceFolder() { return { uri: { fsPath: process.cwd() } }; }, getConfiguration() { return config; }, ...Object.fromEntries(["onDidSaveTextDocument", "onDidChangeTextDocument", "onDidCloseTextDocument"].map(name => [name, cb => { events[name] = cb; return disposable; }])) },
        window: { activeTextEditor: { document: doc }, createOutputChannel() { return { ...disposable, appendLine(s) { calls.push(["output", s]); }, show() {} }; }, showWarningMessage(s) { calls.push(["warning", s]); }, showInformationMessage() {}, showErrorMessage() {} },
        languages: { createDiagnosticCollection() { return { ...disposable, clear() { diagnostics.clear(); }, set(uri, items) { diagnostics.set(uri.fsPath, items); } }; } },
        Uri: { file(file) { return { fsPath: file }; } },
        commands: { registerCommand(name, cb) { commands.set(name, cb); return disposable; } },
        TaskScope: { Global: 1 },
        ProcessExecution: class { constructor(executable, args, options) { this.executable = executable; this.args = args; this.options = options; } },
        Task: class { constructor(definition, scope, name, source, execution) { Object.assign(this, { definition, scope, name, source, execution }); } },
        tasks: { async executeTask(t) { calls.push(["task", t]); } },
        Diagnostic: class { constructor(range, message, severity) { Object.assign(this, { range, message, severity }); } },
        Range: class { constructor(...values) { this.values = values; } }, DiagnosticSeverity: { Error: 0 }
    };
    const original = Module._load;
    const file = require.resolve("../editors/vscode/extension.cjs"); delete require.cache[file];
    Module._load = function(name, parent, main) {
        if (name === "vscode") return vscode;
        if (name === "node:child_process") return { execFile(executable, args, options, callback) { const child = { kill() { calls.push(["kill"]); } }; calls.push(["exec", { executable, args, options, callback }]); return child; } };
        return original.apply(this, arguments);
    };
    const context = { subscriptions: [] };
    try { require(file).activate(context); } finally { Module._load = original; }
    return { doc, calls, commands, events, diagnostics, context };
}
test("untrusted workspaces launch no compiler", async () => {
    const h = harness(false); await h.commands.get("hua.run")(); h.events.onDidSaveTextDocument(h.doc);
    assert.equal(h.calls.filter(c => c[0] === "exec" || c[0] === "task").length, 0);
});
test("run uses process arguments with stdin task terminal and no shell expansion", async () => {
    const h = harness(); h.doc.isDirty = true; await h.commands.get("hua.run")();
    const t = h.calls.find(c => c[0] === "task")[1];
    assert.equal(h.doc.isDirty, false);
    assert.deepEqual(t.execution.args, ["run", h.doc.uri.fsPath, "--", "a b", "$(no-shell)", '"quoted"']);
    assert.equal(t.execution.options.cwd, process.cwd());
    await h.commands.get("hua.build")();
    assert.deepEqual(h.calls.filter(c => c[0] === "task")[1][1].execution.args, ["build", h.doc.uri.fsPath]);
});
test("editing cancels checks and ignores stale callback diagnostics", async () => {
    const h = harness(); await h.commands.get("hua.check")();
    const first = h.calls.find(c => c[0] === "exec")[1];
    h.events.onDidChangeTextDocument({ document: h.doc });
    first.callback(new Error("failure"), "", `error[E3004]: stale\n --> ${h.doc.uri.fsPath}:1:1\n`);
    assert.equal(h.diagnostics.size, 0); assert.equal(h.calls.filter(c => c[0] === "kill").length, 1);
});
test("compiler error populates diagnostic collection and close clears it", async () => {
    const h = harness(); await h.commands.get("hua.check")();
    const call = h.calls.find(c => c[0] === "exec")[1];
    assert.deepEqual(call.args, ["check", h.doc.uri.fsPath]);
    assert.equal(call.options.windowsHide, true);
    call.callback(new Error("failure"), "", `error[E3004]: unknown\n --> ${h.doc.uri.fsPath}:1:1\n`);
    assert.equal(h.diagnostics.get(h.doc.uri.fsPath)[0].code, "E3004");
    h.events.onDidCloseTextDocument(h.doc); assert.equal(h.diagnostics.size, 0);
});
