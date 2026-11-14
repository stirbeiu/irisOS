#!/usr/bin/env python3
"""
irisOS - Default Per-User Wallpaper & Avatar QOI Generator

Generates lightweight, deterministic QOI assets for Administrator and Guest
profiles under resources/wallpaper/ and resources/avatars/ so login cross-fades
and user profile avatars work offline without extra network fetches.
"""

import math
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, SCRIPT_DIR)

from fetch_wallpaper import encode_qoi_rgb  # noqa: E402


def clamp_u8(v: float) -> int:
    if v <= 0.0:
        return 0
    if v >= 255.0:
        return 255
    return int(v + 0.5)


def generate_mesh_wallpaper(path: str, w: int, h: int, c0, c1, c2, glow_x: float, glow_y: float) -> None:
    buf = bytearray(w * h * 3)
    idx = 0
    for y in range(h):
        ny = y / max(1, h - 1)
        for x in range(w):
            nx = x / max(1, w - 1)
            # Smooth bilinear atmospheric base + soft radial aurora highlight
            t = 0.55 * nx + 0.45 * ny
            dx = nx - glow_x
            dy = ny - glow_y
            glow = math.exp(-(dx * dx * 3.2 + dy * dy * 4.5))
            wave = 0.04 * math.sin(nx * 5.0 + ny * 3.0)

            r = (c0[0] * (1.0 - t) + c1[0] * t) * (1.0 - 0.45 * glow) + c2[0] * (0.45 * glow) + wave * 20.0
            g = (c0[1] * (1.0 - t) + c1[1] * t) * (1.0 - 0.45 * glow) + c2[1] * (0.45 * glow) + wave * 18.0
            b = (c0[2] * (1.0 - t) + c1[2] * t) * (1.0 - 0.45 * glow) + c2[2] * (0.45 * glow) + wave * 28.0

            buf[idx] = clamp_u8(r)
            buf[idx + 1] = clamp_u8(g)
            buf[idx + 2] = clamp_u8(b)
            idx += 3

    qoi = encode_qoi_rgb(w, h, bytes(buf))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(qoi)


def generate_admin_avatar(path: str, size: int = 128) -> None:
    buf = bytearray(size * size * 3)
    cx = (size - 1) * 0.5
    cy = (size - 1) * 0.5
    idx = 0
    for y in range(size):
        for x in range(size):
            nx = (x - cx) / (size * 0.5)
            ny = (y - cy) / (size * 0.5)

            # Deep sapphire-to-sky background
            t = (y / max(1, size - 1)) * 0.7 + (x / max(1, size - 1)) * 0.3
            r = 34.0 + (84.0 - 34.0) * (1.0 - t)
            g = 96.0 + (181.0 - 96.0) * (1.0 - t)
            b = 178.0 + (255.0 - 178.0) * (1.0 - t)

            # Crisp white user head & shoulders silhouette
            head_d = math.hypot(nx, ny + 0.16)
            body_d = math.hypot(nx * 0.75, (ny - 0.62) * 1.15)
            alpha = 0.0
            if head_d < 0.28:
                alpha = min(1.0, (0.28 - head_d) * 28.0)
            if body_d < 0.46 and ny > 0.14:
                b_alpha = min(1.0, (0.46 - body_d) * 28.0)
                if b_alpha > alpha:
                    alpha = b_alpha

            r = r * (1.0 - alpha) + 245.0 * alpha
            g = g * (1.0 - alpha) + 248.0 * alpha
            b = b * (1.0 - alpha) + 255.0 * alpha

            buf[idx] = clamp_u8(r)
            buf[idx + 1] = clamp_u8(g)
            buf[idx + 2] = clamp_u8(b)
            idx += 3

    qoi = encode_qoi_rgb(size, size, bytes(buf))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(qoi)


def main() -> int:
    wp_dir = os.path.join(REPO_ROOT, "resources", "wallpaper")
    av_dir = os.path.join(REPO_ROOT, "resources", "avatars")

    admin_wp = os.path.join(wp_dir, "wallpaper_admin.qoi")
    guest_wp = os.path.join(wp_dir, "wallpaper_guest.qoi")
    admin_av = os.path.join(av_dir, "avatar_admin.qoi")

    if not os.path.exists(admin_wp):
        print(f"==> Generating {admin_wp}...")
        generate_mesh_wallpaper(
            admin_wp, 1280, 800,
            c0=(20, 34, 66), c1=(52, 108, 153), c2=(116, 198, 255),
            glow_x=0.35, glow_y=0.30,
        )
    if not os.path.exists(guest_wp):
        print(f"==> Generating {guest_wp}...")
        generate_mesh_wallpaper(
            guest_wp, 1280, 800,
            c0=(38, 48, 58), c1=(86, 104, 120), c2=(178, 198, 214),
            glow_x=0.65, glow_y=0.38,
        )
    if not os.path.exists(admin_av):
        print(f"==> Generating {admin_av}...")
        generate_admin_avatar(admin_av, 128)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
