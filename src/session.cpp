#include "session.h"
#include "config.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

Session::Session()
    : _state(State::IDLE), _sessionStartMs(0), _sessionStartEpoch(0),
      _lastWeightMs(0), _lastWeight(0.0f), _prevRawWeight(0.0f),
      _cumulativeWeight(0.0f), _rowCount(0), _hasRealTime(false), _seqNum(0),
      _lastFlushMs(0), _weightBelowThresholdMs(0), _endedRowCount(0),
      _endedDurationMs(0), _chartHead(0), _chartCount(0) {}

void Session::begin(bool hasRealTime, uint32_t seqNum) {
  _hasRealTime = hasRealTime;
  _seqNum = seqNum;

  // Remove any leftover tmp file from a previous crash
  if (FFat.exists("/current.tmp")) {
    FFat.remove("/current.tmp");
    Serial.println("[Session] Removed stale /current.tmp");
  }
}

void Session::onWeight(float weight_g, uint32_t /*t_ms_abs*/) {
  _lastWeight = weight_g;
  _lastWeightMs = millis();

  if (_state == State::IDLE) {
    // Start logging only when a meaningful weight is placed on the scale.
    // This avoids noise/near-zero packets starting a session immediately.
    if (weight_g >= WEIGHT_REMOVAL_THRESHOLD_G) {
      _startSession();
    } else {
      return;
    }
  }

  if (_state != State::ACTIVE)
    return;

  // Track weight-removal: if reading is below the "empty scale" threshold,
  // record when it first went below; reset timer when weight returns above it.
  if (weight_g < WEIGHT_REMOVAL_THRESHOLD_G) {
    if (_weightBelowThresholdMs == 0 &&
        _cumulativeWeight > WEIGHT_REMOVAL_THRESHOLD_G)
      _weightBelowThresholdMs = millis();
  } else {
    _weightBelowThresholdMs = 0;
    // Tracking cumulative weight by summing positive deltas since the last
    // sample. This is robust to container taring/removal mid-session.
    float delta = weight_g - _prevRawWeight;
    if (delta > 0.05f) { // 50mg noise floor
      _cumulativeWeight += delta;
    }
  }
  _prevRawWeight = weight_g;

  uint32_t t_ms = millis() - _sessionStartMs;

  // Append to chart ring buffer
  int slot = (_chartHead + _chartCount) % CHART_BUF_SIZE;
  if (_chartCount < CHART_BUF_SIZE) {
    _chartCount++;
  } else {
    // Buffer full — advance head (overwrites oldest)
    _chartHead = (_chartHead + 1) % CHART_BUF_SIZE;
  }
  _chart[slot].t_ms = t_ms;
  _chart[slot].weight_g = weight_g;

  // Build CSV row: t_ms since session start, UTC timestamp, weight, event
  char ts[32] = "";
  if (_hasRealTime && _sessionStartEpoch > 0) {
    time_t rowEpoch = _sessionStartEpoch + (time_t)(t_ms / 1000UL);
    struct tm tmUtc;
    gmtime_r(&rowEpoch, &tmUtc);
    snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
             tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);
  }
  char row[96];
  snprintf(row, sizeof(row), "%lu,%s,%.1f,%.1f,\n", (unsigned long)t_ms, ts,
           weight_g, _cumulativeWeight);
  _writeBuf += row;
  _rowCount++;

  // Flush every LOG_FLUSH_INTERVAL_MS
  if (millis() - _lastFlushMs >= LOG_FLUSH_INTERVAL_MS) {
    _flushBuffer();
  }
}

void Session::tick() {
  if (_state != State::ACTIVE)
    return;

  // 15-second inactivity timeout (no weight data received)
  if (millis() - _lastWeightMs >= SESSION_TIMEOUT_MS) {
    Serial.println("[Session] Idle timeout — ending session");
    _endSession();
    return;
  }

  // 3-second weight-removal timeout (weight sustained below threshold)
  if (_weightBelowThresholdMs > 0 &&
      millis() - _weightBelowThresholdMs >= WEIGHT_REMOVAL_TIMEOUT_MS) {
    Serial.println("[Session] Weight removed — ending session");
    _endSession();
    return;
  }
}

uint32_t Session::elapsedMs() const {
  if (_state == State::IDLE)
    return 0;
  return millis() - _sessionStartMs;
}

int Session::weightRemovalCountdownSecs() const {
  if (_state != State::ACTIVE || _weightBelowThresholdMs == 0)
    return 0;
  uint32_t elapsed = millis() - _weightBelowThresholdMs;
  if (elapsed >= WEIGHT_REMOVAL_TIMEOUT_MS)
    return 0;
  int remaining = (int)((WEIGHT_REMOVAL_TIMEOUT_MS - elapsed + 999) / 1000);
  return min(remaining, (int)(WEIGHT_REMOVAL_TIMEOUT_MS / 1000));
}

// ── Private
// ───────────────────────────────────────────────────────────────────

void Session::_startSession() {
  Serial.println("[Session] Starting session");
  _sessionStartMs = millis();
  _sessionStartEpoch = _hasRealTime ? time(nullptr) : 0;
  _lastWeightMs = millis();
  _rowCount = 0;
  _writeBuf = "";
  _lastFlushMs = millis();
  _weightBelowThresholdMs = 0;
  _cumulativeWeight = 0.0f;
  _prevRawWeight = 0.0f;
  _chartHead = 0;
  _chartCount = 0;
  _state = State::ACTIVE;

  // create=true guarantees the temp file can be created on first write
  _file = FFat.open("/current.tmp", FILE_WRITE, true);
  if (!_file) {
    Serial.println("[Session] ERROR: cannot open /current.tmp");
    _state = State::IDLE;
    return;
  }

  _writeHeader();
  // Write START event row
  char startTs[32] = "";
  if (_hasRealTime && _sessionStartEpoch > 0) {
    struct tm tmUtc;
    gmtime_r(&_sessionStartEpoch, &tmUtc);
    snprintf(startTs, sizeof(startTs), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
             tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);
  }
  char startRow[96];
  snprintf(startRow, sizeof(startRow), "0,%s,0.0,0.0,START\n", startTs);
  _writeBuf += startRow;
  _rowCount++;
}

void Session::_endSession() {
  if (_state != State::ACTIVE)
    return;
  _state = State::ENDED;

  // Capture final stats before any file operations (for post-session display)
  _endedDurationMs = millis() - _sessionStartMs;
  _endedRowCount = _rowCount;

  // Flush remaining buffer
  _flushBuffer();
  _file.close();

  Serial.printf("[Session] Duration: %lu ms, Rows: %lu\n",
                (unsigned long)_endedDurationMs, (unsigned long)_endedRowCount);

  if (_endedDurationMs >= SESSION_MIN_DURATION_MS) {
    String name = _buildFilename();
    if (FFat.rename("/current.tmp", name.c_str())) {
      Serial.printf("[Session] Saved: %s\n", name.c_str());
      _lastSavedName = name;
      _seqNum++;
    } else {
      Serial.println("[Session] Rename failed — removing tmp");
      FFat.remove("/current.tmp");
    }
  } else {
    Serial.printf("[Session] Too short (%lu ms < %lu ms) — discarding\n",
                  (unsigned long)_endedDurationMs,
                  (unsigned long)SESSION_MIN_DURATION_MS);
    FFat.remove("/current.tmp");
  }

  // State remains ENDED — main.cpp detects this and shows the success screen
  // before entering deep sleep. (No transition back to IDLE here.)
}

void Session::_flushBuffer() {
  if (_writeBuf.length() == 0)
    return;
  if (!_file)
    return;
  _file.print(_writeBuf);
  _writeBuf = "";
  _lastFlushMs = millis();
}

void Session::_writeHeader() {
  char header[160];
  char startStr[32] = "unknown";

  if (_hasRealTime) {
    time_t now = time(nullptr);
    struct tm *t = gmtime(&now);
    snprintf(startStr, sizeof(startStr), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour,
             t->tm_min, t->tm_sec);
  }

  snprintf(header, sizeof(header),
           "# device=%s\n"
           "# fw_version=%s\n"
           "# session_start=%s\n"
           "t_ms,ts_utc,weight_g,cumulative_g,event\n",
           DEVICE_NAME, FW_VERSION, startStr);

  _file.print(header);
}

String Session::_buildFilename() const {
  if (_hasRealTime) {
    time_t now = time(nullptr);
    struct tm *t = gmtime(&now);
    char buf[40];
    snprintf(buf, sizeof(buf), "/pearls_%04d%02d%02d_%02d%02d%02d.csv",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour,
             t->tm_min, t->tm_sec);
    return String(buf);
  } else {
    char buf[24];
    snprintf(buf, sizeof(buf), "/pearls_%04lu.csv", (unsigned long)_seqNum);
    return String(buf);
  }
}

int Session::uploadToGoogleSheet(const String &path) {
  if (strlen(CLOUD_UPLOAD_URL) < 10 ||
      strstr(CLOUD_UPLOAD_URL, "your-cloud-endpoint")) {
    Serial.println("[Cloud] No valid upload URL configured");
    return -1;
  }

  Serial.println("[Cloud] Connecting to WiFi...");
  WiFi.disconnect(); // Ensure fresh start
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t deadline = millis() + 12000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Cloud] WiFi connect failed");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return 0; // 0 = WiFi Failed
  }

  Serial.println("[Cloud] WiFi connected, opening file...");
  File f = FFat.open(path, FILE_READ);
  if (!f) {
    Serial.println("[Cloud] Failed to open CSV file");
    return -1;
  }
  size_t fileSize = f.size();
  Serial.printf("[Cloud] File: %s (%u bytes)\n", path.c_str(),
                (unsigned int)fileSize);

  WiFiClientSecure client;
  client.setInsecure(); // No certificate validation for Google
  HTTPClient http;

  // Set a robust timeout (15s) to prevent hanging the UI indefinitely
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  // Build URL with parameters: filename and device_name
  String url = String(CLOUD_UPLOAD_URL);
  url += (url.indexOf('?') >= 0) ? "&" : "?";
  url += "filename=";
  String filename = path;
  if (filename.startsWith("/"))
    filename = filename.substring(1);
  url += filename;
  url += "&device_name=";
  url += String(DEVICE_NAME);

  http.begin(client, url);
  http.addHeader("Content-Type", "text/plain");

  Serial.println("[Cloud] Streaming POST request...");
  int httpCode = http.sendRequest("POST", &f, fileSize);
  f.close();

  bool success = (httpCode == 200 || httpCode == 302);

  if (success) {
    Serial.println("[Cloud] Upload SUCCESS");
  } else {
    Serial.printf("[Cloud] Upload FAILED, code: %d\n", httpCode);
    if (httpCode < 0) {
      Serial.printf("[Cloud] Error: %s\n",
                    http.errorToString(httpCode).c_str());
    }
  }

  http.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  return success ? 1 : httpCode;
}
