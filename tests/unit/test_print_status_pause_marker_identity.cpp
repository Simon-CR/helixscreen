// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_pause_marker_identity.cpp
 * @brief A print-status gcode load names the print it was fetched for
 *        (prestonbrown/helixscreen#1509).
 *
 * When a load completes, the panel publishes the scan's scheduled pauses to
 * PrinterState and records whose geometry the viewer holds. Both must name the
 * file that load was for. Two fetches overlap whenever print A's download is
 * still running as print B starts; if A's load lands and publishes under B's
 * name, A's ticks draw on B's progress bar, and ensure_preview_current()
 * believes B's geometry is on screen.
 *
 * Every download is held by the transfer mock until the test releases it by
 * name, so the overlap is ordered by the test instead of by timing.
 */

#include "ui_gcode_viewer.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/update_queue_test_access.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
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
    /// to reach the panel's load callback, which publishes the scan.
    void land(const std::string& filename) {
        const int version = pause_markers_version();
        REQUIRE(transfers_.release(filename));
        REQUIRE(wait_until([&] { return pause_markers_version() > version; }, 30000));
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
        land(PRINT_A);

        // A's scan was published, so the hidden markers are the identity gate
        // rejecting it rather than a publish that never happened.
        REQUIRE_FALSE(state_.get_scheduled_pauses().empty());
        CHECK_FALSE(state_.pause_markers_match_current_file());
        CHECK(gcode_displayed_file() == PRINT_A);

        land(PRINT_B);

        CHECK(state_.get_scheduled_pauses().empty());
        CHECK(gcode_displayed_file() == PRINT_B);
    }

    SECTION("A lands after B has loaded") {
        land(PRINT_B);
        CHECK(gcode_displayed_file() == PRINT_B);

        land(PRINT_A);

        REQUIRE_FALSE(state_.get_scheduled_pauses().empty());
        CHECK_FALSE(state_.pause_markers_match_current_file());
        CHECK(gcode_displayed_file() == PRINT_A);
    }
}
