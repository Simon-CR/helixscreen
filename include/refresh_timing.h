// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>

namespace helix {

/**
 * @brief Refresh pacing settings
 *
 * Every field keeps today's behaviour at its default, so a default-constructed value
 * changes nothing. Read from the environment by refresh_timing_from_env()
 * (refresh_timing_env.h).
 */
struct RefreshTiming {
    static constexpr uint32_t DEFAULT_LOOP_MIN_SLEEP_MS = 5;
    static constexpr uint32_t MIN_PERIOD_MS = 8;
    static constexpr uint32_t MAX_PERIOD_MS = 100;
    static constexpr uint32_t MIN_LOOP_SLEEP_MS = 1;
    static constexpr uint32_t MAX_LOOP_SLEEP_MS = 33;

    /// HELIX_REFR_PERIOD_MS. 0 leaves LVGL's refresh and animation periods alone.
    uint32_t refr_period_ms = 0;
    /// HELIX_REFR_PERIOD_SCOPE=all. Also paces input reads and the update queue.
    bool scope_all = false;
    /// HELIX_SCREENSAVER_REFR_PERIOD_MS. 0 runs screensavers at the global period.
    uint32_t screensaver_refr_period_ms = 0;
    /// HELIX_LOOP_MIN_SLEEP_MS. The main loop never sleeps less than this.
    uint32_t loop_min_sleep_ms = DEFAULT_LOOP_MIN_SLEEP_MS;
};

/**
 * @brief How long the main loop sleeps before its next lv_timer_handler() call
 *
 * LVGL's hint for when the next timer is due, capped so the loop stays responsive (33 ms
 * awake, 200 ms while the display sleeps and only wake events matter) and floored so a
 * timer that is already due cannot spin the loop. A refresh period longer than the awake
 * cap raises the cap to match, so the loop does not wake between refreshes for nothing.
 */
constexpr uint32_t main_loop_sleep_ms(uint32_t time_till_next, bool display_sleeping,
                                      const RefreshTiming& timing) {
    constexpr uint32_t AWAKE_CAP_MS = 33;
    constexpr uint32_t SLEEPING_CAP_MS = 200;
    const uint32_t cap =
        display_sleeping ? SLEEPING_CAP_MS : std::max(AWAKE_CAP_MS, timing.refr_period_ms);
    return std::max(std::min(time_till_next, cap), timing.loop_min_sleep_ms);
}

} // namespace helix
