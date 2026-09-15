// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_starfield_sim.h"

#include "screensaver_motion.h"

#include <cmath>
#include <cstring>

namespace helix::ui {

namespace {

using screensaver::random_below;
using screensaver::unit_random;

// A star's speed is its depth change per this many ms of frame time
constexpr float SPEED_FRAME_MS = 33.0f;
constexpr float COLOR_THRESHOLD = 0.35f; // stars closer than this show color

// Opaque black, with the X byte at the 0xFF SCREENSAVER_CANVAS_FORMAT requires
constexpr uint32_t PIXEL_BLACK = 0xFF000000;

// Star color tints — blue dwarfs, red giants, yellow suns, blue-white hot stars
constexpr uint8_t STAR_TINTS[][3] = {
    {255, 255, 255}, // white (most common)
    {255, 255, 255}, // white
    {255, 255, 255}, // white
    {255, 200, 150}, // warm yellow
    {255, 160, 120}, // orange
    {255, 120, 100}, // red giant
    {150, 180, 255}, // blue dwarf
    {200, 220, 255}, // blue-white
};
constexpr int NUM_TINTS = sizeof(STAR_TINTS) / sizeof(STAR_TINTS[0]);

void assign_tint(std::minstd_rand& rng, uint8_t& r, uint8_t& g, uint8_t& b) {
    int idx = random_below(rng, NUM_TINTS);
    r = STAR_TINTS[idx][0];
    g = STAR_TINTS[idx][1];
    b = STAR_TINTS[idx][2];
}

/// Writes one pixel byte by byte, so the frame's storage type does not matter.
void put_pixel(uint8_t* row, int x, uint32_t pixel) {
    std::memcpy(row + static_cast<size_t>(x) * 4, &pixel, sizeof(pixel));
}

/// Paints the size x size square at (sx, sy), clipped to the frame, and adds what it wrote
/// to `dirty`.
void fill_square(FrameTarget& target, int sx, int sy, int size, uint32_t pixel, DirtyRect& dirty) {
    const int x1 = std::max(sx, 0);
    const int y1 = std::max(sy, 0);
    const int x2 = std::min(sx + size, static_cast<int>(target.w)) - 1;
    const int y2 = std::min(sy + size, static_cast<int>(target.h)) - 1;
    if (x2 < x1 || y2 < y1) {
        return;
    }
    // Rows step by the frame's stride: indexing by width alone skews every row past the
    // first when the stride exceeds w * 4.
    for (int y = y1; y <= y2; y++) {
        uint8_t* row = target.data + static_cast<size_t>(y) * target.stride;
        for (int x = x1; x <= x2; x++) {
            put_pixel(row, x, pixel);
        }
    }
    dirty.add(x1, y1, x2, y2);
}

} // namespace

void fill_starfield_black(FrameTarget& target) {
    for (uint32_t y = 0; y < target.h; y++) {
        uint8_t* row = target.data + static_cast<size_t>(y) * target.stride;
        for (uint32_t x = 0; x < target.w; x++) {
            put_pixel(row, static_cast<int>(x), PIXEL_BLACK);
        }
    }
}

void StarfieldSim::init(uint32_t w, uint32_t h, std::minstd_rand& rng) {
    cx_ = static_cast<float>(w) / 2.0f;
    cy_ = static_cast<float>(h) / 2.0f;
    focal_ = static_cast<float>(w) / 3.0f;

    stars_.resize(NUM_STARS);
    for (auto& star : stars_) {
        float angle = unit_random(rng) * 2.0f * 3.14159265f;
        float radius = 0.1f + unit_random(rng) * 0.9f;
        star.x = radius * std::cos(angle);
        star.y = radius * std::sin(angle);
        star.z = 0.01f + unit_random(rng) * 0.99f;
        star.speed = 0.008f + unit_random(rng) * 0.017f;
        assign_tint(rng, star.tint_r, star.tint_g, star.tint_b);
        star.prev_sx = 0;
        star.prev_sy = 0;
        star.prev_size = 0;
    }
}

void StarfieldSim::recycle(Star& star, std::minstd_rand& rng) {
    // Pick random angle + radius so stars fly uniformly in all directions
    float angle = unit_random(rng) * 2.0f * 3.14159265f;
    float radius = 0.3f + unit_random(rng) * 0.7f;
    star.x = radius * std::cos(angle);
    star.y = radius * std::sin(angle);
    star.z = 1.0f;
    star.speed = 0.008f + unit_random(rng) * 0.017f;
    assign_tint(rng, star.tint_r, star.tint_g, star.tint_b);
}

DirtyRect StarfieldSim::step(uint32_t dt_ms, FrameTarget& target, std::minstd_rand& rng) {
    DirtyRect dirty;
    const int w = static_cast<int>(target.w);
    const int h = static_cast<int>(target.h);

    // Erase previous star positions (incremental clear — avoids a full-frame fill)
    for (auto& star : stars_) {
        if (star.prev_size == 0) {
            continue;
        }
        fill_square(target, star.prev_sx, star.prev_sy, star.prev_size, PIXEL_BLACK, dirty);
        star.prev_size = 0;
    }

    const float frames = static_cast<float>(dt_ms) / SPEED_FRAME_MS;
    for (auto& star : stars_) {
        // Move star closer by its speed, scaled to the time since the previous frame
        star.z -= star.speed * frames;

        if (star.z <= 0.01f) {
            recycle(star, rng);
            continue;
        }

        // Project to frame coordinates
        float sx = cx_ + (star.x / star.z) * focal_;
        float sy = cy_ + (star.y / star.z) * focal_;

        if (sx < 0 || sx >= w || sy < 0 || sy >= h) {
            recycle(star, rng);
            continue;
        }

        int isx = static_cast<int>(sx);
        int isy = static_cast<int>(sy);

        // Size: larger when closer (z near 0)
        int size = std::max(1, static_cast<int>(3.0f * (1.0f - star.z)));

        // Brightness: brighter when closer, with minimum floor
        float bright_f = 80.0f + 175.0f * (1.0f - star.z);

        // Close stars show their color tint; distant stars stay white
        uint8_t r, g, b;
        if (star.z < COLOR_THRESHOLD) {
            float tint_mix = (COLOR_THRESHOLD - star.z) / COLOR_THRESHOLD;
            r = static_cast<uint8_t>(bright_f *
                                     (1.0f - tint_mix + tint_mix * star.tint_r / 255.0f));
            g = static_cast<uint8_t>(bright_f *
                                     (1.0f - tint_mix + tint_mix * star.tint_g / 255.0f));
            b = static_cast<uint8_t>(bright_f *
                                     (1.0f - tint_mix + tint_mix * star.tint_b / 255.0f));
        } else {
            r = g = b = static_cast<uint8_t>(bright_f);
        }

        // X byte 0xFF from PIXEL_BLACK
        const uint32_t pixel =
            PIXEL_BLACK | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
        fill_square(target, isx, isy, size, pixel, dirty);

        // Remember position for next step's erase pass
        star.prev_sx = static_cast<int16_t>(isx);
        star.prev_sy = static_cast<int16_t>(isy);
        star.prev_size = static_cast<uint8_t>(size);
    }

    return dirty;
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
