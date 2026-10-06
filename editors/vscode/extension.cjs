"use strict";
const vscode = require("vscode");
const { execFile } = require("node:child_process");
const path = require("node:path");
const fs = require("node:fs");
const { parseDiagnostics, utf16Column } = require("./diagnostics.cjs");
function activate(context) {
    const collection = vscode.languages.createDiagnosticCollection("hua");
    const output = vscode.window.createOutputChannel("Hua");
    const jobs = new Map();
    const owners = new Map();
    function refresh() {
        collection.clear();
        const merged = new Map();
        for (const items of owners.values()) for (const [file, diagnostics] of items) {
            const values = merged.get(file) || [];
            merged.set(file, values.concat(diagnostics));
        }
        for (const [file, diagnostics] of merged) collection.set(vscode.Uri.file(file), diagnostics);
    }
    function invalidate(doc) {
        const key = doc.uri.toString();
        const job = jobs.get(key);
        jobs.delete(key);
        if (job && job.child) job.child.kill();
        owners.delete(key); refresh();
    }
    function settings(doc) {
        const folder = vscode.workspace.getWorkspaceFolder(doc.uri);
        const cwd = folder ? folder.uri.fsPath : path.dirname(doc.uri.fsPath);
        const config = vscode.workspace.getConfiguration("hua", doc.uri);
        const executable = config.get("compilerPath", "hua").replace(/\$\{workspaceFolder\}/g, cwd);
        return { cwd, config, executable: executable.includes(path.sep) && !path.isAbsolute(executable) ? path.resolve(cwd, executable) : executable };
    }
    async function check(doc, manual) {
        if (!vscode.workspace.isTrusted || doc.languageId !== "hua" || doc.uri.scheme !== "file" || doc.isDirty) return;
        invalidate(doc);
        const key = doc.uri.toString();
        const job = {}; jobs.set(key, job);
        const { cwd, executable } = settings(doc);
        job.child = execFile(executable, ["check", doc.uri.fsPath], { cwd, timeout: 30000, maxBuffer: 4 * 1024 * 1024, windowsHide: true }, (error, stdout, stderr) => {
            if (jobs.get(key) !== job) return;
            jobs.delete(key);
            const diagnostics = new Map();
            for (const item of parseDiagnostics(stderr)) {
                const file = path.resolve(cwd, item.file);
                let lines = [];
                try { lines = fs.readFileSync(file, "utf8").replace(/^\uFEFF/, "").replace(/\r\n?/g, "\n").split("\n"); } catch { /* The CLI may report a missing source. */ }
                const column = utf16Column(lines[item.line] || "", item.byteColumn);
                const diagnostic = new vscode.Diagnostic(new vscode.Range(item.line, column, item.line, column + 1), item.message, vscode.DiagnosticSeverity.Error);
                diagnostic.code = item.code; diagnostic.source = "Hua";
                const values = diagnostics.get(file) || []; values.push(diagnostic); diagnostics.set(file, values);
            }
            owners.set(key, diagnostics); refresh();
            if (error && !diagnostics.size) {
                output.appendLine(stderr || error.message);
                if (manual) { output.show(); vscode.window.showErrorMessage("Hua compiler check failed. See the Hua output panel and hua.compilerPath."); }
            } else if (manual && !error) vscode.window.showInformationMessage("Hua: check passed.");
        });
    }
    async function current(command) {
        if (!vscode.workspace.isTrusted) { vscode.window.showWarningMessage("Trust this workspace before running the Hua compiler."); return; }
        const editor = vscode.window.activeTextEditor;
        if (!editor || editor.document.languageId !== "hua" || editor.document.uri.scheme !== "file") { vscode.window.showWarningMessage("Open a local .hua file first."); return; }
        const doc = editor.document;
        if (doc.isDirty && !await doc.save()) return;
        if (command === "check") { check(doc, true); return; }
        const { cwd, config, executable } = settings(doc);
        const args = [command, doc.uri.fsPath];
        if (command === "run") { const extra = config.get("runArguments", []); if (extra.length) args.push("--", ...extra); }
        const folder = vscode.workspace.getWorkspaceFolder(doc.uri);
        const task = new vscode.Task({ type: "hua", command }, folder || vscode.TaskScope.Global, command + " " + path.basename(doc.uri.fsPath), "Hua", new vscode.ProcessExecution(executable, args, { cwd }), []);
        await vscode.tasks.executeTask(task);
    }
    context.subscriptions.push(collection, output,
        vscode.workspace.onDidSaveTextDocument(doc => { if (vscode.workspace.getConfiguration("hua", doc.uri).get("checkOnSave", true)) check(doc, false); }),
        vscode.workspace.onDidChangeTextDocument(event => { if (event.document.languageId === "hua") invalidate(event.document); }),
        vscode.workspace.onDidCloseTextDocument(invalidate),
        ...["check", "run", "build"].map(command => vscode.commands.registerCommand("hua." + command, () => current(command))),
        { dispose() { for (const job of jobs.values()) if (job.child) job.child.kill(); jobs.clear(); } });
}
module.exports = { activate };
