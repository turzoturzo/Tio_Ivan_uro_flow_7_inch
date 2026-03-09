#pragma once
#include "chart_types.h"
#include <Arduino.h>
#include <FFat.h>
#include <atomic>
#include <time.h>

class Session {
public:
  enum class State { IDLE, ACTIVE, ENDED, UPLOAD, WAITING };

  Session();

  // Call once after FFat is mounted
  void begin(bool hasRealTime, uint32_t seqNum);

  // Called by BLE weight callback
  void onWeight(float weight_g, uint32_t t_ms_abs);

  // Call every loop() to check the idle timeout
  void tick();

  // Moves the session from ENDED to WAITING so we don't repeat the end logic
  void acknowledgeEnded() {
    if (_state == State::ENDED)
      _state = State::WAITING;
  }

  // Reset to IDLE to allow a new measurement
  void reset() {
    _state = State::IDLE;
    _sessionStartMs = 0;
    _lastWeightMs = millis();
    _lastRelativeWeight = 0.0f;
    _sessionTareWeight = 0.0f;
    _prevRawWeight = 0.0f;
    _cumulativeWeight = 0.0f;
    _chartHead = 0;
    _chartCount = 0;
    _lastSavedName = "";
  }

  // Manually trigger a session start (Touch override)
  void forceStart() {
    if (_state == State::IDLE) {
      _startSession();
    }
  }

  // Manually end an active session (saves data, unlike reset())
  void forceEnd() {
    if (_state == State::ACTIVE) {
      _endSession();
    }
  }

  // Accessors for display
  State state() const { return _state; }
  bool isActive() const { return _state == State::ACTIVE; }
  uint32_t rowCount() const { return _rowCount; }
  uint32_t elapsedMs() const;
  uint32_t startTime() const { return _sessionStartMs; }
  // Relative session weight (tared at session start for UX/charting)
  float lastWeight() const { return _lastRelativeWeight; }
  int weightRemovalCountdownSecs() const;
  uint32_t seqNum() const { return _seqNum; }
  String lastSavedName() const { return _lastSavedName; }
  float cumulativeWeight() const { return _cumulativeWeight; }
  float lastFlowRate() const { return _lastFlowRate; }
  uint32_t elapsedSeconds() const { return elapsedMs() / 1000; }
  int uploadToGoogleSheet(const String &path);

  // Accessors for post-session summary (valid when state() == ENDED)
  uint32_t endedRowCount() const { return _endedRowCount; }
  uint32_t endedDurationMs() const { return _endedDurationMs; }
  float endedQmax() const { return _qMax; }
  float endedQave() const { return _endedDurationMs > 0 ? (_cumulativeWeight / (_endedDurationMs / 1000.0f)) : 0.0f; }
  float endedVoidedVolume() const { return _cumulativeWeight; }  // g ≈ mL
  float endedTQmaxS() const { return _tQmaxMs / 1000.0f; }

  // Chart data accessors (ring buffer — use chartHead() as start index)
  const ChartSample *chartData() const { return _chart; }
  int chartCount() const { return _chartCount; }
  int chartHead() const { return _chartHead; }

private:
  void _startSession();
  void _endSession();
  void _flushBuffer();
  void _writeHeader();
  String _buildFilename() const;
  void _processWeight(float weight_g, uint32_t t_ms_abs);

  State _state;
  uint32_t _sessionStartMs;
  time_t _sessionStartEpoch;
  uint32_t _lastWeightMs;
  float _lastWeight;
  float _lastRelativeWeight;
  float _prevRawWeight;
  float _cumulativeWeight;
  float _sessionTareWeight;
  uint32_t _rowCount;
  bool _hasRealTime;
  uint32_t _seqNum;

  File _file;
  String _writeBuf;
  String _lastSavedName;
  uint32_t _lastFlushMs;

  // Weight-removal detection
  uint32_t
      _weightBelowThresholdMs; // 0 = above threshold; millis() when went below
  bool _hasExceededStartThreshold; // true once weight has gone above SESSION_START_THRESHOLD_G

  // Captured at session end — valid when state() == ENDED
  uint32_t _endedRowCount;
  uint32_t _endedDurationMs;

  // Flow rate computation (smoothed derivative of cumulative weight)
  float _lastFlowRate;           // current smoothed flow rate (mL/s)
  float _qMax;                   // peak flow rate during session
  uint32_t _tQmaxMs;             // time (ms since session start) when Qmax occurred
  float _flowRateHistory[4];     // ring buffer for 4-sample moving average
  int _flowRateHistIdx;          // current index into ring buffer
  uint32_t _prevFlowCalcMs;      // millis() of previous flow rate calculation
  float _prevCumulativeForFlow;  // cumulative_g at previous flow calc

  // Thread-safe weight transfer
  std::atomic<float> _pendingWeight{0.0f};
  std::atomic<uint32_t> _pendingTime{0};
  std::atomic<bool> _newWeightPending{false};

  // In-memory chart ring buffer (for live display only, not persisted)
  ChartSample _chart[CHART_BUF_SIZE];
  int _chartHead;  // index of oldest sample
  int _chartCount; // number of valid samples (0..CHART_BUF_SIZE)
};
