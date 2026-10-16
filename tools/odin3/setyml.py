#!/usr/bin/env python3
# setyml.py <in.yml> <out.yml> [--db <serial>] [Section/Sub/Key=value ...]: copies an RPCS3/aPS3e config, setting
# nested keys; --db first applies the game's entry of the app's database_config.json.
# Edits the text line by line so every other value keeps its exact spelling (a YAML round trip turned
# "Aspect ratio: 16:9" into 969 and rewrote the PSID); a key missing from its section is appended to it.
import json
import os
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
lines = open(src).read().splitlines()
KEY = re.compile(r"^( *)([^ #][^:]*):(?: (.*))?$")


def flatten(text):
    """Section/Key=value edits for every valued key of a YAML fragment."""
    stack, edits = [], []
    for line in text.splitlines():
        m = KEY.match(line)
        if not m:
            continue
        while stack and stack[-1][0] >= len(m.group(1)):
            stack.pop()
        stack.append((len(m.group(1)), m.group(2)))
        if m.group(3):
            edits.append("/".join(k for _, k in stack) + "=" + m.group(3))
    return edits


edits = sys.argv[3:]
if edits[:1] == ["--db"]:
    db = json.load(open(os.path.join(os.path.dirname(__file__), "../../app/src/main/assets/database_config.json")))
    edits = (flatten(db[edits[1]]["config"]) if edits[1] in db else []) + edits[2:]


def find(path):
    """Index of the line holding path, or (None, end of the deepest existing section, its child indent)."""
    stack = []
    best = (-1, 0, -1)
    for i, line in enumerate(lines):
        m = KEY.match(line)
        if not m:
            continue
        indent = len(m.group(1))
        while stack and stack[-1][0] >= indent:
            stack.pop()
        stack.append((indent, m.group(2)))
        keys = [k for _, k in stack]
        if keys == path:
            return i
        if keys == path[:len(keys)] and len(keys) > best[0] + 1:
            best = (len(keys) - 1, i, indent)
    depth, start, indent = best
    if depth < 0 or depth != len(path) - 2:
        sys.exit(f"setyml: no section for {'/'.join(path)}")
    end = start + 1
    while end < len(lines) and (not KEY.match(lines[end]) or len(KEY.match(lines[end]).group(1)) > indent):
        end += 1
    return None, end, indent + 2


for edit in edits:
    path, _, value = edit.partition("=")
    path = path.split("/")
    hit = find(path)
    if isinstance(hit, int):
        m = KEY.match(lines[hit])
        lines[hit] = f"{m.group(1)}{m.group(2)}: {value}"
    else:
        _, end, indent = hit
        lines.insert(end, f"{' ' * indent}{path[-1]}: {value}")
open(dst, "w").write("\n".join(lines) + "\n")
