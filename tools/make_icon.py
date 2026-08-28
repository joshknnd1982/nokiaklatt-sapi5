# -*- coding: utf-8 -*-
"""Write src/config/nk_config.ico - a small speaker-and-waves mark.

Hand-rolled rather than pulled from a package: the installer and the desktop
shortcut need an icon, and adding an image library to the build for one 32x32
bitmap is not worth it.
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'src', 'config', 'nk_config.ico')

# Nokia-ish blue on transparency.
BODY = (0x18, 0x60, 0xB0)
WAVE = (0x4A, 0xA8, 0xF0)


def draw(size):
    """A speaker cone with two sound arcs, as (r, g, b, a) rows."""
    px = [[(0, 0, 0, 0)] * size for _ in range(size)]
    s = size / 32.0

    def put(x, y, colour):
        xi, yi = int(x), int(y)
        if 0 <= xi < size and 0 <= yi < size:
            px[yi][xi] = colour + (255,)

    def rect(x0, y0, x1, y1, colour):
        for y in range(int(y0), int(y1)):
            for x in range(int(x0), int(x1)):
                put(x, y, colour)

    # the speaker's back block
    rect(5 * s, 12 * s, 11 * s, 20 * s, BODY)
    # the cone: widens toward the front
    for i in range(int(8 * s)):
        half = int((2 + i * 0.75) * s)
        y = 16 * s
        rect(11 * s + i, y - half, 11 * s + i + 1, y + half, BODY)

    # two arcs, drawn as vertical dashes at increasing radius
    for radius, thickness in ((7.0, 1.6), (11.0, 1.8)):
        for step in range(int(-9 * s), int(9 * s) + 1):
            y = 16 * s + step
            dy = step / s
            if abs(dy) > radius * 0.8:
                continue
            dx = (radius * radius - dy * dy) ** 0.5
            x = 19 * s + dx * s * 0.55
            for t in range(int(thickness * s) + 1):
                put(x + t, y, WAVE)
    return px


def ico(sizes=(16, 24, 32, 48)):
    images = []
    for size in sizes:
        px = draw(size)
        # A BMP inside an ICO carries a doubled height and a bottom-up AND
        # mask after the colour data.
        header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0,
                             0, 0, 0, 0, 0)
        colour = bytearray()
        for y in range(size - 1, -1, -1):
            for x in range(size):
                r, g, b, a = px[y][x]
                colour += bytes((b, g, r, a))
        stride = ((size + 31) // 32) * 4
        mask = bytearray()
        for y in range(size - 1, -1, -1):
            row = bytearray(stride)
            for x in range(size):
                if px[y][x][3] == 0:
                    row[x // 8] |= 0x80 >> (x % 8)
            mask += row
        images.append((size, header + bytes(colour) + bytes(mask)))

    out = bytearray(struct.pack('<HHH', 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in images:
        out += struct.pack('<BBBBHHII', size if size < 256 else 0,
                           size if size < 256 else 0, 0, 0, 1, 32,
                           len(data), offset)
        offset += len(data)
    for _size, data in images:
        out += data
    return bytes(out)


with open(OUT, 'wb') as f:
    f.write(ico())
print(f'wrote {OUT} ({os.path.getsize(OUT)} bytes)')
