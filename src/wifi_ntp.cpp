#include "wifi_ntp.h"
#include <WiFi.h>
#include <time.h>

bool syncTimeViaNtp(const char* ssid, const char* pass, int timeout_s) {
    Serial.printf("[NTP] Connecting to WiFi \"%s\"...\n", ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    uint32_t deadline = millis() + (uint32_t)timeout_s * 1000UL / 2;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(200);
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[NTP] WiFi connect failed — using sequential filenames");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return false;
    }

    Serial.println("[NTP] WiFi connected, syncing time...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "UTC0", 1);
    tzset();

    // Wait for valid epoch
    deadline = millis() + (uint32_t)timeout_s * 1000UL / 2;
    while (time(nullptr) < 1000000000UL && millis() < deadline) {
        delay(200);
    }

    bool ok = (time(nullptr) >= 1000000000UL);

    if (ok) {
        time_t now = time(nullptr);
        struct tm* t = gmtime(&now);
        Serial.printf("[NTP] Time synced: %04d-%02d-%02dT%02d:%02d:%02dZ\n",
                      t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                      t->tm_hour, t->tm_min, t->tm_sec);
    } else {
        Serial.println("[NTP] Time sync timed out — using sequential filenames");
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100); // let radio settle before BLE starts

    return ok;
}
