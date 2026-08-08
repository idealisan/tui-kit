"""
pnglib - minimal dependency-free PNG decoder + pixel analysis helpers.

Pure stdlib (zlib + struct); no PIL. Used by run_tests.py to assert
pixel-level properties of termrenderer output without needing a display.

A "foreground" pixel is anything that differs from the image's top-left
corner pixel (the terminal's default background), within a tolerance. This
keeps the checks robust to whatever default background colour the PTY uses.
"""

import struct
import zlib


def load_png(path):
    """Return (width, height, channel_count, pixels) where pixels is a
    bytes object of RGB(A) samples, row-major, 4 bytes per pixel."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file: %s" % path)
    pos = 8
    width = height = ct = None
    idat = bytearray()
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            width, height, _bd, ct = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = 4 if ct == 6 else 3
    stride = width * ch
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(height):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 0:
                v = line[i]
            elif f == 1:
                v = line[i] + a
            elif f == 2:
                v = line[i] + b
            elif f == 3:
                v = line[i] + ((a + b) >> 1)
            elif f == 4:
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                v = line[i] + pr
            else:
                raise ValueError("bad filter type %d" % f)
            line[i] = v & 0xFF
        out += line
        prev = line
    return width, height, ch, bytes(out)


def background(px, w, ch):
    """Sample the default background colour from the top-left corner."""
    return (px[0], px[1], px[2])


def is_fg(px, off, ch, bg, tol=40):
    r, g, b = px[off], px[off + 1], px[off + 2]
    return (abs(r - bg[0]) > tol or abs(g - bg[1]) > tol or abs(b - bg[2]) > tol)


def fg_count(px, w, h, ch, bg, tol=40):
    n = 0
    for y in range(h):
        for x in range(w):
            if is_fg(px, (y * w + x) * ch, ch, bg, tol):
                n += 1
    return n


def fg_in_rect(px, w, h, ch, bg, x0, x1, y0=None, y1=None, tol=40):
    """Count foreground pixels whose x is in [x0, x1) and y in [y0, y1)."""
    if y0 is None:
        y0 = 0
    if y1 is None:
        y1 = h
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if is_fg(px, (y * w + x) * ch, ch, bg, tol):
                n += 1
    return n


def horizontal_borders(px, w, h, ch, bg, minlen=35, tol=40):
    """Return [(y, runlen), ...] for every row whose longest contiguous
    foreground run is at least `minlen` pixels."""
    out = []
    for y in range(h):
        run = 0
        best = 0
        for x in range(w):
            if is_fg(px, (y * w + x) * ch, ch, bg, tol):
                run += 1
            else:
                if run > best:
                    best = run
                run = 0
        if run > best:
            best = run
        if best >= minlen:
            out.append((y, best))
    return out


def vertical_lines(px, w, h, ch, bg, minpx=70, minseg=3, tol=40):
    """Return [(x, n_segments, total_px), ...] for every column whose total
    foreground height is at least `minpx`. n_segments > 1 means the line is
    broken (e.g. a dashed box character), which is what we guard against for
    solid borders and intentionally expect for U+2506."""
    out = []
    for x in range(w):
        run = 0
        segs = []
        tot = 0
        for y in range(h):
            if is_fg(px, (y * w + x) * ch, ch, bg, tol):
                run += 1
            else:
                if run >= minseg:
                    segs.append(run)
                    tot += run
                run = 0
        if run >= minseg:
            segs.append(run)
            tot += run
        if tot >= minpx:
            out.append((x, len(segs), tot))
    return out


def chromatic_pixels(px, w, h, ch, spread=60):
    """Count pixels whose max-min channel difference exceeds `spread`.
    Non-zero means a colour (BGRA) glyph such as Noto Color Emoji was drawn
    in its own colours rather than tinted with the foreground colour."""
    n = 0
    for y in range(h):
        for x in range(w):
            off = (y * w + x) * ch
            r, g, b = px[off], px[off + 1], px[off + 2]
            if max(r, g, b) - min(r, g, b) > spread:
                n += 1
    return n
