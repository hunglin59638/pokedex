/*
 * Project: Pokemon NFC Pokedex - Pokeball Sender
 * Board: Seeed Studio XIAO ESP32C3
 * Modules: PN532 (I2C) + ESP-NOW
 * Function: Scan NFC cards, read text content (Pokemon ID), and transmit via ESP-NOW
 * Version: v5 (Universal number parsing)
 */

#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ctype.h> // Include ctype.h to use isdigit()
#include "config.h" // Include configuration file

// --- PN532 Configuration ---
#define PN532_IRQ   (-1)
#define PN532_RESET (-1)
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

// Define data structure to transmit
typedef struct struct_message {
  int pokemon_id;
} struct_message;

struct_message myData;
esp_now_peer_info_t peerInfo;

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("\r\nTransmission Status: ");
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("Data sent successfully");
  } else {
    Serial.println("Data transmission failed - Check if receiver is on and channel settings match");
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial);
  Serial.println("ESP-NOW Sender Initialization (v5)...");

  // Display configured receiver MAC address
  Serial.print("Receiver MAC Address: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", RECEIVER_MAC_ADDRESS[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();

  // Set Wi-Fi mode to Station and specify channel
  WiFi.mode(WIFI_STA);
  WiFi.channel(WIFI_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed");
    return;
  }

  esp_now_register_send_cb(OnDataSent);

  // Register communication peer
  memcpy(peerInfo.peer_addr, RECEIVER_MAC_ADDRESS, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("Failed to add communication peer");
    return;
  }
  Serial.println("ESP-NOW initialization complete");

  // --- Initialize PN532 ---
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.print("PN532 module not found, check wiring!");
    while (1);
  }
  Serial.println("PN532 module found!");
  nfc.SAMConfig();
  Serial.println("Waiting for NFC card...");
}


// Universal parsing function: Extract numeric string directly from payload
int parseTextPayload(uint8_t* payload, int payload_length) {
    if (payload_length < 1) return 0;

    char numeric_buffer[payload_length + 1];
    int numeric_idx = 0;
    bool found_digit = false;

    // Iterate through entire payload
    for (int i = 0; i < payload_length; i++) {
        // If current character is a digit
        if (isdigit(payload[i])) {
            numeric_buffer[numeric_idx++] = payload[i];
            found_digit = true;
        } else {
            // If digits already found and non-digit encountered, number sequence ended
            if (found_digit) {
                break;
            }
            // If no digits found yet, continue searching
        }
    }

    // If no digits found in entire payload
    if (!found_digit) {
        Serial.println("No digits found in NDEF payload.");
        return 0;
    }

    // Add string terminator
    numeric_buffer[numeric_idx] = '\0';

    Serial.print("Extracted numeric string from payload: \"");
    Serial.print(numeric_buffer);
    Serial.println("\"");

    // Convert extracted numeric string to integer
    return atoi(numeric_buffer);
}


// Read card raw data, manually parse NDEF structure, and return Pokemon ID
int getPokemonIdFromCard() {
  uint8_t rawCardData[64]; // Store first 16 pages (64 bytes) of Mifare Ultralight
  bool readSuccess = false;

  Serial.println("Reading card raw data...");

  // Read first 16 pages (user data typically starts from page 4)
  for (uint8_t page = 0; page < 16; page++) {
    uint8_t page_buffer[4];
    if (nfc.mifareultralight_ReadPage(page, page_buffer)) {
      readSuccess = true;
      memcpy(rawCardData + (page * 4), page_buffer, 4);
    } else {
      Serial.print("Failed to read page ");
      Serial.print(page);
      Serial.println(".");
      readSuccess = false; // Abort if any page read fails
      break;
    }
  }

  if (!readSuccess) {
    Serial.println("Failed to read card data.");
    return 0;
  }

  Serial.println("Raw data read successfully. Starting NDEF parsing...");

  // --- Manual NDEF Parsing ---
  // Start searching from byte 16 (page 4), common user data start position
  int pos = 16;
  while (pos < 64) {
    uint8_t tlv_type = rawCardData[pos];

    if (tlv_type == 0x03) { // Found NDEF Message TLV (Type)
      Serial.println("Found NDEF Message TLV (0x03).");
      pos++;
      if (pos >= 64) break;

      uint8_t tlv_len = rawCardData[pos]; // Length
      pos++;

      Serial.print("NDEF message length: ");
      Serial.println(tlv_len);

      if (pos + tlv_len <= 64) {
        // Parse NDEF message itself (Value)
        uint8_t* message_ptr = rawCardData + pos;

        uint8_t type_length = message_ptr[1];
        uint8_t payload_length = message_ptr[2]; // Assume short record format

        Serial.print("Detected NDEF record type: ");
        for(int i = 0; i < type_length; i++) {
          Serial.print((char)message_ptr[3+i]);
        }
        Serial.println();

        // If payload exists, attempt to parse
        if (payload_length > 0) {
            Serial.println("Found valid payload, attempting to parse...");
            int payload_start = 3 + type_length;
            return parseTextPayload(message_ptr + payload_start, payload_length);
        } else {
            Serial.println("No payload in NDEF record.");
        }
      }
      return 0; // End after processing first NDEF message
    } else if (tlv_type == 0xFE) { // Found terminator
      Serial.println("Found NDEF terminator (0xFE).");
      break;
    } else if (tlv_type == 0x00) { // Empty TLV
      pos++;
    } else { // Other TLV types, skip directly
      pos++;
      if (pos < 64) {
          pos += rawCardData[pos] + 1;
      }
    }
  }

  Serial.println("No valid NDEF message found on card.");
  return 0;
}


void loop() {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength;

  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);

  if (success) {
    Serial.println("-------------------");
    Serial.println("Card detected!");

    int pokemonId = getPokemonIdFromCard();

    if (pokemonId >= 0) {
      myData.pokemon_id = pokemonId;
      Serial.print("Successfully parsed Pokemon ID: ");
      Serial.println(myData.pokemon_id);

      if (pokemonId == 0) {
        Serial.println("Detected return to home command (ID: 0)");
      }

      Serial.println("Sending via ESP-NOW...");
      esp_err_t result = esp_now_send(RECEIVER_MAC_ADDRESS, (uint8_t *) &myData, sizeof(myData));

      if (result != ESP_OK) {
        Serial.println("Error occurred during transmission");
      }
    } else {
      Serial.println("Unable to read valid Pokemon ID from card.");
    }

    delay(2000); // Avoid immediate repeated reads
  }
}

