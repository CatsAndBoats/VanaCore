"""Which of Windower 4's API the Windower addons use, and whether VanaCore has each.

Reads every addon's Lua (and the Windower libraries each one requires, followed through the libraries' own requires)
for windower.<name> calls, register_event names and native modules, then checks each against what
src/core/lua/compat/windower.lua makes: the `windower` table as windower_dump.lua prints it (run through the luatest
runner, build/obj/luatest/luarun.exe), and its RAISED events. A function there whose body is empty or only returns a
constant is a stub: the call works but does nothing.

Given Windower's own API definitions (definitions/windower.lua in github.com/Windower/Lua), it also lists the API
they name that VanaCore lacks though no addon here calls it, and the fields of get_player, get_info,
get_windower_settings, get_camera and get_item_display that windower.lua never sets.

Usage:
    python tools/windower_coverage.py <addons folder> [<addons folder> ...] [--libs <Windower libs folder>]
        [--definitions <definitions/windower.lua>] [--out <report.md>]
An addons folder holds one folder per addon (<name>/<name>.lua); its libs folder, if it has one, is the libraries
(--libs picks another; default: the first addons folder's libs). The first folder to have an addon is the one read.
"""
import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WINDOWER_LUA = os.path.join(ROOT, 'src', 'core', 'lua', 'compat', 'windower.lua')
DUMP_LUA = os.path.join(ROOT, 'tools', 'windower_dump.lua')
LUARUN = os.path.join(ROOT, 'build', 'obj', 'luatest', 'luarun.exe')

NAMESPACES = ('ffxi', 'text', 'prim', 'packets', 'chat', 'console', 'regex')
# windower.<field> an addon reads, not a call: still API (VanaCore must set them), reported with the calls
USE = re.compile(r'(?<![\w.:])windower\.([A-Za-z_]\w*)(?:\.([A-Za-z_]\w*))?')
DEFINE = re.compile(r'function\s+windower\.([\w.]+)|(?<![\w.])windower\.([\w.]+)\s*=(?!=)')
ALIAS = re.compile(r'\blocal\s+([A-Za-z_]\w*)\s*=\s*windower\.(%s)\b(?!\s*[.\[(])' % '|'.join(NAMESPACES))
# windower.register_event or a bare register_event, not an object's own (findAll's tracker:register_event('reload'))
EVENT = re.compile(r'(?:\bwindower\.|(?<![\w.:]))(?:raw_)?register_event\s*\(\s*((?:(?:\'[^\']*\'|"[^"]*")\s*,\s*)+)')
REQUIRE = re.compile(r'\brequire\s*\(?\s*[\'"]([^\'"]+)[\'"](?!\s*\.\.)')  # not require('nms/' .. name)
LOADLIB = re.compile(r'\bpackage\.loadlib\b')
# modules a VanaCore Lua state has without a file: LuaJIT's, and the preloads (lua_api.cpp: socket.core, mime.core;
# compat/windower.lua: pack)
BUILT_IN = {'ffi', 'bit', 'jit', 'string', 'table', 'math', 'os', 'io', 'coroutine', 'debug', 'package',
            'socket.core', 'mime.core', 'pack'}


def read(path):
    with open(path, 'rb') as f:
        return f.read().decode('utf-8', errors='replace')


def strip_comments(text):
    text = re.sub(r'--\[(=*)\[.*?\]\1\]', ' ', text, flags=re.S)
    return re.sub(r'--[^\n]*', '', text)


def lua_files(folder):
    for here, dirs, files in os.walk(folder):
        dirs[:] = [d for d in dirs if not d.startswith('.')]
        for name in files:
            if name.lower().endswith('.lua'):
                yield os.path.join(here, name)


class Uses:
    """What a body of Lua uses: API paths, events, required module names, native modules."""

    def __init__(self):
        self.api = set()
        self.events = set()
        self.requires = set()
        self.native = set()
        self.defined = set()  # windower.<name>s it sets itself (texts.lua's windower.text.saved_texts)

    def scan(self, text):
        text = strip_comments(text)
        defined = set()
        for m in DEFINE.finditer(text):
            defined.add(m.group(1) or m.group(2))
        found = set()
        for m in USE.finditer(text):
            first, second = m.group(1), m.group(2)
            found.add(f'{first}.{second}' if first in NAMESPACES and second else first)
        for m in ALIAS.finditer(text):
            alias, ns = m.group(1), m.group(2)
            for n in re.finditer(r'(?<![\w.:])%s\.([A-Za-z_]\w*)' % re.escape(alias), text):
                found.add(f'{ns}.{n.group(1)}')
        self.defined |= defined
        self.api |= {p for p in found if p not in defined and not p.startswith('_')}
        for m in EVENT.finditer(text):
            for name in re.findall(r'\'([^\']*)\'|"([^"]*)"', m.group(1)):
                self.events.add((name[0] or name[1]).lower())
        self.requires |= set(REQUIRE.findall(text))
        if LOADLIB.search(text):
            self.native.add('package.loadlib')


class Libraries:
    """Windower's libraries, each read once, with what each needs through its own requires."""

    def __init__(self, folder):
        self.folder = folder
        self.cache = {}
        self.unresolved = defaultdict(set)  # module a library requires that isn't one -> the libraries

    def file_of(self, name):
        if not self.folder:
            return None
        base = os.path.join(self.folder, *re.split(r'[./\\]', name))
        for path in (base + '.lua', os.path.join(base, 'init.lua')):
            if os.path.isfile(path):
                return path
        return None

    def uses(self, name):
        """{ lib name: Uses } for `name` and every library it requires, or {} when it isn't a library."""
        found = {}
        pending = [name]
        while pending:
            lib = pending.pop()
            key = lib.lower().replace('\\', '/').replace('.', '/')
            if key in found:
                continue
            if key not in self.cache:
                path = self.file_of(lib)
                if path is None:
                    self.cache[key] = None
                else:
                    u = Uses()
                    u.scan(read(path))
                    self.cache[key] = u
            if self.cache[key] is None:
                continue
            found[key] = self.cache[key]
            for req in self.cache[key].requires:
                if self.file_of(req) is None and req not in BUILT_IN:
                    self.unresolved[req].add(key)
                pending.append(req)
        return found


class Addon:
    def __init__(self, name, folder):
        self.name = name
        self.folder = folder
        self.own = Uses()
        self.via = {}  # lib -> Uses
        self.dlls = []
        self.unresolved = set()  # what it requires that's neither its own file, a library nor built in


def find_addons(folders):
    addons = {}
    for root in folders:
        if not os.path.isdir(root):
            sys.exit(f'windower_coverage: no folder {root}')
        for entry in sorted(os.listdir(root)):
            folder = os.path.join(root, entry)
            if entry.lower() in ('libs', 'res') or not os.path.isdir(folder):
                continue
            main = [f for f in os.listdir(folder) if f.lower() == entry.lower() + '.lua']
            if main and entry.lower() not in addons:
                addons[entry.lower()] = Addon(entry, folder)
    return addons


def vanacore_api():
    """{ path: 'yes' | 'stub' | 'unsupported' } from windower_dump.lua's run of windower.lua."""
    if not os.path.isfile(LUARUN):
        sys.exit(f'windower_coverage: no {LUARUN}: run tools\\luatest\\run.cmd once (it builds luarun.exe)')
    run = subprocess.run([LUARUN, DUMP_LUA, WINDOWER_LUA], capture_output=True, text=True)
    if run.stderr.strip():
        print(run.stderr.strip(), file=sys.stderr)
    lines = read(WINDOWER_LUA).split('\n')
    api = {}
    for row in run.stdout.splitlines():
        path, kind, first, last = row.split('\t')
        path = path[len('windower.'):]
        if kind != 'function' or first == '0':
            api[path] = 'yes'
            continue
        api[path] = classify(lines[int(first) - 1:int(last)])
    return api


def classify(lines):
    """A function's lines: 'stub' (empty, or returns a constant), 'unsupported' (says so), or 'yes'."""
    text = strip_comments('\n'.join(lines))
    if 'not supported' in text:
        return 'unsupported'
    body = re.sub(r'^.*?function\b[^(]*\([^)]*\)', '', text, count=1, flags=re.S)
    body = re.sub(r'\bend\b[\s,;)}]*$', '', body.strip()).strip()
    constant = r"(nil|true|false|-?\d+|''|\"\"|\{\s*\})"
    if body == '' or re.fullmatch(r'return(\s+%s(\s*,\s*%s)*)?' % (constant, constant), body):
        return 'stub'
    return 'yes'


def raised_events():
    text = read(WINDOWER_LUA)
    m = re.search(r'local RAISED = \{(.*?)\n\}', text, re.S)
    return set(re.findall(r"\['([^']+)'\]", m.group(1))) if m else set()


def definitions(path):
    """(API paths, { class: [fields] }) from Windower's definitions/windower.lua."""
    text = read(path)
    paths = set()
    stack = []  # (indent, name)
    for line in text.split('\n'):
        m = re.match(r'^( *)([A-Za-z_]\w*) = (function|\{)', line)
        if m:
            indent, name, kind = len(m.group(1)), m.group(2), m.group(3)
            while stack and stack[-1][0] >= indent:
                stack.pop()
            if indent == 0:
                stack = [(0, None)]
                continue
            prefix = '.'.join(n for _, n in stack if n)
            full = f'{prefix}.{name}' if prefix else name
            if kind == 'function':
                paths.add(full)
            elif not line.rstrip().endswith('{},') and name in NAMESPACES:
                stack.append((indent, name))
    fields = defaultdict(list)
    current = None
    for line in text.split('\n'):
        m = re.match(r'^\s*---@class\s+([\w.]+)', line)
        if m:
            current = m.group(1)
            continue
        m = re.match(r'^\s*---@field\s+(\w+)', line)
        if m and current:
            fields[current].append(m.group(1))
    for name in ('pol_path', 'ffxi_path', 'windower_path', 'addon_path'):
        paths.add(name)
    return paths, fields


# what each definitions class is the result of
CLASS_OF = {
    'windower.ffxi.player': 'ffxi.get_player', 'windower.ffxi.player.vitals': 'ffxi.get_player().vitals',
    'windower.ffxi.player.skills': 'ffxi.get_player().skills', 'windower.ffxi.player.merits': 'ffxi.get_player().merits',
    'windower.ffxi.info': 'ffxi.get_info', 'windower.windower_settings': 'get_windower_settings',
    'windower.camera': 'get_camera', 'windower.item_display': 'get_item_display',
}


def field_gaps(fields, addons):
    """[(result, field, addons that read .field)] for definitions fields windower.lua never sets."""
    source = strip_comments(read(WINDOWER_LUA))
    corpus = {a.name: strip_comments('\n'.join(read(f) for f in lua_files(a.folder))) for a in addons}
    gaps = []
    for cls, what in CLASS_OF.items():
        for field in fields.get(cls, []):
            if re.search(r'(?<![\w])%s\s*=(?!=)|\[\s*[\'"]%s[\'"]\s*\]\s*=|\.%s\s*=(?!=)' % (field, field, field), source):
                continue
            readers = sorted(n for n, text in corpus.items() if re.search(r'\.%s\b' % field, text))
            gaps.append((what, field, readers))
    return gaps


def names(items, limit=10):
    items = sorted(items, key=str.lower)
    return ', '.join(items[:limit]) + (f', ... (+{len(items) - limit})' if len(items) > limit else '')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('folders', nargs='+')
    parser.add_argument('--libs')
    parser.add_argument('--definitions')
    parser.add_argument('--out')
    args = parser.parse_args()

    libs = Libraries(args.libs or os.path.join(args.folders[0], 'libs'))
    addons = find_addons(args.folders)
    for addon in addons.values():
        own_modules = set()
        for path in lua_files(addon.folder):
            addon.own.scan(read(path))
            rel = os.path.relpath(path, addon.folder)[:-4].replace('\\', '/').lower()
            own_modules.add(rel)
        for req in addon.own.requires:
            key = req.lower().replace('.', '/').replace('\\', '/')
            # an addon's own module: <folder>/<name>.lua, or under its own libs folder (XIPivot's)
            if key in own_modules or 'libs/' + key in own_modules or key + '/init' in own_modules:
                continue
            found = libs.uses(req)
            if found:
                addon.via.update(found)
            elif req not in BUILT_IN:
                addon.unresolved.add(req)
        for lib in addon.via:
            addon.unresolved |= {req for req, by in libs.unresolved.items() if lib in by}
        addon.dlls = sorted(f for f in os.listdir(addon.folder) if f.lower().endswith('.dll'))

    have = vanacore_api()
    raised = raised_events()

    # api path -> { addon: 'direct' | lib }
    users = defaultdict(dict)
    event_users = defaultdict(dict)
    for addon in addons.values():
        # what its libraries set on windower is there for it
        made = set().union(*(u.defined for u in addon.via.values())) if addon.via else set()
        for path in addon.own.api - made:
            users[path][addon.name] = 'direct'
        for event in addon.own.events:
            event_users[event][addon.name] = 'direct'
        for lib, u in addon.via.items():
            for path in u.api - made:
                users[path].setdefault(addon.name, lib)
            for event in u.events:
                event_users[event].setdefault(addon.name, lib)

    def status(path):
        if path in have:
            return have[path]
        # a namespace VanaCore has as a whole (windower.ffxi used as a value)
        if any(p.startswith(path + '.') for p in have):
            return 'yes'
        return 'missing'

    out = []
    w = out.append
    gaps = defaultdict(list)  # addon -> what it lacks
    rows = {'missing': [], 'stub': [], 'unsupported': []}
    for path, who in users.items():
        s = status(path)
        if s in rows:
            rows[s].append((path, who))
            for addon in who:
                gaps[addon].append(f'{s}: windower.{path}')
    event_rows = []
    for event, who in event_users.items():
        if event not in raised:
            event_rows.append((event, who))
            for addon in who:
                gaps[addon].append(f'event: {event}')
    for addon in addons.values():
        for dll in addon.dlls:
            gaps[addon.name].append(f'native: {dll}')
        for n in addon.own.native:
            gaps[addon.name].append(f'native: {n}')
        for req in addon.unresolved:
            gaps[addon.name].append(f'require: {req}')

    clean = [a.name for a in addons.values() if not gaps.get(a.name)]
    w('# Windower API coverage\n')
    w(f'{len(addons)} addons from {", ".join(args.folders)}; libraries from {libs.folder}.\n')
    w(f'**{len(clean)} of {len(addons)} use nothing VanaCore lacks** (by name: what a call returns is not checked).\n')

    def table(title, rows_, what):
        if not rows_:
            return
        w(f'\n## {title}\n')
        w(f'| {what} | addons | which (* only through a library) |')
        w('|---|---|---|')
        for path, who in sorted(rows_, key=lambda r: (-len(r[1]), r[0])):
            listed = [n if how == 'direct' else n + '*' for n, how in who.items()]
            w(f'| `{path}` | {len(who)} | {names(listed)} |')

    table('Missing (the call raises "not supported by VanaCore yet")', [(f'windower.{p}', who) for p, who in rows['missing']], 'API')
    table('Stubs (there, but empty or returning a constant)', [(f'windower.{p}', who) for p, who in rows['stub']], 'API')
    table('Unsupported (there, but says so)', [(f'windower.{p}', who) for p, who in rows['unsupported']], 'API')
    table('Events VanaCore never raises', event_rows, 'event')

    native = [(a.name, a.dlls + sorted(a.own.native)) for a in addons.values() if a.dlls or a.own.native]
    if native:
        w('\n## Native modules (Windower\'s LuaCore ABI: VanaCore can\'t load them)\n')
        for name, what in sorted(native):
            w(f'- **{name}**: {", ".join(what)}')

    unresolved = defaultdict(list)
    for addon in addons.values():
        for req in addon.unresolved:
            unresolved[req].append(addon.name)
    if unresolved:
        w('\n## Requires that find nothing (not the addon\'s own file, a library in the libs folder, or built in)\n')
        w('A name starting with _ is usually one of Windower\'s native modules (a DLL beside Windower.exe).\n')
        w('| module | addons |')
        w('|---|---|')
        for req, who in sorted(unresolved.items(), key=lambda r: (-len(r[1]), r[0])):
            w(f'| `{req}` | {len(who)}: {names(who)} |')

    if args.definitions:
        paths, fields = definitions(args.definitions)
        unused = sorted(p for p in paths if status(p) != 'yes' and p not in users)
        if unused:
            w('\n## In Windower\'s definitions, lacking here, no addon above uses\n')
            w(', '.join(f'`windower.{p}` ({status(p)})' for p in unused))
        fg = field_gaps(fields, addons.values())
        if fg:
            w('\n## Result fields windower.lua never sets (from the definitions)\n')
            w('Addons that read a field of that name anywhere (a rough guide: the name may be another table\'s).\n')
            w('| result | field | addons |')
            w('|---|---|---|')
            for what, field, readers in sorted(fg, key=lambda g: (-len(g[2]), g[0], g[1])):
                w(f'| `{what}` | `{field}` | {len(readers)}: {names(readers, 8)} |')

    w('\n## By addon\n')
    for name in sorted(gaps, key=str.lower):
        if gaps[name]:
            w(f'- **{name}**: {"; ".join(sorted(set(gaps[name])))}')
    w(f'\nNothing lacking: {names(clean, 1000)}')

    text = '\n'.join(out) + '\n'
    if args.out:
        with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        print(f'windower_coverage: {len(clean)}/{len(addons)} addons clean; report in {args.out}')
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
