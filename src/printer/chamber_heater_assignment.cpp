// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chamber_heater_assignment.h"

#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::chamber {

std::string resolve_heater(const std::string& assignment, const PrinterDiscovery& discovery) {
    if (assignment == "auto") {
        return discovery.chamber_heater_name();
    }
    if (assignment == "none") {
        return "";
    }

    const auto& reported = discovery.printer_objects();
    if (std::find(reported.begin(), reported.end(), assignment) != reported.end()) {
        return assignment;
    }

    spdlog::info("[ChamberHeater] Assigned chamber heater '{}' is not a Klipper object on this "
                 "printer; using the discovered one ('{}')",
                 assignment, discovery.chamber_heater_name());
    return discovery.chamber_heater_name();
}

} // namespace helix::chamber
