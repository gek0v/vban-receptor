import os
import math
from PIL import Image, ImageDraw

def draw_vban_icon(size=512, is_round=False):
    # Create RGBA canvas
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    
    center = size / 2.0
    radius = (size * 0.46) if is_round else (size * 0.45)
    
    # 1. Base shape - Audiophile Crimson / Scarlet Red
    # Base red: (220, 38, 38) #DC2626, Border: (185, 28, 28) #B91C1C
    if is_round:
        draw.ellipse(
            [center - radius, center - radius, center + radius, center + radius],
            fill=(220, 38, 38, 255),
            outline=(185, 28, 28, 255),
            width=max(2, int(size * 0.015))
        )
    else:
        # Rounded rectangle
        corner = size * 0.22
        draw.rounded_rectangle(
            [size * 0.05, size * 0.05, size * 0.95, size * 0.95],
            radius=corner,
            fill=(220, 38, 38, 255),
            outline=(185, 28, 28, 255),
            width=max(2, int(size * 0.015))
        )
    
    # Subtle inner vibrant red disc for depth
    glow_r = size * 0.38
    draw.ellipse(
        [center - glow_r, center - glow_r, center + glow_r, center + glow_r],
        fill=(239, 68, 68, 255), # #EF4444
        outline=(255, 255, 255, 45),
        width=max(1, int(size * 0.01))
    )

    # 2. Minimalist Audio Waves / Network Broadcast Arcs in Translucent White
    wave_cx = center
    wave_cy = size * 0.36
    for r_factor, alpha in [(0.28, 70), (0.22, 120), (0.16, 190)]:
        r = size * r_factor
        draw.arc(
            [wave_cx - r, wave_cy - r, wave_cx + r, wave_cy + r],
            start=215,
            end=325,
            fill=(255, 255, 255, alpha),
            width=max(2, int(size * 0.020))
        )

    # 3. Stylized Minimalist "V" Audio Equalizer / Waves in Crisp Pure White
    # 7 bars arranged symmetrically in a V-notch
    bar_width = size * 0.058
    spacing = size * 0.08
    bars = [
        (-3, 0.26, 0.58),  # left outer
        (-2, 0.32, 0.66),  # left mid-outer
        (-1, 0.40, 0.74),  # left inner
        ( 0, 0.50, 0.82),  # center vertex
        ( 1, 0.40, 0.74),  # right inner
        ( 2, 0.32, 0.66),  # right mid-outer
        ( 3, 0.26, 0.58),  # right outer
    ]

    for idx, (pos, top_frac, bot_frac) in enumerate(bars):
        bx = center + pos * spacing - (bar_width / 2.0)
        by_top = size * top_frac
        by_bot = size * bot_frac
        b_corner = bar_width / 2.0
        
        # Solid Pure White (#FFFFFF)
        draw.rounded_rectangle(
            [bx, by_top, bx + bar_width, by_bot],
            radius=b_corner,
            fill=(255, 255, 255, 255)
        )
    
    # Bottom anchor pulse dot (Pure White)
    dot_r = size * 0.024
    dot_cy = size * 0.88
    draw.ellipse(
        [center - dot_r, dot_cy - dot_r, center + dot_r, dot_cy + dot_r],
        fill=(255, 255, 255, 255)
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

    # Generate preview artifact
    artifact_path = r"C:\Users\Geko\.gemini\profiles\b6ae1858-fa4c-4137-99a5-a56383219363\.gemini\antigravity-cli\brain\663ef4fa-8c8c-46bd-b196-d1804f0dc71d\icon_preview.png"
    hires_sq.save(artifact_path, "PNG")
    print(f"Updated preview artifact at: {artifact_path}")

if __name__ == "__main__":
    main()
