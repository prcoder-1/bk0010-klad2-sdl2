#!/usr/bin/env python3
"""bk_dump_to_ppm.py — конвертация дампа памяти БК-0010 в PNG/PPM.

Дамп — это 64 КБ сырой памяти (как сохраняется через `--dump-memory`
или F12 в klad2). Видеобуфер БК-0010 в цветном текстовом режиме лежит
на смещении 0o40000 (= 16384) и занимает 16 КиБ:
  256 пикселей × 256 строк, 4 цвета × 2 бита на пиксель, LSB-first.

Используется для визуальной верификации порта без UI.

Пример:
    ./klad2 --auto-keys "60:Enter,180:1" --max-steps 800000 \\
            --dump-memory /tmp/game.bin
    tools/bk_dump_to_ppm.py /tmp/game.bin /tmp/game.ppm
    convert /tmp/game.ppm -scale 512x512 /tmp/game.png  # или magick

При установленном Pillow можно сразу в PNG:
    tools/bk_dump_to_ppm.py /tmp/game.bin /tmp/game.png --scale 2
"""

import argparse
import struct
import sys

VIDEO_BASE = 0o40000  # 16384
VIDEO_SIZE = 0x4000   # 16384 байт
SCREEN_W   = 256
SCREEN_H   = 256
ROW_BYTES  = 64       # 256 пикс × 2 бита / 8 бит = 64 байт

# Стандартная палитра БК-0010 (цветной режим):
#   0 — чёрный, 1 — синий, 2 — зелёный, 3 — красный
PALETTE = [(0, 0, 0), (0, 0, 255), (0, 255, 0), (255, 0, 0)]


def decode_video(dump_bytes):
    """bk_mem[64K] → RGB-байты 256×256×3."""
    video = dump_bytes[VIDEO_BASE:VIDEO_BASE + VIDEO_SIZE]
    out = bytearray(SCREEN_W * SCREEN_H * 3)
    pos = 0
    for y in range(SCREEN_H):
        row_off = y * ROW_BYTES
        for xb in range(ROW_BYTES):
            b = video[row_off + xb]
            for p in range(4):  # 4 пикселя × 2 бита, LSB-first
                color_idx = (b >> (p * 2)) & 0x3
                r, g, bl = PALETTE[color_idx]
                out[pos] = r
                out[pos + 1] = g
                out[pos + 2] = bl
                pos += 3
    return bytes(out)


def write_ppm(path, rgb_bytes, w, h):
    header = f"P6\n{w} {h}\n255\n".encode("ascii")
    with open(path, "wb") as f:
        f.write(header)
        f.write(rgb_bytes)


def write_png_via_pillow(path, rgb_bytes, w, h, scale=1):
    try:
        from PIL import Image
    except ImportError:
        print("Pillow не установлен; PNG-вывод недоступен. "
              "Сгенерируйте PPM и сконвертируйте через ImageMagick.",
              file=sys.stderr)
        return False
    img = Image.frombytes("RGB", (w, h), rgb_bytes)
    if scale > 1:
        img = img.resize((w * scale, h * scale), Image.NEAREST)
    img.save(path)
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input", help="дамп памяти БК (64 КиБ)")
    p.add_argument("output", help="путь вывода (.ppm или .png)")
    p.add_argument("--scale", type=int, default=1,
                   help="увеличение PNG в N раз (только PNG-вывод)")
    args = p.parse_args()

    with open(args.input, "rb") as f:
        dump = f.read()
    if len(dump) < VIDEO_BASE + VIDEO_SIZE:
        print(f"input: ожидалось >= {VIDEO_BASE + VIDEO_SIZE} байт, "
              f"прочитано {len(dump)}", file=sys.stderr)
        sys.exit(1)

    rgb = decode_video(dump)

    if args.output.lower().endswith(".png"):
        if not write_png_via_pillow(args.output, rgb, SCREEN_W, SCREEN_H,
                                     args.scale):
            sys.exit(1)
    else:
        write_ppm(args.output, rgb, SCREEN_W, SCREEN_H)

    print(f"Записан {args.output}")


if __name__ == "__main__":
    main()
