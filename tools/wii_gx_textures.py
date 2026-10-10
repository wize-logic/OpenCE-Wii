"""The maps' bitmaps in GX's formats (tools/wii_convert_maps.py; port/wii/README.md, "Textures").

A bitmap's pixels are an Xbox texture: its levels one after another, each swizzled (Morton order)
or, for a linear bitmap, in rows, DXT's as rows of 4x4 blocks. The Wii's GPU reads its own
formats, in tiles, so each bitmap is rewritten where it is, in the same bytes, as the GX texture
port/wii/src/d3d8_gx.c binds straight from the texture cache:

    Xbox                                GX
    A8R8G8B8, X8R8G8B8                  RGBA8
    R5G6B5                              RGB565
    A1R5G5B5, X1R5G5B5, A4R4G4B4        RGB5A3
    L8, A8, AL8                         I8 (the device keeps which channels are the texel's)
    A8L8                                IA8
    P8 (bump maps' normals)             CI8 (the same indices, in GX's tiles: the device loads
                                        the palette as a TLUT)
    DXT1                                CMPR (the same blocks, in GX's order)
    DXT3, DXT5                          CMPR for the colour (its levels), then I4 for the alpha (its
                                        levels, as many bytes): two textures the device joins

GX's tiles are larger than the Xbox's smallest levels: the levels kept are the most whose GX
bytes fit in the Xbox texture's (gx_levels, which the device computes the same way); a bitmap
whose first level does not fit, a cube map and a 3D texture are left as they are, and the
device decodes them.
"""

from typing import Optional, Tuple

import numpy as np

# Direct3D's formats (D3DFMT_*: xbox_texture_formats.c), swizzled and linear
D3D_FORMATS = {
    "a8r8g8b8": (0x06, 0x12), "x8r8g8b8": (0x07, 0x1E), "r5g6b5": (0x05, 0x11), "a1r5g5b5": (0x02, 0x10),
    "a4r4g4b4": (0x04, 0x1D), "l8": (0x00, 0x13), "a8": (0x19, 0x1F), "al8": (0x01, 0x1B), "a8l8": (0x1A, 0x20),
    "dxt1": (0x0C, None), "dxt3": (0x0E, None), "dxt5": (0x0F, None), "p8": (0x0B, None),
}
# the bitmaps' formats (source/bitmaps/bitmaps.h's order: xbox_texture_cache.c's table)
BITMAP_FORMATS = {0: "a8", 1: "l8", 2: "al8", 3: "a8l8", 6: "r5g6b5", 8: "a1r5g5b5", 9: "a4r4g4b4",
                  10: "x8r8g8b8", 11: "a8r8g8b8", 14: "dxt1", 15: "dxt3", 16: "dxt5", 17: "p8"}
# bytes per texel (per 4x4 block for DXT)
XBOX_BYTES = {"a8r8g8b8": 4, "x8r8g8b8": 4, "r5g6b5": 2, "a1r5g5b5": 2, "a4r4g4b4": 2, "l8": 1, "a8": 1,
              "al8": 1, "a8l8": 2, "dxt1": 8, "dxt3": 16, "dxt5": 16, "p8": 1}
# GX's tiles: width, height, bytes ("dual": a CMPR level and an I4 level)
GX_TILES = {"a8r8g8b8": (4, 4, 64), "x8r8g8b8": (4, 4, 64), "r5g6b5": (4, 4, 32), "a1r5g5b5": (4, 4, 32),
            "a4r4g4b4": (4, 4, 32), "l8": (8, 4, 32), "a8": (8, 4, 32), "al8": (8, 4, 32), "a8l8": (4, 4, 32),
            "dxt1": (8, 8, 32), "dxt3": (8, 8, 64), "dxt5": (8, 8, 64), "p8": (8, 4, 32)}
GX_MAXIMUM_DIMENSION = 1024


def level_dimension(base: int, level: int) -> int:
    return max(base >> level, 1)


def xbox_level_bytes(kind: str, width: int, height: int, level: int, pitch: int = 0) -> int:
    w, h = level_dimension(width, level), level_dimension(height, level)
    if kind.startswith("dxt"):
        return ((w + 3) // 4) * ((h + 3) // 4) * XBOX_BYTES[kind]
    if pitch:
        return pitch * h
    return w * h * XBOX_BYTES[kind]


def gx_level_bytes(kind: str, width: int, height: int, level: int) -> int:
    tile_width, tile_height, tile_bytes = GX_TILES[kind]
    w, h = level_dimension(width, level), level_dimension(height, level)
    return ((w + tile_width - 1) // tile_width) * ((h + tile_height - 1) // tile_height) * tile_bytes


def gx_levels(kind: str, width: int, height: int, levels: int, pitch: int = 0) -> int:
    """how many levels the GX texture keeps: the most whose bytes fit in the Xbox texture's
    (d3d8_gx.c's gx_native_levels is the same), 0 when it is not converted"""
    if width > GX_MAXIMUM_DIMENSION or height > GX_MAXIMUM_DIMENSION:
        return 0
    budget = sum(xbox_level_bytes(kind, width, height, level, pitch) for level in range(levels))
    used = kept = 0
    for level in range(levels):
        used += gx_level_bytes(kind, width, height, level)
        if used > budget:
            break
        kept = level + 1
    return kept


# ---------- the Xbox's layouts

def spread_masks(width: int, height: int) -> Tuple[int, int]:
    """the Morton bits of x and y (xbox_texture_formats.c's swizzle_masks)"""
    mask_x = mask_y = 0
    bit = mask_bit = 1
    while bit < width or bit < height:
        if bit < width:
            mask_x |= mask_bit
            mask_bit <<= 1
        if bit < height:
            mask_y |= mask_bit
            mask_bit <<= 1
        bit <<= 1
    return mask_x, mask_y


def spread(mask: int, count: int) -> np.ndarray:
    values = np.arange(count, dtype=np.int64)
    out = np.zeros(count, dtype=np.int64)
    source_bit = 0
    for bit in range(32):
        if mask >> bit & 1:
            out |= ((values >> source_bit) & 1) << bit
            source_bit += 1
    return out


def texels(data: bytes, kind: str, width: int, height: int, linear_pitch: int) -> np.ndarray:
    """one level's texels as (height, width, bytes), from swizzled or linear rows"""
    size = XBOX_BYTES[kind]
    if linear_pitch:
        rows = np.frombuffer(data, dtype=np.uint8, count=linear_pitch * height).reshape(height, linear_pitch)
        return rows[:, :width * size].reshape(height, width, size)
    flat = np.frombuffer(data, dtype=np.uint8, count=width * height * size).reshape(width * height, size)
    mask_x, mask_y = spread_masks(width, height)
    index = spread(mask_y, height)[:, None] | spread(mask_x, width)[None, :]
    return flat[index]


# ---------- GX's layouts

def tiled(values: np.ndarray, tile_width: int, tile_height: int) -> np.ndarray:
    """(height, width, ...) in GX's tiles, row after row of them, padded out with zeros"""
    height, width = values.shape[:2]
    padded_height = -(-height // tile_height) * tile_height
    padded_width = -(-width // tile_width) * tile_width
    if (padded_height, padded_width) != (height, width):
        grown = np.zeros((padded_height, padded_width) + values.shape[2:], dtype=values.dtype)
        grown[:height, :width] = values
        values = grown
    rest = values.shape[2:]
    blocks = values.reshape((padded_height // tile_height, tile_height, padded_width // tile_width, tile_width) + rest)
    return blocks.transpose((0, 2, 1, 3) + tuple(range(4, 4 + len(rest))))


def rgba8(texel: np.ndarray, opaque: bool) -> bytes:
    """B, G, R, A bytes to RGBA8: each 4x4 tile's A and R pairs, then its G and B"""
    b, g, r, a = (texel[..., index] for index in range(4))
    if opaque:
        a = np.full_like(a, 255)
    tiles = tiled(np.stack([a, r, g, b], axis=-1), 4, 4)
    count = tiles.shape[0] * tiles.shape[1]
    tiles = tiles.reshape(count, 16, 4)
    ar = tiles[:, :, 0:2].reshape(count, 32)
    gb = tiles[:, :, 2:4].reshape(count, 32)
    return np.concatenate([ar, gb], axis=1).tobytes()


def words16(texel: np.ndarray) -> np.ndarray:
    return texel[..., 0].astype(np.uint16) | texel[..., 1].astype(np.uint16) << 8


def rgb5a3(kind: str, texel: np.ndarray) -> np.ndarray:
    value = words16(texel).astype(np.uint32)
    if kind == "a1r5g5b5":
        opaque = value >> 15 == 1
        # (an opaque texel is GX's 1RRRRRGGGGGBBBBB as it is; a clear one 0AAARRRRGGGGBBBB)
        clear = ((value >> 11 & 0xF) << 8) | ((value >> 6 & 0xF) << 4) | (value >> 1 & 0xF)
        return np.where(opaque, value, clear).astype(np.uint16)
    a, r, g, b = value >> 12 & 0xF, value >> 8 & 0xF, value >> 4 & 0xF, value & 0xF
    five = lambda channel: (channel << 1) | (channel >> 3)  # noqa: E731
    opaque = 0x8000 | five(r) << 10 | five(g) << 5 | five(b)
    clear = (a >> 1) << 12 | r << 8 | g << 4 | b
    return np.where(a == 15, opaque, clear).astype(np.uint16)


def big_words(values: np.ndarray, tile_width: int, tile_height: int) -> bytes:
    return tiled(values, tile_width, tile_height).astype(">u2").tobytes()


# ---------- DXT to CMPR

def _pair_reversal() -> np.ndarray:
    table = np.zeros(256, dtype=np.uint8)
    for value in range(256):
        table[value] = ((value & 3) << 6) | ((value >> 2 & 3) << 4) | ((value >> 4 & 3) << 2) | (value >> 6)
    return table


PAIR_REVERSAL = _pair_reversal()


def dxt_blocks(data: bytes, kind: str, width: int, height: int) -> np.ndarray:
    columns, rows = (width + 3) // 4, (height + 3) // 4
    return np.frombuffer(data, dtype=np.uint8, count=columns * rows * XBOX_BYTES[kind]).reshape(
        rows, columns, XBOX_BYTES[kind])


def cmpr(colour: np.ndarray, four_colour: bool) -> bytes:
    """(rows, columns, 8) DXT colour blocks as CMPR: in 8x8 tiles of four blocks, each block's
    colours big-endian and its texels' indices first to last from the high bits. A DXT3 or DXT5
    block is always four colours, which CMPR is only when the first colour is the greater: the
    two are swapped (and the indices with them) where it is not"""
    c0 = colour[..., 0].astype(np.uint16) | colour[..., 1].astype(np.uint16) << 8
    c1 = colour[..., 2].astype(np.uint16) | colour[..., 3].astype(np.uint16) << 8
    indices = colour[..., 4:8].copy()
    if four_colour:
        swap = c0 < c1
        same = c0 == c1
        c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
        indices = np.where(swap[..., None], indices ^ 0x55, indices)
        indices = np.where(same[..., None], 0, indices).astype(np.uint8)
    block = np.zeros(colour.shape[:2] + (8,), dtype=np.uint8)
    block[..., 0], block[..., 1] = c0 >> 8, c0 & 0xFF
    block[..., 2], block[..., 3] = c1 >> 8, c1 & 0xFF
    block[..., 4:8] = PAIR_REVERSAL[indices]
    return tiled(block, 2, 2).tobytes()


def dxt_alpha(blocks: np.ndarray, kind: str) -> np.ndarray:
    """each block's 16 alphas (rows, columns, 16), 0-255"""
    if kind == "dxt3":
        nibbles = np.stack([blocks[..., :8] & 0xF, blocks[..., :8] >> 4], axis=-1).reshape(blocks.shape[:2] + (16,))
        return (nibbles * 17).astype(np.uint8)
    a0 = blocks[..., 0].astype(np.int32)
    a1 = blocks[..., 1].astype(np.int32)
    bits = np.zeros(blocks.shape[:2], dtype=np.uint64)
    for byte in range(6):
        bits |= blocks[..., 2 + byte].astype(np.uint64) << np.uint64(8 * byte)
    codes = np.stack([(bits >> np.uint64(3 * texel)) & np.uint64(7) for texel in range(16)], axis=-1).astype(np.int32)
    palette = np.zeros(blocks.shape[:2] + (8,), dtype=np.int32)
    palette[..., 0], palette[..., 1] = a0, a1
    eight = (a0 > a1)[..., None]
    for step in range(1, 7):
        palette[..., 1 + step] = np.where(eight[..., 0], ((7 - step) * a0 + step * a1) // 7, palette[..., 1 + step])
    for step in range(1, 5):
        palette[..., 1 + step] = np.where(eight[..., 0], palette[..., 1 + step], ((5 - step) * a0 + step * a1) // 5)
    palette[..., 6] = np.where(eight[..., 0], palette[..., 6], 0)
    palette[..., 7] = np.where(eight[..., 0], palette[..., 7], 255)
    return np.take_along_axis(palette, codes, axis=-1).astype(np.uint8)


def i4(alpha_blocks: np.ndarray, width: int, height: int) -> bytes:
    rows, columns = alpha_blocks.shape[:2]
    image = alpha_blocks.reshape(rows, columns, 4, 4).transpose(0, 2, 1, 3).reshape(rows * 4, columns * 4)
    image = image[:max(height, 1), :max(width, 1)]
    tiles = tiled(image >> 4, 8, 8).reshape(-1, 64)
    return (tiles[:, 0::2] << 4 | tiles[:, 1::2]).astype(np.uint8).tobytes()


# ---------- one texture

def convert(data: bytes, kind: str, width: int, height: int, levels: int, pitch: int = 0) -> Optional[bytes]:
    """an Xbox texture's bytes as the GX texture of its gx_levels levels, padded to the same
    size; None when it stays as it is"""
    kept = gx_levels(kind, width, height, levels, pitch)
    if not kept:
        return None
    out = []
    alphas = []
    offset = 0
    for level in range(kept):
        w, h = level_dimension(width, level), level_dimension(height, level)
        size = xbox_level_bytes(kind, width, height, level, pitch)
        source = data[offset:offset + size]
        offset += size
        if kind.startswith("dxt"):
            blocks = dxt_blocks(source, kind, w, h)
            if kind == "dxt1":
                out.append(cmpr(blocks, four_colour=False))
            else:
                out.append(cmpr(blocks[..., 8:16], four_colour=True))
                alphas.append(i4(dxt_alpha(blocks, kind), w, h))
            continue
        texel = texels(source, kind, w, h, pitch)
        if kind in ("a8r8g8b8", "x8r8g8b8"):
            out.append(rgba8(texel, opaque=kind == "x8r8g8b8"))
        elif kind == "r5g6b5":
            out.append(big_words(words16(texel), 4, 4))
        elif kind in ("a1r5g5b5", "a4r4g4b4"):
            out.append(big_words(rgb5a3(kind, texel), 4, 4))
        elif kind in ("l8", "a8", "al8", "p8"):
            out.append(tiled(texel[..., 0], 8, 4).tobytes())
        elif kind == "a8l8":
            # (IA8: the alpha, then the intensity: the Xbox's word, big-endian)
            out.append(big_words(words16(texel), 4, 4))
    converted = b"".join(out) + b"".join(alphas)
    assert len(converted) <= len(data), (kind, width, height, levels, len(converted), len(data))
    return converted + bytes(len(data) - len(converted))
