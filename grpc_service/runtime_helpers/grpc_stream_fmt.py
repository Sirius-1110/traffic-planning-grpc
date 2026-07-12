#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pretty-print grpcurl stream JSON as UTF-8 progress lines."""
import json
import sys


def iter_objs(text):
    buf, depth = "", 0
    for ch in text:
        buf += ch
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and buf.strip():
                yield json.loads(buf.strip())
                buf = ""


def main():
    raw = sys.stdin.read()
    for obj in iter_objs(raw):
        data = obj.get("data") or {}
        pct = data.get("percent", "-")
        title = data.get("title", "")
        msg = obj.get("message", "")
        line = f"[{pct}%] {title}"
        if msg and msg not in ("Starting", "Running"):
            line += f" ({msg})"
        if data.get("done") == 1:
            result = obj.get("result") or {}
            line += f" => code={obj.get('code')} {result.get('message', '')}"
        print(line, flush=True)
    if raw:
        sys.stdout.write(raw)


if __name__ == "__main__":
    main()
