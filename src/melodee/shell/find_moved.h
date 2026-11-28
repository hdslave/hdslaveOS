// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files, on the MIDI thread. A project opened plays at once with every file in place, as ever;
// its slots whose files are not at their saved paths are looked for in the background (shell/moved_files.h). A sample
// found loads in place as Load Kit's do, its chop read from the file found; the path is kept in memory and written by
// the next save (opening writes nothing). What is still missing then: with Settings' Find moved files on Ask, the
// question (12 samples missing: NOT NOW, FORGET, SEARCH); on Always the search at once; Off, a notice. While the
// search runs its sheet (CANCEL, HIDE) shows how far it got; then the result (a notice, or KEEP / FORGET for what it
// did not find). Play and Stop work under every sheet. Load Kit and Load Bank's slots go through the same check.
// Nothing here is the audio thread's, and no file is read here.
#pragma once
#include "melodee/shell/moved_files.h"
#include <cstdint>
#include <string>
#include <vector>

namespace melodee {

class Context;
struct ForgetSamples;

class FindMoved {
public:
    FindMoved();
    // MIDI thread, all of it.
    void switched();                                      // Context::switch_to: another project; the last one's work dropped
    void opened(const std::string& origin, uint32_t now); // switched to a file's project: its slots looked at
    void kit(const std::string& origin) { added_origin = origin; } // a kit read for Load Kit: where its slots came from
    void added(unsigned slot) { added_slots.push_back(uint16_t(slot)); } // Context::sample_slot filled a slot
    void tick(Context& c, uint32_t now);                  // each pass: answers in, the question, the sheets' figures
    // The keys: the question's and the sheets' (shell/ask.cpp), the Samples page's under Shift.
    void search(Context& c, bool fresh);  // SEARCH (Always: at once), FIND MISSING (fresh: the card walked anew)
    void cancel(Context& c);              // CANCEL
    void hide(Context& c);                // HIDE: the search goes on, its result a notice
    void forget(Context& c);              // FORGET: the missing slots emptied, with the pads that played them; one undo
    void forgot(Context& c, const ForgetSamples& f, bool after); // that step undone or done again (Context::apply_edit)
    bool missing(unsigned slot) const;
    unsigned missing_count() const { return unsigned(not_found.size()); }
    bool searching() const { return phase == Phase::Searching; }
    // Tests and the whole-app harness: nothing due, nothing under way, nothing to take in.
    bool idle() const { return !check_due && added_slots.empty() && quiet_slots.empty() && !ask_due && phase == Phase::Idle && !checks_open && worker.idle(); }
    MovedFiles& files() { return worker; }

private:
    enum class Phase : uint8_t { Idle, Searching };
    void post_check(Context& c, const std::vector<uint16_t>& slots, const std::string& from, bool quiet = false);
    void take(Context& c, const MovedFiles::Answer& a);
    void done(Context& c, const MovedFiles::Answer& a);
    void set_missing(unsigned slot, bool on);
    MovedFiles worker;
    uint32_t generation = 1;
    std::string origin, added_origin, card, card_seen;
    uint32_t card_at = 0;
    bool check_due = false, ask_due = false;
    uint32_t due_at = 0;
    unsigned checks_open = 0;
    std::vector<uint16_t> added_slots, quiet_slots; // quiet: FORGET undone, looked at again with nothing asked
    std::vector<uint16_t> not_found; // in order
    Phase phase = Phase::Idle;
    bool hidden = false, cancelling = false;
    uint16_t asked = 0, result_found = 0;
};

} // namespace melodee
