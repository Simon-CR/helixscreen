// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <random>

/**
 * @file screensaver_motion.h
 * @brief Frame-time-driven motion for the screensavers
 *
 * The savers run their timers at the display refresh period, and a callback can
 * arrive late on a loaded board. Motion here is a function of elapsed time, so a
 * sprite's path and a pipe's growth rate never depend on how often or how evenly
 * the callback runs. No LVGL, so the rules are testable without a display.
 */

namespace helix::ui::screensaver {

/**
 * @brief Time between successive calls, read from a millisecond tick that wraps at 2^32
 */
struct MotionClock {
    /// One gap longer than this counts as this long, so after a stall the motion resumes
    /// where it stopped instead of jumping ahead.
    static constexpr uint32_t MAX_GAP_MS = 500;

    constexpr void reset(uint32_t now_ms) {
        last_ms = now_ms;
    }

    /// Milliseconds since the previous reset() or advance(), at most MAX_GAP_MS.
    constexpr uint32_t advance(uint32_t now_ms) {
        // Unsigned subtraction stays exact across the tick wrap.
        const uint32_t gap = now_ms - last_ms;
        last_ms = now_ms;
        return std::min(gap, MAX_GAP_MS);
    }

    uint32_t last_ms = 0;
};

struct FlightPos {
    int32_t x;
    int32_t y;
    bool started; ///< false while the start delay is still running
};

/**
 * @brief Where a flying object is `elapsed_ms` after the saver started
 *
 * The object waits at (sx, sy) for `delay_ms` (>= 0), then flies `distance` px left and
 * `distance` px down over `fly_ms` (> 0), and starts over from (sx, sy).
 */
constexpr FlightPos flight_pos_at(uint32_t elapsed_ms, int32_t sx, int32_t sy, int32_t fly_ms,
                                  int32_t delay_ms, int32_t distance) {
    const auto delay = static_cast<uint32_t>(delay_ms);
    if (elapsed_ms < delay) {
        return {sx, sy, false};
    }
    const uint32_t t = (elapsed_ms - delay) % static_cast<uint32_t>(fly_ms);
    const auto travelled = static_cast<int32_t>(static_cast<int64_t>(distance) * t / fly_ms);
    return {sx - travelled, sy + travelled, true};
}

/**
 * @brief Wing frame a toaster shows `elapsed_ms` after the saver started
 *
 * The wings cycle 0,1,2,3,2,1 and repeat, one frame per `frame_step_ms` (> 0). The cycle
 * starts at `initial_frame` (0-3, on the rising half), which holds until `delay_ms` (>= 0)
 * has passed.
 */
constexpr uint8_t flap_frame_at(uint32_t elapsed_ms, int32_t delay_ms, uint32_t frame_step_ms,
                                uint8_t initial_frame) {
    constexpr uint8_t cycle[] = {0, 1, 2, 3, 2, 1};
    constexpr uint32_t cycle_len = sizeof(cycle);
    const auto delay = static_cast<uint32_t>(delay_ms);
    const uint32_t steps = elapsed_ms < delay ? 0 : (elapsed_ms - delay) / frame_step_ms;
    return cycle[(initial_frame % cycle_len + steps % cycle_len) % cycle_len];
}

/**
 * @brief Whole fixed-length steps that come due as time passes
 */
struct StepAccumulator {
    /// Steps that came due over `dt_ms`, each `step_ms` (> 0) long, at most `cap`. Steps
    /// past the cap are dropped rather than owed, so a stall costs one capped burst and
    /// never a backlog. The partial step carries to the next call.
    constexpr uint32_t steps_due(uint32_t dt_ms, uint32_t step_ms, uint32_t cap) {
        carry_ms += dt_ms;
        const uint32_t steps = carry_ms / step_ms;
        carry_ms %= step_ms;
        return std::min(steps, cap);
    }

    /// Drops the partial step, so the next step is a full step away.
    constexpr void reset() {
        carry_ms = 0;
    }

    uint32_t carry_ms = 0;
};

/// Uniform in [0, 1]. Computed from the engine's raw output rather than a standard
/// distribution, so a seed gives the same values on every standard library.
inline float unit_random(std::minstd_rand& rng) {
    const auto span = static_cast<float>(std::minstd_rand::max() - std::minstd_rand::min());
    return static_cast<float>(rng() - std::minstd_rand::min()) / span;
}

/// An integer in [0, n), for n > 0.
inline int random_below(std::minstd_rand& rng, int n) {
    return static_cast<int>(rng() % static_cast<std::minstd_rand::result_type>(n));
}

} // namespace helix::ui::screensaver
