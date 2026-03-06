#include "ble_acaia.h"
#include "config.h"
#include <string.h>

BleAcaia *BleAcaia::_instance = nullptr;

// ── Construction
// ──────────────────────────────────────────────────────────────

BleAcaia::BleAcaia()
    : _state(State::IDLE), _client(nullptr), _targetAddr(), _hasTarget(false),
      _lastHeartbeat(0), _lastNotify(0), _charWrite(nullptr),
      _charNotify(nullptr) {}

// ── Public API
// ────────────────────────────────────────────────────────────────

void BleAcaia::begin(WeightCallback cb, const char *storedMac) {
  _cb = cb;
  _instance = this;

  NimBLEDevice::init("PearlsLogger");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  if (storedMac && strlen(storedMac) >= 17) {
    Serial.printf("[BLE] Trying stored MAC: %s\n", storedMac);
    _targetAddr = NimBLEAddress(std::string(storedMac), BLE_ADDR_PUBLIC);
    _hasTarget = true;
    _state = State::CONNECTING;
  } else {
    _startScan();
  }
}

void BleAcaia::tick() {
  switch (_state) {
  case State::IDLE:
    _startScan();
    break;

  case State::SCANNING:
    // NimBLE scan runs in background; results come via onResult()
    break;

  case State::CONNECTING:
    if (!_hasTarget) {
      _startScan();
      return;
    }
    if (!_connectTo(_targetAddr)) {
      Serial.println("[BLE] Connect failed, restarting scan");
      _hasTarget = false;
      _state = State::IDLE;
    }
    break;

  case State::CONNECTED:
    // Heartbeat
    if (millis() - _lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
      _sendHeartbeat();
      _lastHeartbeat = millis();
    }
    break;
  }
}

bool BleAcaia::isConnected() const {
  return _state == State::CONNECTED && _client != nullptr &&
         _client->isConnected();
}

bool BleAcaia::isScanning() const {
  return _state == State::SCANNING || _state == State::CONNECTING;
}

std::string BleAcaia::getMac() const {
  if (_client && _client->isConnected())
    return _client->getPeerAddress().toString();
  return _hasTarget ? _targetAddr.toString() : "";
}

void BleAcaia::getLastMac(char *out_buf, size_t len) const {
  std::string mac = getMac();
  strlcpy(out_buf, mac.c_str(), len);
}

// ── NimBLE scan callback
// ──────────────────────────────────────────────────────

void BleAcaia::onResult(const NimBLEAdvertisedDevice *device) {
  std::string name = device->getName();
  // Match first 5 chars against known Acaia scale name prefixes
  std::string prefix = name.substr(0, 5);
  for (auto &c : prefix)
    c = toupper(c);

  if (prefix == "ACAIA" || prefix == "PEARL" || prefix == "LUNAR" ||
      prefix == "PYXIS" || prefix == "CINCO" || prefix == "PROCH" ||
      prefix == "BOOKO") {
    Serial.printf("[BLE] Found scale: %s [%s]\n", name.c_str(),
                  device->getAddress().toString().c_str());
    NimBLEDevice::getScan()->stop();
    _targetAddr = device->getAddress();
    _hasTarget = true;
    _state = State::CONNECTING;
  }
}

// ── NimBLE client callbacks
// ───────────────────────────────────────────────────

void BleAcaia::onConnect(NimBLEClient *client) {
  Serial.println("[BLE] Connected to scale");
}

void BleAcaia::onDisconnect(NimBLEClient *client, int reason) {
  Serial.println("[BLE] Disconnected, will reconnect");
  _charWrite = nullptr;
  _charNotify = nullptr;
  _state = State::CONNECTING; // attempt direct reconnect first
}

// ── Internal helpers
// ──────────────────────────────────────────────────────────

void BleAcaia::_startScan() {
  Serial.println("[BLE] Starting scan...");
  _state = State::SCANNING;
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(this, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(0, false); // scan indefinitely
}

bool BleAcaia::_connectTo(NimBLEAddress addr) {
  _state = State::CONNECTING;

  if (_client == nullptr) {
    _client = NimBLEDevice::createClient();
    _client->setClientCallbacks(this, false);
    _client->setConnectionParams(12, 12, 0, 51);
    _client->setConnectTimeout(10);
  }

  Serial.printf("[BLE] Connecting to %s...\n", addr.toString().c_str());

  // Stop scanning and wait a moment for the radio to settle
  NimBLEDevice::getScan()->stop();
  delay(100);

  if (!_client->connect(addr)) {
    Serial.println("[BLE] connect() returned false");
    return false;
  }

  _subscribeAndIdentify();
  return true;
}
void BleAcaia::_subscribeAndIdentify() {
  Serial.println("[BLE] _subscribeAndIdentify start");
  Serial.println("[BLE] getService start");
  NimBLERemoteService *svc =
      _client->getService(NimBLEUUID(ACAIA_SERVICE_UUID));
  Serial.printf("[BLE] getService done, svc=%p\n", svc);
  if (svc) {
    _charWrite = svc->getCharacteristic(NimBLEUUID(ACAIA_CHAR_WRITE));
    _charNotify = svc->getCharacteristic(NimBLEUUID(ACAIA_CHAR_NOTIFY));
    Serial.printf("[BLE] chars: write=%p, notify=%p\n", _charWrite,
                  _charNotify);
  }

  // Fallback: enumerate all services if the expected UUID wasn't found
  if (!_charWrite || !_charNotify) {
    Serial.println("[BLE] Primary service not found — scanning all services");
    const auto &services = _client->getServices(true);
    for (auto &s : services) {
      Serial.printf("[BLE] Checking service %s\n",
                    s->getUUID().toString().c_str());
      auto *cw = s->getCharacteristic(NimBLEUUID(ACAIA_CHAR_WRITE));
      auto *cn = s->getCharacteristic(NimBLEUUID(ACAIA_CHAR_NOTIFY));
      if (cw && cn) {
        _charWrite = cw;
        _charNotify = cn;
        break;
      }
    }
  }

  if (!_charWrite || !_charNotify) {
    Serial.println("[BLE] Required characteristics not found");
    _client->disconnect();
    return;
  }

  // Subscribe to notifications
  if (_charNotify && _client->isConnected()) {
    if (!_charNotify->subscribe(true, _notifyCallback)) {
      Serial.println("[BLE] Subscribe failed");
      _client->disconnect();
      return;
    }
  } else {
    Serial.println("[BLE] Cannot subscribe — disconnected or null char");
    return;
  }

  delay(300);

  // Send identification (new Acaia protocol, type 0x0B)
  if (_charWrite && _client->isConnected()) {
    static const uint8_t identCmd[20] = {
        0xef, 0xdd, 0x0b, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x9a, 0x6d};
    _charWrite->writeValue(identCmd, sizeof(identCmd), false);
  }

  delay(300);

  // Request weight notifications (new Acaia protocol, type 0x0C)
  if (_charWrite && _client->isConnected()) {
    static const uint8_t notifCmd[14] = {0xef, 0xdd, 0x0c, 0x09, 0x00,
                                         0x01, 0x01, 0x02, 0x02, 0x05,
                                         0x03, 0x04, 0x15, 0x06};
    _charWrite->writeValue(notifCmd, sizeof(notifCmd), false);
  }

  _state = State::CONNECTED;
  _lastHeartbeat = millis();
  Serial.println("[BLE] Scale ready — receiving weight notifications");
}

void BleAcaia::_sendMessage(uint8_t msgType, const uint8_t *payload,
                            size_t payloadLen) {
  // Frame: [0xEF, 0xDD, msgType, ...payload, cksum1, cksum2]
  std::vector<uint8_t> msg;
  msg.reserve(3 + payloadLen + 2);
  msg.push_back(0xEF);
  msg.push_back(0xDD);
  msg.push_back(msgType);
  for (size_t i = 0; i < payloadLen; i++)
    msg.push_back(payload[i]);

  uint8_t ck1 = 0, ck2 = 0;
  for (uint8_t b : msg) {
    ck1 = (ck1 + b) & 0xFF;
    ck2 = (ck2 + ck1) & 0xFF;
  }
  msg.push_back(ck1);
  msg.push_back(ck2);

  if (_charWrite)
    _charWrite->writeValue(msg.data(), msg.size(), false);
}

void BleAcaia::_sendHeartbeat() {
  static const uint8_t hb[7] = {0xef, 0xdd, 0x00, 0x02, 0x00, 0x02, 0x00};
  if (_charWrite)
    _charWrite->writeValue(hb, sizeof(hb), false);
}

float BleAcaia::_decodeWeight(const uint8_t *data, size_t len) {
  if (len < 6)
    return -1.0f;
  if (data[0] != 0xEF || data[1] != 0xDD)
    return -1.0f;

  // New Acaia protocol (Pearl S, newer Lunar, Pyxis): 13 or 17-byte packet,
  // byte[4] == 0x05 signals a weight event.
  if ((len == 13 || len == 17) && data[4] == 0x05) {
    uint16_t raw = ((uint16_t)(data[6] & 0xFF) << 8) | (data[5] & 0xFF);
    // data[9] = decimal scale factor (0→÷1, 1→÷10, 2→÷100)
    static const float divisors[] = {1.0f, 10.0f, 100.0f};
    float divisor = (data[9] < 3) ? divisors[data[9]] : 10.0f;
    bool negative = (data[10] & 0x02) != 0;
    float weight = (float)raw / divisor;
    return negative ? -weight : weight;
  }

  return -1.0f;
}

// ── Static notification trampoline
// ────────────────────────────────────────────

void BleAcaia::_notifyCallback(NimBLERemoteCharacteristic * /*ch*/,
                               uint8_t *data, size_t len, bool /*isNotify*/) {
  if (!_instance)
    return;

  float weight = _decodeWeight(data, len);
  if (weight < 0.0f)
    return;

  _instance->_lastNotify = millis();
  if (_instance->_cb) {
    _instance->_cb(weight, millis());
  }
}
