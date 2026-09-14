// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_viewer_superseded_load.cpp
 * @brief Only the newest gcode viewer load reports (prestonbrown/helixscreen#1509).
 *
 * A load's result reaches the main thread through the UpdateQueue, so it can be
 * sitting there, already built, when the next ui_gcode_viewer_load_file()
 * starts. That superseded result must be dropped: it must not install its file
 * and must not invoke the load callback. PrintStatusPanel names the print a
 * load was for when the load starts, so a superseded load that still reported
 * would be announced under the newer load's print.
 *
 * Each section holds load X's finished result in the queue, starts load Y,
 * and requires exactly one report, carrying Y.
 */

#include "ui_gcode_viewer.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/update_queue_test_access.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

struct LoadReport {
    bool success;
    std::string filename;
};

std::vector<LoadReport> g_reports;

void on_load_done(lv_obj_t* viewer, void*, bool success) {
    const char* name = ui_gcode_viewer_get_filename(viewer);
    g_reports.push_back({success, name ? name : ""});
}

/// Resolve a shipped gcode asset from the repo root or a build subdirectory.
std::string find_test_asset(const std::string& filename) {
    for (const auto& prefix : {"", "../", "../../"}) {
        std::string path = std::string(prefix) + "assets/test_gcodes/" + filename;
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return {};
}

/// Wait on the real clock, without draining the UpdateQueue, until a worker
/// has queued something.
bool wait_for_queued_result(std::chrono::milliseconds budget) {
    auto& queue = helix::ui::UpdateQueue::instance();
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!helix::ui::UpdateQueueTestAccess::queue_empty(queue)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !helix::ui::UpdateQueueTestAccess::queue_empty(queue);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "GCode viewer: a load superseded by a newer one never reports",
                 "[gcode][viewer][1509][slow]") {
    const std::string first = find_test_asset("pause_markers_demo.gcode");
    const std::string second = find_test_asset("xyz-10mm-calibration-cube.gcode");
    REQUIRE_FALSE(first.empty()); // run helix-tests from the repo root
    REQUIRE_FALSE(second.empty());

    // HELIX_GCODE_STREAMING is read on every load; the guard puts it back so
    // later loads in this binary keep their own mode.
    helix::ScopedEnv restore_streaming("HELIX_GCODE_STREAMING");
    const char* stale_log = nullptr;
    SECTION("full load") {
        ::setenv("HELIX_GCODE_STREAMING", "off", 1);
        stale_log = "Stale async callback";
    }
    SECTION("streaming") {
        ::setenv("HELIX_GCODE_STREAMING", "on", 1);
        stale_log = "Stale streaming callback";
    }
    REQUIRE(stale_log != nullptr);

    lv_obj_t* viewer = ui_gcode_viewer_create(test_screen());
    REQUIRE(viewer != nullptr);
    lv_obj_set_size(viewer, 240, 240);
    lv_obj_update_layout(viewer);

    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    g_reports.clear();
    ui_gcode_viewer_set_load_callback(viewer, on_load_done, nullptr);

    {
        // Worker threads log while this is alive, which only an exclusive
        // capture tolerates.
        helix::ExclusiveLogCapture log(1024);

        ui_gcode_viewer_load_file(viewer, first.c_str());

        // The first load's result is built and waiting in the queue, not yet
        // delivered, when the second load starts.
        REQUIRE(wait_for_queued_result(std::chrono::seconds(30)));
        REQUIRE(g_reports.empty());

        ui_gcode_viewer_load_file(viewer, second.c_str());

        REQUIRE(wait_until([] { return !g_reports.empty(); }, 30000));
        // Keep pumping after the report: a superseded result that slipped past
        // the viewer would be delivered in this window.
        wait_until([] { return false; }, 300);

        // The first load's result was queued and then rejected, so the single
        // report below is the gate working rather than a load that never ran.
        CHECK(log.count_containing(stale_log) >= 1);
    }

    CHECK(g_reports.size() == 1);
    REQUIRE_FALSE(g_reports.empty());
    CHECK(g_reports.front().success);
    CHECK(g_reports.front().filename.find("xyz-10mm-calibration-cube.gcode") != std::string::npos);
    CHECK(ui_gcode_viewer_has_content(viewer));

    ui_gcode_viewer_set_load_callback(viewer, nullptr, nullptr);
    ui_gcode_viewer_clear(viewer);
    lv_obj_delete(viewer);
    process_lvgl(50);
}
