// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once

// A program asked of a plug-in while an earlier ask for the same plug-in still waits in the rack's queue
// (PluginRack::select_program) takes that ask's place: the one loading goes on, and the plug-in then loads the last
// one asked, once. Picks in quick succession (a controller's PROG +/- held, a knob sending a Program Change a step,
// PRESETS' wheel, a companion's list) so cost one load in hand and one waiting, never a queue of every step, and a
// single pick still loads at once. True when it took one's place (nothing to queue).
namespace melodee {

template <class Jobs, class Job> bool replace_waiting_program(Jobs& waiting, const Job& job) {
    for (auto& j : waiting)
        if (j.kind == Job::Kind::Program && j.entry == job.entry && j.generation == job.generation) {
            j.id = job.id;
            return true;
        }
    return false;
}

} // namespace melodee
