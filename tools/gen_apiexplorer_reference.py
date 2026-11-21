#!/usr/bin/env python
"""gen_apiexplorer_reference.py: addons\\vanacore\\apiexplorer\\reference.lua, the API Explorer addon's Reference tab, from
the addon docs:

  - docs\\addons\\15-api-reference.md: every row of its tables is an entry (what the first cell names, its arguments and
    what it returns as the cell has them, and the second cell as the description), filed under the table its name is in
    (vana.game.party() under vana.game whatever heading it sits under); a section's other paragraphs are its table's
    notes;
  - docs\\addons\\02-events.md: every event, with what its handler gets, what returning true does, and when it's raised;
  - a row whose description cell is empty takes the comment its function has in a topic page's code (vana.fs.exists(path)
    -- true if the file or folder is there).

The addon walks the live `vana` table in game for what's there: an entry no VanaCore function answers says so, and a
function or value no entry names is listed as not in the reference yet. So a new function shows up by itself; its
description comes when its line is in 15-api-reference.md and this is run again:

  python tools\\gen_apiexplorer_reference.py
"""
from __future__ import annotations

import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
DOCS = HERE.parent / "docs" / "addons"
OUT = HERE.parent / "addons" / "vanacore" / "apiexplorer" / "reference.lua"

# Rows whose names aren't under vana: the table they're filed under (methods on what a function returns)
OWNER = {"p": "vana.promise", "socket": "vana.ws", "box": "vana.ui", "store": "vana.settings", "response": "vana.http",
         "error": "vana.http", "coroutine": "vana.tasks", "print": "vana", "addon": "addon"}
# Method rows that are also functions in a live table: p:next is vana.promise.next
LIVE_ALIAS = {"p": "vana.promise"}
# vana's own functions filed with the table they go with
FILE_UNDER = {"vana.on": "vana.events", "vana.async": "vana.promise"}

NAME = re.compile(r"[.:]?[A-Za-z_<][\w<>]*(?:[.:/][A-Za-z_<][\w<>]*)*")


def plain(text: str) -> str:
    """Markdown to the text the window shows: no backticks, bold or links; ASCII arrows (ImGui's font may lack U+2192)."""
    text = text.replace("\\|", "|").replace("→", "->")
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    text = text.replace("**", "").replace("`", "")
    return re.sub(r"\s+", " ", text).strip()


def lua_string(text: str) -> str:
    return "'" + text.replace("\\", "\\\\").replace("'", "\\'").replace("\n", "\\n") + "'"


def lua_list(items: list[str]) -> str:
    return "{ " + ", ".join(lua_string(i) for i in items) + " }" if items else "{}"


def split_row(line: str) -> list[str]:
    cells = re.split(r"(?<!\\)\|", line.strip())
    return [c.strip() for c in cells[1:-1]]


def matching_paren(text: str, start: int) -> int:
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return len(text) - 1


class Piece:
    """One name in a row's first cell: `vana.game.entity(index) -> table`, `.urlencode(text)`, `box:show()`."""

    def __init__(self, text: str, previous: "Piece | None"):
        text = text.replace("\\|", "|").strip()
        m = NAME.match(text)
        name = m.group(0) if m else text
        rest = text[len(name):] if m else ""
        if name[:1] in ".:" and previous is not None:  # `.reject(r)` after `vana.promise.resolve(v)`
            name = previous.base + name
        self.name = name
        self.args = None  # None: not a call
        if rest.startswith("("):
            end = matching_paren(rest, 0)
            self.args = rest[1:end]
        cut = max(name.rfind("."), name.rfind(":"))
        self.base = name[:cut] if cut > 0 else name
        self.root = re.split(r"[.:\[]", name)[0]

    def names(self) -> list[str]:
        """The full names it stands for: read_int8/uint8/... is ten."""
        if "/" not in self.name:
            return [self.name]
        head, _, last = self.name.rpartition(".")
        parts = last.split("/")
        prefix = parts[0][: parts[0].rfind("_") + 1]
        return [f"{head}.{parts[0]}"] + [f"{head}.{prefix}{p}" for p in parts[1:]]

    def short(self, ns: str) -> str:
        name = self.name[len(ns) + 1:] if self.name.startswith(ns + ".") else self.name
        return name + (f"({plain(self.args)})" if self.args is not None else "")

    def call(self) -> str | None:
        """Code that calls it, the optional brackets out and the first of each `a | b`: vana.ui.text(props)."""
        if self.args is None or self.root not in ("vana", "coroutine") or "<" in self.name:
            return None
        args = plain(self.args).replace("[", "").replace("]", "")
        args = re.sub(r"\s*\|\s*[^,)]+", "", args)
        return f"{self.names()[0]}({args})"


def live_name(name: str) -> str | None:
    """The path the addon looks up in game for a name (wildcards as *), or None for a method on a returned object."""
    root = re.split(r"[.:\[]", name)[0]
    if root in LIVE_ALIAS:
        member = re.split(r"[.:]", name, maxsplit=1)
        return LIVE_ALIAS[root] + "." + member[1] if len(member) > 1 else None
    if root in ("vana", "coroutine", "addon", "print"):
        return re.sub(r"<[^>]*>.*$", "*", name)
    return None


def find_strings(name: str) -> list[str]:
    """What a snippet's code has when it uses the name: the path itself, or `:method(` for a method."""
    if "<" in name:
        return []
    if ":" in name:
        return [":" + name.split(":", 1)[1] + "("]
    if name.startswith(("vana.", "coroutine.")):
        return [name]
    return []


def topic_comments() -> dict[str, str]:
    """name -> what a topic page says of it where 15-api-reference.md's row has nothing: a table row of its own there
    (| `store:save()` | Writes the file now ... |), else the comment on its line in the page's code
    (`vana.fs.exists(path)   -- true if the file or folder is there`), a line whose arguments are names (not quoted
    values) over one that's an example."""
    rows: dict[str, str] = {}
    found: dict[str, tuple[bool, str]] = {}
    line_re = re.compile(r"^\s*(?:local\s+[\w, ]+=\s*)?([A-Za-z_]\w*(?:[.:]\w+)+)\((.*?)\)[^-]*--\s*(.+)$")
    for path in sorted(DOCS.glob("[01]*.md")):
        if path.name.startswith(("15-", "02-")):
            continue
        in_code = False
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("```"):
                in_code = not in_code
                continue
            if not in_code and line.startswith("|"):
                cells = split_row(line)
                pieces = re.findall(r"`([^`]+)`", cells[0]) if len(cells) >= 2 else []
                if len(pieces) == 1 and plain(cells[1]):
                    m = NAME.match(pieces[0])
                    if m and m.group(0) not in rows:
                        rows[m.group(0)] = plain(cells[1])
                continue
            m = line_re.match(line) if in_code else None
            if not m:
                continue
            name, args, comment = m.group(1), m.group(2), m.group(3).strip()
            generic = "'" not in args and '"' not in args
            if name not in found or (generic and not found[name][0]):
                found[name] = (generic, comment)
    said = {k: plain(v[1]) for k, v in found.items()}
    said.update(rows)
    return said


def reference_entries(comments: dict[str, str]):
    spaces: dict[str, dict] = {}  # name -> { page, notes }, in the order first met
    entries = []
    page, primary, notes_into = "15-api-reference.md", "vana", "vana"
    text = (DOCS / "15-api-reference.md").read_text(encoding="utf-8")
    for line in text.splitlines():
        heading = re.match(r"^(##+) (.*)$", line)
        if heading:
            title = heading.group(2)
            link = re.search(r"\]\(([^)#]+)", title)
            page = link.group(1) if link else "15-api-reference.md"
            first = re.search(r"`([^`]+)`", title)
            primary = first.group(1) if first else None
            notes_into = primary
            if primary and primary not in spaces:
                spaces[primary] = {"page": page, "notes": []}
            continue
        if line.startswith("|"):
            cells = split_row(line)
            if len(cells) < 2 or set(cells[0]) <= set("-: ") or cells[0] == "":
                continue
            first_cell, desc = cells[0], plain(cells[1])
            raw_pieces = re.findall(r"`([^`]+)`", first_cell) or [first_cell]
            pieces: list[Piece] = []
            for raw in raw_pieces:
                pieces.append(Piece(raw, pieces[-1] if pieces else None))
            head = pieces[0]
            if head.name in FILE_UNDER:
                ns = FILE_UNDER[head.name]
            elif head.root == "vana":
                ns = head.names()[0].rpartition(".")[0] if "." in head.name else "vana"
                if "<" in ns:
                    ns = ns[: ns.find("<") - 1]
            else:
                ns = OWNER.get(head.root, primary or "vana")
            names = [n for p in pieces for n in p.names()]
            if not desc:  # from the topic pages' code, each name's comment
                said = [(n, comments[n]) for n in dict.fromkeys(names) if n in comments]
                if len(said) == 1:
                    desc = said[0][1]
                elif said:
                    desc = "; ".join(f"{n.rpartition('.')[2]}: {c}" for n, c in said)
            calls = [c for c in (p.call() for p in pieces) if c]
            entries.append({
                "ns": ns,
                "label": ", ".join(dict.fromkeys(p.short(ns) for p in pieces)),
                "sig": plain(first_cell),
                "desc": desc,
                "page": page,
                "live": list(dict.fromkeys(n for n in (live_name(x) for x in names) if n)),
                "find": list(dict.fromkeys(f for x in names for f in find_strings(x))),
                "call": calls[0] if calls else None,
            })
            if ns not in spaces:
                spaces[ns] = {"page": page, "notes": []}
            continue
        if notes_into in spaces and not line.startswith(("#", "|")):
            spaces[notes_into]["notes"].append(line.strip())  # blank lines end a paragraph
    for s in spaces.values():
        paragraphs = re.split(r"\n\s*\n", "\n".join(s["notes"]))
        s["notes"] = [plain(p) for p in paragraphs if p.strip()]
    return spaces, entries


def event_entries():
    """02-events.md's tables: one entry a row, the bold bullet under it as the description."""
    text = (DOCS / "02-events.md").read_text(encoding="utf-8")
    bullets: dict[str, str] = {}
    for m in re.finditer(r"^- \*\*`(\w+)`\*\*:? (.+?)(?=\n- |\n\n|\Z)", text, re.M | re.S):
        bullets[m.group(1)] = plain(m.group(2))
    notes = []
    listening = re.search(r"## Listening\n(.*?)\n## ", text, re.S).group(1)
    for block in re.split(r"\n\s*\n", listening):
        block = block.strip()
        if block and not block.startswith("```"):
            notes.append(plain(block.replace("\n- ", "\n* ")) if not block.startswith("- ") else
                         "\n".join("* " + plain(b) for b in re.split(r"\n- ", block[2:])))
    entries, header = [], None
    for line in text.splitlines():
        if not line.startswith("|"):
            header = None if not line.strip() else header
            continue
        cells = split_row(line)
        if header is None:
            header = [plain(c) for c in cells]
            continue
        if set(cells[0]) <= set("-: "):
            continue
        events = re.findall(r"`(\w+)`", cells[0])
        more = []
        for title, cell in zip(header[1:], cells[1:]):
            value = plain(cell)
            if not value:
                continue
            title = {"e": "e", "Return": "Return true", "Return true": "Return true"}.get(title, title)
            more.append([title, value])
        desc = " ".join(bullets[e] for e in events if e in bullets)
        entries.append({
            "ns": "events",
            "label": ", ".join(events),
            "sig": ", ".join(events),
            "desc": desc,
            "page": "02-events.md",
            "live": [],
            "find": [f"on('{e}'" for e in events],
            "event": events[0],
            "more": more,
        })
    return notes, entries


def main() -> None:
    comments = topic_comments()
    spaces, entries = reference_entries(comments)
    event_notes, events = event_entries()

    order = []
    for name in spaces:
        order.append(name)
        if name == "vana.events":
            order.append("events")
    spaces["events"] = {"page": "02-events.md", "notes": event_notes, "title": "Events (vana.on)"}

    # 15's own list of events, against 02's tables
    listed = set(re.findall(r"`(\w+)`", re.search(r"Events: (.*?)\n\n", (DOCS / "15-api-reference.md").read_text(encoding="utf-8"), re.S).group(1)))
    known = {e for entry in events for e in entry["label"].split(", ")}
    for missing in sorted(listed - known):
        print(f"note: 15-api-reference.md lists the event {missing}, 02-events.md has no row for it")

    out = [
        "-- Generated by tools/gen_apiexplorer_reference.py from docs/addons/15-api-reference.md, 02-events.md and the",
        "-- topic pages' code comments: edit those and run it again, not this file. apiexplorer.lua's Reference tab reads it.",
        "return {",
        "    spaces = {",
    ]
    for name in order:
        s = spaces[name]
        title = s.get("title")
        out.append(f"        {{ name = {lua_string(name)}, page = {lua_string(s['page'])},"
                   + (f" title = {lua_string(title)}," if title else "")
                   + f" notes = {lua_list(s['notes'])} }},")
    out.append("    },")
    out.append("    entries = {")
    for e in entries + events:
        fields = [f"ns = {lua_string(e['ns'])}", f"label = {lua_string(e['label'])}", f"sig = {lua_string(e['sig'])}",
                  f"desc = {lua_string(e['desc'])}", f"page = {lua_string(e['page'])}", f"live = {lua_list(e['live'])}",
                  f"find = {lua_list(e['find'])}"]
        if e.get("call"):
            fields.append(f"call = {lua_string(e['call'])}")
        if e.get("event"):
            fields.append(f"event = {lua_string(e['event'])}")
        if e.get("more"):
            fields.append("more = { " + ", ".join("{ " + lua_string(t) + ", " + lua_string(v) + " }" for t, v in e["more"]) + " }")
        out.append("        { " + ", ".join(fields) + " },")
    out.append("    },")
    out.append("}")
    OUT.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    empty = sum(1 for e in entries if not e["desc"])
    print(f"{OUT}: {len(order)} tables, {len(entries)} entries ({empty} with no description), {len(events)} event rows")


if __name__ == "__main__":
    main()
