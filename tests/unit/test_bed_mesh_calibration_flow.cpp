// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Mesh calibration over the mock client, through the real MoonrakerAdvancedAPI
// collector.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "moonraker_advanced_api.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;
using json = nlohmann::json;

namespace {

void drain() {
    for (int i = 0; i < 8; ++i) {
        helix::ui::UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    }
}

bool mentions(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

class CalibrationCollectorFixture : public LVGLTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    MoonrakerAdvancedAPI advanced{client, api};

    int completions = 0;
    std::vector<std::string> errors;
    std::vector<std::pair<int, int>> progress;

    CalibrationCollectorFixture() {
        state.init_subjects(false);
    }
    ~CalibrationCollectorFixture() override {
        drain();
    }

    /// A calibration whose RPC never returns, so only console lines can end it.
    void start() {
        client.force_next_gcode_dropped_response("BED_MESH_CALIBRATE");
        advanced.start_bed_mesh_calibrate(
            {"BED_MESH_CALIBRATE BED_TEMP=60", /*self_prepares=*/true},
            [this](int current, int total) { progress.emplace_back(current, total); },
            [this]() { ++completions; },
            [this](const MoonrakerError& err) { errors.push_back(err.message); },
            /*expected_probes=*/81, /*probe_samples=*/1);
        REQUIRE(client.gcode_script_history().size() == 1);
        REQUIRE(completions == 0);
        REQUIRE(errors.empty());
    }
};

} // namespace

// ============================================================================
// Collector: console lines that end a calibration
// ============================================================================

TEST_CASE_METHOD(CalibrationCollectorFixture, "an unknown command during calibration fails it",
                 "[bed_mesh_flow][cc1]") {
    start();
    client.dispatch_gcode_response("// probe at 9.998,9.998 is z=0.265669");
    REQUIRE(progress.size() == 1); // the collector is listening

    // Kalico's spelling: no space after the colon, the name quoted.
    client.dispatch_gcode_response("// Unknown command:\"LOAD_CELL_SAVE_TARE\"");
    REQUIRE(errors.size() == 1);
    CHECK(mentions(errors[0], "LOAD_CELL_SAVE_TARE"));
    CHECK(completions == 0);

    // A failed calibration stays failed.
    client.dispatch_gcode_response("// Mesh Bed Leveling Complete");
    CHECK(completions == 0);
    CHECK(errors.size() == 1);
}

TEST_CASE_METHOD(CalibrationCollectorFixture,
                 "an unknown BED_MESH_CALIBRATE still points at the missing [bed_mesh]",
                 "[bed_mesh_flow]") {
    start();
    client.dispatch_gcode_response("// Unknown command:\"BED_MESH_CALIBRATE\"");
    REQUIRE(errors.size() == 1);
    CHECK(mentions(errors[0], "[bed_mesh]"));
}

TEST_CASE_METHOD(CalibrationCollectorFixture,
                 "a line that only mentions an unknown command does not fail calibration",
                 "[bed_mesh_flow]") {
    start();
    client.dispatch_gcode_response("// Skipping Unknown command:\"FOO\" in a narration line");
    client.dispatch_gcode_response("// Mesh Bed Leveling Complete");
    CHECK(errors.empty());
    CHECK(completions == 1);
}
