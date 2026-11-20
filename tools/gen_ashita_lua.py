"""Generates the Ashita SDK's interfaces for Lua: compat/ashita.lua's section between "-- >>> SDK" and "-- <<< SDK".

Ashita addons call the SDK's interfaces (AshitaCore:GetChatManager():IsInputOpen(), ...). ashita.lua has hand-written
Lua for the most used; everything else goes, through LuaJIT's FFI, to VanaCore's own C++ objects (the ones Ashita
plugins get, compat/ashita), whose vtables are laid out as the SDK's (abi.hpp). This writes, for each interface, its
methods Lua can call: { vtable slot, name, return type, parameter types, what the result becomes }.

The slots are MSVC's: overloads of a name together, at the first one's place, in reverse order of declaration (checked
against MSVC itself). Interface version 4.16's (the objects Lua gets). Left out: ImGui (vana.imgui is Lua's), methods
with output buffers, callbacks, wide strings, varargs or structures passed by value other than RECT.

Usage:
    python tools/gen_ashita_lua.py [src/core/compat/ashita/abi.hpp] [src/core/lua/compat/ashita.lua]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ABI = os.path.join(ROOT, 'src', 'core', 'compat', 'ashita', 'abi.hpp')
LUA = os.path.join(ROOT, 'src', 'core', 'lua', 'compat', 'ashita.lua')

SKIP_INTERFACES = {'IGuiManager', 'IPluginBase', 'IPolPluginBase'}

# C types as LuaJIT's FFI takes them
SCALARS = {
    'void': 'void', 'bool': 'bool', 'char': 'char', 'float': 'float', 'double': 'double',
    'int8_t': 'int8_t', 'uint8_t': 'uint8_t', 'int16_t': 'int16_t', 'uint16_t': 'uint16_t', 'int32_t': 'int32_t',
    'uint32_t': 'uint32_t', 'int64_t': 'int64_t', 'uint64_t': 'uint64_t', 'int': 'int32_t', 'UINT': 'uint32_t',
    'DWORD': 'uint32_t', 'BOOL': 'int32_t', 'LONG': 'int32_t', 'HRESULT': 'int32_t', 'ATOM': 'uint16_t',
    'LRESULT': 'intptr_t', 'WPARAM': 'uintptr_t', 'LPARAM': 'intptr_t', 'LSTATUS': 'int32_t', 'D3DCOLOR': 'uint32_t',
    'uintptr_t': 'uintptr_t', 'size_t': 'uint32_t',
}
HANDLES = {'HWND', 'HANDLE', 'HMODULE', 'HINSTANCE', 'HMENU', 'HBITMAP', 'HHOOK', 'HKEY', 'LPVOID', 'void*'}
# interface pointers returned: the Lua objects ashita.lua makes for them
# The game's structures ashita.lua declares, which a returned pointer is cast to (their fields read with the FFI)
STRUCT_RESULTS = {'treasureitem_t': 'vana_treasureitem_t*', 'itemcheck_t': 'vana_itemcheck_t*'}
KINDS = {'IFontObject': 'font', 'IPrimitiveObject': 'primitive', 'IItem': 'item', 'IAbility': 'ability', 'ISpell': 'spell',
         'IStatusIcon': 'statusicon'}


def enums(text):
    out = {}
    for m in re.finditer(r'enum class (\w+)\s*:\s*(\w+)', text):
        out[m.group(1)] = SCALARS.get(m.group(2))
    return out


def ffi_type(t, enum_types, interfaces, ret=False):
    """The FFI type for C type `t`, and what a result of it becomes (None: as it is); None if Lua can't take it."""
    t = ' '.join(t.replace('CONST ', 'const ').split())
    t = re.sub(r'\s*\*', '*', t)
    if t in ('const char*', 'LPCSTR'):
        return ('const char*', 'string' if ret else None)
    if t in SCALARS:
        return (SCALARS[t], None)
    if t in HANDLES:
        return ('void*', None)
    if t == 'RECT':
        return ('vana_RECT', 'rect' if ret else None)
    if ret and t in ('Ashita::FFXI::combatskill_t', 'Ashita::FFXI::craftskill_t'):
        return ('vana_skill', t.split('::')[-1][:-2])  # combatskill, craftskill: a uint16_t, through a pointer
    base = t.split('::')[-1]
    if base in enum_types and enum_types[base]:
        return (enum_types[base], None)
    if t.endswith('*'):
        inner = t[:-1].replace('const ', '').strip()
        name = inner.split('::')[-1]
        if not ret:
            # a pointer passed in: only the game's own objects and structures (as light userdata or cdata)
            if name in interfaces or 'Ashita::FFXI::' in inner or name.startswith('IDirect3D'):
                return ('void*', None)
            return None
        if name in KINDS:
            return ('void*', KINDS[name])
        if name in interfaces:
            return ('void*', 'iface:' + name)
        if inner in SCALARS and inner not in ('void', 'char'):
            return (SCALARS[inner] + '*', None)  # an array of the game's: cdata, from 0
        if name in STRUCT_RESULTS:
            return (STRUCT_RESULTS[name], None)
        return ('void*', None)
    return None


def interfaces_of(text, version='v416'):
    """{ interface: [(name, return, [params], const)] } for a version's interfaces (v416, v430), in declaration order."""
    start = text.index('namespace %s {' % version)
    end = text.index('} // namespace %s' % version, start)
    block = text[start:end]
    out = {}
    for m in re.finditer(r'^struct (I\w+) \{\n(.*?)^\};', block, re.S | re.M):
        methods = []
        for line in m.group(2).splitlines():
            v = re.match(r'\s*virtual (.+?) (\w+)\((.*)\)( const)? = 0;', line)
            if not v:
                continue
            params = [p.strip() for p in v.group(3).split(',')] if v.group(3).strip() else []
            types = []
            for p in params:
                if p == '...':
                    types.append('...')
                    continue
                pm = re.match(r'(.*?)\s*\b(a\d+)$', p)
                types.append((pm.group(1) if pm else p).strip())
            methods.append((v.group(2), v.group(1).strip(), types))
        out[m.group(1)] = methods
    return out


def slots(methods):
    """Each method's vtable slot, MSVC's way: a name's overloads together at its first one's place, the last declared
    first."""
    order, seen = [], set()
    for name, _, _ in methods:
        if name not in seen:
            seen.add(name)
            order.append(name)
    by_name = {}
    for i, (name, _, _) in enumerate(methods):
        by_name.setdefault(name, []).append(i)
    slot_of = {}
    slot = 0
    for name in order:
        for i in reversed(by_name[name]):
            slot_of[i] = slot
            slot += 1
    return slot_of


def entries_of(methods, enum_types, names, only=None):
    """The Lua entries of an interface's methods (those named in `only`, if given), and how many were left out."""
    slot_of = slots(methods)
    entries, skipped = [], 0
    for i, (name, ret, params) in enumerate(methods):
        if only is not None and name not in only:
            continue
        r = ffi_type(ret, enum_types, names, ret=True)
        ps = [ffi_type(p, enum_types, names) if p != '...' else None for p in params]
        if r is None or any(p is None for p in ps):
            skipped += 1
            continue
        rtype, returns = r
        ptypes = [p[0] for p in ps]
        if returns in ('rect', 'combatskill', 'craftskill'):
            # MSVC: a structure a member function returns goes through a pointer it's given first
            rtype = rtype + '*' if returns != 'rect' else 'vana_RECT*'
            ptypes = [rtype] + ptypes
        entry = "{ %d, '%s', '%s', '%s'" % (slot_of[i], name, rtype, ', '.join(ptypes))
        if returns:
            entry += ", '%s'" % returns
        entries.append(entry + ' }')
    return entries, skipped


def generate(abi_text):
    enum_types = enums(abi_text)
    interfaces = interfaces_of(abi_text)
    names = set(interfaces) | set(KINDS)
    lines = ['-- >>> SDK (tools/gen_ashita_lua.py: the Ashita SDK\'s interfaces, version 4.16\'s, from abi.hpp; don\'t edit)',
             'local SDK = {']
    skipped = 0
    for iface in sorted(interfaces):
        if iface in SKIP_INTERFACES:
            continue
        entries, left_out = entries_of(interfaces[iface], enum_types, names)
        skipped += left_out
        lines.append('    %s = {' % iface)
        lines.extend('        %s,' % e for e in entries)
        lines.append('    },')
    lines.append('}')
    # The methods only interface 4.30 has, at their place in 4.30's vtables: Ashita addons that see 4.30 call them on
    # the objects 4.30's plugins get
    v430 = interfaces_of(abi_text, 'v430')
    lines.append('-- Version 4.30\'s methods that 4.16\'s interfaces don\'t have, by 4.30\'s vtable slots')
    lines.append('local SDK430 = {')
    for iface in sorted(v430):
        if iface in SKIP_INTERFACES:
            continue
        old = {name for name, _, _ in interfaces.get(iface, [])}
        added = {name for name, _, _ in v430[iface]} - old
        if not added:
            continue
        entries, left_out = entries_of(v430[iface], enum_types, names | set(v430), added)
        skipped += left_out
        if entries:
            lines.append('    %s = {' % iface)
            lines.extend('        %s,' % e for e in entries)
            lines.append('    },')
    lines.append('}')
    lines.append('-- <<< SDK')
    return '\n'.join(lines) + '\n', skipped


def main():
    abi = sys.argv[1] if len(sys.argv) > 1 else ABI
    lua = sys.argv[2] if len(sys.argv) > 2 else LUA
    section, skipped = generate(open(abi, encoding='utf-8').read())
    raw = open(lua, 'rb').read()
    crlf = b'\r\n' in raw
    text = raw.decode('utf-8').replace('\r\n', '\n')
    begin, end = text.find('-- >>> SDK'), text.find('-- <<< SDK')
    if begin < 0 or end < 0:
        sys.exit('no "-- >>> SDK" ... "-- <<< SDK" section in ' + lua)
    end = text.index('\n', end) + 1
    text = text[:begin] + section + text[end:]
    if crlf:
        text = text.replace('\n', '\r\n')
    open(lua, 'wb').write(text.encode('utf-8'))
    print('%s: %d lines of SDK methods (%d left out)' % (lua, section.count('\n'), skipped))


if __name__ == '__main__':
    main()
