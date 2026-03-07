#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <functional>

// Callback type: fired on each decoded weight notification
using WeightCallback = std::function<void(float weight_g, uint32_t t_ms)>;

class BleAcaia : public NimBLEClientCallbacks,
                 public NimBLEScanCallbacks {
public:
    BleAcaia();

    // Call once in setup(); optionally provide stored MAC to skip scanning
    void begin(WeightCallback cb, const char* storedMac = nullptr);

    // Call every loop() iteration
    void tick();

    // Connection state queries
    bool isConnected()  const;
    bool isScanning()   const;
    std::string getMac() const;  // returns current or last-known MAC

    // Write the last-connected MAC into out_buf (must be >= 38 bytes)
    void getLastMac(char* out_buf, size_t len) const;

    // Stop scanning/connecting so WiFi can use the radio; tick() auto-resumes
    void pauseForWifi();

private:
    // ── NimBLE callbacks ──────────────────────────────────────────────────
    void onConnect(NimBLEClient* client) override;
    void onDisconnect(NimBLEClient* client, int reason) override;
    void onResult(const NimBLEAdvertisedDevice* device) override; // scan result

    // ── Internal helpers ─────────────────────────────────────────────────
    void _startScan();
    bool _connectTo(NimBLEAddress addr);
    void _subscribeAndIdentify();
    void _sendMessage(uint8_t msgType, const uint8_t* payload, size_t len);
    void _sendHeartbeat();
    static float _decodeWeight(const uint8_t* data, size_t len);

    // ── State ─────────────────────────────────────────────────────────────
    enum class State { IDLE, SCANNING, CONNECTING, CONNECTED };
    State           _state;
    NimBLEClient*   _client;
    NimBLEAddress   _targetAddr;
    bool            _hasTarget;
    uint32_t        _lastHeartbeat;
    uint32_t        _lastNotify;   // millis of last weight notification
    WeightCallback  _cb;

    NimBLERemoteCharacteristic* _charWrite;
    NimBLERemoteCharacteristic* _charNotify;

    // For notification handler (static trampoline)
    static BleAcaia* _instance;
    static void _notifyCallback(NimBLERemoteCharacteristic* ch,
                                uint8_t* data, size_t len, bool isNotify);
};
