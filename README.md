# Pokemon NFC Pokedex

A physical Pokemon Pokedex that displays animated Pokemon GIFs on a TFT screen when NFC cards are scanned. The project consists of two ESP32-based devices communicating via ESP-NOW protocol.

> **Project Status**: This project has been successfully completed and fully tested on **Ubuntu 24.04 LTS**. All features are working as intended, including NFC scanning, ESP-NOW communication, GIF animation display, and poster generation.

## Features

- **Unlimited Pokemon Support**: Display any Pokemon from PokeAPI (not limited to 151)
- **NFC Card Scanning**: Scan Pokemon NFC stickers to trigger display
- **Animated GIF Display**: Shows Pokemon animations on 3.5" TFT screen
- **Wireless Communication**: ESP-NOW protocol for fast, reliable transmission
- **Physical Poster**: Optional A1-size poster with 151 original Pokemon
- **Dual Language Support**: Pokemon names in multiple languages

<video src="https://github.com/user-attachments/assets/87f4468a-dbbc-4f44-bd77-43db51d7032a
" height="480" controls></video>

## Usage

1. **Power On**:
   - Connect 9V battery to Pokedex receiver
   - Power on Pokeball sender (3.7V Li-Po battery)

2. **Scan Pokemon**:
   - Hold NFC card near the PN532 module
   - The sender reads the Pokemon ID and transmits via ESP-NOW
   - The Pokedex receiver displays the Pokemon with animated GIF

3. **Troubleshooting**:
   - Check Serial Monitor for connection status
   - Verify MAC address configuration matches
   - Ensure Wi-Fi channel is identical on both devices
   - Confirm MicroSD card is properly inserted
   - Verify 9V battery has sufficient voltage

## Hardware Requirements

### Pokedex Receiver (Display Unit)
- **Board**: Wemos D1 R32 (ESP32)
- **Display**: 3.5" TFT LCD (ILI9488, 480x320, SPI)
- **Storage**: MicroSD card module (SPI)
- **Power**: 9V battery

### Pokeball Sender (NFC Scanner)
- **Board**: Seeed Studio XIAO ESP32C3
- **NFC Module**: PN532 (I2C mode)
- **Power**: 3.7V Li-Po Battery

### Additional Materials
- NFC stickers (NTAG213/215/216)
- Pokemon poster (optional, A1 size)
- 9V battery connector
- Jumper wires

## Wiring Connections

### Pokedex Receiver (Wemos D1 R32) - TFT Display & MicroSD

| ESP32 (Wemos D1 R32) | TFT pin (ILI9341/ILI9488) | SD pin (on same module) | Notes                   |
| -------------------- | ------------------------- | ----------------------- | ----------------------- |
| GPIO18 (SCK)         | SCK                       | SCK                     | Shared SPI Clock (VSPI) |
| GPIO23 (MOSI)        | MOSI                      | MOSI                    | Shared SPI Data Out     |
| GPIO19 (MISO)        | MISO                      | MISO                    | Shared SPI Data In      |
| GPIO5                | TFT_CS                    | -                       | TFT Chip Select         |
| GPIO14               | -                         | SD_CS                   | MicroSD Chip Select     |
| GPIO2                | DC/RS                     | -                       | Data/Command control    |
| GPIO4                | RST                       | -                       | TFT Reset               |
| 3V3                  | VCC                       | VCC                     | 3.3V Power              |
| GND                  | GND                       | GND                     | Ground                  |

### Pokeball Sender (XIAO ESP32C3) - NFC Module

| XIAO ESP32C3 | PN532 NFC Module | Notes                       |
| ------------ | ---------------- | --------------------------- |
| 3V3          | VCC              | 3.3V Power Supply           |
| GND          | GND              | Ground                      |
| D4 (GPIO4)   | SDA              | I2C Data Line (custom SDA)  |
| D5 (GPIO5)   | SCL              | I2C Clock Line (custom SCL) |

**Power Management (Pokeball):**
| Component          | Connection                      | Notes                     |
| ------------------ | ------------------------------- | ------------------------- |
| 3.7V Li-Po Battery | TP4056 BAT+ / BAT-              | Rechargeable power source |
| TP4056 OUT+        | XIAO ESP32C3 3V3                | Regulated 3.3V output     |
| TP4056 OUT-        | XIAO ESP32C3 GND                | Ground connection         |
| Power Switch       | Between Battery+ and TP4056 IN+ | Main power control        |

## Software Setup

### Prerequisites
- [PlatformIO](https://platformio.org/) (VS Code extension recommended)
- Python 3.7+ (for data fetching and poster generation)
- `uv` (recommended Python package manager) or `pip`
- Git
- `gifsicle` (for GIF compression)

#### Installing gifsicle

**Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install gifsicle
```

**macOS (using Homebrew):**
```bash
brew install gifsicle
```

**Windows:**
1. Download from [gifsicle website](https://www.lcdf.org/gifsicle/)
2. Extract to a folder (e.g., `C:\gifsicle\`)
3. Add the folder to your system PATH
4. Or place `gifsicle.exe` in your project directory

**Verify installation:**
```bash
gifsicle --version
```

### 1. Clone the Repository

```bash
git clone https://github.com/hunglin59638/pokedex
cd pokedex
```

### 2. Install Python Dependencies

Using `uv` (recommended - faster and more reliable):

```bash
# Install uv if not already installed
curl -LsSf https://astral.sh/uv/install.sh | sh

# Create virtual environment and install dependencies
uv venv
source .venv/bin/activate  # On Windows: .venv\Scripts\activate
uv pip install requests pillow
```

Alternative using traditional pip:

```bash
python3 -m venv .venv
source .venv/bin/activate  # On Windows: .venv\Scripts\activate
pip install requests pillow
```

### 3. Download Pokemon Data

Fetch Pokemon data from PokeAPI (JSON, PNG, GIF files):

```bash
# Download first 151 Pokemon
python pokemon_data_fetcher.py --start 1 --limit 151

# Download specific range (e.g., Gen 2 Pokemon)
python pokemon_data_fetcher.py --start 152 --limit 100

# Skip already downloaded Pokemon
python pokemon_data_fetcher.py --start 1 --limit 251 --skip-existing
```

**Parameters:**
- `--start N`: Starting Pokemon ID (1-based, e.g., 1 for Bulbasaur)
- `--limit N`: Number of Pokemon to fetch
- `--skip-existing`: Skip already downloaded files

**Note**: The script automatically compresses GIF files using `gifsicle` for optimal SD card storage.

### 4. Generate Pokemon Poster (Optional)

Create an A1-size poster with 151 original Pokemon:

```bash
# English names
python create_poster.py

# Other languages (e.g., Japanese, Chinese)
python create_poster.py --lang ja
python create_poster.py --lang zh
```

**Output**:
- `pokemon_poster_A1.png` (with transparency)
- `pokemon_poster_A1.tif` (high quality, lossless)
- `pokemon_poster_A1.jpg` (smaller file size)

**Poster Specifications:**
- Size: 59.4 × 84.1 cm (A1)
- Resolution: 300 DPI
- Layout: 151 Pokemon + Pokeball logo
- **Note**: Only A1 size is supported (not customizable)

### 5. Prepare MicroSD Card

1. Format SD card as FAT32
2. Create `pokemon/` folder in root directory
3. Copy downloaded Pokemon files to SD card:
   ```
   SD_CARD/
   └── pokemon/
       ├── 1.json    # Pokemon data
       ├── 1.png     # Static image
       ├── 1.gif     # Animated sprite
       ├── 2.json
       ├── 2.png
       ├── 2.gif
       └── ...
   ```

### 6. Program the Pokedex Receiver

1. Open `250830-102512-wemos_d1_uno32/` in PlatformIO
2. Connect Wemos D1 R32 via USB
3. Build and upload:
   ```bash
   ~/.platformio/penv/bin/pio run -t upload
   ```
4. Open Serial Monitor to view the **MAC address** (you'll need this for the sender)
5. Insert prepared MicroSD card

### 7. Configure the Pokeball Sender

1. Open `250830-104536-seeed_xiao_esp32c3/src/config.h`
2. Set the receiver's MAC address:
   ```cpp
   // Replace with your receiver's MAC address from step 6
   uint8_t RECEIVER_MAC_ADDRESS[] = {0x08, 0x3A, 0xF2, 0xB7, 0xC0, 0xEC};
   ```
3. Verify Wi-Fi channel matches:
   ```cpp
   #define WIFI_CHANNEL 1
   ```

### 8. Program the Pokeball Sender

1. Open `250830-104536-seeed_xiao_esp32c3/` in PlatformIO
2. Connect XIAO ESP32C3 via USB
3. Build and upload:
   ```bash
   ~/.platformio/penv/bin/pio run -t upload
   ```
4. Verify connection in Serial Monitor

## Prepare NFC Cards

### Writing Pokemon IDs to NFC Stickers

**Recommended Tool**: NFC Tools (NCT) app for Android/iOS

**Data Format Configuration:**
- **Content Type**: `text/plain`
- **Data**: Pokemon ID number (1-151 only)

**Step-by-step process:**
1. Open NFC Tools (NCT) app on your smartphone
2. Select "Write" → "Add a record" → "Data (Add a custom record)"
3. Set the text content to the Pokemon ID number (just the number)
4. Place NFC sticker near your phone's NFC antenna
5. Tap "Write" to program the sticker

**Examples:**
- Card #1: Write value `1` (Bulbasaur)
- Card #25: Write value `25` (Pikachu)
- Card #151: Write value `151` (Mew)

### Poster Integration

If using the physical poster:
1. Print the A1 poster (use `pokemon_poster_A1.jpg/png/tiff` for printing)
2. **Place NFC stickers BEHIND the poster** at each Pokemon's position
3. The PN532 scanner can read through the poster paper
4. Scan the poster from the front to trigger the corresponding Pokemon

**Special sticker**: Write "0" to return the Pokedex to home screen

## Project Structure

```
pokedex/
├── 250830-102512-wemos_d1_uno32/          # Pokedex Receiver
│   ├── src/
│   │   └── pokedex_receiver.ino           # Main receiver code
│   └── platformio.ini                      # PlatformIO config
├── 250830-104536-seeed_xiao_esp32c3/      # Pokeball Sender
│   ├── src/
│   │   ├── pokeball_sender.ino            # Main sender code
│   │   └── config.h                        # MAC address configuration
│   └── platformio.ini                      # PlatformIO config
├── pokemon/                                # Pokemon data (created by fetcher)
│   ├── 1.json, 1.png, 1.gif
│   └── ...
├── pokemon_data_fetcher.py                 # Download Pokemon data from PokeAPI
├── create_poster.py                        # Generate A1 poster
├── CHANGELOG.md                            # Development history
└── README.md                               # This file
```

## Technical Details

### ESP-NOW Communication
- Fast, low-latency peer-to-peer protocol
- No Wi-Fi router required
- Typical latency: <10ms

### State Machine Architecture
The receiver uses a state machine to manage SPI bus conflicts:
- **LISTENING**: ESP-NOW active, waiting for Pokemon ID
- **SD_LOADING**: ESP-NOW disabled, reading files from SD card
- **DISPLAYING**: Rendering GIF frames with ESP-NOW disabled

### GIF Rendering
- Memory-buffered playback for smooth animation
- Frame clearing to prevent ghosting artifacts
- Automatic loop handling

### Data Sources
All Pokemon data (JSON, PNG, GIF) comes from [PokeAPI](https://pokeapi.co/):
- **JSON**: Pokemon names, types, stats (displayed on Pokedex)
- **PNG**: Official artwork (used for poster creation)
- **GIF**: Animated sprites from game assets (displayed on Pokedex)

## Language Support

The Pokedex supports multiple languages for Pokemon names. Modify `pokedex_receiver.ino` to change the language:

```cpp
// In loadPokemonJSON() function, line ~578
name = data["names"]["en"].as<String>();  // Change "en" to your language
```

**Currently supported languages:**
- **Pokedex Display**: English (`en`) only - the ESP32 code currently only displays English names
- **Poster Generation**: English (`en`), Japanese (`ja`), Chinese (`zh`), Korean (`ko`) - based on data fetched from PokeAPI

**Note**: The Pokedex receiver is hardcoded to display English names. To add other languages, you would need to modify the ESP32 code to use different language fields from the JSON data.

Generate poster with matching language:
```bash
python create_poster.py --lang ja  # Japanese poster
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Pokemon data from [PokeAPI](https://pokeapi.co/)
- Sprite animations from Pokemon game assets
- Community libraries: Adafruit GFX, PN532, AnimatedGIF

## Additional Documentation

- `CHANGELOG.md`: Development history and technical challenges solved
- PlatformIO documentation: <https://docs.platformio.org/>

---

**Enjoy your Pokemon NFC Pokedex!** 🎮✨
