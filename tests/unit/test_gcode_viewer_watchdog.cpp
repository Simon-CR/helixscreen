// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Unit tests for the gcode-viewer renderer-stall watchdog decision logic.
//
// The watchdog self-heals a dropped lv_obj_invalidate() by force-invalidating
// when the solid cache is behind the print's target layer and not advancing.
// Under a persistent EXTERNAL failure (disk full -> layer load fails -> cache
// stuck), the original logic kicked forever (4000+ kicks / ~2h in bundle
// YZQ47HQ6). watchdog_evaluate() adds a consecutive-stall cap so it gives up
// and surfaces an error instead of thrashing.

#include "gcode_viewer_watchdog.h"

#include "../catch_amalgamated.hpp"

using helix::gcode_viewer::watchdog_evaluate;
using helix::gcode_viewer::WatchdogDecision;
using helix::gcode_viewer::WatchdogObservation;

namespace {
constexpr int NEVER_SAMPLED = -2; // sentinel for "no previous observation"
constexpr int MAX_STALL_KICKS = 30;
} // namespace

TEST_CASE("watchdog: first sample never kicks or gives up", "[gcode_viewer][watchdog]") {
    // prev_cached == -2 sentinel: we have nothing to compare against yet.
    WatchdogObservation obs{/*cached=*/-1,
                            /*target=*/855,
                            /*prev_cached=*/NEVER_SAMPLED,
                            /*prev_target=*/NEVER_SAMPLED,
                            /*stall_streak=*/0,
                            /*visible=*/true};
    auto d = watchdog_evaluate(obs, MAX_STALL_KICKS);
    CHECK_FALSE(d.kick);
    CHECK_FALSE(d.give_up);
    CHECK(d.stall_streak == 0);
}

TEST_CASE("watchdog: caught-up cache idles with zero streak", "[gcode_viewer][watchdog]") {
    // cached >= target: healthy, nothing to do, streak resets even if it was high.
    WatchdogObservation obs{
        /*cached=*/855,      /*target=*/855,      /*prev_cached=*/855,
        /*prev_target=*/855, /*stall_streak=*/12, /*visible=*/true};
    auto d = watchdog_evaluate(obs, MAX_STALL_KICKS);
    CHECK_FALSE(d.kick);
    CHECK_FALSE(d.give_up);
    CHECK(d.stall_streak == 0);
}

TEST_CASE("watchdog: confirmed stall kicks and increments streak", "[gcode_viewer][watchdog]") {
    // Behind target AND same (cached,target) as last tick -> confirmed stall.
    WatchdogObservation obs{/*cached=*/-1,       /*target=*/855,     /*prev_cached=*/-1,
                            /*prev_target=*/855, /*stall_streak=*/0, /*visible=*/true};
    auto d = watchdog_evaluate(obs, MAX_STALL_KICKS);
    CHECK(d.kick);
    CHECK_FALSE(d.give_up);
    CHECK(d.stall_streak == 1);
}

TEST_CASE("watchdog: progress since last tick resets the streak", "[gcode_viewer][watchdog]") {
    // cached advanced (-1 -> 40): the renderer is making progress, so even a
    // large prior streak must reset and we must NOT give up.
    WatchdogObservation obs{/*cached=*/40,       /*target=*/855,      /*prev_cached=*/-1,
                            /*prev_target=*/855, /*stall_streak=*/29, /*visible=*/true};
    auto d = watchdog_evaluate(obs, MAX_STALL_KICKS);
    CHECK_FALSE(d.kick);
    CHECK_FALSE(d.give_up);
    CHECK(d.stall_streak == 0);
}

TEST_CASE("watchdog: gives up after max consecutive stalls and stops kicking",
          "[gcode_viewer][watchdog]") {
    // One more stall tick reaches the cap: stop force-invalidating and signal
    // give_up so the caller can surface the error state.
    WatchdogObservation obs{/*cached=*/-1,
                            /*target=*/855,
                            /*prev_cached=*/-1,
                            /*prev_target=*/855,
                            /*stall_streak=*/MAX_STALL_KICKS - 1,
                            /*visible=*/true};
    auto d = watchdog_evaluate(obs, MAX_STALL_KICKS);
    CHECK_FALSE(d.kick);
    CHECK(d.give_up);
    CHECK(d.stall_streak == MAX_STALL_KICKS);
}

TEST_CASE("watchdog: a hidden viewer never kicks or gives up however long it stalls",
          "[gcode_viewer][watchdog]") {
    // A viewer under a hidden screen is not drawn, and its cache advances only
    // while drawing, so a stall there is expected rather than a render failure.
    constexpr int HIDDEN_TICKS = MAX_STALL_KICKS + 10;
    int streak = 0;
    for (int tick = 0; tick < HIDDEN_TICKS; ++tick) {
        CAPTURE(tick);
        WatchdogObservation obs{/*cached=*/-1,           /*target=*/855,
                                /*prev_cached=*/-1,      /*prev_target=*/855,
                                /*stall_streak=*/streak, /*visible=*/false};
        const WatchdogDecision d = watchdog_evaluate(obs, MAX_STALL_KICKS);
        REQUIRE_FALSE(d.kick);
        REQUIRE_FALSE(d.give_up);
        REQUIRE(d.stall_streak == 0);
        streak = d.stall_streak;
    }
}

TEST_CASE("watchdog: a viewer shown again counts its stall from zero", "[gcode_viewer][watchdog]") {
    // One tick short of giving up when the viewer is hidden: the hidden tick
    // discards that streak instead of completing it.
    WatchdogObservation hidden{/*cached=*/-1,
                               /*target=*/855,
                               /*prev_cached=*/-1,
                               /*prev_target=*/855,
                               /*stall_streak=*/MAX_STALL_KICKS - 1,
                               /*visible=*/false};
    WatchdogDecision d = watchdog_evaluate(hidden, MAX_STALL_KICKS);
    REQUIRE_FALSE(d.give_up);
    REQUIRE(d.stall_streak == 0);

    // Visible again and still stalled: a full run of kicks before it gives up.
    int streak = d.stall_streak;
    for (int tick = 1; tick < MAX_STALL_KICKS; ++tick) {
        CAPTURE(tick);
        WatchdogObservation obs{/*cached=*/-1,           /*target=*/855,
                                /*prev_cached=*/-1,      /*prev_target=*/855,
                                /*stall_streak=*/streak, /*visible=*/true};
        d = watchdog_evaluate(obs, MAX_STALL_KICKS);
        REQUIRE(d.kick);
        REQUIRE_FALSE(d.give_up);
        REQUIRE(d.stall_streak == tick);
        streak = d.stall_streak;
    }

    WatchdogObservation last{/*cached=*/-1,           /*target=*/855,
                             /*prev_cached=*/-1,      /*prev_target=*/855,
                             /*stall_streak=*/streak, /*visible=*/true};
    d = watchdog_evaluate(last, MAX_STALL_KICKS);
    CHECK_FALSE(d.kick);
    CHECK(d.give_up);
}
