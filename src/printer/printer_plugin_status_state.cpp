// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_plugin_status_state.cpp
 * @brief HelixPrint plugin status management extracted from PrinterState
 *
 * Manages plugin installation and phase tracking subjects for UI feature gating.
 * Uses tri-state semantics (-1=unknown, 0=no, 1=yes) to distinguish between
 * "still checking" and "definitely not available" states.
 *
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_plugin_status_state.h"

#include "ui_update_queue.h"

#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

namespace helix {

void PrinterPluginStatusState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterPluginStatusState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterPluginStatusState] Initializing subjects (register_xml={})",
                  register_xml);

    // Plugin status subjects use tri-state: -1=unknown, 0=no, 1=yes
    // Unknown state allows UI to show "checking..." vs "not available"
    INIT_SUBJECT_INT(helix_plugin_installed, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(phase_tracking_enabled, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(helix_macros_status, static_cast<int>(HelixMacrosStatus::Unknown), subjects_,
                     register_xml);

    // Fresh subjects mean a fresh session: re-initialization (test fixtures
    // re-init the shared PrinterState between cases) must not read a base
    // status or pending flag from the previous one.
    macros_base_status_ = static_cast<int>(HelixMacrosStatus::Unknown);
    macros_restart_pending_ = false;

    subjects_initialized_ = true;
    spdlog::trace("[PrinterPluginStatusState] Subjects initialized successfully");
}

void PrinterPluginStatusState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterPluginStatusState] Deinitializing subjects");

    // Expire any setter callback still queued on the UpdateQueue — it captures
    // `this` and writes the subjects torn down immediately below (#1165, #1146).
    async_lifetime_.invalidate();

    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterPluginStatusState::set_installed(bool installed) {
    lv_subject_set_int(&helix_plugin_installed_, installed ? 1 : 0);
    spdlog::info("[PrinterPluginStatusState] HelixPrint plugin installed: {}", installed);
}

void PrinterPluginStatusState::set_phase_tracking_enabled(bool enabled) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    async_lifetime_.defer(
        "PrinterPluginStatusState::set_phase_tracking_enabled", [this, enabled]() {
            lv_subject_set_int(&phase_tracking_enabled_, enabled ? 1 : 0);
            spdlog::info("[PrinterPluginStatusState] Phase tracking enabled: {}", enabled);
        });
}

void PrinterPluginStatusState::set_helix_macros_base_status(HelixMacrosStatus base) {
    macros_base_status_ = static_cast<int>(base);
    if (base == HelixMacrosStatus::Installed || base == HelixMacrosStatus::Outdated) {
        // Discovery sees the macros active: a Klipper restart landed and any
        // staged-install offer is resolved, whatever its fate.
        macros_restart_pending_ = false;
    }
    publish_helix_macros_status();
    spdlog::info("[PrinterPluginStatusState] Helper macros base status: {}", macros_base_status_);
}

void PrinterPluginStatusState::set_helix_macros_restart_pending(bool pending) {
    macros_restart_pending_ = pending;
    publish_helix_macros_status();
    spdlog::info("[PrinterPluginStatusState] Helper macro restart pending: {}", pending);
}

void PrinterPluginStatusState::publish_helix_macros_status() {
    int composed = macros_base_status_;
    if (macros_restart_pending_ &&
        macros_base_status_ == static_cast<int>(HelixMacrosStatus::NotInstalled)) {
        composed = static_cast<int>(HelixMacrosStatus::RestartPending);
    }
    lv_subject_set_int(&helix_macros_status_, composed);
}

} // namespace helix
