"""
Icon DDS Converter & Deployer
- Converts SVGs to PNGs using ImageMagick (if PNG doesn't exist)
- Converts PNGs to DDS using texconv
- Copies PNG and DDS files to the mod textures folder

Requirements:
    - magick.exe in PATH (from ImageMagick)
    - texconv.exe in PATH (from DirectXTex)

Usage:
    python convert_to_dds.py
"""

import shutil
import subprocess
from pathlib import Path


# Configuration - path relative to script location (skse/<mod>/assets/icons/)
_SCRIPT_DIR = Path(__file__).parent.resolve()
_PROJECT_ROOT = _SCRIPT_DIR.parents[3]  # Up 4 levels to project root
# Matches the folder name in mod-registry.ps1 (PapyrusMod = "VR Dress Up"). It used to say
# "DressVR", which no longer exists - and because the deploy step creates the directory it
# writes to, that spelling failed silently by filling an empty folder nothing ever reads.
MOD_TEXTURES_DIR = _PROJECT_ROOT / "papyrus" / "mods" / "VR Dress Up" / "textures" / "VRDressup"


def convert_svgs_to_pngs(svg_dir: Path, png_dir: Path):
    """Convert SVG files to PNG if the PNG doesn't already exist."""
    svg_dir.mkdir(parents=True, exist_ok=True)
    png_dir.mkdir(parents=True, exist_ok=True)

    svg_files = list(svg_dir.glob("*.svg"))

    if not svg_files:
        print("No SVG files found")
        return 0

    converted = 0
    skipped = 0

    for svg_path in svg_files:
        png_name = svg_path.stem + ".png"
        png_path = png_dir / png_name

        # Skip if PNG already exists
        if png_path.exists():
            skipped += 1
            continue

        try:
            # Use ImageMagick to convert SVG to PNG
            # -background none: Transparent background
            result = subprocess.run(
                [
                    r"C:\Program Files\ImageMagick-7.1.2-Q16\magick.exe",
                    "-background", "none",
                    str(svg_path),
                    str(png_path)
                ],
                capture_output=True,
                text=True
            )

            if result.returncode == 0:
                print(f"  [OK] {svg_path.name} -> {png_name}")
                converted += 1
            else:
                print(f"  [ERR] {svg_path.name} - ImageMagick error: {result.stderr}")

        except FileNotFoundError:
            print("  [ERR] magick not found in PATH!")
            print("    Download from: https://imagemagick.org/script/download.php")
            return converted
        except Exception as e:
            print(f"  [ERR] {svg_path.name} - Error: {e}")

    if skipped > 0:
        print(f"  Skipped {skipped} SVGs (PNGs already exist)")

    return converted


def convert_pngs_to_dds(png_dir: Path, dds_dir: Path):
    """Convert all PNG files in png_dir to DDS files in dds_dir using texconv."""
    dds_dir.mkdir(parents=True, exist_ok=True)
    png_files = list(png_dir.glob("*.png"))

    if not png_files:
        print(f"No PNG files found in {png_dir}")
        return 0

    print(f"Converting {len(png_files)} PNG files to DDS...")
    converted = 0

    for png_path in png_files:
        dds_name = png_path.stem + ".dds"

        try:
            # Use texconv to convert PNG to DDS with BC7 compression (good for UI)
            # -f BC7_UNORM: High quality compression with alpha
            # -y: Overwrite existing files
            # -o: Output directory
            result = subprocess.run(
                [
                    "texconv",
                    "-f", "BC7_UNORM",
                    "-y",
                    "-o", str(dds_dir),
                    str(png_path)
                ],
                capture_output=True,
                text=True
            )

            if result.returncode == 0:
                print(f"  [OK] {png_path.name} -> {dds_name}")
                converted += 1
            else:
                print(f"  [ERR] {png_path.name} - texconv error: {result.stderr}")

        except FileNotFoundError:
            print("  [ERR] texconv not found in PATH!")
            print("    Download from: https://github.com/Microsoft/DirectXTex/releases")
            return converted
        except Exception as e:
            print(f"  [ERR] {png_path.name} - Error: {e}")

    return converted


def copy_to_mod_folder(png_dir: Path, dds_dir: Path, mod_dir: Path):
    """Copy PNG and DDS files to the mod textures folder."""
    mod_dir.mkdir(parents=True, exist_ok=True)

    print(f"\nCopying files to mod folder: {mod_dir}")

    copied = 0

    # Copy PNGs
    for png_path in png_dir.glob("*.png"):
        dest = mod_dir / png_path.name
        shutil.copy2(png_path, dest)
        print(f"  [OK] {png_path.name} (PNG)")
        copied += 1

    # Copy DDSs
    for dds_path in dds_dir.glob("*.dds"):
        dest = mod_dir / dds_path.name
        shutil.copy2(dds_path, dest)
        print(f"  [OK] {dds_path.name} (DDS)")
        copied += 1

    return copied


def main():
    script_dir = Path(__file__).parent.resolve()

    svg_dir = script_dir / "svg"  # SVGs in icons/svg/
    png_dir = script_dir / "pngs" # PNGs in icons/pngs/
    dds_dir = script_dir / "dds"  # DDS in icons/dds/

    print("=" * 60)
    print("Icon DDS Converter & Deployer")
    print("=" * 60)

    # Step 1: Convert SVGs to PNGs (if PNG doesn't exist)
    print("\n[1/3] SVG -> PNG conversion")
    print("-" * 40)
    svg_count = convert_svgs_to_pngs(svg_dir, png_dir)

    # Step 2: Convert PNGs to DDS
    print("\n[2/3] PNG -> DDS conversion")
    print("-" * 40)
    dds_count = convert_pngs_to_dds(png_dir, dds_dir)

    # Step 3: Copy to mod folder
    print("\n[3/3] Deploy to mod folder")
    print("-" * 40)
    copy_count = copy_to_mod_folder(png_dir, dds_dir, MOD_TEXTURES_DIR)

    print("\n" + "=" * 60)
    print(f"Done! Converted {svg_count} SVGs, {dds_count} DDS files, copied {copy_count} total files.")
    print("=" * 60)


if __name__ == "__main__":
    main()
