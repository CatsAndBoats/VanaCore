"""Makes addons/vanacore/vana/data/ffximain_functions.bin, the table /vana stats' sampler tells FFXiMain.dll's functions by.

Each entry is two little-endian uint32s: a function's start (an RVA, the Ghidra address minus 0x10000000) and its end
(the first address after it; 0 when it isn't known, and it then runs to the next start).

The starts are Ghidra's (the export's function files, which say where each ends) plus the targets of direct calls in the
live client's memory image (16-byte aligned, and not inside a function Ghidra has): Ghidra misses a few thousand that
are called but never found, and without them a sample in one is put in the function before it.

    python tools/ffximain_functions.py <functions folder> <memory image> [output]

The functions folder is a Ghidra export of the unpacked FFXiMain.dll, one file per function; the memory image is
FFXiMain.dll's memory dumped from a running client.
"""
import os
import re
import struct
import sys

OUTPUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'addons', 'vanacore', 'vana', 'data', 'ffximain_functions.bin')
BASE = 0x10000000
TEXT_LOW, TEXT_HIGH = 0x1000, 0x1000 + 0x325BAE  # the image's .text, as RVAs


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    functions = sys.argv[1]
    image = sys.argv[2]
    output = sys.argv[3] if len(sys.argv) > 3 else OUTPUT

    # Ghidra's: start and end of each, from the Info line at the top of its file
    ghidra = {}
    for name in os.listdir(functions):
        m = re.match(r'([0-9a-fA-F]{8})_', name)
        if not m:
            continue
        start = int(m.group(1), 16) - BASE
        if not TEXT_LOW <= start < TEXT_HIGH:
            continue
        end = 0
        with open(os.path.join(functions, name), 'r', encoding='utf-8', errors='replace') as f:
            head = f.read(600)
        e = re.search(r'"body_end":"([0-9a-fA-F]+)"', head)
        if e:
            end = int(e.group(1), 16) - BASE + 1
        ghidra[start] = end

    # the targets of direct calls in the memory image
    with open(image, 'rb') as f:
        data = f.read()
    inside = sorted((s, e) for s, e in ghidra.items() if e)
    starts = sorted(ghidra)

    def in_a_function(address):
        # the Ghidra function that starts at or before it, and whether it reaches it
        lo, hi = 0, len(starts)
        while lo < hi:
            mid = (lo + hi) // 2
            if starts[mid] <= address:
                lo = mid + 1
            else:
                hi = mid
        if lo == 0:
            return False
        s = starts[lo - 1]
        return s != address and ghidra[s] > address

    extra = set()
    for p in range(TEXT_LOW, TEXT_HIGH - 5):
        if data[p] != 0xE8:
            continue
        target = p + 5 + struct.unpack_from('<i', data, p + 1)[0]
        if TEXT_LOW <= target < TEXT_HIGH and target % 16 == 0 and target not in ghidra and not in_a_function(target):
            extra.add(target)

    table = {}
    table.update(ghidra)
    for target in extra:
        table[target] = 0
    entries = sorted(table.items())
    with open(output, 'wb') as f:
        for start, end in entries:
            f.write(struct.pack('<II', start, end))
    print('%d Ghidra functions + %d from call targets = %d, %d bytes: %s' % (len(ghidra), len(extra), len(entries), os.path.getsize(output), os.path.abspath(output)))


main()
