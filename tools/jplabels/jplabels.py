"""The Japanese menu labels' words for True Font (src/core/jplabels.cpp).

A game in Japanese loads its menus from ROM/91/15.DAT, whose labels ("ステータス", "装備変更") are pictures of whole words on its
own sheets ("menu    menufont", "menu    mn6font", "menu    menu2fon"...), not letters. A label is one or more parts of a
sprite (chunk type 0x31: name, images, sprites, each a count of 61-byte parts) side by side, each drawn twice (its shadow,
taken away: blend A 2, then the letters). True Font draws the label's words in its place, so each label is read once:

    python jplabels.py groups <ROM/91/15.DAT>                  how many labels, and which aren't in labels.tsv yet
    python jplabels.py sheets <ROM/91/15.DAT> <folder>         those as numbered pictures (sheet_NN.png) and to_read.tsv
    python jplabels.py header labels.tsv ../../src/core/jplabels_data.h

Read the pictures, put the words in to_read.tsv's second column and add its rows to labels.tsv, then make the header.
Grouping here is jplabels.cpp's (group): the same pass, the same row, each part no more than kGap units past the last.
"""
import struct
import sys

SHEETS = ['menu    menufont', 'menu    menu2fon', 'menu    menu4fon', 'menu    mn3font ', 'menu    mn5font ',
          'menu    mn6font ', 'menu    mn7font ', 'menu    mn8font ', 'menu    mn9font ', 'menu    mn10font',
          'menu    mn11font', 'menu    mn12font']
GAP = 4


def chunks(data):
    off = 0
    while off + 16 <= len(data):
        packed = struct.unpack_from('<I', data, off + 4)[0]
        size = (packed >> 7) * 16
        if size < 16:
            return
        yield off, packed & 0x7F, size
        off += size


def sprites(data):
    """Each sprite of each sheet chunk: its parts (x0, y0, x1, y1, w, h, u, v, image, shadow)"""
    for off, typ, size in chunks(data):
        if typ != 0x31:
            continue
        p = data[off + 16:off + size]
        q = 17 + 16 * p[16]
        count = struct.unpack_from('<H', p, q)[0]
        q += 2
        for _ in range(count):
            if q >= len(p):
                break
            n = p[q]
            q += 1
            parts = []
            for _ in range(n):
                part = p[q:q + 61]
                q += 61
                v = struct.unpack_from('<12h', part, 0)
                parts.append(dict(x0=v[0], y0=v[1], x1=v[2], y1=v[5], w=v[8], h=v[9], u=v[10], v=v[11],
                                  img=part[45:61].decode('ascii', 'replace'), shadow=part[42] == 2))
            yield parts


def groups(parts):
    """A sprite's labels: its parts on the label sheets, each pass apart, left to right, side by side on one row"""
    out = []
    for shadow in (False, True):
        ps = sorted((p for p in parts if p['img'] in SHEETS and p['shadow'] == shadow), key=lambda p: (p['x0'], p['y0']))
        cur = []
        for p in ps:
            if cur:
                last = cur[-1]
                if not (p['y0'] >= last['y1'] or p['y1'] <= last['y0']) and p['x0'] - last['x1'] <= GAP and p['x0'] >= last['x0']:
                    cur.append(p)
                    continue
                out.append(cur)
            cur = [p]
        if cur:
            out.append(cur)
    return out


def key(group):
    return '|'.join('%s:%d,%d,%d,%d' % (p['img'][8:].strip(), p['u'], p['v'], p['w'], p['h']) for p in group)


def read_tsv(path):
    rows = {}
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or '\t' not in line:
            continue
        k, text = line.rstrip('\n').split('\t', 1)
        rows[k] = text
    return rows


def all_groups(dat):
    data = open(dat, 'rb').read()
    found = {}
    for parts in sprites(data):
        for g in groups(parts):
            if not g[0]['shadow']:
                found.setdefault(key(g), g)
    return data, found


def cmd_groups(dat, tsv='labels.tsv'):
    _, found = all_groups(dat)
    known = read_tsv(tsv)
    new = [k for k in found if k not in known]
    print('%d labels; %d not in %s' % (len(found), len(new), tsv))


def decode_alpha(img):
    """A DXT3 label sheet's alpha (the letters' shape), as rows of bytes"""
    w, h = struct.unpack_from('<ii', img, 21)
    p = 57 + 12  # (the bitmap header, then 'DXT3' reversed, its size and one more)
    out = [bytearray(w) for _ in range(h)]
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            alpha = img[p:p + 8]
            p += 16
            for i in range(16):
                out[by + (i >> 2)][bx + (i & 3)] = ((alpha[i >> 1] >> (4 * (i & 1))) & 15) * 17
    return w, h, out


def cmd_sheets(dat, folder, tsv='labels.tsv'):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.load_default(size=18)
    data, found = all_groups(dat)
    known = read_tsv(tsv)
    todo = [(k, g) for k, g in found.items() if k not in known]
    alpha = {}
    for off, typ, size in chunks(data):
        img = data[off + 16:off + size]
        name = img[1:17].decode('ascii', 'replace')
        if typ == 0x20 and name in SHEETS:
            w, h, rows = decode_alpha(img)
            alpha[name] = Image.frombytes('L', (w, h), bytes(b''.join(rows)))
    with open(folder + '/to_read.tsv', 'w', encoding='utf-8', newline='\n') as rows:
        for i, (k, g) in enumerate(todo):
            rows.write('%s\t\n' % k)
    per = 60
    for page in range(0, len(todo), per):
        sheet = Image.new('L', (900, 54 * 30), 0)
        draw = ImageDraw.Draw(sheet)
        for i, (k, g) in enumerate(todo[page:page + per]):
            x, y = (i // 30) * 450, (i % 30) * 54
            draw.text((x + 4, y + 16), str(page + i), fill=160, font=font)
            x0, y0 = min(p['x0'] for p in g), min(p['y0'] for p in g)
            word = Image.new('L', (max(p['x1'] for p in g) - x0, max(p['y1'] for p in g) - y0), 0)
            for p in g:
                c = alpha[p['img']].crop((p['u'], p['v'], p['u'] + p['w'], p['v'] + p['h']))
                c = c.resize((max(1, p['x1'] - p['x0']), max(1, p['y1'] - p['y0'])))
                word.paste(c, (p['x0'] - x0, p['y0'] - y0), c)
            word = word.resize((word.width * 3, word.height * 3), Image.NEAREST)
            sheet.paste(word, (x + 50, y + 4))
        sheet.save('%s/sheet_%02d.png' % (folder, page // per))
    print('%d labels to read: %s/sheet_*.png, %s/to_read.tsv' % (len(todo), folder, folder))


def cmd_header(tsv, out):
    rows = read_tsv(tsv)
    parts, index, labels = [], {}, []
    for k, text in rows.items():
        if text.startswith('@'):
            continue
        ids = []
        for spec in k.split('|'):
            name, rect = spec.split(':')
            sheet = [s[8:].strip() for s in SHEETS].index(name)
            p = (sheet,) + tuple(int(x) for x in rect.split(','))
            if p not in index:
                index[p] = len(parts)
                parts.append(p)
            ids.append(index[p])
        labels.append((ids, text))
    flat = []
    lines = ['// generated by tools/jplabels/jplabels.py header from tools/jplabels/labels.tsv: do not edit',
             '#pragma once', '', '#include <cstdint>', '',
             'namespace vanacore::jplabels::data {', '',
             '// The label sheets of the menu DAT of a game in Japanese (ROM\\91\\15): a quad\'s texture name, 16 letters',
             'constexpr char kSheets[][17] = {' + ', '.join('"%s"' % s for s in SHEETS) + '};', '',
             '// A part: its sheet (kSheets) and its picture\'s place and size on it, texels',
             'struct Part {', '    std::uint8_t sheet;', '    std::uint16_t u, v, w, h;', '};',
             'constexpr Part kParts[] = {']
    for p in parts:
        lines.append('    {%d, %d, %d, %d, %d},' % p)
    lines += ['};', '',
              '// A label: its parts (kLabelParts from `first`, `count` of them, left to right) and its words, UTF-8',
              'struct Label {', '    std::uint16_t first;', '    std::uint8_t count;', '    const char* text;', '};']
    body = []
    for ids, text in labels:
        escaped = ''.join('\\x%02X' % b for b in text.encode('utf-8'))
        body.append('    {%d, %d, "%s"},' % (len(flat), len(ids), escaped))
        flat += ids
    lines.append('constexpr std::uint16_t kLabelParts[] = {')
    for i in range(0, len(flat), 16):
        lines.append('    ' + ', '.join(str(x) for x in flat[i:i + 16]) + ',')
    lines += ['};', 'constexpr Label kLabels[] = {'] + body + ['};', '', '} // namespace vanacore::jplabels::data', '']
    open(out, 'w', encoding='utf-8', newline='\r\n').write('\n'.join(lines))
    print('%d labels, %d parts -> %s' % (len(labels), len(parts), out))


if __name__ == '__main__':
    {'groups': cmd_groups, 'sheets': cmd_sheets, 'header': cmd_header}[sys.argv[1]](*sys.argv[2:])
