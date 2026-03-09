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
#include "ui.h"
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
static const char *SYNC_QUEUE_PATH = "/sync_queue.txt";

// UI handling is now managed by ui.h/ui.cpp
static lv_obj_t *bg_panel = nullptr;

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

  if (strlen(WIFI_SSID) > 0) {
    ssidOut = WIFI_SSID;
    passOut = WIFI_PASS;
    return true;
  }
  return false;
}

static int readSyncQueue(String *out, int maxItems) {
  if (!out || maxItems <= 0)
    return 0;
  File f = FFat.open(SYNC_QUEUE_PATH, FILE_READ);
  if (!f)
    return 0;
  int count = 0;
  while (f.available() && count < maxItems) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      out[count++] = line;
    }
  }
  f.close();
  return count;
}

static bool writeSyncQueue(const String *items, int count) {
  if (FFat.exists(SYNC_QUEUE_PATH)) {
    FFat.remove(SYNC_QUEUE_PATH);
  }
  File f = FFat.open(SYNC_QUEUE_PATH, FILE_WRITE);
  if (!f)
    return false;
  for (int i = 0; i < count; ++i) {
    if (items[i].length() == 0)
      continue;
    f.println(items[i]);
  }
  f.close();
  return true;
}

static bool queueContains(const String *items, int count, const String &value) {
  for (int i = 0; i < count; ++i) {
    if (items[i] == value)
      return true;
  }
  return false;
}

static void enqueuePendingUpload(const String &path) {
  if (path.length() == 0)
    return;
  static String items[64];
  int count = readSyncQueue(items, 64);
  if (queueContains(items, count, path))
    return;
  if (count < 64) {
    items[count++] = path;
    if (!writeSyncQueue(items, count)) {
      Serial.println("[Cloud] ERROR: failed to persist sync queue");
    } else {
      Serial.printf("[Cloud] Queued for sync: %s\n", path.c_str());
    }
  } else {
    Serial.println("[Cloud] WARNING: sync queue full");
  }
}

static void processPendingUploads(bool updateUi) {
  static String items[64];
  static String keep[64];
  int count = readSyncQueue(items, 64);
  if (count <= 0)
    return;

  // Skip uploads entirely if no valid WiFi credentials are configured.
  String wifiSsid, wifiPass;
  if (!loadWifiCreds(wifiSsid, wifiPass)) {
    Serial.printf("[Cloud] No WiFi configured — skipping %d pending uploads\n",
                  count);
    return;
  }

  Serial.printf("[Cloud] Pending uploads: %d\n", count);
  if (updateUi) {
    ui_set_boot_status("Syncing pending data...", 0);
  }
  // Avoid BLE scan/connect radio contention during WiFi HTTPS uploads.
  gBle.pauseForWifi();

  int keepCount = 0;
  for (int i = 0; i < count; ++i) {
    const String &path = items[i];
    if (!FFat.exists(path)) {
      Serial.printf("[Cloud] Pending file missing, dropping: %s\n", path.c_str());
      continue;
    }
    int rc = gSession.uploadToGoogleSheet(path);
    if (rc == 1) {
      Serial.printf("[Cloud] Synced: %s\n", path.c_str());
    } else {
      keep[keepCount++] = path;
      Serial.printf("[Cloud] Keep pending (rc=%d): %s\n", rc, path.c_str());
    }
    // Only attempt ONE upload per boot — keep BLE connection fast.
    // Preserve remaining files for next boot.
    for (int j = i + 1; j < count && keepCount < 64; ++j) {
      keep[keepCount++] = items[j];
    }
    break;
  }
  writeSyncQueue(keep, keepCount);

  // Resume BLE scanning immediately so loop() doesn't have to wait.
  gBle.tick();
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
  ui_set_boot_status("BLE export:\nmount storage...", 0);
  Serial.println("[BLE-EXPORT] Enter mode");
  if (!FFat.begin(true)) {
    ui_set_boot_status("BLE export:\nFAT mount FAIL", 0);
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

  ui_set_boot_status("BLE Export Ready\nConnect from Mac as 'Logger'\nTap "
                     "screen to export all CSV",
                     0);
  Serial.println(
      "[BLE-EXPORT] Advertising as 'Logger'; tap screen to transfer");

  bool transferDone = false;
  while (true) {
    if (sWifiProvisionRequested) {
      sWifiProvisionRequested = false;
      int sep = sWifiProvisionPayload.indexOf('|');
      if (sep <= 0) {
        ui_set_boot_status("BLE export:\nBad WiFi payload", 0);
        bleExportSendError("WIFI format: WIFI:ssid|password");
      } else {
        String ssid = sWifiProvisionPayload.substring(0, sep);
        String pass = sWifiProvisionPayload.substring(sep + 1);
        ssid.trim();
        pass.trim();
        if (ssid.length() == 0) {
          ui_set_boot_status("BLE export:\nSSID required", 0);
          bleExportSendError("SSID required");
        } else {
          ensurePrefsOpen();
          if (!gPrefsOpen) {
            ui_set_boot_status("BLE export:\nNVS error", 0);
            bleExportSendError("NVS open failed");
          } else {
            gPrefs.putString(NVS_KEY_WIFI_SSID, ssid);
            gPrefs.putString(NVS_KEY_WIFI_PASS, pass);
            Serial.printf("[NVS] Saved WiFi SSID: %s\n", ssid.c_str());
            ui_set_boot_status("BLE export:\nSyncing time...", 0);
            gTimeSynced =
                syncTimeViaNtp(ssid.c_str(), pass.c_str(), WIFI_TIMEOUT_S);
            if (gTimeSynced) {
              bleExportSendPacket(0x7E, (const uint8_t *)"WIFI_OK_TIME_SYNCED",
                                  19);
              ui_set_boot_status("BLE export:\nWiFi saved\nTime synced", 0);
            } else {
              bleExportSendPacket(0x7E, (const uint8_t *)"WIFI_SAVED_TIME_FAIL",
                                  20);
              ui_set_boot_status("BLE export:\nWiFi saved\nTime sync failed",
                                 0);
            }
          }
        }
      }
      delay(900);
      ui_set_boot_status("BLE Export Ready\nConnect from Mac as 'Logger'\nTap "
                         "screen to export all CSV",
                         0);
    }

    if (sExportClientConnected && sExportTransferRequested && !transferDone) {
      ui_set_boot_status("BLE export:\ntransferring all CSV...", 0);
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

        ui_set_boot_status("Delete all exported CSV files?", 0);
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
              ui_set_boot_status("CSV Files Deleted", 100);
              delay(2000);
              FFat.end();
              ESP.restart();
            } else if (ty >= 216 && ty <= 272) { // NO button
              break;
            }
          }
          delay(40);
        }
        ui_set_boot_status("BLE export:\nDONE\nTap to exit", 0);
      } else {
        ui_set_boot_status("BLE export:\nFAILED\nTap to exit", 0);
      }
    }

    if (consumeTouchPress()) {
      if (!sExportClientConnected) {
        ui_set_boot_status("BLE Export Ready\nWaiting for Mac connection...",
                           0);
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

static volatile bool sWifiUiDone = false;
static volatile bool sWifiUiScanRequested = false;
static volatile bool sWifiUiConnectRequested = false;
static lv_obj_t *sWifiKeyboard = nullptr;
static lv_obj_t *sWifiSsidTa = nullptr;
static lv_obj_t *sWifiPassTa = nullptr;
static lv_obj_t *sWifiStatusLabel = nullptr;
static lv_obj_t *sWifiList = nullptr;
static char sWifiScanChoices[16][33];
static int sWifiScanChoiceCount = 0;

static void wifi_ta_focus_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_FOCUSED || !sWifiKeyboard)
    return;
  lv_obj_t *ta = lv_event_get_target(e);
  lv_keyboard_set_textarea(sWifiKeyboard, ta);
}

static void wifi_btn_scan_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    sWifiUiScanRequested = true;
}

static void wifi_btn_connect_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    sWifiUiConnectRequested = true;
}

static void wifi_btn_cancel_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    sWifiUiDone = true;
}

static void wifi_network_pick_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !sWifiSsidTa)
    return;
  const char *ssid = (const char *)lv_event_get_user_data(e);
  if (!ssid || strlen(ssid) == 0)
    return;
  lv_textarea_set_text(sWifiSsidTa, ssid);
  if (sWifiStatusLabel) {
    lv_label_set_text_fmt(sWifiStatusLabel, "Selected: %s", ssid);
  }
}

static void wifi_clear_scan_list() {
  if (!sWifiList)
    return;
  while (lv_obj_get_child_cnt(sWifiList) > 0) {
    lv_obj_del(lv_obj_get_child(sWifiList, 0));
  }
}

static void enterWifiSetupMode() {
  ensurePrefsOpen();
  char ssidBuf[64] = {0};
  char passBuf[64] = {0};
  if (gPrefsOpen) {
    String s = gPrefs.getString(NVS_KEY_WIFI_SSID, "");
    String p = gPrefs.getString(NVS_KEY_WIFI_PASS, "");
    strlcpy(ssidBuf, s.c_str(), sizeof(ssidBuf));
    strlcpy(passBuf, p.c_str(), sizeof(passBuf));
  }

  Serial.println("[WIFI-SETUP] Entering LVGL WiFi modal");

  sWifiUiDone = false;
  sWifiUiScanRequested = false;
  sWifiUiConnectRequested = false;
  sWifiScanChoiceCount = 0;

  lv_obj_t *modal = lv_obj_create(lv_layer_top());
  lv_obj_set_size(modal, 780, 470);
  lv_obj_center(modal);
  lv_obj_set_style_bg_color(modal, lv_color_hex(0x0F0F0F), 0);
  lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(modal, lv_color_hex(0x2A2A2A), 0);
  lv_obj_set_style_border_width(modal, 1, 0);
  lv_obj_set_style_radius(modal, 4, 0);
  lv_obj_set_style_pad_all(modal, 0, 0);

  lv_obj_t *title = lv_label_create(modal);
  lv_label_set_text(title, "WiFi Setup");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xF2F2F2), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 8);

  lv_obj_t *ssidLbl = lv_label_create(modal);
  lv_label_set_text(ssidLbl, "SSID");
  lv_obj_set_style_text_color(ssidLbl, lv_color_hex(0x808080), 0);
  lv_obj_align(ssidLbl, LV_ALIGN_TOP_LEFT, 18, 48);

  sWifiSsidTa = lv_textarea_create(modal);
  lv_obj_set_size(sWifiSsidTa, 740, 40);
  lv_obj_align(sWifiSsidTa, LV_ALIGN_TOP_LEFT, 18, 66);
  lv_textarea_set_one_line(sWifiSsidTa, true);
  lv_textarea_set_text(sWifiSsidTa, ssidBuf);
  lv_obj_add_event_cb(sWifiSsidTa, wifi_ta_focus_cb, LV_EVENT_FOCUSED, nullptr);

  lv_obj_t *passLbl = lv_label_create(modal);
  lv_label_set_text(passLbl, "Password");
  lv_obj_set_style_text_color(passLbl, lv_color_hex(0x808080), 0);
  lv_obj_align(passLbl, LV_ALIGN_TOP_LEFT, 18, 114);

  sWifiPassTa = lv_textarea_create(modal);
  lv_obj_set_size(sWifiPassTa, 740, 40);
  lv_obj_align(sWifiPassTa, LV_ALIGN_TOP_LEFT, 18, 132);
  lv_textarea_set_one_line(sWifiPassTa, true);
  lv_textarea_set_password_mode(sWifiPassTa, true);
  lv_textarea_set_text(sWifiPassTa, passBuf);
  lv_obj_add_event_cb(sWifiPassTa, wifi_ta_focus_cb, LV_EVENT_FOCUSED, nullptr);

  lv_obj_t *btnScan = lv_btn_create(modal);
  lv_obj_set_size(btnScan, 120, 36);
  lv_obj_align(btnScan, LV_ALIGN_TOP_LEFT, 18, 182);
  lv_obj_add_event_cb(btnScan, wifi_btn_scan_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *btnScanLbl = lv_label_create(btnScan);
  lv_label_set_text(btnScanLbl, "SCAN");
  lv_obj_center(btnScanLbl);

  lv_obj_t *btnConnect = lv_btn_create(modal);
  lv_obj_set_size(btnConnect, 160, 36);
  lv_obj_align(btnConnect, LV_ALIGN_TOP_LEFT, 150, 182);
  lv_obj_set_style_bg_color(btnConnect, lv_color_hex(UI_COLOR_GREEN), 0);
  lv_obj_set_style_text_color(btnConnect, lv_color_hex(0x0A0A0A), 0);
  lv_obj_add_event_cb(btnConnect, wifi_btn_connect_cb, LV_EVENT_CLICKED,
                      nullptr);
  lv_obj_t *btnConnLbl = lv_label_create(btnConnect);
  lv_label_set_text(btnConnLbl, "CONNECT");
  lv_obj_center(btnConnLbl);

  lv_obj_t *btnCancel = lv_btn_create(modal);
  lv_obj_set_size(btnCancel, 120, 36);
  lv_obj_align(btnCancel, LV_ALIGN_TOP_LEFT, 322, 182);
  lv_obj_add_event_cb(btnCancel, wifi_btn_cancel_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *btnCancelLbl = lv_label_create(btnCancel);
  lv_label_set_text(btnCancelLbl, "CANCEL");
  lv_obj_center(btnCancelLbl);

  sWifiList = lv_list_create(modal);
  lv_obj_set_size(sWifiList, 740, 110);
  lv_obj_align(sWifiList, LV_ALIGN_TOP_LEFT, 18, 226);
  lv_obj_set_style_bg_color(sWifiList, lv_color_hex(0x121212), 0);
  lv_obj_set_style_border_color(sWifiList, lv_color_hex(0x2A2A2A), 0);
  lv_obj_set_style_border_width(sWifiList, 1, 0);
  lv_obj_set_style_radius(sWifiList, 4, 0);
  lv_obj_set_style_pad_all(sWifiList, 4, 0);
  lv_obj_set_style_text_color(sWifiList, lv_color_hex(0xF2F2F2), 0);

  sWifiStatusLabel = lv_label_create(modal);
  lv_label_set_text(sWifiStatusLabel,
                    "Tap SCAN, then choose a network from the list");
  lv_obj_set_style_text_color(sWifiStatusLabel, lv_color_hex(0x808080), 0);
  lv_obj_set_width(sWifiStatusLabel, 740);
  lv_obj_align(sWifiStatusLabel, LV_ALIGN_TOP_LEFT, 18, 340);

  sWifiKeyboard = lv_keyboard_create(modal);
  lv_obj_set_size(sWifiKeyboard, 740, 118);
  lv_obj_align(sWifiKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(sWifiKeyboard, sWifiSsidTa);

  bool success = false;
  while (!sWifiUiDone) {
    lv_timer_handler();

    if (sWifiUiScanRequested) {
      sWifiUiScanRequested = false;
      lv_label_set_text(sWifiStatusLabel, "Scanning...");
      lv_timer_handler();

      gBle.pauseForWifi();
      WiFi.disconnect(true);
      WiFi.mode(WIFI_STA);
      WiFi.persistent(false);
      int n = WiFi.scanNetworks();
      wifi_clear_scan_list();
      sWifiScanChoiceCount = 0;
      if (n <= 0) {
        lv_label_set_text(sWifiStatusLabel, "No networks found");
      } else {
        for (int i = 0; i < n && sWifiScanChoiceCount < 16; i++) {
          String ssid = WiFi.SSID(i);
          if (ssid.length() == 0)
            continue;
          strlcpy(sWifiScanChoices[sWifiScanChoiceCount], ssid.c_str(),
                  sizeof(sWifiScanChoices[sWifiScanChoiceCount]));
          lv_obj_t *btn =
              lv_list_add_btn(sWifiList, LV_SYMBOL_WIFI,
                              sWifiScanChoices[sWifiScanChoiceCount]);
          lv_obj_add_event_cb(btn, wifi_network_pick_cb, LV_EVENT_CLICKED,
                              (void *)sWifiScanChoices[sWifiScanChoiceCount]);
          sWifiScanChoiceCount++;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "Found %d networks; tap one to select", n);
        lv_label_set_text(sWifiStatusLabel, msg);
      }
      WiFi.scanDelete();
    }

    if (sWifiUiConnectRequested) {
      sWifiUiConnectRequested = false;
      const char *ssid = lv_textarea_get_text(sWifiSsidTa);
      const char *pass = lv_textarea_get_text(sWifiPassTa);
      if (!ssid || strlen(ssid) == 0) {
        lv_label_set_text(sWifiStatusLabel, "SSID is required");
      } else {
        lv_label_set_text(sWifiStatusLabel, "Connecting...");
        lv_timer_handler();

        gBle.pauseForWifi();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.persistent(false);
        WiFi.begin(ssid, pass);

        uint32_t deadline = millis() + (uint32_t)WIFI_TIMEOUT_S * 1000UL;
        while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
          lv_timer_handler();
          delay(100);
        }

        if (WiFi.status() == WL_CONNECTED) {
          if (gPrefsOpen) {
            gPrefs.putString(NVS_KEY_WIFI_SSID, ssid);
            gPrefs.putString(NVS_KEY_WIFI_PASS, pass);
          }
          ui_set_boot_network(ssid);
          bool synced = syncTimeViaNtp(ssid, pass, WIFI_TIMEOUT_S);
          lv_label_set_text(sWifiStatusLabel,
                            synced ? "Saved + time synced"
                                   : "Saved (time sync failed)");
          success = true;
          sWifiUiDone = true;
        } else {
          lv_label_set_text(sWifiStatusLabel, "Connection failed");
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
        }
      }
    }

    delay(20);
  }

  lv_obj_del(modal);
  sWifiKeyboard = nullptr;
  sWifiSsidTa = nullptr;
  sWifiPassTa = nullptr;
  sWifiStatusLabel = nullptr;
  sWifiList = nullptr;
  ui_set_state(UIState::BOOT);
  if (success)
    delay(250);
}

static bool prepareExportFs() {
  ui_set_boot_status("Export: mount FAT...", 0);
  Serial.println("[Export] Mounting FFat");
  if (!FFat.begin(true)) {
    ui_set_boot_status("Export: FAT mount FAIL", 0);
    Serial.println("[Export] ERROR: FFat.begin(true) failed");
    return false;
  }

  // Marker file proves the volume is valid even before any CSV session exists.
  File marker = FFat.open("/EXPORT_OK.TXT", FILE_WRITE);
  if (!marker) {
    ui_set_boot_status("Export: marker FAIL", 0);
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
  ui_set_boot_status("Export: find FAT...", 0);
  Serial.println("[Export] Enter export mode");

  // Find the FAT partition by label (subtype = FAT in partitions_8MB.csv)
  sFatPartition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
  if (!sFatPartition) {
    ui_set_boot_status("No FAT partition!\nReflash device.", 0);
    delay(5000);
    ESP.restart();
    return;
  }
  ui_set_boot_status("Export: mount WL...", 0);
  esp_err_t err = wl_mount(sFatPartition, &sWlHandle);
  if (err != ESP_OK) {
    ui_set_boot_status("Export: WL mount FAIL", 0);
    Serial.printf("[Export] ERROR: wl_mount failed (%d)\n", (int)err);
    delay(5000);
    ESP.restart();
    return;
  }
  sWlSize = wl_size(sWlHandle);
  sWlSectorSize = wl_sector_size(sWlHandle);
  if (sWlSize == 0 || sWlSectorSize == 0 || (sWlSize % 512u) != 0u) {
    ui_set_boot_status("Export: geometry FAIL", 0);
    Serial.printf("[Export] ERROR: bad geometry size=%lu sector=%lu\n",
                  (unsigned long)sWlSize, (unsigned long)sWlSectorSize);
    delay(5000);
    teardownMscBackend();
    ESP.restart();
    return;
  }
  sMscWriteBuf = (uint8_t *)malloc(sWlSectorSize);
  if (!sMscWriteBuf) {
    ui_set_boot_status("No RAM for MSC!", 0);
    delay(5000);
    teardownMscBackend();
    ESP.restart();
    return;
  }

  // Configure and start USB MSC (enumerate before USB.begin())
  ui_set_boot_status("Export: start USB...", 0);
  sMSC.vendorID("UroFlow");    // max 8 chars
  sMSC.productID("Logger");    // max 16 chars
  sMSC.productRevision("1.0"); // max 4 chars
  sMSC.onRead(mscOnRead);
  sMSC.onWrite(mscOnWrite);
  sMSC.onStartStop(mscOnStartStop);
  sMSC.mediaPresent(true);
  bool mscOk = sMSC.begin(sWlSize / 512u, 512u);
  if (!mscOk) {
    ui_set_boot_status("Export: MSC begin FAIL", 0);
    Serial.println("[Export] ERROR: sMSC.begin failed");
    delay(5000);
    teardownMscBackend();
    ESP.restart();
    return;
  }
  bool usbOk = USB.begin();
  if (!usbOk) {
    ui_set_boot_status("Export: USB begin FAIL", 0);
    Serial.println("[Export] ERROR: USB.begin failed");
    delay(5000);
    teardownMscBackend();
    ESP.restart();
    return;
  }
  Serial.printf("[Export] USB MSC started, sectors=%lu\n",
                (unsigned long)(sWlSize / 512u));

  ui_set_boot_status("USB DRIVE MODE\nConnect to PC/Mac", 0);

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

// ── UI Interaction Callbacks
// ────────────────────────────────────────────────────────
// Deferred flags — enterWifiSetupMode/enterBleExportMode block in their own
// lv_timer_handler() loop, which cannot run inside an LVGL event callback
// (reentrant input reads are skipped). Setting a flag here lets the main
// loop() call them outside the callback context.
static volatile bool sDeferWifiSetup = false;
static volatile bool sDeferBleExport = false;
static volatile bool sForceBootScreen = false;
static uint32_t sManualConnectPromptUntilMs = 0;

static void onUiHome() {
  if (ui_get_state() == UIState::BOOT) {
    Serial.println("[UI] WiFi card clicked - WiFi Setup");
    sDeferWifiSetup = true;
    return;
  }

  Serial.println("[UI] Home clicked - showing BOOT screen");
  sForceBootScreen = true;
  if (gSession.isActive() || gSession.state() == Session::State::ENDED ||
      gSession.state() == Session::State::WAITING) {
    gSession.reset();
  }
}

static void onUiStart() {
  if (ui_get_state() == UIState::BOOT) {
    Serial.println("[UI] Begin New Measurement clicked");
    sForceBootScreen = false;
  }
  if (gBle.isConnected() && gSession.state() == Session::State::IDLE) {
    Serial.println("[UI] Start clicked - Forcing session");
    gSession.forceStart();
  } else if (!gBle.isConnected() && gSession.state() == Session::State::IDLE) {
    sForceBootScreen = true;
    sManualConnectPromptUntilMs = millis() + 3000;
    ui_set_boot_status("CONNECT SCALE TO START\nPLEASE TURN ON SCALE", 0);
  }
}

static void onUiEnd() {
  if (gSession.isActive()) {
    Serial.println("[UI] End clicked - Ending session");
    gSession.forceEnd();
  }
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

  // ── Build Base LVGL UI ───────────────────────────────────────────────────
  ui_init();
  ui_set_home_cb(onUiHome);
  ui_set_start_cb(onUiStart);
  ui_set_end_cb(onUiEnd);
  lv_timer_handler(); // Force an initial render pass

  // ── Initialise touch controller early (handled by Display::begin()) ──

  // ── FFat: mount early so we can show file count on boot screen ───────────
  // Partition label "ffat" matches partitions_8MB.csv (subtype=fat).
  // true = format on first use (creates FAT filesystem on blank partition).
  ui_set_boot_status("Mounting storage...", 10);
  if (!FFat.begin(true)) {
    ui_set_boot_status("Storage ERROR!", 0);
    Serial.println("[FS] FFat mount failed");
    while (true) {
      lv_timer_handler();
      delay(10);
    }
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
  if (storedWifiSsid.length() == 0 && strlen(WIFI_SSID) > 0) {
    storedWifiSsid = WIFI_SSID;
  }
  if (storedWifiSsid.length() > 0) {
    ui_set_boot_network(storedWifiSsid.c_str());
  }

  // ── WiFi NTP (fallback if CTS didn't sync time) ──────────────────────────
  // Credential source order: NVS (provisioned over BLE) -> config.h defaults.
  if (!gTimeSynced) {
    String wifiSsid;
    String wifiPass;
    bool haveWifi = loadWifiCreds(wifiSsid, wifiPass);
    if (haveWifi) {
      gTimeSynced =
          syncTimeViaNtp(wifiSsid.c_str(), wifiPass.c_str(), WIFI_TIMEOUT_S);
      Serial.printf("[NTP] %s\n", gTimeSynced ? "Time OK" : "Time FAILED");
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

  // ── Retry ONE pending cloud upload (limit to 1 to keep boot fast for BLE).
  processPendingUploads(true);

  // ── BLE Acaia client init ────────────────────────────────────────────────
  ui_set_boot_status("PREPARING TO CONNECT\nPLEASE TURN ON SCALE", 10);
  gBle.begin(onWeight, storedMac.length() >= 17 ? storedMac.c_str() : nullptr);

  gState = AppState::BLE_SCANNING;
  Serial.println("[Boot] Setup complete");
}

// ── loop()
// ────────────────────────────────────────────────────────────────────

static void restart_btn_cb(lv_event_t *e) {
  if (bg_panel) {
    lv_obj_del(bg_panel);
    bg_panel = nullptr;
  }
  gSession.reset();
}

void loop() {
  static bool endHandled = false;
  static uint32_t lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 1000) {
    lastHeartbeat = millis();
    Serial.print(".");
  }

  lv_task_handler();

  // Handle deferred mode entries (must run outside LVGL callbacks)
  if (sDeferWifiSetup) {
    sDeferWifiSetup = false;
    enterWifiSetupMode();
    return;
  }
  if (sDeferBleExport) {
    sDeferBleExport = false;
    enterBleExportMode();
    return;
  }

  gBle.tick();
  gSession.tick();

  // Update UI state based on system status
  if (gSession.state() == Session::State::UPLOAD) {
    ui_set_state(UIState::SYNCING);
  } else if (gSession.state() == Session::State::ENDED ||
             gSession.state() == Session::State::WAITING) {
    ui_set_state(UIState::SUCCESS);
  } else if (gSession.isActive()) {
    ui_set_state(UIState::ACTIVE);
    static uint32_t lastWeightUpdate = 0;
    if (millis() - lastWeightUpdate >= 200) {
      lastWeightUpdate = millis();
      ui_update_weight(gSession.lastWeight(), gSession.elapsedSeconds(),
                       gSession.weightRemovalCountdownSecs(),
                       (uint32_t)gSession.chartCount());
    }
  } else if (sForceBootScreen && gSession.state() == Session::State::IDLE) {
    ui_set_state(UIState::BOOT);
  } else if (gBle.isConnected()) {
    ui_set_state(UIState::READY);
  } else {
    ui_set_state(UIState::BOOT);
  }

  // Boot status tile behavior (including when BOOT is forced via Home):
  // countdown should animate only while disconnected+idle on BOOT screen.
  static bool bootConnectWindowStarted = false;
  static uint32_t bootConnectStartMs = 0;
  if (ui_get_state() == UIState::BOOT) {
    if (sManualConnectPromptUntilMs > millis()) {
      ui_set_boot_status("CONNECT SCALE TO START\nPLEASE TURN ON SCALE", 0);
      bootConnectWindowStarted = false;
      return;
    }
    if (!gBle.isConnected() && gSession.state() == Session::State::IDLE) {
      if (!bootConnectWindowStarted) {
        bootConnectWindowStarted = true;
        bootConnectStartMs = millis();
      }
      uint32_t elapsedS = (millis() - bootConnectStartMs) / 1000UL;
      int remaining = (elapsedS < 10U) ? (int)(10U - elapsedS) : 0;
      if (remaining > 0) {
        ui_set_boot_status("PREPARING TO CONNECT\nPLEASE TURN ON SCALE",
                           remaining);
      } else {
        ui_set_boot_status("UNABLE TO CONNECT SCALE\nRESTART SCALE", 0);
      }
    } else {
      bootConnectWindowStarted = false;
      ui_set_boot_status(gBle.isConnected() ? "SCALE CONNECTED"
                                            : "PREPARING TO CONNECT",
                         0);
    }
  } else {
    bootConnectWindowStarted = false;
  }

  // Handle upload pipeline exactly once per ended session:
  // queue locally and defer sync to next boot so UI remains responsive.
  if (gSession.state() == Session::State::ENDED && !endHandled) {
    String saved = gSession.lastSavedName();
    if (saved.length() > 0) {
      enqueuePendingUpload(saved);
      String wifiSsid, wifiPass;
      if (loadWifiCreds(wifiSsid, wifiPass)) {
        ui_set_sync_status("Data saved\nCloud sync on next boot", false);
      } else {
        ui_set_sync_status("Data saved locally\nConfigure WiFi to enable sync", false);
      }
    }
    gSession.acknowledgeEnded();
    endHandled = true;
  } else if (gSession.state() == Session::State::IDLE) {
    endHandled = false;
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
  delay(5);
}
