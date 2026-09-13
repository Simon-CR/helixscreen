// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "lane_resolver.h"
#include "lane_source_store.h"

namespace helix::ams {

/// Combine the resolver's presence answer with the backend's lane flavour.
///
/// SlotStatus carries four unrelated facts: presence, which lane is at the
/// extruder (LOADED), a fault (BLOCKED), and Happy Hare's gate_status 2
/// (FROM_BUFFER). Only presence is the resolver's, and it is the only one this
/// narrows: an absent lane reads EMPTY whatever the backend wrote, and a
/// present lane never reads EMPTY or UNKNOWN. The other three flavours belong
/// to firmware state machines that read them back, so they pass through.
[[nodiscard]] SlotStatus narrow_status(SlotStatus backend_status, bool present);

/// Lay a resolved lane onto the SlotInfo a backend has just built.
///
/// The fields SlotInfo carries that the resolver does not own (tool mapping,
/// extruder name, endless-spool group, error, environment, remaining length,
/// temps, indices) are left exactly as the backend set them. Pure: no clock,
/// no globals, no I/O.
void apply_resolved(SlotInfo& slot, const ResolvedLane& resolved);

/// This lane's resolved values. Never calls into a backend: backends call it
/// while holding their own mutex_.
[[nodiscard]] ResolvedLane resolved_lane(LaneId lane);

} // namespace helix::ams
