import os
import json
import argparse
import requests
import subprocess
import shutil
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

# Directory to save Pokemon data
POKEMON_DIR = "pokemon"

# Ensure the directory exists
os.makedirs(POKEMON_DIR, exist_ok=True)


def make_session():
    s = requests.Session()
    retries = Retry(
        total=5, backoff_factor=1, status_forcelist=[429, 500, 502, 503, 504]
    )
    adapter = HTTPAdapter(max_retries=retries)
    s.mount("https://", adapter)
    s.mount("http://", adapter)
    s.headers.update({"User-Agent": "pokedex-data-fetcher/1.0"})
    return s


def compress_gif(gif_path):
    """
    Compresses a GIF using gifsicle if it's over 110KB.
    Recursively compresses with smaller sizes if still over 100KB.
    Overwrites the original file.
    """
    if not shutil.which("gifsicle"):
        print("--> gifsicle not found, skipping compression.")
        return

    try:
        file_size = os.path.getsize(gif_path)
        if file_size > 110 * 1024:
            print(f"--> GIF is {(file_size / 1024):.1f}KB, compressing...")

            # Try different heights: 50, then 40 if still over 100KB
            heights = [50, 40]

            for height in heights:
                command = [
                    "gifsicle",
                    "--resize-height",
                    str(height),
                    "-o",
                    gif_path,  # gifsicle can overwrite with -o
                    gif_path,
                ]

                result = subprocess.run(command, capture_output=True, text=True)

                if result.returncode == 0:
                    compressed_size = os.path.getsize(gif_path)
                    print(
                        f"--> Successfully compressed to {(compressed_size / 1024):.1f}KB (height: {height}px)."
                    )

                    # If under 100KB, we're done
                    if compressed_size <= 100 * 1024:
                        break
                    # If this was the last attempt, we're done regardless
                    elif height == heights[-1]:
                        break
                    else:
                        print(f"--> Still over 100KB, trying smaller size...")
                else:
                    print(f"--> gifsicle failed for {gif_path}: {result.stderr}")
                    break
        else:
            pass

    except FileNotFoundError:
        print(f"--> Error: GIF file not found at {gif_path} for compression.")
    except Exception as e:
        print(f"--> An error occurred during compression: {e}")


def extract_pokemon_info(data, species_data=None):
    """Extract only the necessary information from Pokemon data"""
    names = {"en": data.get("name")}

    if species_data:
        for name_info in species_data.get("names", []):
            lang = name_info["language"]["name"]
            if lang == "zh-Hant":
                names["zh"] = name_info["name"]
            elif lang == "ja":
                names["ja"] = name_info["name"]
            elif lang == "ko":
                names["ko"] = name_info["name"]

    default_name = names.get("zh", names.get("en"))

    info = {
        "id": data.get("id"),
        "name": default_name,
        "names": names,
        "types": [t["type"]["name"] for t in data.get("types", [])],
        "abilities": [a["ability"]["name"] for a in data.get("abilities", [])],
        "stats": {s["stat"]["name"]: s["base_stat"] for s in data.get("stats", [])},
        "height": data.get("height"),
        "weight": data.get("weight"),
    }
    return info


def find_sprite_urls(data):
    """Return (png_url, gif_url) best-effort from the sprite data structure."""
    sprites = data.get("sprites", {})

    # Prefer generation-v animated gif
    try:
        gif = sprites["versions"]["generation-v"]["black-white"]["animated"][
            "front_default"
        ]
    except Exception:
        gif = None

    # Preferred PNG sources
    png = None
    # official-artwork
    try:
        png = sprites["other"]["official-artwork"]["front_default"]
    except Exception:
        png = png

    # fallback: home, dream_world, front_default
    if not png:
        png = sprites.get("other", {}).get("home", {}).get("front_default")
    if not png:
        png = sprites.get("other", {}).get("dream_world", {}).get("front_default")
    if not png:
        png = sprites.get("front_default")

    return png, gif


def fetch_pokemon_entry(session, url):
    resp = session.get(url)
    if resp.status_code != 200:
        return None
    return resp.json()


def fetch_pokemon_data(session, pokemon_url, skip_existing=True):
    data = fetch_pokemon_entry(session, pokemon_url)
    if not data:
        print(f"Failed to fetch data for url {pokemon_url}")
        return None

    pokemon_id = data.get("id")
    if not pokemon_id:
        print(f"No id for {pokemon_url}")
        return None

    json_path = os.path.join(POKEMON_DIR, f"{pokemon_id}.json")
    png_path = os.path.join(POKEMON_DIR, f"{pokemon_id}.png")
    gif_path = os.path.join(POKEMON_DIR, f"{pokemon_id}.gif")

    if skip_existing and os.path.exists(json_path):
        print(f"Skipping existing {pokemon_id}")
        return None

    # Fetch species data for names
    species_url = data.get("species", {}).get("url")
    species_data = None
    if species_url:
        sresp = session.get(species_url)
        if sresp.status_code == 200:
            species_data = sresp.json()

    pokemon_info = extract_pokemon_info(data, species_data)

    # Save JSON
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(pokemon_info, f, ensure_ascii=False, indent=4)

    png_url, gif_url = find_sprite_urls(data)

    if png_url:
        presp = session.get(png_url)
        if presp.status_code == 200:
            with open(png_path, "wb") as f:
                f.write(presp.content)
        else:
            print(
                f"Failed to download PNG for {pokemon_id} ({pokemon_info.get('name')}) - URL: {png_url}"
            )

    if gif_url:
        gresp = session.get(gif_url)
        if gresp.status_code == 200:
            with open(gif_path, "wb") as f:
                f.write(gresp.content)
            print(
                f"Downloaded data and GIF for {pokemon_id} ({pokemon_info.get('name')})"
            )
            # Compress GIF if it's too large
            compress_gif(gif_path)
        else:
            print(
                f"Failed to download GIF for {pokemon_id} ({pokemon_info.get('name')}) - URL: {gif_url}"
            )
    else:
        print(f"No animated GIF for {pokemon_id} ({pokemon_info.get('name')})")

    return pokemon_info


def get_all_pokemon_list(session):
    # Get total count
    list_url = "https://pokeapi.co/api/v2/pokemon?limit=1"
    resp = session.get(list_url)
    if resp.status_code != 200:
        raise RuntimeError("Failed to fetch pokemon list")
    count = resp.json().get("count")

    # Fetch all entries
    resp = session.get(f"https://pokeapi.co/api/v2/pokemon?limit={count}")
    if resp.status_code != 200:
        raise RuntimeError("Failed to fetch full pokemon list")
    return resp.json().get("results", [])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Limit number of pokemon to fetch (for testing)",
    )
    parser.add_argument(
        "--start",
        type=int,
        default=1,
        help="Starting Pokemon ID (1-based, e.g., 1 for Bulbasaur)",
    )
    parser.add_argument(
        "--skip-existing",
        action="store_true",
        help="Skip entries with existing JSON files",
    )
    args = parser.parse_args()

    session = make_session()

    all_entries = get_all_pokemon_list(session)
    total = len(all_entries)
    print(f"Total pokemon entries (including forms): {total}")

    # Convert 1-based to 0-based index for Python list
    start_index = max(0, args.start - 1)
    limit = args.limit if args.limit > 0 else total

    end_index = min(total, start_index + limit)

    for idx in range(start_index, end_index):
        entry = all_entries[idx]
        url = entry.get("url")
        print(f"Processing {idx+1}/{end_index}: {entry.get('name')} -> {url}")
        try:
            fetch_pokemon_data(session, url, skip_existing=args.skip_existing)
        except Exception as e:
            print(f"Error processing {entry.get('name')}: {e}")


if __name__ == "__main__":
    main()
