"""Checks that every pointer VanaCore takes from FFXiMain.dll means the same thing in other builds of the game, offline.

    python tools/sigverify.py REF.dll OTHER.dll [OTHER.dll ...] [--src src] [--xi-tools D:\\xi-tools] [--out build/sigverify]

REF is the build VanaCore was written against (CatsEyeXI's client); each OTHER (retail's) is checked against it. Run it with
xi-tools' python (capstone, pefile, and its POL1 codec to unpack the packed DLLs; tools/sigaudit.py's helpers are used):

    D:\\xi-tools\\.venv\\Scripts\\python.exe tools\\sigverify.py "<CatsEyeXI>\\FFXiMain.dll" "<retail>\\FFXiMain.dll"

Nothing is loaded or run. sigaudit.py says whether each pattern is still FOUND; this says whether what VanaCore then
reads is still the SAME THING, which a pattern that matches doesn't prove (retail's 2026-11 build kept every pattern but
moved the target's pointer, and the target index read garbage). How:

  * Each value is worked out the way VanaCore does (the pattern's match, the dword at match + offset, a fixed distance
    added), in both builds.
  * The value is then recognised by how the game's own code uses it, never by its number (addresses move between builds):
    - a global (in the image, outside .text): every instruction in .text that names it, with the two after it, addresses
      masked: the same global has (nearly) the same set of uses in both builds;
    - code: its first instructions, addresses and large displacements masked, compared in order;
    - anything else (a displacement, a limit, a count): told as it is, old and new.
  * Every pattern in the source (as sigaudit.py collects them) is followed from its match while the code stays the same
    shape in both builds (same instructions at the same offsets): an offset VanaCore reads inside that span lands on the
    same operand in both, and each address operand there is checked as above. Offset constants in the pattern's file
    that land inside the span in REF but not in OTHER are reported (heuristic: a constant isn't tied to one pattern).
  * Structure offsets: the functions the two builds share are aligned (sigaudit.py's way) and every displacement in
    their memory operands compared, which gives how each offset the game uses moved (the character object's +0x8 and
    +0x120 blocks, the item data's +0x800). A hex constant in the source that moved in most of the game's uses of it is
    a FAIL unless its file is in HANDLED with the reason (e.g. sigs::item_data_shift added to it).

A FAIL is something VanaCore would read wrong in that build. The exit code is 1 if there is any.
"""
import argparse
import difflib
import re
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sigaudit  # noqa: E402  (its pattern and image helpers)

BASE = sigaudit.BASE
ROOT = Path(__file__).resolve().parent.parent
SPAN_MAX = 0x400      # bytes followed from a match
BACK_MAX = 0x200      # bytes compared before a match
SAME_GLOBAL = 0.5     # share of a global's uses that must agree
SAME_CODE = 0.75      # similarity of a function's first instructions
HEXNUM = re.compile(r'0x[0-9a-f]+|\b\d+\b')


# ---------- the builds ----------

def load(path, out, xi_tools):
    """The unpacked image of `path` (unpacked into `out`, named by its header's stamp, if it is the packed file)."""
    image = sigaudit.Image(path)
    if not image.packed():
        return image
    sys.path.insert(0, str(Path(xi_tools) / 'src'))
    from xi.dll.pol1_io import unpack_dll
    out.mkdir(parents=True, exist_ok=True)
    target = out / f'FFXiMain_{image.stamp:08x}_unpacked.dll'
    if not target.exists():
        unpack_dll(Path(path), target)
    return sigaudit.Image(target)


class Build:
    def __init__(self, image, label):
        self.img, self.label = image, label
        self.text = image.text
        self.lo = BASE + image.text_va
        self.hi = self.lo + len(image.text)
        self.end = BASE + image.image_size
        self.md = sigaudit.disassembler()
        self._uses = {}

    def functions(self):
        """sigaudit's list of the build's functions (made once: it's most of the run's time)."""
        if not hasattr(self, '_functions'):
            self._functions = sigaudit.functions(self.md, self.img)
        return self._functions

    def kind(self, value):
        if self.lo <= value < self.hi:
            return 'code'
        if BASE <= value < self.end:
            return 'global'
        return 'value'

    def dword(self, va):
        off = va - self.lo
        if 0 <= off <= len(self.text) - 4:
            return struct.unpack_from('<I', self.text, off)[0]
        foff = self.img.file_off(va - BASE)
        return struct.unpack_from('<I', self.img.data, foff)[0] if foff is not None else None

    def ins(self, va, count=1):
        off = va - self.lo
        return list(self.md.disasm(self.text[off:off + 16 * count], va, count=count)) if 0 <= off < len(self.text) else []

    def norm(self, i, me=None):
        """An instruction with addresses masked (`G` for `me`), branch targets and large displacements too."""
        mn = i.mnemonic
        if mn == 'call' or mn.startswith('j') or mn.startswith('loop'):
            return mn + ' T'

        def sub(m):
            v = int(m.group(0), 16)
            if me is not None and v == me:
                return 'G'
            if BASE <= v < BASE + 0x1000000:
                return 'A'
            return 'D' if v >= 0x1000 else m.group(0)
        return f"{mn} {re.sub(r'0x[0-9a-f]+', sub, i.op_str)}"

    def uses(self, g):
        """Counter of how .text uses global `g`: the instruction naming it and the next two, normalised."""
        if g in self._uses:
            return self._uses[g]
        key, uses, pos = struct.pack('<I', g), Counter(), self.text.find(struct.pack('<I', g))
        hexg = f'{g:#x}'
        while pos >= 0:
            start = self.instruction_at(self.lo + pos)
            seq = self.ins(start, 3) if start is not None else []
            if seq and hexg in seq[0].op_str:
                uses[' / '.join(self.norm(x, g) for x in seq)] += 1
            pos = self.text.find(key, pos + 1)
        self._uses[g] = uses
        return uses

    def instruction_at(self, va):
        """The start of the instruction holding byte `va`: x86 decoding falls into step within a few instructions, so
        decode from several places before it and take the start most of them agree on."""
        votes = Counter()
        for lead in range(24, 40):
            for i in self.ins(va - lead, 24):
                if i.address <= va < i.address + i.size:
                    votes[i.address] += 1
                    break
                if i.address > va:
                    break
        return votes.most_common(1)[0][0] if votes else None

    def code_shape(self, va, n=40):
        return [self.norm(i) for i in self.ins(va, n)]


def contents(build, g, n=16):
    off = build.img.file_off(g - BASE)
    return build.img.data[off:off + n] if off is not None else b''


def same_global(ref, a, other, b):
    ca, cb = contents(ref, a), contents(other, b)
    if len(ca) == 16 and ca == cb and sum(x != 0 for x in ca) >= 4:
        return True, 'initialised data, its first 16 bytes the same in both'
    ua, ub = ref.uses(a), other.uses(b)
    na, nb = sum(ua.values()), sum(ub.values())
    if na == 0 and nb == 0:
        return None, 'no instruction names it in either build (reached by arithmetic): not checked'
    share = sum((ua & ub).values()) / max(na, nb)
    return share >= SAME_GLOBAL, f'{na} uses in {ref.label}, {nb} in {other.label}, {share:.0%} alike'


def same_code(ref, a, other, b):
    r = difflib.SequenceMatcher(None, ref.code_shape(a), other.code_shape(b), autojunk=False).ratio()
    return r >= SAME_CODE, f'first instructions {r:.0%} alike'


def judge(ref, a, other, b, data=False, fixed=False):
    """(verdict, evidence) for value `a` in REF and `b` in OTHER: True same, False not, None not checkable. `data`: the
    value is read or written through (a memory operand), so a table inside .text is a global, not code. `fixed`: a fixed
    address, the same number in both builds by definition, so what is there is compared."""
    if a is None or b is None:
        return False, 'not found'
    if a == b and not fixed:
        return True, 'same value'
    ka, kb = ref.kind(a), other.kind(b)
    if ka != kb:
        return False, f'{ka} in {ref.label}, {kb} in {other.label}'
    if ka == 'global' or (data and ka == 'code'):
        return same_global(ref, a, other, b)
    if ka == 'code':
        return same_code(ref, a, other, b)
    return None, f'{a:#x} -> {b:#x} (read from the code)'


# ---------- the source ----------

def literal_after(text, name):
    """The string a `constexpr char NAME[] = "..." "...";` holds, or None."""
    m = re.search(r'\b' + re.escape(name) + r'\s*\[\]\s*=\s*((?:"[^"]*"\s*)+);', text)
    return ''.join(re.findall(r'"([^"]*)"', m.group(1))) if m else None


def number_after(text, name):
    m = re.search(r'\b' + re.escape(name) + r'\s*=\s*(-?0x[0-9A-Fa-f]+|-?\d+)\b', text)
    return int(m.group(1), 0) if m else None


def pattern_bytes(s):
    """A pattern's alternatives as byte lists (None = any), spaced or not."""
    out = []
    for part in s.split('|'):
        t = part.replace(' ', '')
        out.append([None if t[i] == '?' else int(t[i:i + 2], 16) for i in range(0, len(t), 2)])
    return out


def find(build, pattern, nth=0):
    """scan::find's answer: the first alternative found anywhere, its `nth` match (VA), or None."""
    for alt in pattern_bytes(pattern):
        hits = sigaudit.find_all(build.text, alt, limit=nth + 1)
        if len(hits) > nth:
            return build.lo + hits[nth]
    return None


class Spec:
    """One value VanaCore reads: `steps` after the match: ('add', n), ('dword',), ('rva', n) for a fixed address, ('call',) an E8's target."""

    def __init__(self, name, where, pattern, steps, nth=0, note=''):
        self.name, self.where, self.pattern, self.steps, self.nth, self.note = name, where, pattern, steps, nth, note

    def value(self, build):
        v = find(build, self.pattern, self.nth) if self.pattern is not None else None
        for step in self.steps:
            if step[0] == 'rva':
                v = BASE + step[1]
            elif v is None:
                break
            elif step[0] == 'add':
                v += step[1]
            elif step[0] == 'call':  # an E8 rel32 there: its target
                d = build.dword(v + 1)
                v = v + 5 + (d - (1 << 32) if d is not None and d >= 1 << 31 else d) if d is not None and build.ins(v) and build.ins(v)[0].mnemonic == 'call' else None
            else:
                v = build.dword(v)
        return v


def specs(src):
    out = []
    game = (src / 'core/game.cpp').read_text(encoding='utf-8')
    # game.cpp: kXPattern + kXOffset, the dword there (the core data every feature reads)
    for m in re.finditer(r'constexpr char (k\w+)Pattern\[\]', game):
        base = m.group(1)
        pat, off = literal_after(game, base + 'Pattern'), number_after(game, base + 'Offset')
        if pat and off is not None:
            out.append(Spec(f'game {base}', 'core/game.cpp', pat, [('add', off), ('dword',)]))
    tp = literal_after(game, 'kTargetPointerPattern')
    if tp:
        out.append(Spec('game target pointer', 'core/game.cpp', tp,
                        [('add', number_after(game, 'kTargetPointerOffset')), ('dword',)]))
    legacy = literal_after(game, 'kTargetPattern')
    if legacy and number_after(game, 'kTargetPointer') is not None:
        out.append(Spec('game target pointer, fallback (+kTargetPointer)', 'core/game.cpp', legacy,
                        [('add', number_after(game, 'kTargetOffset')), ('dword',), ('add', number_after(game, 'kTargetPointer'))],
                        note='used only when kTargetPointerPattern is not found'))
    # Ashita compat's places(): operand(kX, n), code_dword(found + n), find_in_game(kX[, nth])
    mem = (src / 'core/compat/ashita/memory.cpp').read_text(encoding='utf-8')
    for name, off in re.findall(r'= operand\((k\w+), (\d+)\)', mem):
        out.append(Spec(f'ashita places {name}', 'core/compat/ashita/memory.cpp', literal_after(mem, name), [('add', int(off)), ('dword',)]))
    for name, nth in re.findall(r'= find_in_game\((k\w+)(?:, (\d+))?\)', mem):
        out.append(Spec(f'ashita places {name}', 'core/compat/ashita/memory.cpp', literal_after(mem, name), [], nth=int(nth or 0)))
    inv = literal_after(mem, 'kInventoryPattern')
    for off, what in ((1, 'character'), (8, 'inventory')):
        out.append(Spec(f'ashita places {what}', 'core/compat/ashita/memory.cpp', inv, [('add', off), ('dword',)]))
    end = number_after(mem, 'kSpellRecastEnd')
    out.append(Spec('ashita spell recasts end', 'core/compat/ashita/memory.cpp', literal_after(mem, 'kSpellRecastPattern'),
                    [('add', end), ('dword',)]))
    # Ashita's pointer table (ashita.pointers.ini): match + offset. A positive offset lands on an operand, which plugins
    # read (a global's address); 0 or a negative one is a function's start, which they call.
    core = (src / 'core/compat/ashita/core.cpp').read_text(encoding='utf-8')
    for name, mods, pat, off, cnt in re.findall(r'\{"([\w.]+)", "([^"]+)", "([0-9A-F?]+)", (-?\d+), (\d+)\}', core):
        if 'FFXiMain' not in mods:
            continue
        steps = [('add', int(off)), ('dword',)] if int(off) > 0 else [('add', int(off))]
        out.append(Spec(f'ashita pointer {name}', 'core/compat/ashita/core.cpp', pat, steps, nth=int(cnt)))
    # input.cpp: the gamepad's fixed addresses (no pattern: the same RVA in every build, or the gamepad does nothing)
    for site in (0x580D, 0x587A):
        out.append(Spec(f'input IsXInputDevice, called at +{site:#x}', 'core/input.cpp', None, [('rva', site), ('call',)],
                        note='the call site is fixed; the function is found from it'))
    for rva, what in ((0x55A0, 'set pad GUID'), (0x5490, 'pad mode setter'),
                      (0x5130, 'pad button setter'), (0x5320, 'pad button setter 2'), (0x5600, 'inactive switch code')):
        out.append(Spec(f'input fixed +{rva:#x} {what}', 'core/input.cpp', None, [('rva', rva)],
                        note='fixed address: checked as the same function at the same place'))
    return out


def offset_constants(text):
    """Offsets a file reads from its matches: kXxxAt / kXxxOffset constants and literal operand(x, n) arguments."""
    vals = {int(v, 0) for v in re.findall(r'constexpr[^=;]*\bk\w*(?:At|Offset)\s*=\s*(-?0x[0-9A-Fa-f]+|-?\d+)', text)}
    vals |= {int(v, 0) for v in re.findall(r'operand\(\w+,\s*(0x[0-9A-Fa-f]+|\d+)\)', text)}
    return sorted(v for v in vals if -BACK_MAX <= v < SPAN_MAX)


# ---------- following a match ----------

def shape(i):
    return (i.size, i.mnemonic, HEXNUM.sub('#', i.op_str))


def span(ref, a, other, b):
    """(bytes after the matches with the same instruction shapes, [(ref value, other value)] of their address operands)."""
    ia, ib = ref.ins(a, 200), other.ins(b, 200)
    n, pairs = 0, []
    for x, y in zip(ia, ib):
        if shape(x) != shape(y) or n >= SPAN_MAX:
            break
        mem = ' '.join(re.findall(r'\[[^\]]*\]', x.op_str))
        for p, q in zip(re.findall(r'0x[0-9a-f]+', x.op_str), re.findall(r'0x[0-9a-f]+', y.op_str)):
            data = p in mem
            p, q = int(p, 16), int(q, 16)
            if x.mnemonic not in ('call', 'jmp') and not x.mnemonic.startswith('j') and ref.kind(p) != 'value':
                pairs.append((p, q, data))
        n += x.size
    return n, pairs


def back_span(ref, a, other, b):
    """Bytes before the matches that are equal, or both part of an address."""
    ta, tb = ref.text, other.text
    oa, ob = a - ref.lo, b - other.lo
    k = 0
    while k < BACK_MAX and oa - k - 1 >= 0 and ob - k - 1 >= 0:
        i, j = oa - k - 1, ob - k - 1
        if ta[i] != tb[j] and not any(
                BASE <= struct.unpack_from('<I', ta, i - s)[0] < ref.end and BASE <= struct.unpack_from('<I', tb, j - s)[0] < other.end
                for s in range(4) if i - s >= 0 and j - s >= 0):
            break
        k += 1
    return k


# ---------- structure offsets ----------

# Files whose moved offsets are already handled, and how. A constant in another file that the game's code shows moved is
# a FAIL until it's read from the code or added here with the reason.
HANDLED = {
    'core/itemrarity.cpp': "the item data's fields: every one has sigs::item_data_shift added",
    'core/itemlist.cpp': "the item data's name: sigs::item_data_shift added",
    'core/sigs.cpp': "the icon field item_data_shift reads the shift from",
    'core/compat/windower/abi.hpp': "Windower's own structure (Info), not the game's",
}
MOVED_SHARE = 0.5   # a constant counts as moved when this share of the game's uses of it moved
MOVED_USES = 5      # ... and, below 0x1000, the game uses it at least this often (a small number used less is as likely
                    # something else: a GUID's byte, an opcode)
MEM = re.compile(r'\[([^\]]*)\]')


def displacements(ins_lines):
    out = []
    for line in ins_lines:
        found = []
        for mem in MEM.findall(line):
            if 'esp' in mem or 'ebp' in mem:
                continue
            found += [int(n, 16) for n in re.findall(r'0x[0-9a-f]+', mem) if int(n, 16) < 0x100000]
        out.append(found)
    return out


def displacement_map(ref, other):
    """{offset: Counter(its change)} for every structure offset in the game's code, over the functions the two builds
    share: an unchanged function counts each of its offsets as not moved; a changed one of the same length compares its
    instructions of the same shape."""
    fo, fn = ref.functions(), other.functions()
    sm = difflib.SequenceMatcher(None, [f[2] for f in fo], [f[2] for f in fn], autojunk=False)
    moves = {}
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            for f in fo[i1:i2]:
                for ds in displacements(f[3]):
                    for d in ds:
                        moves.setdefault(d, Counter())[0] += 1
        elif tag == 'replace' and i2 - i1 == j2 - j1:
            for x, y in zip(fo[i1:i2], fn[j1:j2]):
                a, b = x[3], y[3]
                ops = difflib.SequenceMatcher(None, [HEXNUM.sub('#', l) for l in a], [HEXNUM.sub('#', l) for l in b],
                                              autojunk=False).get_opcodes()
                for t, p1, p2, q1, q2 in ops:
                    if t != 'equal':
                        continue
                    for da, db in zip(displacements(a[p1:p2]), displacements(b[q1:q2])):
                        if len(da) == len(db):
                            for u, w in zip(da, db):
                                moves.setdefault(u, Counter())[w - u] += 1
    return moves


def source_constants(src):
    """{value: [file:line]} for each hex number in the C++ outside strings and comments."""
    strings = re.compile(r'"(?:\\.|[^"\\])*"')
    out = {}
    for p in sorted(src.rglob('*')):
        if p.suffix not in ('.cpp', '.h', '.hpp', '.c', '.inc'):
            continue
        rel = p.relative_to(src).as_posix()
        for n, line in enumerate(p.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            code = strings.sub('""', line.split('//')[0])
            for h in re.findall(r'\b0x[0-9A-Fa-f]+\b', code):
                out.setdefault(int(h, 16), []).append(f'{rel}:{n}')
    return out


def struct_offsets(ref, other, src):
    print('\n### Structure offsets written in the source\n')
    moves = displacement_map(ref, other)
    bands = {}
    for d in sorted(k for k in moves if k >= 0x1000):
        delta = max(moves[d], key=lambda k: moves[d][k])
        if delta:
            bands.setdefault(delta, []).append(d)
    moved_at = sum(1 for c in moves.values() if any(k for k in c))
    print(f'{len(moves)} offsets the game\'s code uses, {moved_at} of them moved somewhere. Large offsets that moved, by how '
          f'much (old offsets):\n')
    for delta, ds in sorted(bands.items(), key=lambda kv: kv[1][0]):
        if len(ds) >= 3:
            print(f'- {delta:+#x}: {len(ds)} offsets, {ds[0]:#x} to {ds[-1]:#x}')
    fails, handled = [], []
    for value, where in sorted(source_constants(src).items()):
        c = moves.get(value)
        if value < 0x20 or not c:
            continue
        total, still = sum(c.values()), c.get(0, 0)
        if (value < 0x1000 and total < MOVED_USES) or (total - still) / total < MOVED_SHARE:
            continue
        delta = max((k for k in c if k), key=lambda k: c[k])
        for w in where:
            f = w.rsplit(':', 1)[0]
            if f in HANDLED:
                handled.append(f'`{w}` {value:#x} ({delta:+#x}): {HANDLED[f]}')
            else:
                fails.append(f'`{w}`: {value:#x} moved {delta:+#x} in {total - still} of the game\'s {total} uses')
    print(f'\nHex constants in the source that the game\'s code shows moved (in at least {MOVED_SHARE:.0%} of its uses of '
          f'them, and used at least {MOVED_USES} times): {len(fails)} not handled, {len(handled)} handled.\n')
    for f in fails:
        print(f'- FAIL {f}')
    for h in handled:
        print(f'- ok {h}')
    print('\nNot covered: offsets written in decimal or as a structure\'s layout (Ashita\'s `abi.hpp`), a constant the game '
          'never uses as a displacement, and one that moved in only a few of the game\'s uses of that number.')
    return fails


# ---------- the report ----------

def check(ref, other, src):
    fails, rows = [], []
    print(f'\n## {other.label} against {ref.label}\n')

    print('### Pointers VanaCore works out\n')
    print('| | value | in ' + ref.label + ' | in ' + other.label + ' | evidence |\n|---|---|---|---|---|')
    for s in specs(src):
        a, b = s.value(ref), s.value(other)
        if a is None and b is None:
            continue
        verdict, why = judge(ref, a, other, b, fixed=s.steps == [('rva', s.steps[0][1])] if s.steps else False)
        mark = 'ok' if verdict else ('FAIL' if verdict is False else '?')
        if s.note:
            why += f'; {s.note}'
        if verdict is False and 'fallback' not in s.name:
            fails.append(f'{s.name} ({s.where}): {why}')
        fa = f'{a:#x}' if a is not None else '-'
        fb = f'{b:#x}' if b is not None else '-'
        print(f'| {mark} | {s.name} (`{s.where}`) | {fa} | {fb} | {why} |')

    print('\n### Every pattern in the source, followed from its match\n')
    patterns = sigaudit.source_patterns(str(src))
    texts = {}
    lost, bad_operands, short = [], [], []
    followed = checked = 0
    cache = {}
    for alts, where in patterns.items():
        ra = rb = None
        for alt in alts:
            if ra is None and (h := sigaudit.find_all(ref.text, list(alt), 1)):
                ra = ref.lo + h[0]
            if rb is None and (h := sigaudit.find_all(other.text, list(alt), 1)):
                rb = other.lo + h[0]
        if ra is None:
            continue
        if rb is None:
            lost.append(where)
            continue
        followed += 1
        n, pairs = span(ref, ra, other, rb)
        back = back_span(ref, ra, other, rb)
        for p, q, data in pairs:
            if (p, q) not in cache:
                cache[(p, q)] = judge(ref, p, other, q, data)
            verdict, why = cache[(p, q)]
            checked += 1
            if verdict is False:
                bad_operands.append((where, p, q, why))
        files = {w.rsplit(':', 1)[0] for w in where}
        for f in files:
            text = texts.setdefault(f, (src / f).read_text(encoding='utf-8', errors='replace'))
            for off in offset_constants(text):
                if not (-back <= off < n):
                    ref_ok = (-BACK_MAX <= off < SPAN_MAX)
                    if ref_ok and off < len(alts[0]) + 0x40 and off >= 0:
                        short.append((where, off, n))
    print(f'{followed} patterns followed in both builds, {checked} address operands near them checked; '
          f'{len(lost)} found only in {ref.label}.\n')
    for where in lost:
        fails.append(f'pattern lost: {", ".join(where[:3])}')
        print(f'- FAIL lost in {other.label}: `{", ".join(where[:3])}`')
    seen = set()
    for where, p, q, why in bad_operands:
        if (p, q) in seen:
            continue
        seen.add((p, q))
        fails.append(f'operand {p:#x} -> {q:#x} near {", ".join(where[:2])}: {why}')
        print(f'- FAIL `{", ".join(where[:2])}`: operand {p:#x} in {ref.label} is {q:#x} in {other.label}, and isn\'t the same: {why}')
    # an offset the file uses just past where the code stops being the same: worth a look (heuristic)
    near = {}
    for where, off, n in short:
        near.setdefault(where[0].rsplit(':', 1)[0], set()).add((off, n))
    if near:
        print('\nOffsets a file reads that fall past where its pattern\'s code stays the same in both builds (heuristic: '
              'the constant may belong to another pattern of the file; look at these by hand):\n')
        for f, items in sorted(near.items()):
            print(f'- `{f}`: ' + ', '.join(f'{off:#x} (same for {n:#x} bytes)' for off, n in sorted(items)[:8]))
    fails += [f'structure offset {f}' for f in struct_offsets(ref, other, src)]
    return fails


def self_test(ref, src):
    """The check must tell a pointer from its neighbour: each global VanaCore works out, against itself (same) and
    against the address 4 bytes on (not the same: what the 2026-11 target pointer was). Returns the failures."""
    bad, n = [], 0
    for s in specs(src):
        a = s.value(ref)
        if a is None or ref.kind(a) != 'global' or not sum(ref.uses(a).values()):
            continue
        n += 1
        if judge(ref, a, ref, a)[0] is not True:
            bad.append(f'{s.name}: not the same as itself')
        moved = judge(ref, a, ref, a + 4)
        if moved[0] is not False and not (contents(ref, a) and contents(ref, a) == contents(ref, a + 4)):
            bad.append(f'{s.name}: {a:#x} taken for {a + 4:#x} ({moved[1]})')
    print(f'Self-test: {n} globals, each against itself and its neighbour: {len(bad)} wrong.')
    for b in bad:
        print(f'- {b}')
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('ref', help="the build VanaCore was written against (CatsEyeXI's FFXiMain.dll)")
    ap.add_argument('others', nargs='*', help="other builds (retail's FFXiMain.dll)")
    ap.add_argument('--self-test', action='store_true', help='check the check: each global against itself and its neighbour')
    ap.add_argument('--src', default=str(ROOT / 'src'))
    ap.add_argument('--xi-tools', default=r'D:\xi-tools')
    ap.add_argument('--out', default=str(ROOT / 'build' / 'sigverify'))
    args = ap.parse_args()

    out, src = Path(args.out), Path(args.src)

    def build(path):
        img = load(path, out, args.xi_tools)
        known = {0x694E5965: 'CatsEyeXI 2025-12', 0x6A995428: 'retail 2026-09', 0x6AE818B4: 'retail 2026-11'}
        return Build(img, known.get(img.stamp, f'build {img.stamp:08X}'))

    ref = build(args.ref)
    print('# FFXiMain.dll pointer check\n')
    print(f'- reference: {ref.label} (header {ref.img.stamp:08X}, image {ref.img.image_size:X})')
    if args.self_test:
        return 1 if self_test(ref, src) else 0
    if not args.others:
        ap.error('give one or more builds to check, or --self-test')
    builds = [build(p) for p in args.others]
    for b in builds:
        print(f'- checked: {b.label} (header {b.img.stamp:08X}, image {b.img.image_size:X})')
    fails = []
    for b in builds:
        fails += [f'{b.label}: {f}' for f in check(ref, b, src)]
    print('\n## Result\n')
    if fails:
        print(f'**{len(fails)} FAIL**\n')
        for f in fails:
            print(f'- {f}')
    else:
        print('Everything VanaCore reads is the same in every build checked.')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
