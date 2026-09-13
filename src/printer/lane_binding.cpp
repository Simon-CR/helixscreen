// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_binding.h"

#include "lane_resolver.h"

namespace helix::ams {

namespace {

/// The spool id this lane's sources DECLARE, or 0 when none of them do.
///
/// Resolving a copy with VendorCache removed rather than reading the two
/// records by hand: the ranking between Spoolman and LocalUser then has one
/// definition, resolve()'s, and cannot drift from the ladder the UI paints
/// from.
int declared_spool_id(const LaneSources& sources) {
    LaneSources declared = sources;
    declared.drop(ObservationSource::VendorCache);
    return resolve(declared).spoolman_id.value_or(0);
}

} // namespace

BindingVerdict classify_binding(const LaneSources& sources, const BindingReading& reading) {
    const int declared = declared_spool_id(sources);

    // An external re-bind. Another well-behaved writer - Mainsail, the AFC
    // plugin, a macro - has put a DIFFERENT spool on this lane. That is a
    // statement rather than a guess, so the declared identity stops standing
    // and firmware's own reading paints. Never gated by the retention setting:
    // an explicit external write is not a preference. Never fires on 0, which
    // is the eject signal below and not a spool.
    if (reading.firmware_spool_id > 0 && declared > 0 && reading.firmware_spool_id != declared &&
        reading.firmware_spool_id != reading.own_write_old_id &&
        reading.firmware_spool_id != reading.own_write_new_id) {
        return BindingVerdict::Rebound;
    }

    // The eject signal. Meaningful only where firmware names an id while a
    // spool is loaded: there 0 is the plugin's own clear. Elsewhere 0 is the
    // everyday reading and clearing on it would empty every lane on every
    // poll. Gated by the setting because retaining identity across an eject is
    // a preference, unlike a re-bind.
    if (reading.printer_reports_spool_ids && reading.firmware_spool_id <= 0 && declared > 0 &&
        !reading.keep_spool_info_on_eject) {
        return BindingVerdict::Ejected;
    }

    return BindingVerdict::Holds;
}

BindingVerdict reconcile_binding(LaneId lane, const BindingReading& reading) {
    const BindingVerdict verdict = classify_binding(lane_sources(lane), reading);
    if (verdict != BindingVerdict::Holds) {
        drop_lane_source(lane, ObservationSource::Spoolman);
        drop_lane_source(lane, ObservationSource::LocalUser);
    }
    return verdict;
}

} // namespace helix::ams
