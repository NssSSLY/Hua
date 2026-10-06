"use strict";
function parseDiagnostics(text) {
    const result = [];
    const expression = /error\[([^\]]+)\]: ([^\r\n]*)\r?\n\s*--> (.*):(\d+):(\d+)/g;
    let m;
    while ((m = expression.exec(text))) {
        result.push({ code: m[1], message: m[2], file: m[3], line: Number(m[4]) - 1, byteColumn: Number(m[5]) - 1 });
    }
    return result;
}
function utf16Column(line, byteColumn) {
    let bytes = 0, units = 0;
    for (const rune of line) {
        const width = Buffer.byteLength(rune, "utf8");
        if (bytes + width > byteColumn) break;
        bytes += width; units += rune.length;
    }
    return units;
}
module.exports = { parseDiagnostics, utf16Column };
