#!/usr/bin/env python3
"""
Resize image to 512x512 square with transparency padding.
- Resizes so larger side is 512px
- Pads smaller side with transparency to make it square
Outputs PNG for texconv compatibility.
"""

import sys
from pathlib import Path
from PIL import Image


def resize_to_512_square(input_path: str, output_path: str = None) -> str:
    """
    Resize image to 512x512 square.
    - Scale so larger dimension fits 512px
    - Pad smaller dimension with transparency to center the image
    Returns the output path.
    """
    input_path = Path(input_path)

    if output_path is None:
        output_path = input_path.parent / f"{input_path.stem}_512.png"
    else:
        output_path = Path(output_path)

    with Image.open(input_path) as img:
        # Convert to RGBA to ensure alpha channel for UI elements
        if img.mode != 'RGBA':
            img = img.convert('RGBA')

        width, height = img.size

        # Scale so the larger side fits 512px
        if width >= height:
            # Width is larger, scale to 512 width
            new_width = 512
            new_height = int(height * (512 / width))
        else:
            # Height is larger, scale to 512 height
            new_height = 512
            new_width = int(width * (512 / height))

        # Use LANCZOS for high quality scaling
        resized = img.resize((new_width, new_height), Image.LANCZOS)

        # Create 512x512 transparent canvas
        canvas = Image.new('RGBA', (512, 512), (0, 0, 0, 0))

        # Center the resized image on the canvas
        paste_x = (512 - new_width) // 2
        paste_y = (512 - new_height) // 2
        canvas.paste(resized, (paste_x, paste_y))

        # Save as PNG
        canvas.save(output_path, 'PNG')

        print(f"Resized {input_path.name}: {width}x{height} -> {new_width}x{new_height}")
        if new_width != 512 or new_height != 512:
            print(f"Padded to 512x512 (centered)")
        print(f"Saved to: {output_path}")

    return str(output_path)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: resize_image.py <input_path> [output_path]")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None

    resize_to_512_square(input_file, output_file)
