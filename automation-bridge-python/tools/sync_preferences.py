#!/usr/bin/env python3
"""Generate the built-in Defold preference catalog from editor.prefs/default-schema."""

import argparse
import json
import pprint
import re
from pathlib import Path


class Reader:
    def __init__(self, text):
        self.text = text
        self.index = 0

    def skip(self):
        while self.index < len(self.text):
            if self.text[self.index].isspace() or self.text[self.index] == ",":
                self.index += 1
            elif self.text[self.index] == ";":
                self.index = self.text.find("\n", self.index)
                if self.index < 0:
                    self.index = len(self.text)
            elif self.text.startswith("#_", self.index):
                self.index += 2
                self.read()
            else:
                return

    def read(self):
        self.skip()
        ch = self.text[self.index]
        if ch == "{":
            self.index += 1
            value = {}
            while True:
                self.skip()
                if self.text[self.index] == "}":
                    self.index += 1
                    return value
                key = self.read()
                value[key] = self.read()
        if ch in "[(":
            end = "]" if ch == "[" else ")"
            self.index += 1
            value = []
            while True:
                self.skip()
                if self.text[self.index] == end:
                    self.index += 1
                    return value
                value.append(self.read())
        if ch == '"':
            self.index += 1
            out = []
            while True:
                ch = self.text[self.index]
                self.index += 1
                if ch == '"':
                    return "".join(out)
                if ch == "\\":
                    escaped = self.text[self.index]
                    self.index += 1
                    out.append({"n": "\n", "t": "\t", "r": "\r"}.get(escaped, escaped))
                else:
                    out.append(ch)
        start = self.index
        while self.index < len(self.text) and not self.text[self.index].isspace() and self.text[self.index] not in "{}[](),":
            self.index += 1
        token = self.text[start:self.index]
        if token.startswith(":"):
            return token[1:]
        if token == "nil":
            return None
        if token in ("true", "false"):
            return token == "true"
        try:
            return float(token) if any(c in token for c in ".eE") else int(token)
        except ValueError:
            return token


DESCRIPTION_OVERRIDES = {
    "code/font/name": "Font family used by the code editor.",
    "code/font/size": "Font size used by the code editor.",
    "code/auto-closing-parens": "Whether the code editor automatically inserts matching parentheses.",
    "build/lint-code": "Whether project builds run the configured code lint checks.",
    "build/texture-compression": "Whether builds use texture compression.",
    "run/instance-count": "Number of local engine instances launched by Build and Run.",
    "run/engine-arguments": "Command-line arguments supplied to locally launched engines.",
    "scene/grid/size/x": "Scene editor grid spacing on the X axis.",
}


def constant_name(path):
    return re.sub(r"[^A-Z0-9]+", "_", path.upper()).strip("_")


def default_description(path, group):
    if path in DESCRIPTION_OVERRIDES:
        return DESCRIPTION_OVERRIDES[path]
    label = path.replace("/", " ").replace("-", " ").replace("+", " and ")
    return f"Built-in Defold editor {'preference group' if group else 'preference'} for {label}."


def catalog(schema, descriptions=None):
    result = []
    names = set()

    def visit(node, path=(), inherited_scope="global"):
        if not isinstance(node, dict):
            return
        scope = str(node.get("scope", inherited_scope))
        properties = node.get("properties")
        group = node.get("type") == "object" and isinstance(properties, dict)
        if path:
            path_text = "/".join(path)
            name = constant_name(path_text)
            if name in names:
                raise RuntimeError(f"preference constant collision: {name}")
            names.add(name)
            has_default = "default" in node
            default = node.get("default")
            if isinstance(default, list) and default and not all(isinstance(v, (str, int, float, bool, type(None))) for v in default):
                default = None
                has_default = False
            result.append({
                "name": name,
                "path": path_text,
                "type": str(node.get("type", "unknown")),
                "scope": scope,
                "default": default,
                "has_default": has_default,
                "description": (
                    descriptions[path_text]
                    if descriptions is not None and path_text in descriptions
                    else default_description(path_text, group)
                ),
                "group": group,
                "enum_values": node.get("values") if node.get("type") == "enum" else None,
                "ui": node.get("ui") if isinstance(node.get("ui"), dict) else None,
            })
        if group:
            for key, child in properties.items():
                visit(child, path + (str(key),), scope)

    visit(schema)
    if descriptions is not None:
        missing = sorted(item["path"] for item in result if item["path"] not in descriptions)
        if missing:
            raise RuntimeError("missing curated preference descriptions: " + ", ".join(missing))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--defold-repo", type=Path, default=Path("../defold"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--descriptions", type=Path, default=Path(__file__).with_name("preference_descriptions.json"))
    parser.add_argument("--write-description-template", action="store_true")
    args = parser.parse_args()
    source = args.defold_repo / "editor/src/clj/editor/prefs.clj"
    text = source.read_text(encoding="utf-8")
    marker = "(def default-schema"
    start = text.index(marker) + len(marker)
    schema = Reader(text[start:]).read()
    if args.write_description_template:
        entries = catalog(schema)
        descriptions = {item["path"]: item["description"] for item in entries}
        args.descriptions.write_text(json.dumps(descriptions, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"wrote {len(descriptions)} descriptions to {args.descriptions}")
    descriptions = json.loads(args.descriptions.read_text(encoding="utf-8"))
    entries = catalog(schema, descriptions)
    output = args.output or Path(__file__).parents[1] / "automation_bridge/_preferences_catalog.py"
    rendered = (
        '"""Generated built-in Defold preference metadata. Do not edit manually."""\n\n'
        + "CATALOG = "
        + pprint.pformat(tuple(entries), width=120, sort_dicts=False)
        + "\n"
    )
    output.write_text(rendered, encoding="utf-8")
    print(f"wrote {len(entries)} preferences to {output}")


if __name__ == "__main__":
    main()
