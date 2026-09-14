// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_panel_chamber.cpp
 * @brief FilamentPanel's chamber heater is the one PrinterState resolved, the
 *        same heater TemperatureController sends to.
 *
 * Cool Down's default gcode and a material's chamber target are where the panel
 * asks whether the printer has a chamber heater. The rule itself is pinned in
 * test_chamber_heater_assignment.cpp.
 */

#include "ui_panel_filament.h"
#include "ui_temperature_utils.h"

#include "../../include/moonraker_client_mock.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "ams_state.h"
#include "filament_database.h"
#include "moonraker_api.h"
#include "preset_materials.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "tool_state.h"

#include <initializer_list>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using TA = helix::ui::FilamentPanelTestAccess;

namespace {

/// A FilamentPanel over a mock printer, so the gcode Cool Down sends is observable.
struct ChamberPanelHarness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    std::unique_ptr<FilamentPanel> panel;

    ChamberPanelHarness() {
        ToolState::instance().init_subjects(true);
        helix::AmsState::instance().init_subjects(true);
        state.init_subjects(false);
        panel = std::make_unique<FilamentPanel>(state, &api);
        panel->init_subjects();
    }

    ~ChamberPanelHarness() {
        panel.reset();
        helix::AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
        helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
    }

    /// Discovery lands with these objects under this chamber heater assignment.
    void discover(const char* heater_assignment, std::initializer_list<const char*> objects) {
        helix::SettingsManager::instance().set_chamber_heater_assignment(heater_assignment);
        helix::PrinterDiscovery hw;
        nlohmann::json list = nlohmann::json::array();
        for (const char* object : objects) {
            list.push_back(object);
        }
        hw.parse_objects(list);
        state.set_hardware(std::move(hw));
        state.set_klippy_state_sync(helix::KlippyState::READY);
        client.clear_gcode_script_history();
    }

    bool sent(const std::string& fragment) const {
        for (const auto& gcode : client.gcode_script_history()) {
            if (gcode.find(fragment) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

/// The first preset slot whose material asks for a heated chamber.
int first_slot_with_chamber_temp() {
    for (int slot = 0; slot < helix::presets::PRESET_COUNT; ++slot) {
        auto material = filament::find_material(helix::presets::name(slot));
        if (material && material->chamber_temp_c > 0) {
            return slot;
        }
    }
    return -1;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Cool Down turns off the chamber heater PrinterState resolved",
                 "[filament][chamber]") {
    ChamberPanelHarness h;

    SECTION("an assigned heater no chamber keyword names") {
        h.discover("heater_generic ptc_heater",
                   {"heater_generic ptc_heater", "extruder", "heater_bed"});
        TA::handle_cooldown(*h.panel);

        REQUIRE(h.sent("HEATER=heater_bed TARGET=0"));
        CHECK(h.sent("HEATER=ptc_heater TARGET=0"));
    }
    SECTION("never a heater the printer is set not to use") {
        h.discover("none", {"heater_generic chamber", "extruder", "heater_bed"});
        TA::handle_cooldown(*h.panel);

        REQUIRE(h.sent("HEATER=heater_bed TARGET=0"));
        CHECK_FALSE(h.sent("HEATER=chamber TARGET=0"));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A material's chamber temperature targets only the chamber heater PrinterState "
                 "resolved",
                 "[filament][chamber]") {
    const int slot = first_slot_with_chamber_temp();
    REQUIRE(slot >= 0);
    const auto material = filament::find_material(helix::presets::name(slot));
    REQUIRE(material);
    ChamberPanelHarness h;

    SECTION("an assigned heater no chamber keyword names") {
        h.discover("heater_generic ptc_heater",
                   {"heater_generic ptc_heater", "extruder", "heater_bed"});
        h.panel->set_material(slot);

        CHECK(TA::chamber_target(*h.panel) ==
              helix::ui::temperature::degrees_to_deci(material->chamber_temp_c));
    }
    SECTION("never a heater the printer is set not to use") {
        h.discover("none", {"heater_generic chamber", "extruder", "heater_bed"});
        h.panel->set_material(slot);

        int nozzle_target = 0;
        h.panel->get_temp(nullptr, &nozzle_target);
        // set_material ran for this material.
        REQUIRE(nozzle_target == material->nozzle_recommended());
        CHECK(TA::chamber_target(*h.panel) == 0);
    }
}
