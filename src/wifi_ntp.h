#pragma once
#include <Arduino.h>

// Connects to WiFi, syncs time via NTP, then disconnects.
// Returns true if a valid UTC time was obtained.
// On failure the RTC is left at epoch 0 and the caller should use
// sequential session numbering instead.
bool syncTimeViaNtp(const char* ssid, const char* pass, int timeout_s = 12);
