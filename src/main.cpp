#include <Arduino.h>
#include <FFat.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <USB.h>
#include <USBMSC.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <lvgl.h>
#include <wear_levelling.h>

#include "ble_acaia.h"
#include "ble_timesync.h"
#include "config.h"
#include "display.h"
#include "main.h"
#include "session.h"
#include "wifi_ntp.h"

// ── Globals
// ───────────────────────────────────────────────────────────────────

static AppState gState = AppState::BOOT;
static bool gTimeSynced = false;
static Display gDisplay;
static BleAcaia gBle;
static Session gSession;
static Preferences gPrefs;
static bool gPrefsOpen = false;

static uint32_t gLastDisplayMs = 0;
static const uint32_t DISPLAY_INTERVAL_MS = 200; // 5 Hz

// Touch handling is now managed by gDisplay using the GT911 driver.

// ── USB MSC export mode
// ─────────────────────────────────────────────────────── The device exposes
// the FAT partition as a read-only USB drive. Files are visible in Finder /
// Explorer without any special software. This is entered after a double-boot
// triggered by tapping the countdown screen.

static USBMSC sMSC;
static const esp_partition_t *sFatPartition = nullptr;
static wl_handle_t sWlHandle = WL_INVALID_HANDLE;
static size_t sWlSize = 0;
static size_t sWlSectorSize = 0;
static uint8_t *sMscWriteBuf = nullptr;

static void teardownMscBackend() {
  if (sWlHandle != WL_INVALID_HANDLE) {
    wl_unmount(sWlHandle);
    sWlHandle = WL_INVALID_HANDLE;
  }
  if (sMscWriteBuf) {
    free(sMscWriteBuf);
    sMscWriteBuf = nullptr;
  }
}

static int32_t mscOnRead(uint32_t lba, uint32_t offset, void *buf,
                         uint32_t bufsize) {
  if (sWlHandle == WL_INVALID_HANDLE)
    return -1;
  esp_err_t err =
      wl_read(sWlHandle, (uint32_t)lba * 512u + offset, buf, bufsize);
  return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

// Scratch buffer for read-modify-erase-write at WL erase-sector granularity.
// macOS writes metadata (.Spotlight-V100, .DS_Store, etc.) on every mount; if
// those writes are silently discarded the host reads back stale data, gets
// confused, and refuses to keep the volume mounted.  We do a real
// read-modify-erase-write so the host always sees a consistent filesystem.
static int32_t mscOnWrite(uint32_t lba, uint32_t offset, uint8_t *buf,
                          uint32_t bufsize) {
  if (sWlHandle == WL_INVALID_HANDLE || !sMscWriteBuf || sWlSectorSize == 0)
    return -1;
  uint32_t written = 0;
  while (written < bufsize) {
    uint32_t byteOff = (uint32_t)lba * 512u + offset + written;
    uint32_t blkBase = (byteOff / sWlSectorSize) * sWlSectorSize;
    uint32_t inBlk = byteOff - blkBase;
    uint32_t chunk = sWlSectorSize - inBlk;
    if (chunk > bufsize - written)
      chunk = bufsize - written;

    if (wl_read(sWlHandle, blkBase, sMscWriteBuf, sWlSectorSize) != ESP_OK)
      return -1;
    memcpy(sMscWriteBuf + inBlk, buf + written, chunk);
    if (wl_erase_range(sWlHandle, blkBase, sWlSectorSize) != ESP_OK)
      return -1;
    if (wl_write(sWlHandle, blkBase, sMscWriteBuf, sWlSectorSize) != ESP_OK)
      return -1;

    written += chunk;
  }
  return (int32_t)bufsize;
}

static bool mscOnStartStop(uint8_t /*power*/, bool /*start*/, bool /*eject*/) {
  return true;
}

// ── BLE Export-All mode
// ─────────────────────────────────────────────────────── Dedicated mode:
// device becomes a BLE peripheral and streams all CSV files. Packet format on
// TX notify characteristic:
//   0x01 + name_len(2) + file_size(4) + file_name bytes
//   0x02 + raw file chunk bytes
//   0x03 (end of current file)
//   0x04 + file_count(2) (all files done)
//   0x7F + UTF-8 error text

static const char *BLE_EXPORT_DEV_NAME = "Logger";
static const char *BLE_EXPORT_SVC_UUID = "7E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *BLE_EXPORT_TX_UUID =
    "7E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // notify
static const char *BLE_EXPORT_RX_UUID =
    "7E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // write

static NimBLECharacteristic *sExportTx = nullptr;
static volatile bool sExportClientConnected = false;
static volatile bool sExportTransferRequested = false;
static volatile bool sWifiProvisionRequested = false;
static String sWifiProvisionPayload = "";

class ExportServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer * /*server*/,
                 NimBLEConnInfo & /*connInfo*/) override {
    sExportClientConnected = true;
    Serial.println("[BLE-EXPORT] Client connected");
  }
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo & /*connInfo*/,
                    int /*reason*/) override {
    sExportClientConnected = false;
    sExportTransferRequested = false;
    Serial.println("[BLE-EXPORT] Client disconnected");
    server->startAdvertising();
  }
};

class ExportRxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr,
               NimBLEConnInfo & /*connInfo*/) override {
    std::string raw = chr->getValue();
    String cmd(raw.c_str());
    String upper = cmd;
    upper.toUpperCase();
    if (upper == "EXPORT_ALL" || upper == "START") {
      sExportTransferRequested = true;
      Serial.println("[BLE-EXPORT] START command received");
      return;
    }
    if (upper.startsWith("WIFI:")) {
      int sep = cmd.indexOf('|', 5);
      if (sep > 5) {
        sWifiProvisionPayload = cmd.substring(5);
        sWifiProvisionRequested = true;
      }
    }
  }
};

static ExportServerCallbacks sExportServerCbs;
static ExportRxCallbacks sExportRxCbs;

static bool consumeTouchPress() {
  int x, y;
  if (!gDisplay.getTouch(x, y))
    return false;
  delay(120);
  bool confirmed = gDisplay.getTouch(x, y);
  while (gDisplay.getTouch(x, y))
    delay(30); // wait for release
  return confirmed;
}

// Unified touch interface via gDisplay.getTouch()

static void ensurePrefsOpen() {
  if (!gPrefsOpen) {
    gPrefsOpen = gPrefs.begin(NVS_NAMESPACE, false);
    if (!gPrefsOpen) {
      Serial.println("[NVS] ERROR: gPrefs.begin failed");
    }
  }
}

static bool loadWifiCreds(String &ssidOut, String &passOut) {
  ensurePrefsOpen();
  if (gPrefsOpen) {
    ssidOut = gPrefs.getString(NVS_KEY_WIFI_SSID, "");
    passOut = gPrefs.getString(NVS_KEY_WIFI_PASS, "");
    if (ssidOut.length() > 0)
      return true;
  }

  if (strcmp(WIFI_SSID, "YourWiFiSSID") != 0) {
    ssidOut = WIFI_SSID;
    passOut = WIFI_PASS;
    return true;
  }
  return false;
}

static bool bleExportSendPacket(uint8_t type, const uint8_t *payload,
                                size_t payloadLen) {
  if (!sExportTx || !sExportClientConnected)
    return false;
  const size_t maxPayload = 180;
  if (payloadLen > maxPayload)
    return false;

  uint8_t pkt[1 + maxPayload];
  pkt[0] = type;
  if (payloadLen && payload)
    memcpy(pkt + 1, payload, payloadLen);
  sExportTx->setValue(pkt, payloadLen + 1);
  sExportTx->notify();
  delay(8); // pacing for phone BLE stacks
  return true;
}

static bool bleExportSendError(const char *msg) {
  size_t n = strlen(msg);
  if (n > 180)
    n = 180;
  return bleExportSendPacket(0x7F, reinterpret_cast<const uint8_t *>(msg), n);
}

static bool bleExportSendFile(File &f, const String &name) {
  uint8_t hdr[2 + 4 + 120];
  size_t nameLen = name.length();
  if (nameLen > 120)
    nameLen = 120;
  hdr[0] = (uint8_t)(nameLen & 0xFF);
  hdr[1] = (uint8_t)((nameLen >> 8) & 0xFF);
  uint32_t sz = (uint32_t)f.size();
  hdr[2] = (uint8_t)(sz & 0xFF);
  hdr[3] = (uint8_t)((sz >> 8) & 0xFF);
  hdr[4] = (uint8_t)((sz >> 16) & 0xFF);
  hdr[5] = (uint8_t)((sz >> 24) & 0xFF);
  memcpy(hdr + 6, name.c_str(), nameLen);
  if (!bleExportSendPacket(0x01, hdr, 6 + nameLen))
    return false;

  uint8_t chunk[180];
  while (true) {
    int n = f.read(chunk, sizeof(chunk));
    if (n < 0)
      return false;
    if (n == 0)
      break;
    if (!bleExportSendPacket(0x02, chunk, (size_t)n))
      return false;
    if (!sExportClientConnected)
      return false;
  }
  return bleExportSendPacket(0x03, nullptr, 0);
}

static bool bleExportAllCsv() {
  File root = FFat.open("/");
  if (!root || !root.isDirectory()) {
    bleExportSendError("FS root open failed");
    return false;
  }

  uint16_t fileCount = 0;
  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    String lower = name;
    lower.toLowerCase();
    bool isCsv = !f.isDirectory() && lower.endsWith(".csv");
    if (isCsv) {
      Serial.printf("[BLE-EXPORT] Sending %s (%lu bytes)\n", name.c_str(),
                    (unsigned long)f.size());
      if (!bleExportSendFile(f, name)) {
        f.close();
        root.close();
        bleExportSendError("Transfer interrupted");
        return false;
      }
      fileCount++;
    }
    f.close();
    if (!sExportClientConnected) {
      root.close();
      return false;
    }
    f = root.openNextFile();
  }
  root.close();

  uint8_t done[2] = {(uint8_t)(fileCount & 0xFF),
                     (uint8_t)((fileCount >> 8) & 0xFF)};
  Serial.printf("[BLE-EXPORT] Completed, files=%u\n", fileCount);
  return bleExportSendPacket(0x04, done, sizeof(done));
}

static void enterBleExportMode() {
  gDisplay.showBoot("BLE export:\nmount storage...");
  Serial.println("[BLE-EXPORT] Enter mode");
  if (!FFat.begin(true)) {
    gDisplay.showBoot("BLE export:\nFAT mount FAIL");
    Serial.println("[BLE-EXPORT] ERROR: FFat.begin(true) failed");
    delay(5000);
    ESP.restart();
    return;
  }

  // Marker file makes it obvious export mode initialized.
  File marker = FFat.open("/EXPORT_OK.TXT", FILE_WRITE);
  if (marker) {
    marker.printf("BLE export ready\nuptime_ms=%lu\n", (unsigned long)millis());
    marker.close();
  }

  NimBLEDevice::init(BLE_EXPORT_DEV_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&sExportServerCbs, false);
  NimBLEService *svc = server->createService(BLE_EXPORT_SVC_UUID);
  sExportTx =
      svc->createCharacteristic(BLE_EXPORT_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *rx =
      svc->createCharacteristic(BLE_EXPORT_RX_UUID, NIMBLE_PROPERTY::WRITE);
  rx->setCallbacks(&sExportRxCbs);
  svc->start();
  {
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_EXPORT_SVC_UUID);
    adv->enableScanResponse(
        true); // respond to scan requests so name appears in all scanners
    adv->start();
  }

  gDisplay.showBoot("BLE Export Ready\nConnect from Mac as 'Logger'\nTap "
                    "screen to export all CSV");
  Serial.println(
      "[BLE-EXPORT] Advertising as 'Logger'; tap screen to transfer");

  bool transferDone = false;
  while (true) {
    if (sWifiProvisionRequested) {
      sWifiProvisionRequested = false;
      int sep = sWifiProvisionPayload.indexOf('|');
      if (sep <= 0) {
        gDisplay.showBoot("BLE export:\nBad WiFi payload");
        bleExportSendError("WIFI format: WIFI:ssid|password");
      } else {
        String ssid = sWifiProvisionPayload.substring(0, sep);
        String pass = sWifiProvisionPayload.substring(sep + 1);
        ssid.trim();
        pass.trim();
        if (ssid.length() == 0) {
          gDisplay.showBoot("BLE export:\nSSID required");
          bleExportSendError("SSID required");
        } else {
          ensurePrefsOpen();
          if (!gPrefsOpen) {
            gDisplay.showBoot("BLE export:\nNVS error");
            bleExportSendError("NVS open failed");
          } else {
            gPrefs.putString(NVS_KEY_WIFI_SSID, ssid);
            gPrefs.putString(NVS_KEY_WIFI_PASS, pass);
            Serial.printf("[NVS] Saved WiFi SSID: %s\n", ssid.c_str());
            gDisplay.showBoot("BLE export:\nSyncing time...");
            gTimeSynced =
                syncTimeViaNtp(ssid.c_str(), pass.c_str(), WIFI_TIMEOUT_S);
            if (gTimeSynced) {
              bleExportSendPacket(0x7E, (const uint8_t *)"WIFI_OK_TIME_SYNCED",
                                  19);
              gDisplay.showBoot("BLE export:\nWiFi saved\nTime synced");
            } else {
              bleExportSendPacket(0x7E, (const uint8_t *)"WIFI_SAVED_TIME_FAIL",
                                  20);
              gDisplay.showBoot("BLE export:\nWiFi saved\nTime sync failed");
            }
          }
        }
      }
      delay(900);
      gDisplay.showBoot("BLE Export Ready\nConnect from Mac as 'Logger'\nTap "
                        "screen to export all CSV");
    }

    if (sExportClientConnected && sExportTransferRequested && !transferDone) {
      gDisplay.showBoot("BLE export:\ntransferring all CSV...");
      sExportTransferRequested = false;
      transferDone = bleExportAllCsv();

      if (transferDone) {
        // Count remaining CSV files for the delete prompt
        int exportedCount = 0;
        {
          File root = FFat.open("/");
          if (root && root.isDirectory()) {
            File f = root.openNextFile();
            while (f) {
              String nm = String(f.name());
              nm.toLowerCase();
              if (!f.isDirectory() && nm.endsWith(".csv"))
                exportedCount++;
              f.close();
              f = root.openNextFile();
            }
            root.close();
          }
        }

        gDisplay.showBleExportDeletePrompt(exportedCount);
        uint32_t promptStart = millis();
        while (millis() - promptStart < 15000) {
          int tx = 0, ty = 0;
          if (gDisplay.getTouch(tx, ty)) {
            delay(80);
            int dx, dy;
            while (gDisplay.getTouch(dx, dy))
              delay(20); // wait for release

            if (ty >= 130 && ty <= 206) { // YES button
              int deleted = 0;
              File root = FFat.open("/");
              if (root && root.isDirectory()) {
                File f = root.openNextFile();
                while (f) {
                  String nm = String(f.name());
                  String nmL = nm;
                  nmL.toLowerCase();
                  bool isCsv = !f.isDirectory() && nmL.endsWith(".csv");
                  f.close();
                  if (isCsv) {
                    String path = "/" + nm;
                    if (FFat.remove(path.c_str()))
                      deleted++;
                  }
                  f = root.openNextFile();
                }
                root.close();
              }
              Serial.printf("[BLE-EXPORT] Deleted %d CSV files\n", deleted);
              gDisplay.showBleExportDeleted(deleted);
              delay(2000);
              FFat.end();
              ESP.restart();
            } else if (ty >= 216 && ty <= 272) { // NO button
              break;
            }
          }
          delay(40);
        }
        gDisplay.showBoot("BLE export:\nDONE\nTap to exit");
      } else {
        gDisplay.showBoot("BLE export:\nFAILED\nTap to exit");
      }
    }

    if (consumeTouchPress()) {
      if (!sExportClientConnected) {
        gDisplay.showBoot("BLE Export Ready\nWaiting for Mac connection...");
        delay(500);
      } else if (!transferDone) {
        sExportTransferRequested = true;
      } else {
        FFat.end();
        ESP.restart();
      }
    }
    delay(40);
  }
}

// ── WiFi setup mode (on-screen keyboard)
// ────────────────────────────── User types SSID and password directly on the
// touchscreen using a full QWERTY keyboard.  No BLE needed.

static void enterWifiSetupMode() {
  ensurePrefsOpen();
  // Pre-fill from NVS if credentials already exist
  char ssidBuf[64] = {};
  char passBuf[64] = {};
  if (gPrefsOpen) {
    String s = gPrefs.getString(NVS_KEY_WIFI_SSID, "");
    String p = gPrefs.getString(NVS_KEY_WIFI_PASS, "");
    strlcpy(ssidBuf, s.c_str(), sizeof(ssidBuf));
    strlcpy(passBuf, p.c_str(), sizeof(passBuf));
  }

  int activeField = 0; // 0 = SSID, 1 = password
  bool shifted = false;
  gDisplay.drawWifiKeyboard(ssidBuf, passBuf, activeField, shifted);
  Serial.println("[WIFI-SETUP] On-screen keyboard active");

  while (true) {
    int tx, ty;
    if (!gDisplay.getTouch(tx, ty)) {
      delay(30);
      continue;
    }

    // Debounce: wait for release
    delay(80);
    int dx, dy;
    while (gDisplay.getTouch(dx, dy))
      delay(20);

    char key = gDisplay.mapWifiKeyTouch(tx, ty, shifted);
    if (key == 0)
      continue;

    char *buf = (activeField == 0) ? ssidBuf : passBuf;
    size_t maxLen = (activeField == 0) ? 32 : 63;
    size_t len = strlen(buf);

    switch (key) {
    case 0x1B: // BACK
      ESP.restart();
      return;

    case 0x02: // tap SSID field
      if (activeField != 0) {
        activeField = 0;
        gDisplay.updateWifiField(0, ssidBuf, true);
        gDisplay.updateWifiField(1, passBuf, false);
      }
      break;

    case 0x03: // tap password field
      if (activeField != 1) {
        activeField = 1;
        gDisplay.updateWifiField(0, ssidBuf, false);
        gDisplay.updateWifiField(1, passBuf, true);
      }
      break;

    case 0x01: // SHIFT
      shifted = !shifted;
      gDisplay.drawWifiKeys(shifted);
      break;

    case '\b': // backspace
      if (len > 0) {
        buf[len - 1] = '\0';
        gDisplay.updateWifiField(activeField, buf, true);
      }
      break;

    case '\n': { // CONNECT / GO
      if (strlen(ssidBuf) == 0) {
        gDisplay.updateWifiStatus("SSID required!", 0xF800);
        break;
      }
      // Save credentials
      if (gPrefsOpen) {
        gPrefs.putString(NVS_KEY_WIFI_SSID, ssidBuf);
        gPrefs.putString(NVS_KEY_WIFI_PASS, passBuf);
        Serial.printf("[NVS] Saved WiFi SSID: %s\n", ssidBuf);
      }
      gDisplay.updateWifiStatus("Connecting...", 0xFFE0);
      gTimeSynced = syncTimeViaNtp(ssidBuf, passBuf, WIFI_TIMEOUT_S);
      if (gTimeSynced) {
        gDisplay.updateWifiStatus("Time synced!", 0x07E0);
        Serial.println("[WIFI-SETUP] Connected & time synced");
      } else {
        gDisplay.updateWifiStatus("Saved (no sync)", 0xFFE0);
        Serial.println("[WIFI-SETUP] Saved but NTP sync failed");
      }
      delay(2000);
      ESP.restart();
      return;
    }

    default: // printable character
      if (len < maxLen) {
        buf[len] = key;
        buf[len + 1] = '\0';
        gDisplay.updateWifiField(activeField, buf, true);
        // Auto-disable shift after one character
        if (shifted) {
          shifted = false;
          gDisplay.drawWifiKeys(shifted);
        }
      }
      break;
    }
    delay(30);
  }
}

static bool prepareExportFs() {
  gDisplay.showBoot("Export: mount FAT...");
  Serial.println("[Export] Mounting FFat");
  if (!FFat.begin(true)) {
    gDisplay.showBoot("Export: FAT mount FAIL");
    Serial.println("[Export] ERROR: FFat.begin(true) failed");
    return false;
  }

  // Marker file proves the volume is valid even before any CSV session exists.
  File marker = FFat.open("/EXPORT_OK.TXT", FILE_WRITE);
  if (!marker) {
    gDisplay.showBoot("Export: marker FAIL");
    Serial.println("[Export] ERROR: cannot create /EXPORT_OK.TXT");
    FFat.end();
    return false;
  }
  marker.printf("UroFlow export ready\nuptime_ms=%lu\n",
                (unsigned long)millis());
  marker.close();
  FFat.end(); // release VFS before exposing same storage via USB MSC
  Serial.println("[Export] FAT ready, marker written");
  return true;
}

static void enterExportMode() {
  gDisplay.showBoot("Export: find FAT...");
  Serial.println("[Export] Enter export mode");

  // Find the FAT partition by label (subtype = FAT in partitions_8MB.csv)
  sFatPartition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
  if (!sFatPartition) {
    gDisplay.showBoot("No FAT partition!\nReflash device.");
    delay(5000);
    ESP.restart();
    return;
  }
  gDisplay.showBoot("Export: mount WL...");
  esp_err_t err = wl_mount(sFatPartition, &sWlHandle);
  if (err != ESP_OK) {
    gDisplay.showBoot("Export: WL mount FAIL");
    Serial.printf("[Export] ERROR: wl_mount failed (%d)\n", (int)err);
    delay(5000);
    ESP.restart();
    return;
  }
  sWlSize = wl_size(sWlHandle);
  sWlSectorSize = wl_sector_size(sWlHandle);
  if (sWlSize == 0 || sWlSectorSize == 0 || (sWlSize % 512u) != 0u) {
    gDisplay.showBoot("Export: geometry FAIL");
    Serial.printf("[Export] ERROR: bad geometry size=%lu sector=%lu\n",
                  (unsigned long)sWlSize, (unsigned long)sWlSectorSize);
    delay(5000);
    teardownMscBackend();
    ESP.restart();
    return;
  }
  sMscWriteBuf = (uint8_t *)malloc(sWlSectorSize);
  if (!sMscWriteBuf) {
    gDisplay.showBoot("No RAM for MSC!");
    delay(5000);
    teardownMscBackend();
    ESP.restart();
    return;
  }

  // Configure and start USB MSC (enumerate before USB.begin())
  gDisplay.showBoot("Export: start USB...");
  sMSC.vendorID("UroFlow");    // max 8 chars
  sMSC.productID("Logger");    // max 16 chars
  sMSC.productRevision("1.0"); // max 4 chars
  sMSC.onRead(mscOnRead);
  sMSC.onWrite(mscOnWrite);
  sMSC.onStartStop(mscOnStartStop);
  sMSC.mediaPresent(true);
  bool mscOk = sMSC.begin(sWlSize / 512u, 512u);
  if (!mscOk) {
    gDisplay.showBoot("Export: MSC begin FAIL");
    Serial.println("[Export] ERROR: sMSC.begin failed");
    delay(5000);
    teardownMscBackend();
    ESP.restart();
    return;
  }
  bool usbOk = USB.begin();
  if (!usbOk) {
    gDisplay.showBoot("Export: USB begin FAIL");
    Serial.println("[Export] ERROR: USB.begin failed");
    delay(5000);
    teardownMscBackend();
    ESP.restart();
    return;
  }
  Serial.printf("[Export] USB MSC started, sectors=%lu\n",
                (unsigned long)(sWlSize / 512u));

  gDisplay.showMscMode();

  // Wait: computer mounts drive, user copies files, tap screen to exit
  while (true) {
    int tx, ty;
    if (gDisplay.getTouch(tx, ty)) {
      delay(500);
      teardownMscBackend();
      ESP.restart();
    }
    delay(100);
  }
}

// ── BLE weight callback (called from BLE task context)
// ────────────────────────

static void onWeight(float weight_g, uint32_t t_ms) {
  gSession.onWeight(weight_g, t_ms);
}

// ── AppState mapping
// ──────────────────────────────────────────────────────────

static AppState deriveState() {
  if (gBle.isConnected()) {
    return gSession.isActive() ? AppState::SESSION_ACTIVE
                               : AppState::CONNECTED_IDLE;
  }
  return gBle.isScanning() ? AppState::BLE_SCANNING : AppState::BLE_CONNECTING;
}

// ── setup()
// ───────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  // Wait for Serial to initialize (native USB CDC takes a moment)
  uint32_t start = millis();
  while (!Serial && (millis() - start < 2000)) {
    delay(10);
  }
  Serial.println("\n\n=== BOOT START === v" FW_VERSION);

  // ── Display init (SPI, no USB dependency) ────────────────────────────────
  gDisplay.begin();

  // ── Build Base LVGL Test Screen ───────────────────────────────────────────
  lv_obj_t *bg = lv_obj_create(lv_scr_act());
  lv_obj_set_size(bg, 800, 480);
  lv_obj_set_style_bg_color(bg, lv_color_hex(0x1a1a1a), 0);

  lv_obj_t *lbl = lv_label_create(bg);
  lv_label_set_text(lbl, "UroFlow LVGL Base UI Active");
  lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);

  lv_obj_t *chart = lv_chart_create(bg);
  lv_obj_set_size(chart, 600, 300);
  lv_obj_align(chart, LV_ALIGN_CENTER, 0, 20);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_series_t *ser = lv_chart_add_series(
      chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_next_value(chart, ser, 10);
  lv_chart_set_next_value(chart, ser, 50);
  lv_chart_set_next_value(chart, ser, 30);
  lv_chart_set_next_value(chart, ser, 90);

  lv_timer_handler(); // Force an initial render pass

  // ── Initialise touch controller early (handled by Display::begin()) ──

  // ── FFat: mount early so we can show file count on boot screen ───────────
  // Partition label "ffat" matches partitions_8MB.csv (subtype=fat).
  // true = format on first use (creates FAT filesystem on blank partition).
  gDisplay.showBoot("Mounting storage...");
  if (!FFat.begin(true)) {
    gDisplay.showBoot("Storage ERROR!\nReflash device.");
    Serial.println("[FS] FFat mount failed");
    while (true)
      delay(1000);
  }
  Serial.printf("[FS] Free: %lu KB\n",
                (unsigned long)(FFat.totalBytes() - FFat.usedBytes()) / 1024);

  // Count CSV files for boot screen display
  int csvCount = 0;
  {
    File root = FFat.open("/");
    if (root && root.isDirectory()) {
      File f = root.openNextFile();
      while (f) {
        String nm = String(f.name());
        nm.toLowerCase();
        if (!f.isDirectory() && nm.endsWith(".csv"))
          csvCount++;
        f.close();
        f = root.openNextFile();
      }
      root.close();
    }
  }
  Serial.printf("[FS] CSV files: %d\n", csvCount);

  // ── NVS open early (needed for WiFi SSID display on boot screen) ─────────
  ensurePrefsOpen();
  String storedWifiSsid =
      gPrefsOpen ? gPrefs.getString(NVS_KEY_WIFI_SSID, "") : "";

  // ── Boot countdown (10 seconds) ───────────────────────────────────────────
  // NOTE: bleTimeSync_start/stop temporarily disabled — dual-role NimBLE
  // (GATT server + BLE client) crashes on IDF 5.x; re-enable once stable.
  // bleTimeSync_start();

  for (int remaining = 10; remaining >= 0; remaining--) {
    gDisplay.showBootCountdown(remaining, gTimeSynced, csvCount,
                               storedWifiSsid.length() ? storedWifiSsid.c_str()
                                                       : nullptr);
    uint32_t tickStart = millis();
    while (millis() - tickStart < 1000) {
      int tx = 0, ty = 0;
      if (gDisplay.getTouch(tx, ty)) {
        delay(120); // debounce
        int dummyX, dummyY;
        while (gDisplay.getTouch(dummyX, dummyY))
          delay(30);

        if (ty >= 105 && ty <= 180) {
          // BLE EXPORT button (800x480 zone)
          FFat.end(); // release before BLE export mode remounts
          gDisplay.showBoot("Opening BLE export...");
          delay(250);
          enterBleExportMode(); // does not return
          return;
        } else if (ty >= 190 && ty <= 260) {
          // WIFI SETUP button (800x480 zone)
          FFat.end();
          enterWifiSetupMode(); // does not return
          return;
        }
      }
      delay(50);
    }
    if (remaining == 0)
      break;
  }

  // bleTimeSync_stop();

  // ── WiFi NTP (fallback if CTS didn't sync time) ──────────────────────────
  // Credential source order: NVS (provisioned over BLE) -> config.h defaults.
  if (!gTimeSynced) {
    String wifiSsid;
    String wifiPass;
    bool haveWifi = loadWifiCreds(wifiSsid, wifiPass);
    if (haveWifi) {
      gDisplay.showBoot("Syncing time...");
      gTimeSynced =
          syncTimeViaNtp(wifiSsid.c_str(), wifiPass.c_str(), WIFI_TIMEOUT_S);
      gDisplay.showBoot(gTimeSynced ? "Time OK"
                                    : "Time FAILED\n(sequential names)");
      delay(600);
    } else {
      Serial.println("[NTP] No WiFi credentials provisioned");
    }
  }

  // ── NVS — load stored MAC and session counter ─────────────────────────────
  String storedMac = gPrefs.getString(NVS_KEY_MAC, "");
  uint32_t seqNum = gPrefs.getUInt(NVS_KEY_SEQNUM, 0);
  Serial.printf("[NVS] Stored MAC: \"%s\", SeqNum: %lu\n", storedMac.c_str(),
                (unsigned long)seqNum);

  // ── Session init ─────────────────────────────────────────────────────────
  gSession.begin(gTimeSynced, seqNum);

  // ── BLE Acaia client init ────────────────────────────────────────────────
  gDisplay.showBoot("Scanning for\nAcaia scale...");
  gBle.begin(onWeight, storedMac.length() >= 17 ? storedMac.c_str() : nullptr);

  gState = AppState::BLE_SCANNING;
  Serial.println("[Boot] Setup complete");
}

// ── loop()
// ────────────────────────────────────────────────────────────────────

static lv_obj_t *bg_panel = nullptr;

static void restart_btn_cb(lv_event_t *e) {
  if (bg_panel) {
    lv_obj_del(bg_panel);
    bg_panel = nullptr;
  }
  gSession.reset();
}

void loop() {
  lv_task_handler();

  gBle.tick();
  gSession.tick();

  // ── Post-session: upload and wait for restart ─────────────────────
  // We use LVGL to render an opaque overlay.
  if (gSession.state() == Session::State::ENDED) {
    bg_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bg_panel, 800, 480);
    lv_obj_align(bg_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(bg_panel, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(bg_panel, LV_OPA_COVER, 0); // Opaque to hide chart
    lv_obj_set_style_border_width(bg_panel, 0, 0);

    lv_obj_t *status_label = lv_label_create(bg_panel);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_24, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -40);

    String savedName = gSession.lastSavedName();
    bool measurementSaved = savedName.length() > 0;

    if (measurementSaved) {
      lv_label_set_text(status_label, "Cloud Syncing...");
      lv_task_handler(); // Update screen before blocking

      int status = gSession.uploadToGoogleSheet(savedName);

      if (status == 1) {
        lv_label_set_text(status_label, "Cloud Sync OK");
      } else {
        lv_label_set_text_fmt(status_label, "Cloud Sync FAIL (Code: %d)",
                              status);
      }
    } else {
      lv_label_set_text(status_label, "Measurement too short to save.");
    }

    lv_obj_t *btn = lv_btn_create(bg_panel);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x007BFF), 0);
    lv_obj_set_size(btn, 300, 70);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Start New Measurement");
    lv_obj_center(btn_label);

    lv_obj_add_event_cb(btn, restart_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_task_handler(); // Process one more time to show updated status

    gSession.acknowledgeEnded();
  }

  // Persist MAC once connected
  static bool macSaved = false;
  if (!macSaved && gBle.isConnected()) {
    char mac[40];
    gBle.getLastMac(mac, sizeof(mac));
    if (strlen(mac) >= 17) {
      gPrefs.putString(NVS_KEY_MAC, mac);
      macSaved = true;
      Serial.printf("[NVS] Saved MAC: %s\n", mac);
    }
  }

  // Persist sequence counter when it changes
  static uint32_t lastSavedSeq = UINT32_MAX;
  uint32_t currentSeq = gSession.seqNum();
  if (currentSeq != lastSavedSeq) {
    gPrefs.putUInt(NVS_KEY_SEQNUM, currentSeq);
    lastSavedSeq = currentSeq;
    Serial.printf("[NVS] Saved SeqNum: %lu\n", (unsigned long)currentSeq);
  }

  gState = deriveState();

  if (millis() - gLastDisplayMs >= DISPLAY_INTERVAL_MS) {
    gLastDisplayMs = millis();
    // Legacy display primitive drawing disabled — LVGL now owns the screen
  }

  delay(5);
}
