// SPDX-License-Identifier: GPL-3.0-or-later

#include "grid_edit_drop.h"

#include "grid_edit_cross_page.h"

#include <algorithm>

namespace helix {

DropResolution resolve_drop(const DropInput& in, const GridLayout& occupancy) {
    const bool on_next_page_slot = in.page_index >= in.page_count;
    DropResolution nothing;
    nothing.outcome =
        in.page_index != in.origin_page ? DropOutcome::ReturnToOrigin : DropOutcome::Cancel;

    if (cross_page_drop_creates_page(in.page_index, in.page_count, in.has_next_page_slot,
                                     in.past_right_border)) {
        DropResolution created;
        created.outcome = DropOutcome::CreatePage;
        if (on_next_page_slot) {
            created.col = in.target_col;
            created.row = in.target_row;
        } else {
            // From the last page the widget sits past the right border.
            created.col = occupancy.cols() - in.colspan;
            created.row = std::max(in.target_row, 0);
        }
        const GridLayout empty_page(occupancy.breakpoint(), occupancy.dimensions());
        return empty_page.can_place(created.col, created.row, in.colspan, in.rowspan) ? created
                                                                                      : nothing;
    }
    if (on_next_page_slot) {
        return nothing;
    }
    // A page change is itself a move, even onto the origin cell's coordinates.
    const bool moved = in.target_col != in.origin_col || in.target_row != in.origin_row ||
                       in.page_index != in.origin_page;
    if (in.target_col < 0 || in.target_row < 0 || !moved ||
        !occupancy.can_place(in.target_col, in.target_row, in.colspan, in.rowspan)) {
        return nothing;
    }
    DropResolution move;
    move.outcome = DropOutcome::Move;
    move.col = in.target_col;
    move.row = in.target_row;
    return move;
}

} // namespace helix
