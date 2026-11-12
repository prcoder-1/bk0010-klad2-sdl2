#!/usr/bin/env python3
"""frames_to_gif.py — сборка анимированного GIF из кадров `klad2 --record`.

Каждый кадр — 16 КиБ видеоОЗУ БК (файлы DIR/NNNNNN.bin, NNNNNN — номер
кадра SDL, 60 кадров/с). Декодирование и палитра — из bk_dump_to_ppm.py.

Пример:
    ./klad2 --record /tmp/rec --auto-keys "60:Enter,..."
    tools/frames_to_gif.py /tmp/rec docs/screens/gameplay.gif \\
            --from 1200 --to 1800 --step 3 --scale 2

Требует Pillow.
"""

import argparse
import os
import sys

from PIL import Image

from bk_dump_to_ppm import PALETTE, SCREEN_H, SCREEN_W, video_slice

FRAME_RATE = 60  # кадров SDL в секунду (VSYNC)


def decode_indexed(frame_bytes):
    """Кадр → индексы палитры 256×256 (по байту на пиксель)."""
    video = video_slice(frame_bytes)
    out = bytearray(SCREEN_W * SCREEN_H)
    pos = 0
    for b in video:
        out[pos] = b & 3
        out[pos + 1] = (b >> 2) & 3
        out[pos + 2] = (b >> 4) & 3
        out[pos + 3] = (b >> 6) & 3
        pos += 4
    return bytes(out)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("dir", help="каталог с кадрами --record")
    p.add_argument("output", help="путь к GIF")
    p.add_argument("--from", dest="first", type=int, default=0,
                   help="первый кадр (включительно)")
    p.add_argument("--to", dest="last", type=int, default=None,
                   help="последний кадр (включительно)")
    p.add_argument("--step", type=int, default=3,
                   help="брать каждый N-й кадр (3 → 20 кадров/с)")
    p.add_argument("--scale", type=int, default=2,
                   help="увеличение в N раз")
    args = p.parse_args()

    numbers = sorted(int(n[:-4]) for n in os.listdir(args.dir)
                     if n.endswith(".bin") and n[:-4].isdigit())
    numbers = [n for n in numbers
               if n >= args.first and (args.last is None or n <= args.last)]
    numbers = numbers[::args.step]
    if not numbers:
        print("frames_to_gif: в диапазоне нет кадров", file=sys.stderr)
        sys.exit(1)

    palette = [c for rgb in PALETTE for c in rgb]
    size = (SCREEN_W * args.scale, SCREEN_H * args.scale)
    images = []
    for n in numbers:
        with open(os.path.join(args.dir, f"{n:06d}.bin"), "rb") as f:
            img = Image.frombytes("P", (SCREEN_W, SCREEN_H),
                                  decode_indexed(f.read()))
        img.putpalette(palette)
        if args.scale > 1:
            img = img.resize(size, Image.NEAREST)
        images.append(img)

    images[0].save(args.output, save_all=True, append_images=images[1:],
                   duration=round(1000 * args.step / FRAME_RATE),
                   loop=0, optimize=False, disposal=1)
    print(f"Записан {args.output}: {len(images)} кадров, "
          f"{os.path.getsize(args.output) // 1024} КиБ")


if __name__ == "__main__":
    main()
