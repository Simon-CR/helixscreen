// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_chamber_heater_assignment.cpp
 * @brief helix::chamber::resolve_heater(): which chamber heater a printer has
 *        under its chamber-heater assignment.
 *
 * The wiring (PrinterState publishing the answer, and the consumers reading
 * it) is pinned in test_printer_state.cpp, test_temperature_controller.cpp,
 * test_material_temps_chamber.cpp and test_filament_panel_chamber.cpp.
 */

#include "chamber_heater_assignment.h"
#include "printer_discovery.h"

#include <initializer_list>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using helix::chamber::resolve_heater;

namespace {

PrinterDiscovery discovered(std::initializer_list<const char*> objects) {
    PrinterDiscovery hw;
    nlohmann::json list = nlohmann::json::array();
    for (const char* object : objects) {
        list.push_back(object);
    }
    hw.parse_objects(list);
    return hw;
}

/// The name a model preset seeds for its family's chamber heater before the
/// wizard runs, and so before anything has checked the printer has one.
constexpr const char* PRESET_CHAMBER_HEATER = "heater_generic chamber_heater";

} // namespace

TEST_CASE("auto takes the chamber heater discovery picked", "[chamber][assignment]") {
    CHECK(resolve_heater("auto", discovered({"heater_generic chamber", "extruder"})) ==
          "heater_generic chamber");
    CHECK(resolve_heater("auto", discovered({"temperature_fan chamber_fan", "extruder"})) ==
          "temperature_fan chamber_fan");
    CHECK(resolve_heater("auto", discovered({"extruder", "heater_bed"})).empty());
}

TEST_CASE("none disables a chamber heater discovery found", "[chamber][assignment]") {
    const auto hw = discovered({"heater_generic chamber", "extruder", "heater_bed"});
    REQUIRE(hw.has_chamber_heater());
    CHECK(resolve_heater("none", hw).empty());
}

TEST_CASE("a named heater Klipper reports is the chamber heater", "[chamber][assignment]") {
    SECTION("one no chamber keyword names, so only the assignment can pick it") {
        const auto hw = discovered({"heater_generic ptc_heater", "extruder", "heater_bed"});
        REQUIRE_FALSE(hw.has_chamber_heater());
        CHECK(resolve_heater("heater_generic ptc_heater", hw) == "heater_generic ptc_heater");
    }
    SECTION("over the heater discovery picked") {
        const auto hw = discovered({"heater_generic chamber", "heater_generic ptc_heater"});
        REQUIRE(hw.chamber_heater_name() == "heater_generic chamber");
        CHECK(resolve_heater("heater_generic ptc_heater", hw) == "heater_generic ptc_heater");
    }
    SECTION("the preset's own heater on a printer that has it") {
        const auto hw = discovered({PRESET_CHAMBER_HEATER, "temperature_fan chamber_fan"});
        CHECK(resolve_heater(PRESET_CHAMBER_HEATER, hw) == PRESET_CHAMBER_HEATER);
    }
}

TEST_CASE("a named heater Klipper does not report is not the chamber heater",
          "[chamber][assignment]") {
    SECTION("a printer with no chamber heater has none") {
        const auto hw = discovered({"temperature_sensor chamber_temp", "extruder", "heater_bed"});
        REQUIRE_FALSE(hw.has_chamber_heater());
        CHECK(resolve_heater(PRESET_CHAMBER_HEATER, hw).empty());
    }
    SECTION("a printer whose chamber heater has another name keeps discovery's pick") {
        const auto hw = discovered({"heater_generic chamber", "extruder", "heater_bed"});
        CHECK(resolve_heater(PRESET_CHAMBER_HEATER, hw) == "heater_generic chamber");
    }
    SECTION("the name must match a reported object exactly") {
        const auto hw = discovered({PRESET_CHAMBER_HEATER, "extruder", "heater_bed"});
        CHECK(resolve_heater("heater_generic chamber", hw) == PRESET_CHAMBER_HEATER);
    }
    SECTION("nothing counts before Klipper has reported its objects") {
        const PrinterDiscovery hw;
        REQUIRE_FALSE(hw.objects_reported());
        CHECK(resolve_heater(PRESET_CHAMBER_HEATER, hw).empty());
    }
}
