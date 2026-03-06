#pragma once
#include "chart_types.h"
#include <Arduino.h>
#include <FFat.h>
#include <atomic>
#include <time.h>

class Session {
public:
  enum class State { IDLE, ACTIVE, ENDED, WAITING };

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
    _lastWeightMs = millis();
  }

  // Accessors for display
  State state() const { return _state; }
  bool isActive() const { return _state == State::ACTIVE; }
  uint32_t rowCount() const { return _rowCount; }
  uint32_t elapsedMs() const;
  float lastWeight() const { return _lastWeight; }
  int weightRemovalCountdownSecs() const;
  uint32_t seqNum() const { return _seqNum; }
  String lastSavedName() const { return _lastSavedName; }
  int uploadToGoogleSheet(const String &path);

  // Accessors for post-session summary (valid when state() == ENDED)
  uint32_t endedRowCount() const { return _endedRowCount; }
  uint32_t endedDurationMs() const { return _endedDurationMs; }

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
  float _prevRawWeight;
  float _cumulativeWeight;
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

  // Captured at session end — valid when state() == ENDED
  uint32_t _endedRowCount;
  uint32_t _endedDurationMs;

  // Thread-safe weight transfer
  std::atomic<float> _pendingWeight{0.0f};
  std::atomic<uint32_t> _pendingTime{0};
  std::atomic<bool> _newWeightPending{false};

  // In-memory chart ring buffer (for live display only, not persisted)
  ChartSample _chart[CHART_BUF_SIZE];
  int _chartHead;  // index of oldest sample
  int _chartCount; // number of valid samples (0..CHART_BUF_SIZE)
};
