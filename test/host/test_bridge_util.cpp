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

    // ── v0.4.3 ──
    { // stuck line: brief highs (normal IRQ pulses) never trip; > 1 s high with no ISR does
        StuckLineWatch w; bool tripped = false;
        for (uint32_t t = 0; t < 10000; t += 10) tripped |= w.poll(t, (t % 160) < 5, false, 1000);
        CHECK(!tripped, "short IRQ pulses never trip the stuck-line watch");
        StuckLineWatch v; int at = -1;
        for (uint32_t t = 0; t <= 3000; t += 10) if (v.poll(t, true, false, 1000) && at < 0) at = (int)t;
        CHECK(at == 1010, "line high with no ISR trips just after 1 s");
        StuckLineWatch x; bool f = false;
        for (uint32_t t = 0; t <= 3000; t += 10) f |= x.poll(t, true, true, 1000);
        CHECK(!f, "an ISR flag pending means it isn't stuck");
    }
    { // silence watch at the deployed ~400/min: no action while IRQs flow
        SilenceWatch s; int acts = 0; uint32_t irq = 0;
        for (uint32_t t = 0; t < 60u * 60u * 1000u; t += 150) { irq++; acts += s.poll(t, irq, 20, 300000) != SilenceWatch::NONE; }
        CHECK(acts == 0, "steady 400/min: no action in an hour");
        // then silence: REINIT after 5 min, RESTART 5 min later
        uint32_t t0 = 60u * 60u * 1000u; int reinitAt = -1, restartAt = -1;
        for (uint32_t t = t0; t < t0 + 15u * 60u * 1000u; t += 1000) {
            auto a = s.poll(t, irq, 20, 300000);
            if (a == SilenceWatch::REINIT && reinitAt < 0) reinitAt = (int)((t - t0) / 1000);
            if (a == SilenceWatch::RESTART && restartAt < 0) restartAt = (int)((t - t0) / 1000);
        }
        CHECK(reinitAt >= 299 && reinitAt <= 301, "silence: re-init after ~5 min");
        CHECK(restartAt >= 599 && restartAt <= 601, "still silent: restart ~5 min after the re-init");
    }
    { // IRQs resuming after the re-init cancel the restart
        SilenceWatch s; uint32_t irq = 0, t = 0;
        for (; t < 30u * 60u * 1000u; t += 150) { irq++; s.poll(t, irq, 20, 300000); }
        bool reinit = false, restart = false;
        for (uint32_t u = t; u < t + 301000; u += 1000) reinit |= s.poll(u, irq, 20, 300000) == SilenceWatch::REINIT;
        t += 301000; irq += 5;                                  // the re-init brought them back
        for (uint32_t u = t; u < t + 20u * 60u * 1000u; u += 150) { irq++; restart |= s.poll(u, irq, 20, 300000) == SilenceWatch::RESTART; }
        CHECK(reinit && !restart, "IRQs resuming after a re-init cancel the restart");
    }
    { // after a restart (fresh watch, rate 0) a silent sensor is never acted on: no loops
        SilenceWatch s; int acts = 0;
        for (uint32_t t = 0; t < 3u * 3600u * 1000u; t += 1000) acts += s.poll(t, 0, 20, 300000) != SilenceWatch::NONE;
        CHECK(acts == 0, "fresh watch on a silent sensor stays disarmed (3 h)");
    }
    { // low rate (5/min): silences are normal, never armed
        SilenceWatch s; int acts = 0; uint32_t irq = 0;
        for (uint32_t t = 0; t < 4u * 3600u * 1000u; t += 1000) { if (t % 12000 == 0 && (t / 3600000) % 2 == 0) irq++; acts += s.poll(t, irq, 20, 300000) != SilenceWatch::NONE; }
        CHECK(acts == 0, "5/min with hour-long gaps: never acts");
    }
    { // millis() wrap during silence
        SilenceWatch s; uint32_t irq = 0, t = 0xFFFFFFFFu - 40u * 60u * 1000u;
        for (int i = 0; i < 30 * 60 * 1000 / 150; i++, t += 150) { irq++; s.poll(t, irq, 20, 300000); }
        int reinitAt = -1; uint32_t t0 = t;
        for (int i = 0; i < 400; i++, t += 1000) if (s.poll(t, irq, 20, 300000) == SilenceWatch::REINIT && reinitAt < 0) reinitAt = (int)((uint32_t)(t - t0) / 1000);
        CHECK(reinitAt >= 299 && reinitAt <= 301, "silence detection survives the millis() wrap");
    }
    std::printf("%d passed, %d failed\n", pass, fail);
    return fail != 0;
}
