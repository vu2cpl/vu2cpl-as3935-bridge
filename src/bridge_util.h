// bridge_util.h — small pure helpers for the bridge firmware (v0.4.1).
//
// No Arduino dependencies, so both can be compiled and tested on a host:
// everything takes the current time as a millis()-style uint32_t, and all
// comparisons use unsigned subtraction, so they survive the 49.7-day wrap.
#pragma once
#include <stdint.h>

// Remembers the last N MQTT reconnect times. record() returns true once N of
// them fall inside windowMs: the "connected, dropped, reconnected" churn of
// 2026-10-10 and 2026-10-14 (a keepalive timeout every ~23 s for hours), which
// the no-successful-publish watchdog never sees because upstream publishes
// keep succeeding.
template <uint8_t N>
class ChurnTracker {
public:
    explicit ChurnTracker(uint32_t windowMs) : windowMs_(windowMs) {}

    bool record(uint32_t nowMs) {
        times_[next_] = nowMs;
        next_ = (uint8_t)((next_ + 1) % N);
        if (filled_ < N) filled_++;
        if (filled_ < N) return false;
        // After advancing, next_ points at the oldest of the last N entries.
        return (uint32_t)(nowMs - times_[next_]) <= windowMs_;
    }

private:
    uint32_t windowMs_;
    uint32_t times_[N] = {};
    uint8_t  next_   = 0;
    uint8_t  filled_ = 0;
};

// Publishes at most one event per interval for a chatty event type (AS3935
// disturbers ran at ~6/s, i.e. ~12 MQTT publishes/s with last_event). The first
// event always goes out; later ones inside the interval are only counted.
// onEvent() returns 0 to suppress, or how many events this publish stands for
// (this one plus every one suppressed since the previous publish).
// intervalMs == 0 publishes every event (v0.4.0 behaviour).
struct EventThrottle {
    uint32_t lastPubMs = 0;
    uint32_t pending   = 0;
    bool     published = false;

    uint32_t onEvent(uint32_t nowMs, uint32_t intervalMs) {
        pending++;
        if (intervalMs == 0 || !published || (uint32_t)(nowMs - lastPubMs) >= intervalMs) {
            uint32_t n = pending;
            pending   = 0;
            lastPubMs = nowMs;
            published = true;
            return n;
        }
        return 0;
    }
};
