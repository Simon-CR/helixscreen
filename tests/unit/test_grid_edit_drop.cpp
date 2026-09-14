// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_edit_drop.cpp
 * @brief How a released home-grid edit drag resolves: move its widget, create
 * the page past the last one, return to its origin page, or snap back
 * (prestonbrown/helixscreen#1638).
 */

#include "grid_edit_drop.h"
#include "grid_layout.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::DropInput;
using helix::DropOutcome;
using helix::DropResolution;
using helix::GridLayout;
using helix::GridPlacement;
using helix::resolve_drop;

namespace {

/// A 12x8 track grid, and a one-cell widget of two tracks each way.
constexpr int COLS = 12;
constexpr int ROWS = 8;
constexpr int SPAN = 2;
/// Two config pages: page 1 is the last, page 2 the next-page slot.
constexpr int PAGES = 2;

/// A release on page @p page, previewing (@p col, @p row), of a drag whose entry
/// sits at (0,0) on page 0, with a next-page slot and the widget inside the
/// page frame.
DropInput release_on(int page, int col, int row) {
    DropInput in;
    in.page_index = page;
    in.page_count = PAGES;
    in.origin_page = 0;
    in.origin_col = 0;
    in.origin_row = 0;
    in.colspan = SPAN;
    in.rowspan = SPAN;
    in.target_col = col;
    in.target_row = row;
    in.has_next_page_slot = true;
    in.past_right_border = false;
    return in;
}

DropInput past_border(DropInput in) {
    in.past_right_border = true;
    return in;
}

DropInput without_slot(DropInput in) {
    in.has_next_page_slot = false;
    return in;
}

GridLayout occupied_by(const std::vector<GridPlacement>& widgets) {
    GridLayout grid(UiBreakpoint::Medium, {COLS, ROWS});
    for (const GridPlacement& widget : widgets) {
        REQUIRE(grid.place(widget));
    }
    return grid;
}

struct Row {
    const char* what;
    DropInput in;
    std::vector<GridPlacement> occupants;
    DropOutcome outcome;
    int col; ///< Checked for Move and CreatePage only
    int row;
};

void check_rows(const std::vector<Row>& rows) {
    for (const Row& row : rows) {
        INFO(row.what);
        const DropResolution drop = resolve_drop(row.in, occupied_by(row.occupants));
        CHECK(drop.outcome == row.outcome);
        if (row.outcome == DropOutcome::Move || row.outcome == DropOutcome::CreatePage) {
            CHECK(drop.col == row.col);
            CHECK(drop.row == row.row);
        }
    }
}

} // namespace

TEST_CASE("a release on a config page moves its widget, snaps back or returns home",
          "[1638][grid_edit][cross_page]") {
    const GridPlacement fan{"fan", 4, 0, SPAN, SPAN};
    check_rows({
        {"a free cell on the origin page", release_on(0, 6, 2), {fan}, DropOutcome::Move, 6, 2},
        {"the origin cell on the origin page",
         release_on(0, 0, 0),
         {fan},
         DropOutcome::Cancel,
         -1,
         -1},
        {"a cell another widget holds, on the origin page",
         release_on(0, 4, 0),
         {fan},
         DropOutcome::Cancel,
         -1,
         -1},
        {"a cell the span overhangs the grid from",
         release_on(0, COLS - 1, 0),
         {fan},
         DropOutcome::Cancel,
         -1,
         -1},
        {"no previewed cell, on the origin page",
         release_on(0, -1, -1),
         {fan},
         DropOutcome::Cancel,
         -1,
         -1},
        {"the origin cell's coordinates on another page",
         release_on(1, 0, 0),
         {fan},
         DropOutcome::Move,
         0,
         0},
        {"a cell another widget holds, on another page",
         release_on(1, 4, 0),
         {fan},
         DropOutcome::ReturnToOrigin,
         -1,
         -1},
        {"no previewed cell, on another page",
         release_on(1, -1, -1),
         {fan},
         DropOutcome::ReturnToOrigin,
         -1,
         -1},
    });
}

TEST_CASE("a release creates a page anywhere on the next-page slot, and from the last page only "
          "past its right border",
          "[1638][grid_edit][cross_page]") {
    // The last page is full where a created page's entry lands: a created page
    // is empty, so the last page's widgets never decide it.
    const GridPlacement rightmost{"clock", COLS - SPAN, 4, SPAN, SPAN};
    check_rows({
        {"a previewed cell on the next-page slot",
         release_on(PAGES, 6, 2),
         {},
         DropOutcome::CreatePage,
         6,
         2},
        {"the next-page slot with no previewed cell",
         release_on(PAGES, -1, -1),
         {},
         DropOutcome::ReturnToOrigin,
         -1,
         -1},
        {"the next-page slot with no page to create",
         without_slot(release_on(PAGES, 6, 2)),
         {},
         DropOutcome::ReturnToOrigin,
         -1,
         -1},
        {"past the last page's right border",
         past_border(release_on(PAGES - 1, 8, 4)),
         {rightmost},
         DropOutcome::CreatePage,
         COLS - SPAN,
         4},
        {"past the last page's right border with no previewed row",
         past_border(release_on(PAGES - 1, -1, -1)),
         {},
         DropOutcome::CreatePage,
         COLS - SPAN,
         0},
        {"inside the last page, as after a flip back off the slot",
         release_on(PAGES - 1, 6, 2),
         {},
         DropOutcome::Move,
         6,
         2},
        {"past an earlier page's right border",
         past_border(release_on(0, 6, 2)),
         {},
         DropOutcome::Move,
         6,
         2},
        {"past the last page's right border with no page to create",
         without_slot(past_border(release_on(PAGES - 1, 6, 2))),
         {},
         DropOutcome::Move,
         6,
         2},
    });
}

TEST_CASE("a created page whose landing cell does not fit an empty page is not created",
          "[1638][grid_edit][cross_page]") {
    DropInput on_slot = release_on(PAGES, COLS - 1, 0);
    CHECK(resolve_drop(on_slot, occupied_by({})).outcome == DropOutcome::ReturnToOrigin);

    DropInput too_wide = past_border(release_on(PAGES - 1, 0, 0));
    too_wide.colspan = COLS + SPAN;
    CHECK(resolve_drop(too_wide, occupied_by({})).outcome == DropOutcome::ReturnToOrigin);
}
