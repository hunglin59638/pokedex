/*
 * Configuration File - Pokeball Sender
 * Set your receiver MAC address here
 */

#ifndef CONFIG_H
#define CONFIG_H

// Receiver (D1 R32) MAC Address
// Replace this value with the MAC address displayed on your Pokedex receiver
uint8_t RECEIVER_MAC_ADDRESS[] = {0x08, 0x3A, 0xF2, 0xB7, 0xC0, 0xEC};

// Wi-Fi Channel (must match the receiver)
#define WIFI_CHANNEL 1

#endif