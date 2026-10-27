"""Generate app icons for rts-tools (serial) and rts-plot (curves).

Run as tools/make_icons.py; writes resources/images/rts-tools.{png,ico} and resources/images/rts-plot.{png,ico}.
"""
import math
import os

from PIL import Image, ImageDraw

S = 1024  # supersampled canvas, downscaled to 256 for anti-aliasing
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "resources", "images")


def rounded_mask(size, radius, inset=0):
    mask = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(mask)
    draw.rounded_rectangle(
        [inset, inset, size - 1 - inset, size - 1 - inset], radius=radius, fill=255
    )
    return mask


def vertical_gradient(size, top, bottom, radius):
    grad = Image.new("RGB", (1, size))
    for y in range(size):
        t = y / (size - 1)
        grad.putpixel(
            (0, y),
            (
                round(top[0] + (bottom[0] - top[0]) * t),
                round(top[1] + (bottom[1] - top[1]) * t),
                round(top[2] + (bottom[2] - top[2]) * t),
            ),
        )
    grad = grad.resize((size, size))
    icon = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    icon.paste(grad, (0, 0), rounded_mask(size, radius))
    return icon


def draw_tools():
    icon = vertical_gradient(S, (0x25, 0xA8, 0xEC), (0x07, 0x64, 0x9C), 200)
    draw = ImageDraw.Draw(icon)
    white = (255, 255, 255, 255)

    # 上箭头（发送）
    draw.rounded_rectangle([210, 370, 700, 450], radius=40, fill=white)
    draw.polygon([(660, 290), (830, 410), (660, 530)], fill=white)
    # 下箭头（接收）
    draw.rounded_rectangle([324, 574, 814, 654], radius=40, fill=white)
    draw.polygon([(364, 494), (194, 614), (364, 734)], fill=white)
    return icon


def draw_plot():
    """三条完全相同的正弦波，只是颜色不同：配色与 rts-tools 一致（蓝色渐变底）"""
    icon = vertical_gradient(S, (0x25, 0xA8, 0xEC), (0x07, 0x64, 0x9C), 200)
    draw = ImageDraw.Draw(icon)

    # 同一波形复制三条（相位、振幅、线宽都一样），仅换颜色区分不同系列
    colors = [
        (0x9E, 0xD4, 0xF1, 255),
        (0xD5, 0xEC, 0xFA, 255),
        (255, 255, 255, 255),
    ]
    centers = [296, 512, 728]
    left, right, step = 170, 854, 6
    wavelength = 470.0
    amplitude = 86.0
    width = 50
    for center, color in zip(centers, colors):
        points = []
        x = left
        while x <= right:
            y = center + amplitude * math.sin(2.0 * math.pi * (x - left) / wavelength)
            points.append((x, y))
            x += step
        draw.line(points, fill=color, width=width, joint="curve")
    return icon


def save(icon, name):
    os.makedirs(OUT, exist_ok=True)
    small = icon.resize((256, 256), Image.LANCZOS)
    png = os.path.join(OUT, name + ".png")
    small.save(png)
    ico = os.path.join(OUT, name + ".ico")
    small.save(
        ico,
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
    )
    print("wrote", png, "and", ico)


save(draw_tools(), "rts-tools")
save(draw_plot(), "rts-plot")
