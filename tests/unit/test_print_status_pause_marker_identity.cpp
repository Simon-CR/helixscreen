// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_pause_marker_identity.cpp
 * @brief A print-status gcode load only takes effect for the print
 *        PrinterState currently reports as effective (prestonbrown/helixscreen#1509).
 *
 * A gcode fetch crosses a metadata lookup, a download and the viewer's own
 * background build, and the print can change at any point along that chain.
 * Every stage compares the print it is fetching or loading against
 * PrinterState::get_effective_print_filename() before acting on it, and drops
 * the result instead of applying it when the two no longer match: the
 * currently-displayed print's geometry, gcode_displayed_file_ and its pause
 * markers are left exactly as they were, and ensure_preview_current()
 * reconciles against whichever print is effective by then.
 *
 * A load that DOES apply also leaves the running print's own state alone when
 * it turns out to be scoped wrong: the runout badge is not scoped to a file
 * the viewer no longer displays, and a stale file's layer count does not
 * become the print's.
 *
 * Every download is held by the transfer mock until the test releases it by
 * name, so two fetches can be made to land in either order.
 */

#include "ui_gcode_viewer.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_state.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Carries scheduled pauses, so its load publishes a non-empty list.
constexpr const char* PRINT_A = "pause_markers_demo.gcode";
/// Carries none.
constexpr const char* PRINT_B = "xyz-10mm-calibration-cube.gcode";

/// File transfers that park every download until the test completes it by
/// path, so two prints' downloads can land in either order.
class HeldFileTransfers : public MoonrakerFileTransferAPIMock {
  public:
    using MoonrakerFileTransferAPIMock::MoonrakerFileTransferAPIMock;

    void download_file_to_path(const std::string& root, const std::string& path,
                               const std::string& dest_path, StringCallback on_success,
                               ErrorCallback on_error, ProgressCallback on_progress) override {
        (void)on_progress;
        held_.push_back({path, [this, root, path, dest_path, on_success = std::move(on_success),
                                on_error = std::move(on_error)]() {
                             MoonrakerFileTransferAPIMock::download_file_to_path(
                                 root, path, dest_path, on_success, on_error);
                         }});
    }

    /// Complete the held download of @p path. False when none is held.
    bool release(const std::string& path) {
        for (auto it = held_.begin(); it != held_.end(); ++it) {
            if (it->path == path) {
                auto finish = std::move(it->finish);
                held_.erase(it);
                finish();
                return true;
            }
        }
        return false;
    }

    size_t held_count() const {
        return held_.size();
    }

  private:
    struct Held {
        std::string path;
        std::function<void()> finish;
    };
    std::vector<Held> held_;
};

class HeldTransfersAPIMock : public MoonrakerAPIMock {
  public:
    HeldTransfersAPIMock(helix::MoonrakerClient& client, helix::PrinterState& state,
                         HeldFileTransfers& transfers)
        : MoonrakerAPIMock(client, state), transfers_(transfers) {}

    MoonrakerFileTransferAPI& transfers() override {
        return transfers_;
    }

  private:
    HeldFileTransfers& transfers_;
};

/// A cache directory of this fixture's own. A copy left by an earlier load of
/// the same file takes the panel's cached-file branch, which loads without any
/// download and so never overlaps with anything.
struct FreshCacheDir {
    helix::ScopedEnv restore{"HELIX_CACHE_DIR"};
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("pause_marker_identity_" + std::to_string(::getpid()));

    FreshCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
        ::setenv("HELIX_CACHE_DIR", dir.c_str(), 1);
    }

    ~FreshCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

class PauseMarkerIdentityFixture : public LVGLTestFixture {
  public:
    PauseMarkerIdentityFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        // register_xml=false keeps these subjects out of the process-wide XML
        // registry, which outlives this fixture.
        state_.init_subjects(false);
        api_ = std::make_unique<HeldTransfersAPIMock>(client_, state_, transfers_);
        panel_ = std::make_unique<PrintStatusPanel>(state_, api_.get());

        // The streaming completion path sizes its 2D renderer from the widget's
        // coords, so the viewer gets a real size.
        viewer_ = ui_gcode_viewer_create(test_screen());
        REQUIRE(viewer_ != nullptr);
        lv_obj_set_size(viewer_, 240, 240);
        lv_obj_update_layout(viewer_);
        PrintStatusPanelTestAccess::set_gcode_viewer(*panel_, viewer_);
    }

    ~PauseMarkerIdentityFixture() override {
        // The viewer's load callback points at the panel, so the viewer goes first.
        PrintStatusPanelTestAccess::set_gcode_viewer(*panel_, nullptr);
        ui_gcode_viewer_clear(viewer_);
        lv_obj_delete(viewer_);
        process_lvgl(50);
        panel_.reset();
        helix::ui::UpdateQueue::instance().drain();
        state_.deinit_subjects();
    }

    /// Report @p filename as the printing file the way Moonraker does, so the
    /// print's identity is decided where production decides it.
    void report_print(const std::string& filename) {
        nlohmann::json status = {{"print_stats", {{"filename", filename}}}};
        state_.update_from_status(status);
        drain();
    }

    /// Start fetching @p filename for the viewer and run the fetch up to its
    /// download, which the transfer mock holds.
    void start_fetch(const std::string& filename) {
        const size_t held_before = transfers_.held_count();
        PrintStatusPanelTestAccess::load_gcode_for_viewing(*panel_, filename);
        drain();
        REQUIRE(transfers_.held_count() == held_before + 1);
    }

    /// Complete @p filename's download and wait for the viewer load it starts
    /// to reach the panel's load callback, which publishes the scan. The
    /// callback also defers state writes through the UpdateQueue, so the queue
    /// is drained before anything is read back.
    void land(const std::string& filename) {
        const int version = pause_markers_version();
        REQUIRE(transfers_.release(filename));
        REQUIRE(wait_until([&] { return pause_markers_version() > version; }, 30000));
        drain();
    }

    /// Complete @p filename's held download for a load the panel is expected
    /// to drop as no longer the effective print. A dropped load never reaches
    /// the viewer and never starts a background build, so there is no publish
    /// to wait on: releasing the download and draining the queue that carries
    /// it to load_gcode_file() is the whole round trip in the passing case.
    /// The extra settle wait matters only when a gate under test has been
    /// removed by hand: it gives a load that slipped past it time to finish
    /// its background build and deliver, so the assertions that follow see
    /// its result rather than a work-in-progress false negative.
    void land_dropped(const std::string& filename) {
        REQUIRE(transfers_.release(filename));
        drain();
        // A load that slipped past the entry gate (a temporary revert, when
        // proving that gate is load-bearing) starts a real background build,
        // and the very first one run against a fresh viewer can take well
        // over a second before it is queued for delivery. Waiting on the
        // queue rather than a fixed sleep is what makes this reliable either
        // way; the drain that follows is what actually delivers it, which is
        // also where the load callback's own identity check - still present
        // even with the entry gate reverted - routes a stale delivery through
        // ensure_preview_current() and clears whatever it just installed.
        wait_for_queued_result(std::chrono::seconds(5));
        drain();
    }

    /// Wait on the real clock, without draining the UpdateQueue, until a
    /// worker has queued something. Lets a test observe a load's background
    /// build finishing before the result is delivered to the panel.
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

    int pause_markers_version() {
        return lv_subject_get_int(state_.get_pause_markers_version_subject());
    }

    const std::string& gcode_displayed_file() const {
        return PrintStatusPanelTestAccess::gcode_displayed_file(*panel_);
    }

    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    FreshCacheDir cache_;
    MoonrakerClientMock client_;
    PrinterState state_;
    HeldFileTransfers transfers_{client_, ""};
    std::unique_ptr<MoonrakerAPIMock> api_;
    std::unique_ptr<PrintStatusPanel> panel_;
    lv_obj_t* viewer_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(PauseMarkerIdentityFixture,
                 "Print status: a gcode load for the current print shows its pause markers",
                 "[print_status][pause_markers][1509][slow]") {
    report_print(PRINT_A);
    start_fetch(PRINT_A);
    land(PRINT_A);

    REQUIRE_FALSE(state_.get_scheduled_pauses().empty());
    CHECK(state_.pause_markers_match_current_file());
    CHECK(gcode_displayed_file() == PRINT_A);
}

TEST_CASE_METHOD(PauseMarkerIdentityFixture,
                 "Print status: a superseded print's gcode load cannot mark the new print",
                 "[print_status][pause_markers][1509][slow]") {
    report_print(PRINT_A);
    start_fetch(PRINT_A);

    // A is cancelled and B started while A's download is still running.
    report_print(PRINT_B);
    start_fetch(PRINT_B);

    SECTION("A lands while B is still downloading") {
        land_dropped(PRINT_A);

        // A's load never reached the viewer: neither print has anything
        // displayed yet, its scan was never published, and the widget itself
        // holds no geometry - not just the panel's own bookkeeping.
        CHECK(state_.get_scheduled_pauses().empty());
        CHECK(gcode_displayed_file().empty());
        CHECK_FALSE(ui_gcode_viewer_has_content(viewer_));

        land(PRINT_B);

        // B's own load is the only one that ever applied: PRINT_B carries no
        // pauses, and its displayed-file name is its own, not A's.
        CHECK(state_.get_scheduled_pauses().empty());
        CHECK(gcode_displayed_file() == PRINT_B);
    }

    SECTION("A lands after B has loaded") {
        land(PRINT_B);
        CHECK(gcode_displayed_file() == PRINT_B);
        const int version_after_b = pause_markers_version();
        const char* widget_file_raw = ui_gcode_viewer_get_filename(viewer_);
        const std::string widget_file_after_b = widget_file_raw ? widget_file_raw : "";

        land_dropped(PRINT_A);

        // A's late load is dropped before it reaches the viewer: B's own
        // geometry and pause markers, published above, are untouched by it -
        // including the widget's own record of which file it holds, not just
        // the panel's copy of that name.
        CHECK(pause_markers_version() == version_after_b);
        CHECK(state_.get_scheduled_pauses().empty());
        CHECK(gcode_displayed_file() == PRINT_B);
        widget_file_raw = ui_gcode_viewer_get_filename(viewer_);
        CHECK((widget_file_raw ? std::string(widget_file_raw) : std::string()) ==
              widget_file_after_b);
    }
}

TEST_CASE_METHOD(PauseMarkerIdentityFixture,
                 "Print status: a load that goes stale mid-flight is dropped when it is delivered",
                 "[print_status][pause_markers][1509][slow]") {
    report_print(PRINT_A);
    start_fetch(PRINT_A);

    // A is still the effective print when its download lands, so the entry
    // check passes and the viewer's own background build actually starts.
    REQUIRE(transfers_.release(PRINT_A));
    drain();

    // A's build finished and its result is sitting in the UpdateQueue,
    // undelivered, when the print moves on. Nothing has fetched B's own gcode
    // yet, so the viewer's widget-level load generation never advances: only
    // the panel's own identity check, made when the result is delivered, can
    // catch this.
    REQUIRE(wait_for_queued_result(std::chrono::seconds(30)));

    nlohmann::json status = {{"print_stats", {{"filename", PRINT_B}}}};
    state_.update_from_status(status);
    REQUIRE(state_.get_effective_print_filename() == PRINT_B);

    drain();

    CHECK(gcode_displayed_file().empty());
    CHECK(state_.get_scheduled_pauses().empty());
}

namespace {

constexpr const char* RUNOUT_SENSOR = "filament_switch_sensor runout";
/// Uses tools 0-3 across nine layers, so a load of it has a tool scope and a
/// layer count to impose.
constexpr const char* STALE_PRINT = "u1_4color_ring.gcode";

/// The panel as a running print sees it: subjects up, the print reported as
/// printing, and one runout sensor with no filament system, so the print-scoped
/// runout badge takes its value from the tools of the file in the viewer.
class StaleLoadFixture : public PauseMarkerIdentityFixture {
  public:
    StaleLoadFixture() {
        AmsState::instance().init_subjects(true);
        AmsState::instance().clear_backends();

        auto& fsm = FilamentSensorManager::instance();
        fsm.init_subjects();
        PostUnloadGraceTestAccess::reset(fsm);
        fsm.set_master_enabled(true);
        fsm.discover_sensors({RUNOUT_SENSOR});
        fsm.set_sensor_role(RUNOUT_SENSOR, FilamentSensorRole::RUNOUT);
        PostUnloadGraceTestAccess::clear_startup_grace(fsm);
        fsm.update_from_status(
            nlohmann::json{{RUNOUT_SENSOR, {{"filament_detected", true}, {"enabled", true}}}});

        panel_->init_subjects();
        drain();
    }

    ~StaleLoadFixture() override {
        PostUnloadGraceTestAccess::reset(FilamentSensorManager::instance());
    }

    void report_printing(const std::string& filename) {
        nlohmann::json status = {{"print_stats", {{"filename", filename}, {"state", "printing"}}}};
        state_.update_from_status(status);
        drain();
    }

    int scoped_runout() {
        return lv_subject_get_int(FilamentSensorManager::instance().get_scoped_runout_subject());
    }

    int layer_total() {
        return lv_subject_get_int(state_.get_print_layer_total_subject());
    }
};

} // namespace

TEST_CASE_METHOD(StaleLoadFixture,
                 "Print status: a superseded print's gcode load leaves the new print's state alone",
                 "[print_status][pause_markers][1509][slow]") {
    report_printing(STALE_PRINT);
    start_fetch(STALE_PRINT);

    // The stale print is cancelled and B started while its download is running.
    report_printing(PRINT_B);
    start_fetch(PRINT_B);

    // B has no file in the viewer yet: no tools to scope the badge to, and no
    // layer count from metadata.
    REQUIRE(scoped_runout() == -1);
    REQUIRE(layer_total() == 0);

    land_dropped(STALE_PRINT);

    // The stale load is dropped before it reaches the viewer: nothing has
    // been displayed yet, for either print, and the widget itself holds no
    // geometry - not just the panel's own bookkeeping.
    REQUIRE(gcode_displayed_file().empty());
    CHECK_FALSE(ui_gcode_viewer_has_content(viewer_));
    CHECK(scoped_runout() == -1);
    CHECK(layer_total() == 0);

    // B's own load does apply both, so the two checks above are the drop
    // holding the stale load back rather than effects that never run.
    land(PRINT_B);
    CHECK(scoped_runout() == 1);
    CHECK(layer_total() > 0);
}

TEST_CASE_METHOD(
    StaleLoadFixture,
    "Print status: a runout-sensor edge does not scope the badge to a stale load's tools",
    "[print_status][pause_markers][1509][slow]") {
    // The stale print's own load lands while it is still the effective print,
    // so it applies normally: the viewer really does hold its geometry.
    report_printing(STALE_PRINT);
    start_fetch(STALE_PRINT);
    land(STALE_PRINT);
    REQUIRE(gcode_displayed_file() == STALE_PRINT);
    REQUIRE(scoped_runout() == 1);

    // The print moves on, but nothing has fetched B's own gcode yet: the
    // viewer's widget still physically holds the stale print's geometry
    // (tools 0-3), even though set_filename() cleared the displayed-file
    // marker to force a reload.
    report_printing(PRINT_B);
    REQUIRE(gcode_displayed_file().empty());

    // A runout-sensor edge or an AMS slots_version bump recomputes the badge
    // independently of any load completing (the observers at
    // scoped_runout_observer_ / scoped_runout_slots_observer_ call
    // recompute_scoped_runout() directly). Without the guard this would read
    // the stale print's tools straight off the viewer.
    PrintStatusPanelTestAccess::recompute_scoped_runout(*panel_);
    CHECK(scoped_runout() == -1);

    // B's own load does apply the real value once it lands, so the check
    // above is the guard holding the badge back rather than a badge that
    // never updates.
    start_fetch(PRINT_B);
    land(PRINT_B);
    CHECK(scoped_runout() == 1);
}
