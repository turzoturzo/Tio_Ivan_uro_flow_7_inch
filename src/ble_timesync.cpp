#include "ble_timesync.h"
#include <NimBLEDevice.h>
#include <time.h>

// BLE CTS standard UUIDs (Bluetooth SIG assigned)
#define CTS_SERVICE_UUID   "00001805-0000-1000-8000-00805f9b34fb"
#define CTS_CURRENT_TIME   "00002a2b-0000-1000-8000-00805f9b34fb"

static bool             sSynced  = false;
static NimBLEAdvertising* sAdv   = nullptr;

// ── CTS write callback ────────────────────────────────────────────────────────
// iOS / Android write a 10-byte Current Time characteristic when connecting to
// a BLE peripheral that advertises CTS.  After initial pairing, the phone
// auto-reconnects on subsequent boots and writes time without user action.
//
// CTS payload (10 bytes, little-endian fields):
//   [0-1] year  [2] month  [3] day  [4] hour  [5] min  [6] sec
//   [7] day-of-week  [8] fractions256  [9] adjust-reason

class TimeChrCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChr) override {
        std::string val = pChr->getValue();
        if (val.size() < 7) return;

        const uint8_t* d = reinterpret_cast<const uint8_t*>(val.data());
        uint16_t year  = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
        uint8_t  month = d[2];
        uint8_t  day   = d[3];
        uint8_t  hour  = d[4];
        uint8_t  min   = d[5];
        uint8_t  sec   = d[6];

        // Sanity check: CTS from a synced phone should report a plausible year
        if (year < 2020 || year > 2100) return;

        struct tm t = {};
        t.tm_year  = year - 1900;
        t.tm_mon   = month - 1;  // tm_mon is 0-based
        t.tm_mday  = day;
        t.tm_hour  = hour;
        t.tm_min   = min;
        t.tm_sec   = sec;
        t.tm_isdst = 0;

        time_t epoch = mktime(&t);
        if (epoch < 1000000000L) return;  // reject pre-2001 timestamps

        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        sSynced = true;
        Serial.printf("[CTS] Time synced from phone: %04d-%02d-%02dT%02d:%02d:%02dZ\n",
                      year, month, day, hour, min, sec);
    }
};

static TimeChrCallbacks sTimeCbs;

// ── Public API ────────────────────────────────────────────────────────────────

void bleTimeSync_start() {
    sSynced = false;

    NimBLEDevice::init("UroFlow");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    NimBLEServer*  server = NimBLEDevice::createServer();
    NimBLEService* svc    = server->createService(CTS_SERVICE_UUID);

    NimBLECharacteristic* chr = svc->createCharacteristic(
        CTS_CURRENT_TIME,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
    );
    chr->setCallbacks(&sTimeCbs);

    // Provide a zero-filled default value so the characteristic is readable
    uint8_t zeroTime[10] = {0};
    chr->setValue(zeroTime, sizeof(zeroTime));

    svc->start();

    // Configure and start advertising
    sAdv = NimBLEDevice::getAdvertising();
    sAdv->addServiceUUID(CTS_SERVICE_UUID);
    sAdv->setScanResponse(true);
    sAdv->start();

    Serial.println("[CTS] Advertising as 'UroFlow' (CTS peripheral)");
}

bool bleTimeSync_synced() {
    return sSynced;
}

void bleTimeSync_stop() {
    if (sAdv) {
        sAdv->stop();
        sAdv = nullptr;
    }
    // Do NOT deinit NimBLE — esp_nimble_hci_and_controller_deinit() is
    // unreliable on IDF 5.x (arduino-esp32 3.x) and causes a panic.
    // WiFi is skipped when credentials are placeholder, so no radio conflict.
    // NimBLE stays running; BleAcaia will add client/scan role alongside the
    // existing (now non-advertising) CTS GATT server.
    Serial.println("[CTS] Advertising stopped");
}
