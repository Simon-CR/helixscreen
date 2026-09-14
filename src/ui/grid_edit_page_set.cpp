// SPDX-License-Identifier: GPL-3.0-or-later

#include "grid_edit_page_set.h"

#include <algorithm>

namespace helix {

namespace {

/// Page @p page, numbered before @p change, in the numbering after it; -1 when
/// no page after the change is that page.
int renumbered(const PageSetChange& change, int page) {
    // The slot's tile is a page afterward only when the change added one there.
    if (page < 0 || page > change.page_count || (page == change.page_count && !change.page_added)) {
        return -1;
    }
    if (change.removed_page < 0 || page < change.removed_page) {
        return page;
    }
    return page == change.removed_page ? -1 : page - 1;
}

} // namespace

int page_set_focus(const PageSetChange& change) {
    const int focus = renumbered(change, change.focus_page);
    if (focus >= 0) {
        return focus;
    }
    const int pages_after =
        change.page_count + (change.page_added ? 1 : 0) - (change.removed_page >= 0 ? 1 : 0);
    return std::clamp(change.focus_page, 0, std::max(pages_after - 1, 0));
}

PageSetLanding page_set_landing(const PageSetChange& change, int shown) {
    PageSetLanding landing;
    landing.focus = page_set_focus(change);
    const int on_screen = renumbered(change, shown);
    landing.shown = on_screen >= 0 ? on_screen : landing.focus;
    return landing;
}

} // namespace helix
