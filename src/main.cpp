#include <Arduino.h>
#include <FFat.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <USB.h>
#include <USBMSC.h>
#include <Wire.h>
#include <esp_partition.h>
#include <esp_sleep.h>
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
  void onConnect(NimBLEServer * /*server*/) override {
    sExportClientConnected = true;
    Serial.println("[BLE-EXPORT] Client connected");
  }
  void onDisconnect(NimBLEServer *server) override {
    sExportClientConnected = false;
    sExportTransferRequested = false;
    Serial.println("[BLE-EXPORT] Client disconnected");
    server->startAdvertising();
  }
};

class ExportRxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr) override {
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
    adv->setScanResponse(
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

// ── WiFi setup mode (on-device BLE provisioning)
// ────────────────────────────── Advertises as "Logger" with the same UUIDs as
// BLE export mode so the same ble_export_receive.py --wifi-ssid tool can
// provision credentials.

static void enterWifiSetupMode() {
  ensurePrefsOpen();
  String storedSsid = gPrefsOpen ? gPrefs.getString(NVS_KEY_WIFI_SSID, "") : "";

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
    adv->setScanResponse(
        true); // respond to scan requests so name appears in all scanners
    adv->start();
  }

  gDisplay.showWifiSetup(gTimeSynced,
                         storedSsid.length() ? storedSsid.c_str() : nullptr);
  Serial.println(
      "[WIFI-SETUP] Advertising as 'Logger'; awaiting WIFI:ssid|pass");

  while (true) {
    if (sWifiProvisionRequested) {
      sWifiProvisionRequested = false;
      int sep = sWifiProvisionPayload.indexOf('|');
      if (sep <= 0) {
        gDisplay.showBoot("WiFi setup:\nBad payload format");
      } else {
        String ssid = sWifiProvisionPayload.substring(0, sep);
        String pass = sWifiProvisionPayload.substring(sep + 1);
        ssid.trim();
        pass.trim();
        if (ssid.length() == 0) {
          gDisplay.showBoot("WiFi setup:\nSSID required");
        } else {
          if (gPrefsOpen) {
            gPrefs.putString(NVS_KEY_WIFI_SSID, ssid);
            gPrefs.putString(NVS_KEY_WIFI_PASS, pass);
            Serial.printf("[NVS] Saved WiFi SSID: %s\n", ssid.c_str());
          }
          gDisplay.showBoot("WiFi setup:\nSyncing time...");
          gTimeSynced =
              syncTimeViaNtp(ssid.c_str(), pass.c_str(), WIFI_TIMEOUT_S);
          if (gTimeSynced) {
            bleExportSendPacket(0x7E, (const uint8_t *)"WIFI_OK_TIME_SYNCED",
                                19);
            gDisplay.showBoot("WiFi setup:\nSaved & time synced!\nTap to exit");
          } else {
            bleExportSendPacket(0x7E, (const uint8_t *)"WIFI_SAVED_TIME_FAIL",
                                20);
            gDisplay.showBoot("WiFi setup:\nSaved, sync failed\nTap to exit");
          }
          delay(2000);
          ESP.restart();
        }
      }
      delay(900);
      gDisplay.showWifiSetup(
          gTimeSynced, storedSsid.length() ? storedSsid.c_str() : nullptr);
    }

    if (consumeTouchPress()) {
      ESP.restart();
    }
    delay(40);
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
  Serial.begin(
      115200); // UART0 (GPIO43/44) — not visible via USB in CDC_ON_BOOT=0 mode

  // ── Display init (SPI, no USB dependency) ────────────────────────────────
  gDisplay.begin();

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

void loop() {
  gBle.tick();
  gSession.tick();

  // If connected but user never starts a measurement, sleep after 30s.
  static uint32_t connectedIdleStartMs = 0;
  if (gBle.isConnected() && gSession.state() == Session::State::IDLE) {
    if (connectedIdleStartMs == 0)
      connectedIdleStartMs = millis();
    if (millis() - connectedIdleStartMs >= CONNECT_TO_WEIGHT_TIMEOUT_MS) {
      gDisplay.showBoot("No weight in 90s.\nSleeping...");
      delay(1200);
      digitalWrite(TFT_BL_PIN, LOW);
      Serial.println("[Sleep] No measurement started, entering deep sleep");
      Serial.flush();
      esp_deep_sleep_start();
    }
  } else {
    connectedIdleStartMs = 0;
  }

  // ── Post-session: show success screen then deep sleep ─────────────────────
  // Checked before deriveState() so we never try to render a normal screen
  // against an ENDED session.  Deep sleep acts like a hard reset — the next
  // RESET or power cycle runs setup() fresh.
  if (gSession.state() == Session::State::ENDED) {
    gDisplay.showSessionComplete(gSession.endedRowCount(),
                                 gSession.endedDurationMs());
    delay(2000); // Show checkmark for 2 seconds

    // Trigger Cloud Upload (Google Sheets)
    gDisplay.showBoot("Cloud Syncing...");
    String savedName = gSession.lastSavedName();
    int status = gSession.uploadToGoogleSheet(savedName);

    if (status == 1) {
      gDisplay.showBoot("Cloud Sync OK");
      delay(2000);
    } else {
      char failMsg[64];
      if (status == 0) {
        snprintf(failMsg, sizeof(failMsg), "Cloud Sync FAIL\n(WiFi Timeout)");
      } else {
        snprintf(failMsg, sizeof(failMsg), "Cloud Sync FAIL\n(Code: %d)",
                 status);
      }
      gDisplay.showBoot(failMsg);
      delay(6000); // Extra time to read the error code
    }

    // After success checkmark, show summary before sleep
    gDisplay.showBoot("Measurement Saved");
    delay(1500);

    digitalWrite(TFT_BL_PIN, LOW); // backlight off
    Serial.println("[Sleep] Entering deep sleep");
    Serial.flush();
    esp_deep_sleep_start(); // no wakeup source — RESET to wake
                            // never returns
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
    gDisplay.update(gState, gSession.lastWeight(), gSession.isActive(),
                    gSession.elapsedMs(), gSession.rowCount(), gTimeSynced,
                    gSession.chartData(), gSession.chartCount(),
                    gSession.chartHead(),
                    gSession.weightRemovalCountdownSecs());
  }

  delay(10);
}
