"use strict";
const { spawn } = require("node:child_process");

class HuaLspClient {
    constructor(executable, cwd, output, trace) {
        this.executable = executable;
        this.cwd = cwd;
        this.output = output;
        this.trace = Boolean(trace);
        this.child = null;
        this.buffer = Buffer.alloc(0);
        this.nextId = 1;
        this.pending = new Map();
        this.handlers = new Map();
        this.capabilities = null;
        this.readyPromise = null;
        this.disposed = false;
    }

    onNotification(method, handler) {
        this.handlers.set(method, handler);
    }

    async start(rootUri) {
        if (this.readyPromise) return this.readyPromise;
        this.readyPromise = new Promise((resolve, reject) => {
            try {
                this.child = spawn(this.executable, ["lsp"], {
                    cwd: this.cwd,
                    stdio: ["pipe", "pipe", "pipe"],
                    windowsHide: true
                });
            } catch (err) {
                reject(err);
                return;
            }

            this.child.stdout.on("data", chunk => this._onData(chunk));
            this.child.stderr.on("data", chunk => {
                const text = chunk.toString("utf8").trim();
                if (text && this.output) this.output.appendLine("[hua lsp stderr] " + text);
            });
            this.child.on("error", err => {
                if (this.output) this.output.appendLine("[hua lsp error] " + err.message);
                this._rejectAll(err);
                reject(err);
            });
            this.child.on("exit", (code, signal) => {
                if (!this.disposed && this.output) {
                    this.output.appendLine(`[hua lsp] exited (code=${code}, signal=${signal})`);
                }
                this._rejectAll(new Error("hua lsp process exited"));
                this.child = null;
                this.readyPromise = null;
            });

            this.request("initialize", {
                processId: process.pid,
                rootUri: rootUri || null,
                capabilities: {
                    textDocument: {
                        publishDiagnostics: { relatedInformation: true },
                        semanticTokens: {
                            requests: { full: true },
                            tokenTypes: [],
                            tokenModifiers: [],
                            formats: ["relative"]
                        },
                        hover: { contentFormat: ["markdown", "plaintext"] },
                        documentSymbol: { hierarchicalDocumentSymbolSupport: true }
                    }
                }
            }).then(result => {
                this.capabilities = result && result.capabilities ? result.capabilities : {};
                this.notify("initialized", {});
                resolve(this.capabilities);
            }).catch(reject);
        });
        return this.readyPromise;
    }

    isRunning() {
        return Boolean(this.child && !this.child.killed && this.capabilities);
    }

    _send(message) {
        if (!this.child || !this.child.stdin || this.child.stdin.destroyed) return false;
        const json = JSON.stringify(message);
        const bytes = Buffer.from(json, "utf8");
        const header = `Content-Length: ${bytes.length}\r\n\r\n`;
        if (this.trace && this.output) this.output.appendLine("[lsp ->] " + json.slice(0, 500));
        this.child.stdin.write(header, "ascii");
        this.child.stdin.write(bytes);
        return true;
    }

    notify(method, params) {
        this._send({ jsonrpc: "2.0", method, params });
    }

    request(method, params) {
        return new Promise((resolve, reject) => {
            const id = this.nextId++;
            this.pending.set(id, { resolve, reject });
            if (!this._send({ jsonrpc: "2.0", id, method, params })) {
                this.pending.delete(id);
                reject(new Error("hua lsp is not running"));
            }
        });
    }

    _onData(chunk) {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        while (true) {
            const sep = this.buffer.indexOf("\r\n\r\n");
            if (sep === -1) return;
            const headerStr = this.buffer.slice(0, sep).toString("ascii");
            const match = /Content-Length:\s*(\d+)/i.exec(headerStr);
            if (!match) {
                this.buffer = this.buffer.slice(sep + 4);
                continue;
            }
            const length = Number(match[1]);
            const bodyStart = sep + 4;
            if (this.buffer.length < bodyStart + length) return;
            const bodyStr = this.buffer.slice(bodyStart, bodyStart + length).toString("utf8");
            this.buffer = this.buffer.slice(bodyStart + length);
            if (this.trace && this.output) this.output.appendLine("[lsp <-] " + bodyStr.slice(0, 500));
            let msg;
            try {
                msg = JSON.parse(bodyStr);
            } catch {
                continue;
            }
            if (msg.id !== undefined && this.pending.has(msg.id)) {
                const entry = this.pending.get(msg.id);
                this.pending.delete(msg.id);
                if (msg.error) entry.reject(new Error(msg.error.message || "LSP error"));
                else entry.resolve(msg.result);
            } else if (msg.method && this.handlers.has(msg.method)) {
                try {
                    this.handlers.get(msg.method)(msg.params);
                } catch (err) {
                    if (this.output) this.output.appendLine("[lsp handler error] " + err.message);
                }
            }
        }
    }

    _rejectAll(err) {
        for (const entry of this.pending.values()) entry.reject(err);
        this.pending.clear();
    }

    async stop() {
        this.disposed = true;
        if (!this.child) return;
        const proc = this.child;
        try {
            await Promise.race([
                this.request("shutdown", null),
                new Promise(r => setTimeout(r, 800))
            ]);
            this.notify("exit", null);
        } catch {
            // Ignore shutdown errors on exit
        }
        setTimeout(() => {
            if (proc && !proc.killed) {
                try { proc.kill(); } catch {}
            }
        }, 200);
        this.child = null;
        this.capabilities = null;
        this.readyPromise = null;
    }
}

module.exports = { HuaLspClient };
