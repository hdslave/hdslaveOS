// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include <algorithm>
#include <cstdint>

// Step Edit's spare steps (2026-11-18). Past a pattern's last step the caret walks on into the rest of the last
// 16-step page, or into the next page when the last one is full (UiState::spare_step; step_cursor stays on the
// last step, so whatever reads it sees a real step). A step written there makes the pattern longer, to the end of
// the bar the step is in; leaving them empty adds nothing. The page marks the right edge with + and an arrow while
// the caret is on the last page and the pattern can grow (128 bars at most). Pure, for the shell, the page, the
// pad lights and the tests.
namespace melodee {

// The length that holds `step`: to the end of the bar it falls in. 0 when it holds it already or would pass 128 bars.
inline uint32_t spare_length(const Project& project, const Pattern& p, unsigned step) {
    uint64_t bar = std::max(project.bar_ticks(), 1u), at = uint64_t(step + 1) * p.q();
    uint64_t length = (at + bar - 1) / bar * bar;
    if (length > project.max_length() || length <= p.length) return 0;
    return uint32_t(length);
}
// The end of the page after the last step: the rest of a part page, else the whole next page.
inline unsigned spare_page_end(const Pattern& p) { return (p.steps() / bar_steps + 1) * bar_steps; }
// Where the caret may walk to: the spare steps' end, or the pattern's own steps when it cannot grow.
inline unsigned spare_end(const Project& project, const Pattern& p) {
    return spare_length(project, p, p.steps()) ? spare_page_end(p) : p.steps();
}
// The caret stands in the spare steps (on Step Edit, while step_cursor still marks the last step).
inline bool in_spare(const UiState& ui, const Pattern& p) {
    int steps = int(p.steps());
    return ui.page == PageId::StepEdit && ui.spare_step >= steps && ui.spare_step < int(spare_page_end(p)) && ui.step_cursor == steps - 1;
}
// The caret's step on Step Edit: a spare step, else step_cursor in the pattern.
inline unsigned edit_caret(const UiState& ui, const Pattern& p) {
    return in_spare(ui, p) ? unsigned(ui.spare_step) : unsigned(std::max<int>(ui.step_cursor, 0)) % p.steps();
}
// Shift + the wheel on Step Edit (melodee-hdslave): the caret `bars` pages of sixteen steps on, at the same step of
// the page it lands on. Round the pattern's pages as the caret goes round its steps, the spare page after the last
// while the pattern can grow; on a last page that ends short of it, the last step there.
inline unsigned bar_caret(const Project& project, const Pattern& p, unsigned caret, int bars) {
    int end = int(std::max(spare_end(project, p), p.steps()));
    int pages = std::max((end + int(bar_steps) - 1) / int(bar_steps), 1);
    int page = ((int(caret / bar_steps) + bars) % pages + pages) % pages;
    return unsigned(std::min(page * int(bar_steps) + int(caret % bar_steps), end - 1));
}
// The +▸ mark: the caret is on the last page or in the spare steps, and the pattern can grow.
inline bool spare_mark(const Project& project, const UiState& ui, const Pattern& p) {
    unsigned steps = p.steps(), caret = edit_caret(ui, p);
    return spare_end(project, p) > steps && caret / bar_steps >= (steps - 1) / bar_steps;
}

} // namespace melodee
