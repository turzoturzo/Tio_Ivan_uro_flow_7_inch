#include "session.h"
#include "config.h"
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

Session::Session()
    : _state(State::IDLE), _sessionStartMs(0), _sessionStartEpoch(0),
      _lastWeightMs(0), _lastWeight(0.0f), _preTareWeight(0.0f),
      _lastRelativeWeight(0.0f),
      _prevRawWeight(0.0f), _cumulativeWeight(0.0f), _sessionTareWeight(0.0f),
      _rowCount(0), _hasRealTime(false), _seqNum(0), _lastFlushMs(0),
      _weightBelowThresholdMs(0), _hasExceededStartThreshold(false),
      _manualMode(false),
      _endedRowCount(0), _endedDurationMs(0),
      _lastFlowRate(0.0f), _qMax(0.0f), _tQmaxMs(0),
      _flowRateHistIdx(0), _prevFlowCalcMs(0), _prevCumulativeForFlow(0.0f),
      _chartHead(0), _chartCount(0) {
  memset(_flowRateHistory, 0, sizeof(_flowRateHistory));
}

void Session::begin(bool hasRealTime, uint32_t seqNum) {
  _hasRealTime = hasRealTime;
  _seqNum = seqNum;

  // Remove any leftover tmp file from a previous crash
  if (FFat.exists("/current.tmp")) {
    FFat.remove("/current.tmp");
    Serial.println("[Session] Removed stale /current.tmp");
  }
}

void Session::onWeight(float weight_g, uint32_t t_ms_abs) {
  _pendingWeight = weight_g;
  _pendingTime = t_ms_abs;
  _newWeightPending = true;
}

void Session::tick() {
  // 1. Process pending weight from BLE task (safe context)
  if (_newWeightPending) {
    float w = _pendingWeight.load();
    uint32_t t = _pendingTime.load();
    _newWeightPending = false;
    _processWeight(w, t);
  }

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

void Session::_processWeight(float weight_g, uint32_t /*t_ms_abs*/) {
  _lastWeight = weight_g;
  _lastWeightMs = millis();

  if (_state == State::IDLE) {
    // Auto-start only when weight clearly exceeds the start threshold
    if (weight_g >= SESSION_START_THRESHOLD_G) {
      if (!_manualMode) {
        _startSession();
      } else {
        return; // Manual mode: suppress auto-start
      }
    } else {
      // Track last sub-threshold weight as tare baseline
      _preTareWeight = weight_g;
      return;
    }
  }

  if (_state != State::ACTIVE)
    return;

  float relativeWeight = weight_g - _sessionTareWeight;
  if (relativeWeight < -2.0f) {
    // Significant negative weight (e.g. cup removed) — trigger end countdown
    if (_hasExceededStartThreshold && _weightBelowThresholdMs == 0) {
      _weightBelowThresholdMs = millis();
      Serial.printf("[Session] Negative weight (%.1fg) — starting end countdown\n",
                    relativeWeight);
    }
    relativeWeight = 0.0f;
  } else if (relativeWeight < 0.0f) {
    relativeWeight = 0.0f;
  }
  _lastRelativeWeight = relativeWeight;

  // Track if weight has crossed the start threshold during this session
  if (!_hasExceededStartThreshold && weight_g >= SESSION_START_THRESHOLD_G) {
    _hasExceededStartThreshold = true;
  }

  // Auto-end countdown: only after threshold was reached, then weight drops to ~0g
  if (_hasExceededStartThreshold && weight_g <= SESSION_END_ZERO_G) {
    if (_weightBelowThresholdMs == 0) {
      _weightBelowThresholdMs = millis();
    }
  } else if (weight_g > SESSION_END_ZERO_G) {
    _weightBelowThresholdMs = 0;
  }

  // Track cumulative positive weight deltas (ignores drops / zero / negative)
  if (relativeWeight > 0.0f) {
    float delta = relativeWeight - _prevRawWeight;
    if (delta > 0.05f) {
      _cumulativeWeight += delta;
    }
  }
  _prevRawWeight = relativeWeight;

  uint32_t t_ms = millis() - _sessionStartMs;

  // ── Flow rate computation (smoothed derivative of cumulative weight) ──
  // Compute instantaneous flow rate as delta_volume / delta_time, then smooth
  // with a 4-sample moving average to filter scale noise.
  // Since urine density ≈ 1 g/mL, cumulative_g ≈ volume in mL.
  float instantFlowRate = 0.0f;
  if (_prevFlowCalcMs > 0) {
    uint32_t now = millis();
    uint32_t dt_ms = now - _prevFlowCalcMs;
    if (dt_ms > 0) {
      float dVol = _cumulativeWeight - _prevCumulativeForFlow;  // mL (≈ g)
      instantFlowRate = (dVol / (float)dt_ms) * 1000.0f;  // mL/s
      if (instantFlowRate < 0.0f) instantFlowRate = 0.0f;
    }
  }
  _prevFlowCalcMs = millis();
  _prevCumulativeForFlow = _cumulativeWeight;

  // 4-sample moving average for smoothing
  _flowRateHistory[_flowRateHistIdx % 4] = instantFlowRate;
  _flowRateHistIdx++;
  int numSamples = (_flowRateHistIdx < 4) ? _flowRateHistIdx : 4;
  float sum = 0.0f;
  for (int i = 0; i < numSamples; i++) sum += _flowRateHistory[i];
  _lastFlowRate = sum / (float)numSamples;

  // Track Qmax and time-to-Qmax
  if (_lastFlowRate > _qMax) {
    _qMax = _lastFlowRate;
    _tQmaxMs = t_ms;
  }

  // Append to chart ring buffer
  int slot = (_chartHead + _chartCount) % CHART_BUF_SIZE;
  if (_chartCount < CHART_BUF_SIZE) {
    _chartCount++;
  } else {
    _chartHead = (_chartHead + 1) % CHART_BUF_SIZE;
  }
  _chart[slot].t_ms = t_ms;
  _chart[slot].weight_g = relativeWeight;

  // Build CSV row (now includes flow_rate_ml_s)
  char ts[32] = "";
  if (_hasRealTime && _sessionStartEpoch > 0) {
    time_t rowEpoch = _sessionStartEpoch + (time_t)(t_ms / 1000UL);
    struct tm tmUtc;
    gmtime_r(&rowEpoch, &tmUtc);
    snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
             tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);
  }
  char row[128];
  snprintf(row, sizeof(row), "%lu,%s,%.1f,%.1f,%.2f,\n", (unsigned long)t_ms, ts,
           relativeWeight, _cumulativeWeight, _lastFlowRate);
  _writeBuf += row;
  _rowCount++;

  if (millis() - _lastFlushMs >= LOG_FLUSH_INTERVAL_MS) {
    _flushBuffer();
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
  _hasExceededStartThreshold = false;
  _cumulativeWeight = 0.0f;
  _prevRawWeight = 0.0f;
  _lastRelativeWeight = 0.0f;
  _sessionTareWeight = _preTareWeight;
  _lastFlowRate = 0.0f;
  _qMax = 0.0f;
  _tQmaxMs = 0;
  _flowRateHistIdx = 0;
  _prevFlowCalcMs = 0;
  _prevCumulativeForFlow = 0.0f;
  memset(_flowRateHistory, 0, sizeof(_flowRateHistory));
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
  char startRow[128];
  snprintf(startRow, sizeof(startRow), "0,%s,0.0,0.0,0.00,START\n", startTs);
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

  // Compute average flow rate (Qave = voided_volume / voiding_time)
  float qAve = (_endedDurationMs > 0)
                   ? (_cumulativeWeight / (_endedDurationMs / 1000.0f))
                   : 0.0f;

  // Write END summary row with clinical parameters
  // Format: t_ms, ts_utc, voided_vol_ml, Qmax_ml_s, Qave_ml_s, END|TQmax_s=X
  char endTs[32] = "";
  if (_hasRealTime && _sessionStartEpoch > 0) {
    time_t endEpoch =
        _sessionStartEpoch + (time_t)(_endedDurationMs / 1000UL);
    struct tm tmUtc;
    gmtime_r(&endEpoch, &tmUtc);
    snprintf(endTs, sizeof(endTs), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
             tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);
  }
  char endRow[192];
  snprintf(endRow, sizeof(endRow),
           "%lu,%s,%.1f,%.1f,0.00,"
           "END|voided_ml=%.1f|Qmax=%.2f|Qave=%.2f|TQmax_s=%.1f|"
           "duration_s=%.1f\n",
           (unsigned long)_endedDurationMs, endTs, 0.0f, _cumulativeWeight,
           _cumulativeWeight, _qMax, qAve, _tQmaxMs / 1000.0f,
           _endedDurationMs / 1000.0f);
  _writeBuf += endRow;

  Serial.printf("[Session] Summary: Voided=%.1f mL, Qmax=%.2f mL/s, "
                "Qave=%.2f mL/s, TQmax=%.1f s\n",
                _cumulativeWeight, _qMax, qAve, _tQmaxMs / 1000.0f);

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
  char header[192];
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
           "t_ms,ts_utc,weight_g,cumulative_g,flow_rate_ml_s,event\n",
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
  if (path.length() == 0) {
    Serial.println("[Cloud] No saved CSV path; skipping upload");
    return -1;
  }

  if (strlen(CLOUD_UPLOAD_URL) < 10 ||
      strstr(CLOUD_UPLOAD_URL, "your-cloud-endpoint")) {
    Serial.println("[Cloud] No valid upload URL configured");
    return -1;
  }

  String wifiSsid = WIFI_SSID;
  String wifiPass = WIFI_PASS;
  bool haveCreds = strlen(WIFI_SSID) > 0;

  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    String nvsSsid = prefs.getString(NVS_KEY_WIFI_SSID, "");
    String nvsPass = prefs.getString(NVS_KEY_WIFI_PASS, "");
    prefs.end();
    if (nvsSsid.length() > 0) {
      wifiSsid = nvsSsid;
      wifiPass = nvsPass;
      haveCreds = true;
    }
  }
  if (!haveCreds) {
    Serial.println("[Cloud] No WiFi credentials available");
    return 0;
  }

  // Reuse existing WiFi connection if already connected (batch uploads).
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Cloud] Connecting to WiFi...");
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect();
    }
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

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
  }

  Serial.println("[Cloud] WiFi connected, opening file...");
  File f = FFat.open(path, FILE_READ);
  if (!f) {
    Serial.println("[Cloud] Failed to open CSV file");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return -1;
  }
  size_t fileSize = f.size();
  Serial.printf("[Cloud] File: %s (%u bytes)\n", path.c_str(),
                (unsigned int)fileSize);

  WiFiClientSecure *client = new WiFiClientSecure();
  HTTPClient *http = new HTTPClient();
  if (!client || !http) {
    Serial.println("[Cloud] ERROR: Out of memory for HTTP client");
    if (client) delete client;
    if (http) delete http;
    f.close();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return -1;
  }
  client->setInsecure();
  client->setTimeout(15000);
  // HTTPClient::setTimeout() takes uint16_t ms — max 65535!
  http->setTimeout(15000);
  http->setConnectTimeout(10000);
  // No redirects needed — Cloudflare Worker returns 200 directly.
  http->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  // Build path with query parameters
  String filename = path;
  if (filename.startsWith("/"))
    filename = filename.substring(1);
  String urlPath = "/?filename=" + filename + "&device_name=" + String(DEVICE_NAME);

  String url = String(CLOUD_UPLOAD_URL) + urlPath; // for logging

  // Use explicit host/port/path form to avoid DNS parsing issues with long URLs.
  if (!http->begin(*client, "mongoflo-relay.black-heart-3a5a.workers.dev", 443, urlPath, true)) {
    Serial.println("[Cloud] ERROR: HTTP begin failed");
    f.close();
    http->end();
    delete http;
    delete client;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return -1;
  }
  http->addHeader("Content-Type", "text/plain");
  http->addHeader("User-Agent", "ESP32-MongoFlo/1.4");
  http->addHeader("Accept", "*/*");
  http->addHeader("Connection", "close");
  http->setReuse(false);

  Serial.printf("[Cloud] WiFi IP: %s\n", WiFi.localIP().toString().c_str());

  // Read file into memory (bulk read, not char-by-char).
  char *buf = (char *)malloc(fileSize + 1);
  if (!buf) {
    Serial.println("[Cloud] ERROR: malloc failed for file buffer");
    f.close();
    http->end();
    delete http;
    delete client;
    return -1;
  }
  size_t bytesRead = f.read((uint8_t *)buf, fileSize);
  f.close();
  buf[bytesRead] = '\0';
  String body;
  body.reserve(bytesRead + 1);
  body = buf;
  free(buf);

  Serial.printf("[Cloud] POST %u bytes to %s\n", body.length(), url.c_str());
  uint32_t postStart = millis();
  int httpCode = http->POST(body);
  uint32_t elapsed = millis() - postStart;
  Serial.printf("[Cloud] POST returned in %lu ms, code: %d\n",
                (unsigned long)elapsed, httpCode);

  // 200 = Cloudflare Worker accepted and forwarded to Google Apps Script.
  bool success = (httpCode == 200);

  if (success) {
    String resp = http->getString();
    Serial.printf("[Cloud] Upload SUCCESS: %s\n", resp.c_str());
  } else {
    Serial.printf("[Cloud] Upload FAILED, code: %d\n", httpCode);
    if (httpCode < 0) {
      Serial.printf("[Cloud] Error: %s\n",
                    http->errorToString(httpCode).c_str());
    } else {
      String resp = http->getString();
      if (resp.length() > 0) {
        Serial.printf("[Cloud] Response: %.200s\n", resp.c_str());
      }
    }
  }

  http->end();
  delete http;
  delete client;
  // Don't disconnect WiFi here — caller manages WiFi lifecycle for batch uploads.
  return success ? 1 : httpCode;
}
