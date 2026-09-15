// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "screensaver.h"

#ifdef HELIX_ENABLE_SCREENSAVER

namespace helix {

// Test-only seam. The manager owns its saver instances privately, and a saver's own
// timer is only reachable from the instance that is running.
class ScreensaverManagerTestAccess {
  public:
    /// The saver the manager is running, or nullptr.
    static Screensaver* active(const ScreensaverManager& mgr) {
        return mgr.active_;
    }
};

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
