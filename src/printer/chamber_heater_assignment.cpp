// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chamber_heater_assignment.h"

#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::chamber {

namespace {

/// One chamber role's assignment against discovery's pick for that role. Blind to
/// the object's type: whatever discovery picked for the role is the fallback.
std::string resolve(const std::string& assignment, const std::string& discovered,
                    const PrinterDiscovery& discovery, const char* role) {
    if (assignment == "auto") {
        return discovered;
    }
    if (assignment == "none") {
        return "";
    }

    const auto& reported = discovery.printer_objects();
    if (std::find(reported.begin(), reported.end(), assignment) != reported.end()) {
        return assignment;
    }

    // Every discovery pass (each klippy ready and reconnect) lands here while the saved
    // name stays stale, so it is not news each time.
    spdlog::debug("[ChamberAssignment] Assigned chamber {} '{}' is not a Klipper object on this "
                  "printer; using the discovered one ('{}')",
                  role, assignment, discovered);
    return discovered;
}

} // namespace

std::string resolve_heater(const std::string& assignment, const PrinterDiscovery& discovery) {
    return resolve(assignment, discovery.chamber_heater_name(), discovery, "heater");
}

std::string resolve_sensor(const std::string& assignment, const PrinterDiscovery& discovery) {
    return resolve(assignment, discovery.chamber_sensor_name(), discovery, "sensor");
}

} // namespace helix::chamber
