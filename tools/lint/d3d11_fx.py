#!/usr/bin/env python3
"""Direct3D 11's scene effects still build: gfx_d3d12.c's effect HLSL, rewritten for Direct3D 11 as gfx_d3d11.c's
fx_source does, compiles for every pass fx_build and up_build ask for.

Direct3D 11 has no effect shaders of its own: fx_source takes Direct3D 12's (gfx_fx_hlsl), swaps its heap tables and the
helpers that index them for Direct3D 11's registers, and gives up when what it finds isn't in the form it was fitted to.
Giving up leaves every scene effect off on Direct3D 11 (shadows, occlusion, bloom, rays, the grade), with one line in
the log. That shipped in 0.8.142 to 0.8.152 unseen: two shadow debug views read Direct3D 12's tables in a way fx_source
had no rewrite for. A change to Direct3D 12's effects that does this again now fails here.

The rewrite below mirrors fx_source; its markers, helpers and leftover tests are read from gfx_d3d11.c itself, the
entry points from fx_build's and up_build's lists, and the HLSL's parts from gfx_d3d12.c's fx_hlsl. Change fx_source's
steps and change this with it. Compiling is d3dcompiler_47.dll's D3DCompile (CPU only, no graphics card), as
gfx_d3d11.c's compile() calls it.

  python tools/lint/d3d11_fx.py               the working tree
  python tools/lint/d3d11_fx.py --rev HEAD~3  a commit's sources
lint.py runs it as its d3d11fx check.
"""

from __future__ import annotations

import argparse
import ctypes
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # the repository
D3D11 = "src/gfx/gfx_d3d11.c"
D3D12 = "src/gfx/gfx_d3d12.c"
D3DCOMPILE_OPTIMIZATION_LEVEL3 = 1 << 15  # compile()'s flags
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0"}


class Unfit(Exception):
    """The sources aren't in a form this check can read (it, or they, need fixing)."""


IDENT = re.compile(r"[A-Za-z_]\w*")


def literals(src: str, i: int, name: str, to_semicolon: bool, seen: tuple[str, ...] = ()) -> str:
    """Adjacent string literals from src[i] joined, comments between them skipped and a macro of literals alone spliced in
    (macro_literal): up to the `;` (to_semicolon) or the end of src (a macro's body)."""
    out = []
    while i < len(src):
        if src.startswith("/*", i):
            i = src.index("*/", i) + 2
        elif src.startswith("//", i):
            i = src.index("\n", i)
        elif src[i] == '"':
            i += 1
            while src[i] != '"':
                if src[i] == "\\":
                    if src[i + 1] not in ESCAPES:
                        raise Unfit(f"{name}: escape \\{src[i + 1]} not read here")
                    out.append(ESCAPES[src[i + 1]])
                    i += 2
                else:
                    out.append(src[i])
                    i += 1
            i += 1
        elif src[i] == ";" and to_semicolon:
            return "".join(out)
        elif src[i].isspace():
            i += 1
        elif (m := IDENT.match(src, i)) and m.group(0) not in seen:
            out.append(macro_literal(m.group(0), seen + (name,)))
            i = m.end()
        else:
            raise Unfit(f"{name}: {src[i:i + 40]!r} between its literals (a #if?)")
    if to_semicolon:
        raise Unfit(f"{name}: no end")
    return "".join(out)


def macro_literal(name: str, seen: tuple[str, ...] = ()) -> str:
    """The string a `#define NAME` of string literals alone holds (its lines joined by their backslashes), found in
    src/gfx's sources: gfx_hlsl.h's GRASS_RINGS_HLSL is spliced into FX_HLSL_2 so."""
    define = re.compile(r"^#define " + re.escape(name) + r"\b[ \t]*((?:.*\\\r?\n)*.*)$", re.M)
    for path in sorted((ROOT / "src" / "gfx").glob("*.[ch]")):
        m = define.search(path.read_text(encoding="utf-8", errors="replace"))
        if m:
            body = re.sub(r"\\\r?\n", " ", m.group(1))
            return literals(body, 0, name, False, seen)
    raise Unfit(f"{name}: between the literals, and no #define of string literals for it in src/gfx")


def literal(src: str, name: str, start: int = 0) -> str:
    """The string a `static const char NAME[] =` holds: its adjacent literals joined, comments between them skipped."""
    m = re.compile(r"static const char " + re.escape(name) + r"\[\] =").search(src, start)
    if not m:
        raise Unfit(f"no `static const char {name}[]`")
    return literals(src, m.end(), name, True)


def function(src: str, signature: str) -> str:
    """A C function's body, from its signature (the definition, not a declaration) to the closing brace at the start of
    a line."""
    m = re.compile(re.escape(signature) + r"\s*\{").search(src)
    if not m:
        raise Unfit(f"no `{signature}` defined")
    end = src.find("\n}\n", m.start())
    return src[m.start():end + 3]


def fx_hlsl(d12: str) -> str:
    """gfx_fx_hlsl: fx_hlsl's parts (FX_HLSL_1, FX_HLSL_2, ...) without FX_PREFIX, as Direct3D 11 is given it."""
    m = re.search(r"static char whole\[([^\]]*)\]", d12)
    parts = re.findall(r"sizeof (FX_HLSL_\d+)", m.group(1)) if m else []
    if not parts:
        raise Unfit("fx_hlsl's parts not found (`static char whole[... sizeof FX_HLSL_n ...]`)")
    return "".join(literal(d12, p) for p in parts)


def fx_source(hlsl: str, d11: str) -> tuple[str | None, list[str]]:
    """fx_source's rewrite of hlsl: (the source, []) or (None, why it gives up)."""
    fn = function(d11, "static char* fx_source(void)")
    k = {n: literal(fn, n) for n in ("BIND_AT", "BIND_END", "HELP_AT", "HELP_END", "NDET", "NDET11", "CMP", "BIND", "HELP")}
    every_cmp = "(c = strstr(out, CMP))" in fn  # the comparisons after the first rewritten too (else a second one refuses)
    b0 = hlsl.find(k["BIND_AT"])
    b1 = hlsl.find(k["BIND_END"], b0) if b0 >= 0 else -1
    h0 = hlsl.find(k["HELP_AT"], b1) if b1 >= 0 else -1
    h1 = hlsl.find(k["HELP_END"], h0) if h0 >= 0 else -1
    n0 = hlsl.find(k["NDET"], h1) if h1 >= 0 else -1
    c0 = hlsl.find(k["CMP"], n0) if n0 >= 0 else -1
    for name, at in (("BIND_AT", b0), ("BIND_END", b1), ("HELP_AT", h0), ("HELP_END", h1), ("NDET", n0), ("CMP", c0)):
        if at < 0:
            return None, [f"fx_source's {name} ({k[name].strip()[:60]!r}) isn't in the HLSL where it looks for it"]
    why = []
    if hlsl.find(k["NDET"], n0 + 1) >= 0:
        why.append(f"a second normal-detail read ({k['NDET']}): fx_source takes one")
    if not every_cmp and hlsl.find(k["CMP"], c0 + 1) >= 0:
        why.append(f"a second shadow comparison ({k['CMP'].strip()}): fx_source takes one")
    if why:
        return None, why
    out = (hlsl[:b0] + k["BIND"] + hlsl[b1 + len(k["BIND_END"]):h0] + k["HELP"] + hlsl[h1 + len(k["HELP_END"]):n0] + k["NDET11"]
           + hlsl[n0 + len(k["NDET"]):c0] + "cmp_tap(km, " + hlsl[c0 + len(k["CMP"]):])
    if every_cmp:
        out = out.replace(k["CMP"], "cmp_tap(km, ")
    # "nothing of Direct3D 12's tables left": the same strstr tests
    tail = fn[fn.find("nothing of Direct3D 12's tables left"):]
    for token in re.findall(r'strstr\(out, "([^"]+)"\)', tail):
        for m in re.finditer(re.escape(token), out):
            line = out.count("\n", 0, m.start()) + 1
            why.append(f"Direct3D 12's {token!r} left at HLSL line {line} (no Direct3D 11 helper for it): "
                       f"{out.splitlines()[line - 1].strip()[:110]}")
    return (None, why) if why else (out, [])


def entries(d11: str) -> list[tuple[str, str]]:
    """(entry, target) for each function fx_build and up_build compile, as they list them."""
    m = re.search(r"FX_ENTRY\[FX_NPS\] = \{([^}]*)\}", d11)
    if not m:
        raise Unfit("FX_ENTRY not found")
    out = [("fx_vs", "vs_5_0")] + [(e, "ps_5_0") for e in re.findall(r'"(\w+)"', m.group(1)) if e != "fx_mip"]
    up = function(d11, "static void up_build(const char* src)")
    for lst in re.findall(r"static const char\* const \w+\[\d+\] = \{([^}]*)\}", up):
        out += [(e, "ps_5_0") for e in re.findall(r'"(\w+)"', lst)]
    return out


def compile_all(src: str, wanted: list[tuple[str, str]]) -> list[str]:
    """D3DCompile each entry as gfx_d3d11.c's compile() does: what failed, with the compiler's errors."""
    d3dc = ctypes.WinDLL("d3dcompiler_47.dll")
    D3DCompile = d3dc.D3DCompile
    D3DCompile.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
                           ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p),
                           ctypes.POINTER(ctypes.c_void_p)]
    D3DCompile.restype = ctypes.c_long
    # ID3DBlob: IUnknown's three, then GetBufferPointer, GetBufferSize
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)

    def blob(b: ctypes.c_void_p) -> tuple[str, ctypes.Array]:
        vt = ctypes.cast(b, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        text = ctypes.string_at(get_ptr(vt[3])(b), get_size(vt[4])(b)).decode("utf-8", "replace")
        release(vt[2])(b)
        return text, vt

    data = src.encode("utf-8")
    failed = []
    for entry, target in wanted:
        code, err = ctypes.c_void_p(), ctypes.c_void_p()
        hr = D3DCompile(data, len(data), b"hlsl", None, None, entry.encode(), target.encode(), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        ctypes.byref(code), ctypes.byref(err))
        said = blob(err)[0] if err.value else ""
        if code.value:
            blob(code)
        if hr < 0:
            errors = [ln.strip() for ln in said.splitlines() if "error" in ln][:4]
            failed.append(f"{entry} ({target}) doesn't compile: " + ("; ".join(errors) or f"HRESULT 0x{hr & 0xFFFFFFFF:08X}"))
    return failed


def read(path: str, rev: str | None) -> str:
    if rev is None:
        return (ROOT / path).read_text(encoding="utf-8")
    proc = subprocess.run(["git", "show", f"{rev}:{path}"], cwd=ROOT, capture_output=True, check=True)
    return proc.stdout.decode("utf-8")


def check(rev: str | None = None) -> tuple[str, list[str]]:
    """(a one-line summary, the problems: none when Direct3D 11's effects build)."""
    d11, d12 = read(D3D11, rev), read(D3D12, rev)
    try:
        src, why = fx_source(fx_hlsl(d12), d11)
        if src is None:
            return "fx_source gives up: Direct3D 11 would have no scene effects at all", why
        wanted = entries(d11)
    except Unfit as e:
        return "can't read the sources", [str(e)]
    failed = compile_all(src, wanted)
    if failed:
        return f"{len(failed)} of {len(wanted)} functions don't compile (one of fx_build's turns every effect off)", failed
    return f"all {len(wanted)} functions compile", []


def main() -> int:
    ap = argparse.ArgumentParser(description="Direct3D 11's scene effects still build (see the top of this file).")
    ap.add_argument("--rev", help="a git revision's sources instead of the working tree")
    args = ap.parse_args()
    summary, problems = check(args.rev)
    print(f"d3d11fx: {summary}")
    for p in problems:
        print(f"   {p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
