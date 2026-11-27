"""End-to-end LSP protocol tests for `hua lsp` (diagnostics, semantic tokens, hover, outline, definition, completion, multi-error recovery)."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path


TOKEN_TYPES = [
    "namespace", "type", "class", "enum", "interface", "struct",
    "typeParameter", "parameter", "variable", "property", "enumMember",
    "function", "method", "keyword", "comment", "string", "number", "operator",
]
TOKEN_MODS = [
    "declaration", "definition", "readonly", "static", "deprecated", "defaultLibrary", "modification",
]


def send_msg(proc: subprocess.Popen, payload: dict) -> None:
    body = json.dumps(payload).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
    assert proc.stdin is not None
    proc.stdin.write(header + body)
    proc.stdin.flush()


def read_msg(proc: subprocess.Popen) -> dict:
    assert proc.stdout is not None
    headers = {}
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("Unexpected EOF from hua lsp")
        if line in (b"\r\n", b"\n"):
            break
        k, v = line.decode("ascii").split(":", 1)
        headers[k.strip().lower()] = v.strip()
    length = int(headers["content-length"])
    body = proc.stdout.read(length)
    assert len(body) == length, f"Short read: {len(body)} != {length}"
    return json.loads(body.decode("utf-8"))


def wait_response(proc: subprocess.Popen, req_id: int, notifications: list) -> dict:
    while True:
        msg = read_msg(proc)
        if msg.get("id") == req_id:
            return msg
        notifications.append(msg)


def wait_diagnostics(proc: subprocess.Popen, notifications: list, target_uri: str) -> list:
    for idx, msg in enumerate(notifications):
        if msg.get("method") == "textDocument/publishDiagnostics" and msg["params"]["uri"].lower() == target_uri.lower():
            notifications.pop(idx)
            return msg["params"]["diagnostics"]
    while True:
        msg = read_msg(proc)
        if msg.get("method") == "textDocument/publishDiagnostics" and msg["params"]["uri"].lower() == target_uri.lower():
            return msg["params"]["diagnostics"]
        notifications.append(msg)


def decode_tokens(source_text: str, data: list[int]) -> list[tuple[str, str, set[str]]]:
    lines = source_text.splitlines()
    out = []
    line = 0
    col = 0
    assert len(data) % 5 == 0
    for i in range(0, len(data), 5):
        dl, dc, length, t_idx, m_mask = data[i:i + 5]
        line += dl
        col = dc if dl > 0 else col + dc
        text = lines[line][col:col + length] if line < len(lines) else ""
        mods = {TOKEN_MODS[b] for b in range(len(TOKEN_MODS)) if m_mask & (1 << b)}
        out.append((text, TOKEN_TYPES[t_idx], mods))
    return out


def main() -> int:
    hua_exe = Path(sys.argv[1]).resolve()
    proc = subprocess.Popen(
        [str(hua_exe), "lsp"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    notifications: list[dict] = []
    req_id = 0

    def next_id() -> int:
        nonlocal req_id
        req_id += 1
        return req_id

    try:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = Path(tmp)
            main_path = tmp_dir / "main.hua"
            helper_path = tmp_dir / "helper.hua"
            helper_path.write_text(
                "pub struct Item {\n"
                "    score int\n"
                "}\n"
                "pub fn add(a int, b int) int { return a + b }\n"
                "pub fn make_item(s int) Item { return Item{score: s} }\n",
                encoding="utf-8",
            )
            main_code = (
                "import helper\n"                              # line 0
                "import std.strings as s\n"                    # line 1
                "struct User {\n"                              # line 2
                "    name string\n"                            # line 3
                "    age int\n"                                # line 4
                "}\n"                                          # line 5
                "fn User.title() string { return self.name }\n"  # line 6
                "enum Role {\n"                                # line 7
                "    Admin\n"                                  # line 8
                "    Guest\n"                                  # line 9
                "}\n"                                          # line 10
                "const MAX_AGE int = 120\n"                    # line 11
                "fn greet(u User) string {\n"                  # line 12
                "    var count = helper.add(u.age, 1)\n"       # line 13
                "    let label = s.trim(u.name)\n"             # line 14
                "    let r = Role.Admin\n"                     # line 15
                "    return label\n"                           # line 16
                "}\n"                                          # line 17
                "fn main() {\n"                                # line 18
                "    let user = User{name: \"hua\", age: 3}\n" # line 19
                "    print(greet(user), MAX_AGE)\n"            # line 20
                "    let a, b = 10, 20\n"                      # line 21
                "    let label = a + b\n"                      # line 22
                "    let it = helper.make_item(label)\n"       # line 23
                "    print(it.score)\n"                        # line 24
                "}\n"                                          # line 25
            )
            main_path.write_text(main_code, encoding="utf-8")
            main_uri = main_path.as_uri()
            helper_uri = helper_path.as_uri()

            # 1. Initialize handshake & capabilities
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "initialize",
                "params": {"processId": None, "rootUri": tmp_dir.as_uri(), "capabilities": {}},
            })
            caps = wait_response(proc, rid, notifications)["result"]["capabilities"]
            assert caps["hoverProvider"] is True
            assert caps["definitionProvider"] is True
            assert caps["documentSymbolProvider"] is True
            assert "." in caps["completionProvider"]["triggerCharacters"]
            assert caps["semanticTokensProvider"]["full"] is True
            assert caps["semanticTokensProvider"]["legend"]["tokenTypes"] == TOKEN_TYPES
            send_msg(proc, {"jsonrpc": "2.0", "method": "initialized", "params": {}})

            # 2. didOpen clean file -> 0 diagnostics
            send_msg(proc, {
                "jsonrpc": "2.0", "method": "textDocument/didOpen",
                "params": {"textDocument": {"uri": main_uri, "languageId": "hua", "version": 1, "text": main_code}},
            })
            diags = wait_diagnostics(proc, notifications, main_uri)
            assert diags == [], f"Expected 0 diagnostics, got {diags}"

            # 3. Semantic tokens classification check
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": main_uri}},
            })
            decoded = decode_tokens(main_code, wait_response(proc, rid, notifications)["result"]["data"])
            by_text = {}
            for txt, kind, mods in decoded:
                by_text.setdefault(txt, []).append((kind, mods))
            assert ("struct", {"declaration"}) in by_text.get("User", []), by_text.get("User")
            assert ("enum", {"declaration"}) in by_text.get("Role", []), by_text.get("Role")
            assert any(k == "variable" and "readonly" in m and "static" in m for k, m in by_text.get("MAX_AGE", []))
            assert any(k == "function" and "declaration" in m for k, m in by_text.get("greet", []))
            assert any(k == "variable" and "modification" in m for k, m in by_text.get("count", []))
            assert any(k == "variable" and "readonly" in m for k, m in by_text.get("label", []))
            assert any(k == "namespace" for k, _ in by_text.get("helper", []))

            # 4. Hover check: local fn + stdlib fn + imported fn
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/hover",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 12, "character": 4}},
            })
            assert "fn greet(u User) string" in wait_response(proc, rid, notifications)["result"]["contents"]["value"]

            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/hover",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 14, "character": 19}},  # s.trim
            })
            std_hover = wait_response(proc, rid, notifications)["result"]["contents"]["value"]
            assert "std.strings.trim" in std_hover, std_hover

            # 5. Go to Definition: local fn, cross-file helper.add, struct field u.age, enum variant Role.Admin
            # 5a. greet(user) -> line 12
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/definition",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 20, "character": 11}},
            })
            def_greet = wait_response(proc, rid, notifications)["result"]
            assert def_greet["uri"].lower() == main_uri.lower() and def_greet["range"]["start"]["line"] == 12, def_greet

            # 5b. helper.add -> helper.hua line 3
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/definition",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 13, "character": 24}},
            })
            def_add = wait_response(proc, rid, notifications)["result"]
            assert "helper.hua" in def_add["uri"].lower() and def_add["range"]["start"]["line"] == 3, def_add

            # 5c. u.age -> struct User field age at line 4
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/definition",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 13, "character": 30}},
            })
            def_age = wait_response(proc, rid, notifications)["result"]
            assert def_age["range"]["start"]["line"] == 4, def_age

            # 5d. Role.Admin -> enum Role variant Admin at line 8
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/definition",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 15, "character": 18}},
            })
            def_admin = wait_response(proc, rid, notifications)["result"]
            assert def_admin["range"]["start"]["line"] == 8, def_admin

            # 5e. self.name in fn User.title -> struct User field name at line 3
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/definition",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 6, "character": 39}},
            })
            def_self_name = wait_response(proc, rid, notifications)["result"]
            assert def_self_name["range"]["start"]["line"] == 3, def_self_name

            # 5f. MultiBinding variable `b` in main -> line 21
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/definition",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 22, "character": 20}},
            })
            def_b = wait_response(proc, rid, notifications)["result"]
            assert def_b["range"]["start"]["line"] == 21, def_b

            # 5g. Scoped variable `label` in main -> line 22 (NOT greet's line 14!)
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/definition",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 23, "character": 32}},
            })
            def_label_main = wait_response(proc, rid, notifications)["result"]
            assert def_label_main["range"]["start"]["line"] == 22, def_label_main

            # 5h. Cross-file inferred struct field `it.score` -> helper.hua line 1
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/definition",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 24, "character": 15}},
            })
            def_score = wait_response(proc, rid, notifications)["result"]
            assert "helper.hua" in def_score["uri"].lower() and def_score["range"]["start"]["line"] == 1, def_score

            # 6. Completion: stdlib module (422 stdlib), import std., imported helper., struct u., enum Role.
            comp_code = main_code + (
                "import std.\n"             # line 26
                "fn comp_test(u User) {\n"  # line 27
                "    s.\n"                  # line 28
                "    helper.\n"             # line 29
                "    u.\n"                  # line 30
                "    Role.\n"               # line 31
                "}\n"
            )
            send_msg(proc, {
                "jsonrpc": "2.0", "method": "textDocument/didChange",
                "params": {"textDocument": {"uri": main_uri, "version": 2}, "contentChanges": [{"text": comp_code}]},
            })
            _ = wait_diagnostics(proc, notifications, main_uri)

            # 6a. `import std.` -> 32 builtin modules + SDK modules
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/completion",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 26, "character": 11}},
            })
            std_mods = {item["label"] for item in wait_response(proc, rid, notifications)["result"]}
            for m in ("strings", "http", "regex", "compress", "task", "simd", "stream", "csv", "sse"):
                assert m in std_mods, f"Missing std module {m} in {std_mods}"
            assert len(std_mods) >= 35, len(std_mods)

            # 6b. `s.` -> std.strings functions with signatures
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/completion",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 28, "character": 6}},
            })
            str_items = {item["label"]: item.get("detail", "") for item in wait_response(proc, rid, notifications)["result"]}
            for fn in ("split", "trim", "replace", "starts_with"):
                assert fn in str_items and "std.strings." in str_items[fn], (fn, str_items.get(fn))

            # 6c. `helper.` -> user module export `add`
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/completion",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 29, "character": 11}},
            })
            helper_items = {item["label"] for item in wait_response(proc, rid, notifications)["result"]}
            assert "add" in helper_items, helper_items

            # 6d. `u.` -> struct User fields + inherent method `title`
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/completion",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 30, "character": 6}},
            })
            u_items = {item["label"] for item in wait_response(proc, rid, notifications)["result"]}
            assert {"name", "age", "title"}.issubset(u_items), u_items

            # 6e. `Role.` -> enum variants Admin, Guest
            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/completion",
                "params": {"textDocument": {"uri": main_uri}, "position": {"line": 31, "character": 9}},
            })
            role_items = {item["label"] for item in wait_response(proc, rid, notifications)["result"]}
            assert {"Admin", "Guest"}.issubset(role_items), role_items

            # 7. Parser multi-error recovery: multiple syntax errors reported at once AND later valid fn preserved!
            multi_err_code = (
                "fn broken_one() {\n"
                "    let x = \n"
                "    let y = 1\n"
                "    let z = \n"
                "}\n"
                "fn broken_two( {\n"
                "}\n"
                "fn after_errors() int {\n"
                "    return 42\n"
                "}\n"
            )
            send_msg(proc, {
                "jsonrpc": "2.0", "method": "textDocument/didChange",
                "params": {"textDocument": {"uri": main_uri, "version": 3}, "contentChanges": [{"text": multi_err_code}]},
            })
            diags = wait_diagnostics(proc, notifications, main_uri)
            assert len(diags) >= 3, f"Expected >= 3 recovered syntax errors, got {len(diags)}: {diags}"

            rid = next_id()
            send_msg(proc, {
                "jsonrpc": "2.0", "id": rid, "method": "textDocument/documentSymbol",
                "params": {"textDocument": {"uri": main_uri}},
            })
            recovered_syms = [s["name"] for s in wait_response(proc, rid, notifications)["result"]]
            assert "after_errors" in recovered_syms, f"Expected after_errors in outline after recovery: {recovered_syms}"

            # 8. Cross-file VFS overlay check: break helper.hua in memory -> main.hua fails -> fix -> 0 errors
            send_msg(proc, {
                "jsonrpc": "2.0", "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": helper_uri, "languageId": "hua", "version": 1,
                        "text": "pub struct Item { score int }\npub fn add(a string, b string) string { return a + b }\npub fn make_item(s int) Item { return Item{score: s} }\n",
                    }
                },
            })
            _ = wait_diagnostics(proc, notifications, helper_uri)
            send_msg(proc, {
                "jsonrpc": "2.0", "method": "textDocument/didChange",
                "params": {"textDocument": {"uri": main_uri, "version": 4}, "contentChanges": [{"text": main_code}]},
            })
            diags = wait_diagnostics(proc, notifications, main_uri)
            assert len(diags) == 1 and diags[0]["code"].startswith("E3"), f"Expected cross-file VFS type error, got {diags}"

            send_msg(proc, {
                "jsonrpc": "2.0", "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": helper_uri, "version": 2},
                    "contentChanges": [{"text": "pub struct Item { score int }\npub fn add(a int, b int) int { return a + b }\npub fn make_item(s int) Item { return Item{score: s} }\n"}],
                },
            })
            _ = wait_diagnostics(proc, notifications, helper_uri)
            send_msg(proc, {
                "jsonrpc": "2.0", "method": "textDocument/didChange",
                "params": {"textDocument": {"uri": main_uri, "version": 5}, "contentChanges": [{"text": main_code}]},
            })
            diags = wait_diagnostics(proc, notifications, main_uri)
            assert diags == [], f"Expected 0 diagnostics after fix, got {diags}"

            # 9. Clean shutdown + exit
            rid = next_id()
            send_msg(proc, {"jsonrpc": "2.0", "id": rid, "method": "shutdown", "params": None})
            assert wait_response(proc, rid, notifications)["result"] is None
            send_msg(proc, {"jsonrpc": "2.0", "method": "exit", "params": None})
            assert proc.wait(timeout=5) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
    print("OK: all hua lsp protocol tests passed (definition, 422-stdlib completion, multi-error recovery)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
