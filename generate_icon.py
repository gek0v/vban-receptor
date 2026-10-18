import os
import math
from PIL import Image, ImageDraw

def draw_vban_icon(size=512, is_round=False):
    # Create RGBA canvas
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    
    center = size / 2.0
    radius = (size * 0.46) if is_round else (size * 0.45)
    
    # 1. Base shape
    if is_round:
        draw.ellipse(
            [center - radius, center - radius, center + radius, center + radius],
            fill=(12, 18, 30, 255),
            outline=(30, 41, 59, 255),
            width=max(2, int(size * 0.015))
        )
    else:
        # Rounded rectangle
        corner = size * 0.22
        draw.rounded_rectangle(
            [size * 0.05, size * 0.05, size * 0.95, size * 0.95],
            radius=corner,
            fill=(12, 18, 30, 255),
            outline=(30, 41, 59, 255),
            width=max(2, int(size * 0.015))
        )
    
    # Subtle inner dark circular glow
    glow_r = size * 0.38
    draw.ellipse(
        [center - glow_r, center - glow_r, center + glow_r, center + glow_r],
        fill=(17, 24, 39, 255),
        outline=(0, 229, 255, 35),
        width=max(1, int(size * 0.01))
    )

    # 2. Minimalist Audio Waves / Network Broadcast Arcs behind
    # Concentric soundwave arcs at top
    wave_cx = center
    wave_cy = size * 0.36
    for r_factor, alpha in [(0.28, 60), (0.22, 100), (0.16, 160)]:
        r = size * r_factor
        draw.arc(
            [wave_cx - r, wave_cy - r, wave_cx + r, wave_cy + r],
            start=215,
            end=325,
            fill=(0, 229, 255, alpha),
            width=max(2, int(size * 0.018))
        )

    # 3. Stylized Minimalist "V" Audio Equalizer / Waves
    # Central "V" formed by dynamic vertical audio bars
    # Bars arranged symmetrically in a V-notch:
    # 7 bars: x offsets relative to center
    bar_count = 7
    bar_width = size * 0.055
    spacing = size * 0.08
    # Heights from outer to center: tall -> short (forming a V bottom), with soundwave variation
    # Heights: [0.38, 0.30, 0.22, 0.14, 0.22, 0.30, 0.38]
    # Y positions form a V shape:
    bars = [
        (-3, 0.26, 0.58),  # left outer: y_top = 0.26, y_bottom = 0.58
        (-2, 0.32, 0.66),  # left mid-outer
        (-1, 0.40, 0.74),  # left inner
        ( 0, 0.50, 0.82),  # center point of the V
        ( 1, 0.40, 0.74),  # right inner
        ( 2, 0.32, 0.66),  # right mid-outer
        ( 3, 0.26, 0.58),  # right outer
    ]

    for idx, (pos, top_frac, bot_frac) in enumerate(bars):
        bx = center + pos * spacing - (bar_width / 2.0)
        by_top = size * top_frac
        by_bot = size * bot_frac
        b_corner = bar_width / 2.0
        
        # Color gradient transition from Cyan (0, 229, 255) to Neon Emerald (0, 230, 118)
        t = (pos + 3) / 6.0  # 0.0 to 1.0
        r = int(0 * (1 - t) + 0 * t)
        g = int(229 * (1 - t) + 230 * t)
        b = int(255 * (1 - t) + 118 * t)
        bar_color = (r, g, b, 255)
        
        draw.rounded_rectangle(
            [bx, by_top, bx + bar_width, by_bot],
            radius=b_corner,
            fill=bar_color
        )
    
    # Bottom anchor pulse dot (representing receiving node / audio jack point)
    dot_r = size * 0.022
    dot_cy = size * 0.88
    draw.ellipse(
        [center - dot_r, dot_cy - dot_r, center + dot_r, dot_cy + dot_r],
        fill=(0, 230, 118, 255)
    )

    return img

def main():
    sizes = {
        "mipmap-mdpi": 48,
        "mipmap-hdpi": 72,
        "mipmap-xhdpi": 96,
        "mipmap-xxhdpi": 144,
        "mipmap-xxxhdpi": 192
    }
    
    res_dir = os.path.join("app", "src", "main", "res")
    os.makedirs(res_dir, exist_ok=True)
    
    # Generate for all densities
    for folder, px in sizes.items():
        out_folder = os.path.join(res_dir, folder)
        os.makedirs(out_folder, exist_ok=True)
        
        # Standard icon
        sq_img = draw_vban_icon(size=px, is_round=False)
        sq_img.save(os.path.join(out_folder, "ic_launcher.png"), "PNG")
        
        # Round icon
        rd_img = draw_vban_icon(size=px, is_round=True)
        rd_img.save(os.path.join(out_folder, "ic_launcher_round.png"), "PNG")
        print(f"Generated {folder}: {px}x{px}")

    # Generate 512x512 play store / hi-res icon
    hires_sq = draw_vban_icon(size=512, is_round=False)
    hires_sq.save(os.path.join(res_dir, "ic_launcher-web.png"), "PNG")
    print("Generated 512x512 hi-res icon")

if __name__ == "__main__":
    main()
