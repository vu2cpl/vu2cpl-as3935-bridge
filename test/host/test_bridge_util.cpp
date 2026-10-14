// Host test for src/bridge_util.h — build and run on the Mac/Pi (no ESP32 needed):
//   c++ -std=c++17 -Wall -Wextra -I src -o /tmp/tbu test/host/test_bridge_util.cpp && /tmp/tbu
#include "bridge_util.h"
#include <cstdio>
#include <initializer_list>
static int pass = 0, fail = 0;
#define CHECK(c, what) do { if (c) { pass++; } else { fail++; std::printf("FAIL: %s\n", what); } } while (0)

int main() {
    const uint32_t WIN = 10u * 60u * 1000u;

    { // flapping cadence: reconnect every 23 s -> fires on the 5th
        ChurnTracker<5> c(WIN); uint32_t t = 1000; int firedAt = -1;
        for (int i = 1; i <= 8; i++, t += 23000) if (c.record(t) && firedAt < 0) firedAt = i;
        CHECK(firedAt == 5, "23 s flapping fires on the 5th reconnect (~92 s)");
    }
    { // occasional reconnects (broker restart every 3 h) never fire
        ChurnTracker<5> c(WIN); bool fired = false; uint32_t t = 0;
        for (int i = 0; i < 50; i++, t += 3u * 3600u * 1000u) fired |= c.record(t);
        CHECK(!fired, "a reconnect every 3 h never fires");
    }
    { // 5 reconnects spread over just over 10 min: no; then a burst: yes
        ChurnTracker<5> c(WIN);
        uint32_t ts[5] = {0, 150001, 300002, 450003, 600004};   // oldest->newest = 600004 > WIN
        bool f = false; for (uint32_t t : ts) f |= c.record(t);
        CHECK(!f, "5 reconnects spanning 10 min + 4 ms do not fire");
        CHECK(c.record(600005), "6th reconnect makes 5 within the window -> fires");
    }
    { // exactly at the window edge fires (<=)
        ChurnTracker<5> c(WIN); bool f = false;
        for (uint32_t t : {0u, 1u, 2u, 3u}) f |= c.record(t);
        CHECK(!f && c.record(WIN), "5th reconnect exactly windowMs after the 1st fires");
    }
    { // millis() wrap
        ChurnTracker<5> c(WIN); uint32_t t = 0xFFFFFFFFu - 50000u; int firedAt = -1;
        for (int i = 1; i <= 5; i++, t += 23000) if (c.record(t) && firedAt < 0) firedAt = i;
        CHECK(firedAt == 5, "churn detection survives the millis() wrap");
    }
    { // throttle: first event published, rest counted, summary after the interval
        EventThrottle th; const uint32_t IV = 60000;
        CHECK(th.onEvent(0, IV) == 1, "first event published with count 1");
        int suppressed = 0; for (uint32_t t = 166; t < 60000; t += 166) if (th.onEvent(t, IV) == 0) suppressed++;
        CHECK(suppressed == 361, "events inside the interval are suppressed");
        CHECK(th.onEvent(60000, IV) == 362, "next publish carries this event + the 361 suppressed");
        CHECK(th.onEvent(60001, IV) == 0, "and the interval restarts");
    }
    { // interval 0 = every event (v0.4.0 behaviour)
        EventThrottle th; bool all = true;
        for (uint32_t t = 0; t < 1000; t += 10) all &= (th.onEvent(t, 0) == 1);
        CHECK(all, "interval 0 publishes every event with count 1");
    }
    { // rare events (1 per 5 min) are never delayed
        EventThrottle th; bool all = true;
        for (uint32_t t = 0; t < 3600000; t += 300000) all &= (th.onEvent(t, 60000) == 1);
        CHECK(all, "events further apart than the interval always publish");
    }
    { // throttle across the millis() wrap
        EventThrottle th; uint32_t t0 = 0xFFFFFFFFu - 1000u;
        th.onEvent(t0, 60000);
        CHECK(th.onEvent(t0 + 30000, 60000) == 0, "suppressed 30 s after, across the wrap");
        CHECK(th.onEvent(t0 + 61000, 60000) == 2, "published 61 s after, across the wrap");
    }
    std::printf("%d passed, %d failed\n", pass, fail);
    return fail != 0;
}
