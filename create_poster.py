#!/usr/bin/env python3
"""
Pokemon Poster Generator - 60x90cm Optimized Layout
Creates a poster with all 151 original Pokemon arranged in optimized grid:
- Row 1: 6 Pokemon + Pokemon Logo (7 positions)
- Rows 2-11: 10 Pokemon each (100 total)  
- Rows 12-16: 9 Pokemon each (45 total)
Total: 151 Pokemon perfectly arranged
"""

import json
import os
from PIL import Image, ImageDraw, ImageFont
import math


def load_pokemon_data(pokemon_id):
    """Load Pokemon data from JSON file"""
    json_path = f"pokemon/{pokemon_id}.json"
    if os.path.exists(json_path):
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            return data.get("name", f"Pokemon #{pokemon_id}")
    return f"Pokemon #{pokemon_id}"


def paste_pokeball_logo(poster, x, y, width, height):
    """Loads the Pokeball logo, makes its background transparent, and pastes it."""
    try:
        logo_img = Image.open("assets/logo.png")

        # --- New background removal logic ---
        logo_img = logo_img.convert("RGBA")
        datas = logo_img.getdata()
        newData = []
        for item in datas:
            # If pixel is close to white, make it transparent
            if item[0] >= 245 and item[1] >= 245 and item[2] >= 245:
                newData.append((255, 255, 255, 0))
            else:
                newData.append(item)
        logo_img.putdata(newData)
        # --- End of new logic ---

        # Resize to fit the cell, preserving aspect ratio
        logo_img.thumbnail((width, height), Image.Resampling.LANCZOS)

        # Calculate position to center it
        img_x = x + (width - logo_img.width) // 2
        img_y = y + (height - logo_img.height) // 2

        poster.paste(logo_img, (img_x, img_y), logo_img)

    except FileNotFoundError:
        print(
            "Error: logo.png not found. Please make sure the logo file is in the directory."
        )
        # Draw a placeholder
        draw = ImageDraw.Draw(poster)
        draw.rectangle([x, y, x + width, y + height], fill=(200, 200, 200))
        draw.text((x + 10, y + 10), "logo.png\nnot found", fill="red")
    except Exception as e:
        print(f"Error pasting Pokeball logo: {e}")


def create_pokemon_poster():
    """Create optimized Pokemon poster 60x90cm"""
    # Poster specifications for A1 size at 300 DPI
    DPI = 300
    WIDTH_CM = 59.4
    HEIGHT_CM = 84.1
    POSTER_WIDTH = int(WIDTH_CM * DPI / 2.54)
    POSTER_HEIGHT = int(HEIGHT_CM * DPI / 2.54)

    print(
        f"Creating poster: {POSTER_WIDTH}x{POSTER_HEIGHT} pixels ({WIDTH_CM}x{HEIGHT_CM}cm at {DPI} DPI)"
    )

    # Layout configuration
    BORDER_WIDTH = 150
    TITLE_HEIGHT = 0

    # Available space for Pokemon grid
    GRID_WIDTH = POSTER_WIDTH - (2 * BORDER_WIDTH)
    GRID_HEIGHT = POSTER_HEIGHT - TITLE_HEIGHT - (2 * BORDER_WIDTH)

    # Row specifications
    TOTAL_ROWS = 16
    ROW_HEIGHT = GRID_HEIGHT // TOTAL_ROWS

    # Column specifications for different row types
    ROW1_COLS = 7  # 6 Pokemon + 1 Logo
    MIDDLE_COLS = 10  # Rows 2-11
    BOTTOM_COLS = 9  # Rows 12-16

    # Pokemon image sizes (adjust based on available space)
    POKEMON_SIZE_ROW1 = min(GRID_WIDTH // ROW1_COLS - 20, ROW_HEIGHT - 85)
    POKEMON_SIZE_MIDDLE = min(GRID_WIDTH // MIDDLE_COLS - 10, ROW_HEIGHT - 85)
    POKEMON_SIZE_BOTTOM = min(GRID_WIDTH // BOTTOM_COLS - 15, ROW_HEIGHT - 85)

    # Colors - Pokemon theme
    BACKGROUND_COLOR = (252, 250, 245)
    BORDER_COLOR = (60, 90, 150)
    TEXT_COLOR = (50, 50, 50)  # Dark gray

    print(
        f"Pokemon sizes - Row1: {POKEMON_SIZE_ROW1}px, Middle: {POKEMON_SIZE_MIDDLE}px, Bottom: {POKEMON_SIZE_BOTTOM}px"
    )

    # Create poster image
    poster = Image.new("RGB", (POSTER_WIDTH, POSTER_HEIGHT), BACKGROUND_COLOR)
    draw = ImageDraw.Draw(poster)

    # Draw decorative border
    for i in range(BORDER_WIDTH // 4):
        draw.rectangle(
            [i * 2, i * 2, POSTER_WIDTH - 1 - (i * 2), POSTER_HEIGHT - 1 - (i * 2)],
            outline=BORDER_COLOR,
            width=3,
        )

    # Load fonts - with Chinese support
    try:
        # Try to find Chinese-capable fonts first
        chinese_fonts = [
            "/usr/share/fonts/opentype/noto/NotoSerifCJK-Bold.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
            "/usr/share/fonts/truetype/arphic/uming.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        ]

        font_found = None
        for font_path in chinese_fonts:
            if os.path.exists(font_path):
                font_found = font_path
                break

        if font_found:
            title_font = ImageFont.truetype(font_found, 72)
            name_font = ImageFont.truetype(font_found, 36)
        else:
            raise Exception("No suitable font found")
    except:
        title_font = ImageFont.load_default()
        name_font = ImageFont.load_default()

    # Title removed by user request

    # Starting positions
    grid_start_x = BORDER_WIDTH
    grid_start_y = BORDER_WIDTH + TITLE_HEIGHT

    pokemon_id = 1

    # ROW 1: 6 Pokemon + Pokemon Logo (positions 1-6 + logo)
    row_y = grid_start_y
    cell_width_row1 = GRID_WIDTH // ROW1_COLS

    print("Processing Row 1: 6 Pokemon + Logo...")

    # Track Pokemon for Row 1 - we want Pokemon 1,2,3,4,5,6 and logo in center
    row1_pokemon_positions = [1, 2, 3, None, 4, 5, 6]  # None = logo position

    for col in range(ROW1_COLS):
        cell_x = grid_start_x + (col * cell_width_row1)
        cell_center_x = cell_x + cell_width_row1 // 2

        if col == 3:  # Center position for Pokemon logo
            paste_pokeball_logo(poster, cell_x, row_y, cell_width_row1, ROW_HEIGHT)
        else:
            # Get the Pokemon ID for this position
            current_pokemon = row1_pokemon_positions[col]

            pokemon_path = f"pokemon/{current_pokemon}.png"
            if os.path.exists(pokemon_path):
                try:
                    pokemon_img = Image.open(pokemon_path)
                    if pokemon_img.mode != "RGBA":
                        pokemon_img = pokemon_img.convert("RGBA")

                    pokemon_img.thumbnail(
                        (POKEMON_SIZE_ROW1, POKEMON_SIZE_ROW1), Image.Resampling.LANCZOS
                    )

                    # Center image in cell with balanced spacing
                    img_x = cell_center_x - pokemon_img.width // 2
                    img_y = row_y + 10  # Consistent top margin

                    poster.paste(pokemon_img, (img_x, img_y), pokemon_img)

                    # Add Pokemon number and name on same line with proper spacing
                    pokemon_name = load_pokemon_data(current_pokemon)
                    combined_text = f"#{current_pokemon:03d} {pokemon_name}"
                    text_bbox = draw.textbbox((0, 0), combined_text, font=name_font)
                    text_width = text_bbox[2] - text_bbox[0]
                    text_x = cell_center_x - text_width // 2
                    text_y = (
                        img_y + pokemon_img.height + 15
                    )  # Consistent spacing from image

                    draw.text(
                        (text_x, text_y), combined_text, fill=TEXT_COLOR, font=name_font
                    )

                except Exception as e:
                    print(f"Error loading Pokemon #{current_pokemon}: {e}")

    # Set pokemon_id to 7 for the next rows
    pokemon_id = 7

    # ROWS 2-11: 10 Pokemon each (Pokemon #7-106)
    print("Processing Rows 2-11: 10 Pokemon each...")
    cell_width_middle = GRID_WIDTH // MIDDLE_COLS

    for row in range(2, 12):  # Rows 2-11
        row_y = grid_start_y + (row - 1) * ROW_HEIGHT

        for col in range(MIDDLE_COLS):
            if pokemon_id > 151:
                break

            cell_x = grid_start_x + (col * cell_width_middle)
            cell_center_x = cell_x + cell_width_middle // 2

            pokemon_path = f"pokemon/{pokemon_id}.png"
            if os.path.exists(pokemon_path):
                try:
                    pokemon_img = Image.open(pokemon_path)
                    if pokemon_img.mode != "RGBA":
                        pokemon_img = pokemon_img.convert("RGBA")

                    pokemon_img.thumbnail(
                        (POKEMON_SIZE_MIDDLE, POKEMON_SIZE_MIDDLE),
                        Image.Resampling.LANCZOS,
                    )

                    img_x = cell_center_x - pokemon_img.width // 2
                    img_y = row_y + 10  # Balanced space from top

                    poster.paste(pokemon_img, (img_x, img_y), pokemon_img)

                    # Add Pokemon number and name on same line with better spacing
                    pokemon_name = load_pokemon_data(pokemon_id)
                    combined_text = f"#{pokemon_id:03d} {pokemon_name}"
                    text_bbox = draw.textbbox((0, 0), combined_text, font=name_font)
                    text_width = text_bbox[2] - text_bbox[0]
                    text_x = cell_center_x - text_width // 2
                    text_y = (
                        img_y + pokemon_img.height + 15
                    )  # Consistent spacing from image

                    draw.text(
                        (text_x, text_y), combined_text, fill=TEXT_COLOR, font=name_font
                    )

                except Exception as e:
                    print(f"Error loading Pokemon #{ pokemon_id}: {e}")

            pokemon_id += 1

        if row % 2 == 0:
            print(f"Completed row {row}, up to Pokemon #{pokemon_id-1}")

    # ROWS 12-16: 9 Pokemon each (Pokemon #107-151)
    print("Processing Rows 12-16: 9 Pokemon each...")
    cell_width_bottom = GRID_WIDTH // BOTTOM_COLS

    for row in range(12, 17):  # Rows 12-16
        row_y = grid_start_y + (row - 1) * ROW_HEIGHT

        for col in range(BOTTOM_COLS):
            if pokemon_id > 151:
                break

            cell_x = grid_start_x + (col * cell_width_bottom)
            cell_center_x = cell_x + cell_width_bottom // 2

            pokemon_path = f"pokemon/{pokemon_id}.png"
            if os.path.exists(pokemon_path):
                try:
                    pokemon_img = Image.open(pokemon_path)
                    if pokemon_img.mode != "RGBA":
                        pokemon_img = pokemon_img.convert("RGBA")

                    pokemon_img.thumbnail(
                        (POKEMON_SIZE_BOTTOM, POKEMON_SIZE_BOTTOM),
                        Image.Resampling.LANCZOS,
                    )

                    img_x = cell_center_x - pokemon_img.width // 2
                    img_y = row_y + 10  # Balanced space from top

                    poster.paste(pokemon_img, (img_x, img_y), pokemon_img)

                    # Add Pokemon number and name on same line with better spacing
                    pokemon_name = load_pokemon_data(pokemon_id)
                    combined_text = f"#{pokemon_id:03d} {pokemon_name}"
                    text_bbox = draw.textbbox((0, 0), combined_text, font=name_font)
                    text_width = text_bbox[2] - text_bbox[0]
                    text_x = cell_center_x - text_width // 2
                    text_y = (
                        img_y + pokemon_img.height + 15
                    )  # Consistent spacing from image

                    draw.text(
                        (text_x, text_y), combined_text, fill=TEXT_COLOR, font=name_font
                    )

                except Exception as e:
                    print(f"Error loading Pokemon #{ pokemon_id}: {e}")

            pokemon_id += 1

        print(f"Completed row {row}, up to Pokemon #{ pokemon_id-1}")

    # Save the poster in multiple formats
    base_filename = "pokemon_poster_A1"
    output_paths = []

    print("\nSaving poster in multiple formats...")

    # Save as PNG (with transparency support)
    png_path = f"{base_filename}.png"
    poster.save(png_path, "PNG", optimize=True)
    output_paths.append(png_path)
    print(f"✅ PNG saved: {png_path}")

    # Save as TIFF (high quality, lossless)
    tif_path = f"{base_filename}.tif"
    poster.save(tif_path, "TIFF", compression="lzw")
    output_paths.append(tif_path)
    print(f"✅ TIFF saved: {tif_path}")

    # Save as JPEG (smaller file size, but convert to RGB first)
    jpg_path = f"{base_filename}.jpg"
    # Convert to RGB for JPEG (removes transparency)
    poster_rgb = Image.new("RGB", poster.size, BACKGROUND_COLOR)
    poster_rgb.paste(poster, mask=poster.split()[-1] if poster.mode == "RGBA" else None)
    poster_rgb.save(jpg_path, "JPEG", quality=95, optimize=True)
    output_paths.append(jpg_path)
    print(f"✅ JPEG saved: {jpg_path}")

    print("\n✅ Poster created successfully in 3 formats!")
    print(f"📁 Files: {', '.join(output_paths)}")
    print(f"📏 Size: {poster.size[0]}x{poster.size[1]} pixels")
    print(f"📐 Physical: {WIDTH_CM}x{HEIGHT_CM} cm at {DPI} DPI")
    print(f"🔢 Pokemon included: {min(pokemon_id-1, 151)}")

    return output_paths


if __name__ == "__main__":
    create_pokemon_poster()
