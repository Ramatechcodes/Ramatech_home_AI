
from pathlib import Path
from PIL import Image

icons_dir = Path("icons")
icons_dir.mkdir(exist_ok=True)

# Find an existing icon image to use as the source.
candidates = [
    icons_dir / "icon-512.png",
    icons_dir / "icon-192.png",
    icons_dir / "apple-touch-icon.png",
]

source = next((p for p in candidates if p.exists()), None)

if source is None:
    raise SystemExit(
        "No icon image found in the icons folder. "
        "Place your existing logo/icon image there first."
    )

# Pillow detects the actual image format, even if the file
# has a misleading .png extension.
with Image.open(source) as image:
    image = image.convert("RGBA")

    # Make a square image by cropping from the center.
    width, height = image.size
    side = min(width, height)
    left = (width - side) // 2
    top = (height - side) // 2
    image = image.crop((left, top, left + side, top + side))

    outputs = [
        ("icon-192.png", 192),
        ("icon-512.png", 512),
        ("apple-touch-icon.png", 180),
    ]

    for filename, size in outputs:
        resized = image.resize((size, size), Image.Resampling.LANCZOS)
        destination = icons_dir / filename
        temp = icons_dir / f"{filename}.tmp.png"
        resized.save(temp, format="PNG", optimize=True)
        temp.replace(destination)
        print(f"Created real PNG: {destination} ({size}x{size})")

print("Icon conversion completed.")
