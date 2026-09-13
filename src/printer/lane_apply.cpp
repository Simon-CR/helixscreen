// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_apply.h"

namespace helix::ams {

SlotStatus narrow_status(SlotStatus backend_status, bool present) {
    if (!present) {
        return SlotStatus::EMPTY;
    }
    if (backend_status == SlotStatus::EMPTY || backend_status == SlotStatus::UNKNOWN) {
        return SlotStatus::AVAILABLE;
    }
    return backend_status;
}

void apply_resolved(SlotInfo& slot, const ResolvedLane& resolved) {
    slot.status = narrow_status(slot.status, resolved.present);

    slot.color_rgb = resolved.color_rgb;
    slot.color_name = resolved.color_name;
    slot.material = resolved.material;
    slot.brand = resolved.brand;
    slot.spool_name = resolved.spool_name;
    slot.catalog_id = resolved.catalog_id;
    slot.product_name = resolved.product_name;
    slot.spoolman_id = resolved.spoolman_id;
    slot.spoolman_vendor_id = resolved.spoolman_vendor_id;
    slot.remaining_weight_g = resolved.remaining_weight_g;
    slot.total_weight_g = resolved.total_weight_g;
}

ResolvedLane resolved_lane(LaneId lane) {
    return resolve(lane_sources(lane));
}

} // namespace helix::ams
