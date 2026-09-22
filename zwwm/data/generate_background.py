#!/usr/bin/env python3

import math
import struct
import zlib
from pathlib import Path


WIDTH = 3840
HEIGHT = 2160
ASPECT = WIDTH / HEIGHT
FLOW_WIDTH = 481
FLOW_HEIGHT = 271
FLOW_STEPS = 40
FLOW_TIME = 1.6
# Center (in image coordinates), circulation, and finite vortex core radius.
VORTICES = (
    (complex(0.24 * ASPECT, 0.67), 0.16, 0.085),
    (complex(0.43 * ASPECT, 0.64), -0.19, 0.10),
    (complex(0.61 * ASPECT, 0.47), 0.24, 0.115),
    (complex(0.76 * ASPECT, 0.32), -0.15, 0.075),
    (complex(0.89 * ASPECT, 0.23), 0.11, 0.065),
)


def velocity(z: complex) -> complex:
    # Each regularized vortex has tangential velocity and zero divergence.
    result = complex(0.025, -0.012)
    for center, circulation, core in VORTICES:
        delta = z - center
        radius_squared = delta.real ** 2 + delta.imag ** 2
        if radius_squared > 1e-14:
            strength = -math.expm1(-radius_squared / (core * core))
            result += 1j * delta * (circulation * strength / (math.tau * radius_squared))

    # u = d(psi)/dy, v = -d(psi)/dx for a multiscale stream function.
    for frequency, amplitude, phase in ((3.0, 0.006, 0.7), (7.0, 0.0015, 2.1)):
        a, b = frequency * z.real + phase, frequency * z.imag - phase
        result += complex(amplitude * frequency * math.sin(a) * math.cos(b),
                          -amplitude * frequency * math.cos(a) * math.sin(b))
    return result


def flow_map() -> list[list[complex]]:
    # Backtrace dye coordinates using midpoint Runge-Kutta integration.
    # The field extends beyond the canvas, avoiding clamped edge streaks.
    dt = FLOW_TIME / FLOW_STEPS
    field = []
    for y in range(FLOW_HEIGHT):
        row = []
        for x in range(FLOW_WIDTH):
            z = complex(x / (FLOW_WIDTH - 1) * ASPECT, y / (FLOW_HEIGHT - 1))
            for _ in range(FLOW_STEPS):
                midpoint = z - 0.5 * dt * velocity(z)
                z -= dt * velocity(midpoint)
            row.append(z)
        field.append(row)
    return field


def advected(field: list[list[complex]], u: float, v: float) -> complex:
    x, y = u * (FLOW_WIDTH - 1), v * (FLOW_HEIGHT - 1)
    ix, iy = min(int(x), FLOW_WIDTH - 2), min(int(y), FLOW_HEIGHT - 2)
    tx, ty = x - ix, y - iy
    upper = field[iy][ix] * (1.0 - tx) + field[iy][ix + 1] * tx
    lower = field[iy + 1][ix] * (1.0 - tx) + field[iy + 1][ix + 1] * tx
    return upper * (1.0 - ty) + lower * ty


def chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def clamp(value: float) -> int:
    return max(0, min(255, round(value)))


def blend(color: tuple[float, float, float], tint: tuple[float, float, float],
          amount: float) -> tuple[float, float, float]:
    return tuple(a + (b - a) * amount for a, b in zip(color, tint))


def turbulence(x: float, y: float) -> float:
    value = 0.0
    amplitude = 0.55
    for _ in range(5):
        value += amplitude * math.sin(x + 0.7 * math.cos(y)) * math.cos(y - 0.6 * math.sin(x))
        x, y = 1.64 * x - 1.18 * y + 2.3, 1.18 * x + 1.64 * y - 1.7
        amplitude *= 0.48
    return value


def pixel(x: int, y: int, field_map: list[list[complex]]) -> tuple[int, int, int, int]:
    screen_u = x / (WIDTH - 1)
    screen_v = y / (HEIGHT - 1)
    dye = advected(field_map, screen_u, screen_v)
    u, v = dye.real / ASPECT, dye.imag

    color = (8.0, 12.0, 29.0)
    haze = math.exp(-((u - 0.57) / 0.58) ** 2 - ((v - 0.5) / 0.65) ** 2)
    color = blend(color, (37.0, 29.0, 86.0), haze * 0.8)

    # Rotate a domain-warped field around an off-center logarithmic vortex.
    px, py = (u - 0.63) * WIDTH / HEIGHT, v - 0.48
    radius = math.hypot(px, py)
    twist = 1.15 * math.exp(-2.2 * radius) * math.log1p(1.0 / (radius + 0.12))
    cosine, sine = math.cos(twist), math.sin(twist)
    qx, qy = px * cosine - py * sine, px * sine + py * cosine
    warp_x = turbulence(qx * 3.1 + 1.2, qy * 3.1 - 0.8)
    warp_y = turbulence(qx * 3.1 - 4.7, qy * 3.1 + 2.6)
    field = turbulence(qx * 5.0 + 1.8 * warp_x, qy * 5.0 + 1.8 * warp_y)
    flow = 0.70 - 0.40 * u + 0.15 * math.sin(5.2 * u - 0.8)
    distance = v - flow + 0.20 * field + 0.18 * (qy - py)
    width = 0.13 + 0.07 * math.sin(2.8 * u + 0.4) ** 2
    blue = math.exp(-(distance / width) ** 2)
    color = blend(color, (33.0, 99.0, 179.0), blue * (0.9 - 0.3 * u))

    violet = math.exp(-((distance + 0.19) / 0.19) ** 2 - ((u - 0.67) / 0.42) ** 2)
    color = blend(color, (131.0, 66.0, 171.0), violet * 0.85)

    warm = math.exp(-((u - 0.83) / 0.24) ** 2 - ((distance + 0.12) / 0.12) ** 2)
    color = blend(color, (235.0, 147.0, 138.0), warm * 0.82)

    teal = math.exp(-((u - 0.21) / 0.32) ** 2 - ((distance - 0.04) / 0.09) ** 2)
    color = blend(color, (49.0, 165.0, 184.0), teal * 0.60)

    folds = turbulence(qx * 10.0 + 2.4 * warp_y, qy * 10.0 - 2.4 * warp_x)
    sheen = math.exp(-((distance + 0.015 + 0.045 * folds) / 0.038) ** 2)
    light = 0.5 + 0.5 * math.tanh(2.0 * (warp_x - warp_y))
    color = blend(color, (124.0, 184.0, 219.0), sheen * light * 0.48)
    color = blend(color, (12.0, 16.0, 40.0), max(0.0, -folds) * blue * 0.42)

    shade = math.exp(-((distance - 0.25) / 0.16) ** 2)
    color = blend(color, (6.0, 11.0, 28.0), shade * 0.75)
    vignette = 1.0 - 0.35 * min(1.0, ((screen_u - 0.5) / 0.75) ** 2 + ((screen_v - 0.5) / 0.85) ** 2)
    seed = ((x * 374761393 + y * 668265263) ^ 0x9E3779B9) & 0xFFFFFFFF
    seed = ((seed ^ (seed >> 13)) * 1274126177) & 0xFFFFFFFF
    grain = ((seed & 1023) / 1023.0 - 0.5) * 2.2
    return *(clamp(channel * vignette + grain) for channel in color), 255


def main() -> None:
    field_map = flow_map()
    rows = bytearray()
    for y in range(HEIGHT):
        rows.append(0)
        for x in range(WIDTH):
            rows.extend(pixel(x, y, field_map))

    signature = b"\x89PNG\r\n\x1a\n"
    header = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 6, 0, 0, 0)
    image = signature + chunk(b"IHDR", header)
    image += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    image += chunk(b"IEND", b"")
    Path(__file__).with_name("background.png").write_bytes(image)


if __name__ == "__main__":
    main()
