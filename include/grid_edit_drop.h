// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "grid_layout.h"

/**
 * @file grid_edit_drop.h
 * @brief What a released home-grid edit drag does: move its widget, create the
 * page past the last one, go back to its origin page, or snap back.
 *
 * Free of live objects: GridEditMode gathers the release into a DropInput and
 * the scoped page's occupancy, and commits what resolve_drop() returns
 * (prestonbrown/helixscreen#1638).
 */

namespace helix {

/// A released drag, in the terms its drop resolves on.
struct DropInput {
    int page_index = 0;  ///< Page the session is scoped to; page_count is the next-page slot
    int page_count = 0;  ///< Pages in the config
    int origin_page = 0; ///< Page the dragged entry lives on
    int origin_col = 0;  ///< The entry's cell on its origin page
    int origin_row = 0;
    int colspan = 1; ///< The entry's span
    int rowspan = 1;
    int target_col = -1; ///< Cell the drag previewed on the scoped page; -1 when none
    int target_row = -1;
    bool has_next_page_slot = false; ///< A page past the last one can be created
    bool past_right_border = false; ///< The widget's majority lies past the page frame's right edge
};

enum class DropOutcome {
    Move,           ///< The entry lands on the scoped page at the resolved cell
    CreatePage,     ///< A page is added past the last one, and the entry lands on it
    ReturnToOrigin, ///< Nothing commits, and the drag goes back to its origin page first
    Cancel,         ///< Nothing commits, on the origin page: the widget snaps back
};

struct DropResolution {
    DropOutcome outcome = DropOutcome::Cancel;
    int col = -1; ///< Landing cell for Move and CreatePage
    int row = -1;
};

/**
 * @brief Resolve a released drag.
 *
 * - It creates a page when cross_page_drop_creates_page() holds for the scope
 *   the release lands in. On the next-page slot the entry lands at the previewed
 *   cell; from the last page's border, in the rightmost columns its span allows,
 *   on the previewed row. A landing cell that does not fit an empty page creates
 *   nothing.
 * - Otherwise, on a config page, it moves the entry to the previewed cell when
 *   that cell or the page differs from the origin and the span fits
 *   @p occupancy there.
 * - Anything else commits nothing: off the origin page the drag returns to it,
 *   on the origin page it snaps back.
 *
 * @param in The release.
 * @param occupancy The scoped page's widgets, the dragged one left out, on the
 *        grid the page is laid out on. Its dimensions also bound a created
 *        page's landing cell.
 */
DropResolution resolve_drop(const DropInput& in, const GridLayout& occupancy);

} // namespace helix
