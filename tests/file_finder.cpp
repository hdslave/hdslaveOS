// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files on a card made in a temporary folder (src/melodee/shell/file_finder.h, moved_files.h):
// each step of finding a sample, the index's walk and its limits, a plug-in moved, and the finder's thread (checks,
// searches, CANCEL, a project opened mid-search, the index kept for the session).
#include "melodee/shell/file_finder.h"
#include "melodee/shell/moved_files.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

using namespace melodee;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__ ":") + std::to_string(__LINE__) + ": " #x); } while (false)

namespace {
fs::path root;
void file(const fs::path& path, size_t bytes = 16) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
}
std::string at(const std::string& rel) { return (root / rel).string(); }
void fresh(const std::string& name) {
    root = fs::temp_directory_path() / ("finder-" + std::to_string(::getpid())) / name;
    fs::remove_all(root);
    fs::create_directories(root);
}
CardIndex walked(const std::string& card, const std::vector<std::string>& internal = {}) {
    CardIndex index;
    index.begin(card, internal);
    while (index.step() == CardIndex::Step::More) {}
    CHECK(index.complete());
    return index;
}
// Waits for the finder's answers until a Done comes (or a few seconds pass).
std::vector<MovedFiles::Answer> answers(MovedFiles& f, unsigned dones = 1) {
    std::vector<MovedFiles::Answer> out;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    unsigned seen = 0;
    while (seen < dones && std::chrono::steady_clock::now() < until) {
        MovedFiles::Answer a;
        if (f.take(a)) { seen += a.kind == MovedFiles::Answer::Kind::Done; out.push_back(std::move(a)); }
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(seen == dones);
    return out;
}
const MovedFiles::Answer* of(const std::vector<MovedFiles::Answer>& list, unsigned slot, MovedFiles::Answer::Kind kind) {
    for (const auto& a : list) if (a.kind == kind && a.slot == slot) return &a;
    return nullptr;
}
const MovedFiles::Answer& done(const std::vector<MovedFiles::Answer>& list) {
    for (const auto& a : list) if (a.kind == MovedFiles::Answer::Kind::Done) return a;
    throw std::runtime_error("no Done");
}

void steps() {
    fresh("steps");
    const std::string card = at("media/NEW");
    // 1. Upstream's: Samples/ at the card's top into Melodee/Samples/.
    file(card + "/Melodee/Samples/Old/Kick.wav");
    auto r = find_moved_sample(card + "/Samples/Old/Kick.wav", card, "", {}, nullptr);
    CHECK(r.path == card + "/Melodee/Samples/Old/Kick.wav" && r.step == 1);
    // 2. A renamed card: the saved volume swapped for the card's top, the rest of the path kept.
    file(card + "/MPC-Sample/Projects/tech_[ProjectData]/Snare.wav");
    r = find_moved_sample(at("media/F2EC-CC50/MPC-Sample/Projects/tech_[ProjectData]/Snare.wav"), card, "", {}, nullptr);
    CHECK(r.path == card + "/MPC-Sample/Projects/tech_[ProjectData]/Snare.wav" && r.step == 2);
    // and on it, the samples moved into Melodee/Samples since
    r = find_moved_sample(at("media/F2EC-CC50/Samples/Old/Kick.wav"), card, "", {}, nullptr);
    CHECK(r.path == card + "/Melodee/Samples/Old/Kick.wav" && r.step == 2);
    // and upstream's repair with the extension left off
    r = find_moved_sample(card + "/Samples/Old/Kick", card, "", {}, nullptr);
    CHECK(r.path == card + "/Melodee/Samples/Old/Kick.wav" && r.step == 1);
    // 3. The project's folder moved with its samples: its last folder and the name under the project's folder.
    file(card + "/Moved/Proj_[ProjectData]/Hat.wav");
    r = find_moved_sample(at("media/F2EC-CC50/MPC-Sample/Projects/Proj_[ProjectData]/Hat.wav"), card, card + "/Moved/Proj.mldp", {}, nullptr);
    CHECK(r.path == card + "/Moved/Proj_[ProjectData]/Hat.wav" && r.step == 3);
    // beside the project itself
    file(card + "/Beside/Clap.wav");
    r = find_moved_sample(at("media/X/Far/Away/Clap.wav"), card, card + "/Beside/P.mldp", {}, nullptr);
    CHECK(r.path == card + "/Beside/Clap.wav" && r.step == 3);
    // an .xpj's own data folder
    file(card + "/Stock/Song_[ProjectData]/Tom.wav");
    r = find_moved_sample(at("media/X/Other/Tom.wav"), card, card + "/Stock/Song.xpj", {}, nullptr);
    CHECK(r.path == card + "/Stock/Song_[ProjectData]/Tom.wav");
    // A stock project's name without its extension: .wav, .WAV, ... tried in turn.
    file(card + "/Stock/Song_[ProjectData]/GS_KICK_06.WAV");
    r = find_moved_sample(card + "/Stock/Song_[ProjectData]/GS_KICK_06", card, card + "/Stock/Song.xpj", {}, nullptr);
    CHECK(fold_case(r.path) == fold_case(card + "/Stock/Song_[ProjectData]/GS_KICK_06.WAV")); // as is, on a card that ignores case
    // 4. With the rest of the kit: the folder another sample of the project was found in.
    file(card + "/Kits/Mine/Perc.wav");
    r = find_moved_sample(at("media/X/Gone/Perc.wav"), card, "", {}, nullptr);
    CHECK(r.path.empty());
    r = find_moved_sample(at("media/X/Gone/Perc.wav"), card, "", {card + "/Kits/Mine"}, nullptr);
    CHECK(r.path == card + "/Kits/Mine/Perc.wav" && r.step == 4);
    // A pad of a program keeps its "#..." on the file found.
    file(card + "/Moved/Proj_[ProjectData]/Kit.xpm");
    r = find_moved_sample(at("media/F2EC-CC50/Projects/Proj_[ProjectData]/Kit.xpm#3.1"), card, card + "/Moved/Proj.mldp", {}, nullptr);
    CHECK(r.path == card + "/Moved/Proj_[ProjectData]/Kit.xpm#3.1");
    // A '#' in a file's own name is the name.
    CHECK(split_saved("/x/KeysPh G#m90.wav").part.empty() && split_saved("/x/A.xpm#2.4").part == "#2.4");
}

void index_and_ties() {
    fresh("index");
    const std::string card = at("media/CARD");
    // Samples moved into Melodee/Samples: only the index finds them.
    file(card + "/Melodee/Samples/KICK_06.wav", 100);
    const std::string saved = at("media/CARD/MPC-Sample/Projects/Song_[ProjectData]/KICK_06.wav");
    CHECK(find_moved_sample(saved, card, card + "/Melodee/Projects/P.mldp", {}, nullptr).path.empty());
    // The same name in two kits: the one sharing the most trailing folders with the saved path.
    file(card + "/Moved/KitA/Kick.wav", 10);
    file(card + "/Moved/KitB/Kick.wav", 20);
    // A same-size tie (copies of one file): the first by path. A different-size tie: left missing.
    file(card + "/Copies/One/Ride.wav", 30);
    file(card + "/Copies/Two/Ride.wav", 30);
    file(card + "/Diff/One/Crash.wav", 30);
    file(card + "/Diff/Two/Crash.wav", 31);
    // A ._ twin, hidden folders, the boot folder and Melodee's own are left out.
    file(card + "/Moved/KitA/._Kick.wav", 4);
    file(card + "/.Trashes/Kick.wav", 4);
    file(card + "/autorun/Kick.wav", 4);
    file(card + "/Melodee/Cache/Kick.wav", 4);
    file(card + "/Melodee/Plugins/Kick.wav", 4);
    file(card + "/System Volume Information/Kick.wav", 4);
    // The internal sample folders after the card.
    file(at("internal/Samples/Factory/Bell.wav"), 8);
    // Linked samples count (the card copies of the harness link large files).
    file(at("elsewhere/Long.wav"), 50);
    fs::create_directories(card + "/Links");
    fs::create_symlink(at("elsewhere/Long.wav"), card + "/Links/Long.wav");
    auto index = walked(card, {at("internal/Samples")});
    auto r = find_moved_sample(saved, card, card + "/Melodee/Projects/P.mldp", {}, &index);
    CHECK(r.path == card + "/Melodee/Samples/KICK_06.wav" && r.step == 5);
    CHECK(index.files("kick.wav").size() == 2); // only the two kits'
    CHECK(find_moved_sample(at("media/OLD/Lib/KitB/Kick.wav"), card, "", {}, &index).path == card + "/Moved/KitB/Kick.wav");
    CHECK(find_moved_sample(at("media/OLD/Lib/KitA/Kick.wav"), card, "", {}, &index).path == card + "/Moved/KitA/Kick.wav");
    r = find_moved_sample(at("media/OLD/Lib/KitC/Kick.wav"), card, "", {}, &index); // neither shares a folder: sizes differ
    CHECK(r.path.empty() && r.tie);
    CHECK(find_moved_sample(at("media/OLD/Ride.wav"), card, "", {}, &index).path == card + "/Copies/One/Ride.wav");
    r = find_moved_sample(at("media/OLD/Crash.wav"), card, "", {}, &index);
    CHECK(r.path.empty() && r.tie);
    CHECK(find_moved_sample(at("media/OLD/Factory/Bell.wav"), card, "", {}, &index).path == at("internal/Samples/Factory/Bell.wav"));
    CHECK(find_moved_sample(at("media/OLD/LONG.WAV"), card, "", {}, &index).path == card + "/Links/Long.wav"); // any case
    // Among ties, one in the project's folder, then one in Melodee/Samples.
    file(card + "/Proj/Snare.wav", 5);
    file(card + "/Melodee/Samples/Snare.wav", 6);
    file(card + "/Else/Snare.wav", 7);
    index = walked(card);
    CHECK(find_moved_sample(at("media/OLD/Snare.wav"), card, card + "/Proj/P.mldp", {}, &index).path == card + "/Proj/Snare.wav");
    CHECK(find_moved_sample(at("media/OLD/Snare.wav"), card, card + "/Other/P.mldp", {}, &index).path == card + "/Melodee/Samples/Snare.wav");
    // A name saved without its extension, by name with one.
    CHECK(find_moved_sample(at("media/OLD/x_[ProjectData]/KICK_06"), card, "", {}, &index).path == card + "/Melodee/Samples/KICK_06.wav");
    // The limits: past them it stops, the index cleared (as a cancel).
    CardIndex small;
    small.entry_limit = 5;
    small.begin(card, {});
    CardIndex::Step s;
    while ((s = small.step()) == CardIndex::Step::More) {}
    CHECK(s == CardIndex::Step::TooBig && !small.complete() && small.files("snare.wav").empty());
    CardIndex shallow; // deeper than its limit: stopped too, rather than say what it did not reach is missing
    shallow.depth_limit = 1;
    shallow.begin(card, {});
    while ((s = shallow.step()) == CardIndex::Step::More) {}
    CHECK(s == CardIndex::Step::TooBig && !shallow.complete() && shallow.files("snare.wav").empty());
    // A file gone since the index was made is not named: the slot stays missing.
    fs::remove(card + "/Else/Snare.wav");
    fs::remove(card + "/Proj/Snare.wav");
    fs::remove(card + "/Melodee/Samples/Snare.wav");
    CHECK(find_moved_sample(at("media/OLD/Snare.wav"), card, card + "/Proj/P.mldp", {}, &index).path.empty());
}

void plugins() {
    fresh("plugins");
    const std::string card = at("media/CARD");
    // The hdslave plug-ins moved into Melodee/Plugins/hdslave/: by name, any case.
    file(card + "/Melodee/Plugins/hdslave/Anvil.clap");
    CHECK(find_moved_plugin(card + "/Melodee/Plugins/Anvil.clap", card) == card + "/Melodee/Plugins/hdslave/Anvil.clap");
    CHECK(find_moved_plugin(card + "/Melodee/Plugins/ANVIL.CLAP", card) == card + "/Melodee/Plugins/hdslave/Anvil.clap");
    // A VST2 folder; a bundle is not looked into.
    fs::create_directories(card + "/Melodee/Plugins/Vendor/Synth VST");
    file(card + "/Melodee/Plugins/Vendor/Synth VST/synth.so");
    CHECK(find_moved_plugin(card + "/Melodee/Plugins/Synth VST", card) == card + "/Melodee/Plugins/Vendor/Synth VST");
    // Several: the most trailing folders in common, then the shortest path.
    file(card + "/Melodee/Plugins/a/b/Dup.clap");
    file(card + "/Melodee/Plugins/c/Dup.clap");
    CHECK(find_moved_plugin(card + "/Melodee/Plugins/x/Dup.clap", card) == card + "/Melodee/Plugins/c/Dup.clap");
    CHECK(find_moved_plugin(card + "/Melodee/Plugins/b/Dup.clap", card) == card + "/Melodee/Plugins/a/b/Dup.clap");
    // Four folders down at most.
    file(card + "/Melodee/Plugins/1/2/3/4/Deep.clap");
    CHECK(find_moved_plugin(card + "/Melodee/Plugins/Deep.clap", card).empty());
    // The card renamed.
    file(card + "/Melodee/Plugins/Own/Lead.clap");
    CHECK(find_moved_plugin(at("media/OLD/Melodee/Plugins/Own/Lead.clap"), card) == card + "/Melodee/Plugins/Own/Lead.clap");
    // A plug-in replaced by another one is not guessed.
    CHECK(find_moved_plugin(card + "/Melodee/Plugins/Gone.clap", card).empty());
}

void worker() {
    fresh("worker");
    const std::string card = at("media/CARD");
    for (int n = 0; n < 4; ++n) file(card + "/Kit/S" + std::to_string(n) + ".wav", 10);
    file(card + "/Melodee/Samples/Far.wav", 10);
    for (int d = 0; d < 40; ++d) file(card + "/Noise/D" + std::to_string(d) + "/n.txt");
    MovedFiles f;
    // Every file in place: a Done and nothing else, and no walk.
    MovedFiles::Job job;
    job.generation = 1; job.card = card;
    for (int n = 0; n < 4; ++n) job.slots.push_back({uint16_t(n), card + "/Kit/S" + std::to_string(n) + ".wav"});
    f.current(1);
    f.post(job);
    auto got = answers(f);
    CHECK(got.size() == 1 && done(got).found_count == 0 && done(got).missing_count == 0 && f.walks() == 0);
    // A renamed card (steps 1-4 at once), one sample only the index knows, one gone.
    job.slots = {{0, at("media/OLD/Kit/S0.wav")}, {1, at("media/OLD/Kit/S1.wav")}, {2, at("media/OLD/Lib/Far.wav")}, {3, at("media/OLD/Gone.wav")}};
    f.post(job);
    got = answers(f);
    CHECK(of(got, 0, MovedFiles::Answer::Kind::Found) && of(got, 0, MovedFiles::Answer::Kind::Found)->found == card + "/Kit/S0.wav");
    CHECK(of(got, 1, MovedFiles::Answer::Kind::Found));
    CHECK(of(got, 2, MovedFiles::Answer::Kind::Missing) && of(got, 3, MovedFiles::Answer::Kind::Missing));
    CHECK(done(got).missing_count == 2 && !done(got).indexed && f.walks() == 0); // no search without being asked
    // CANCEL of a search not begun yet: it ends at once, with no walk.
    MovedFiles::Job search = job;
    search.search = true;
    search.slots = {{2, at("media/OLD/Lib/Far.wav")}, {3, at("media/OLD/Gone.wav")}};
    f.folder_pause_us = 20000; // a slow card
    {
        MovedFiles::Job busy = job; // a check first, so the search waits behind it
        busy.slots.clear();
        for (int n = 0; n < 200; ++n) busy.slots.push_back({uint16_t(n), at("media/OLD/Gone" + std::to_string(n) + ".wav")});
        f.post(busy);
        f.post(search);
        f.cancel();
        got = answers(f, 2);
        unsigned cancelled = 0;
        for (const auto& a : got) cancelled += a.kind == MovedFiles::Answer::Kind::Done && a.cancelled;
        CHECK(cancelled == 1 && !f.progress().walking);
    }
    const unsigned walks_before = f.walks();
    // SEARCH, then CANCEL within a folder, once the part walked has found one: that one stays (Found), and nothing is
    // said missing (the rest of the card was never walked).
    f.post(search);
    const auto began = std::chrono::steady_clock::now();
    while (f.progress().matched < 1 && std::chrono::steady_clock::now() - began < std::chrono::seconds(10)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(f.progress().walking && f.progress().matched == 1);
    f.cancel();
    const auto asked = std::chrono::steady_clock::now();
    got = answers(f);
    CHECK(done(got).cancelled && !f.indexed() && f.walks() == walks_before + 1);
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::milliseconds(500)); // within a folder or so
    CHECK(of(got, 2, MovedFiles::Answer::Kind::Found) && !of(got, 3, MovedFiles::Answer::Kind::Missing) && done(got).found_count == 1);
    // SEARCH again: walked again from the start, then found by the index.
    f.folder_pause_us = 0;
    f.post(search);
    got = answers(f);
    CHECK(f.walks() == walks_before + 2 && f.indexed() && !done(got).cancelled);
    CHECK(of(got, 2, MovedFiles::Answer::Kind::Found) && of(got, 2, MovedFiles::Answer::Kind::Found)->found == card + "/Melodee/Samples/Far.wav");
    CHECK(of(got, 3, MovedFiles::Answer::Kind::Missing) && done(got).found_count == 1 && done(got).missing_count == 1);
    // A second project: its check answered from the index at once, no walk.
    job.generation = 2;
    f.current(2);
    job.slots = {{7, at("media/OTHER/X/Far.wav")}, {8, at("media/OTHER/Gone.wav")}};
    f.post(job);
    got = answers(f);
    CHECK(of(got, 7, MovedFiles::Answer::Kind::Found) && of(got, 8, MovedFiles::Answer::Kind::Missing) && done(got).indexed && f.walks() == walks_before + 2);
    // A file copied back where it was, after the index dropped (FIND MISSING: fresh): found where it was saved.
    file(at("media/OTHER/Gone.wav"), 3);
    search.generation = 2;
    search.fresh = true;
    search.slots = {{8, at("media/OTHER/Gone.wav")}};
    f.post(search);
    got = answers(f);
    CHECK(of(got, 8, MovedFiles::Answer::Kind::Found) && of(got, 8, MovedFiles::Answer::Kind::Found)->found == at("media/OTHER/Gone.wav") && f.walks() == walks_before + 3);
    // Another project opened mid-search: nothing of the old one's comes, and the walk goes on for the index.
    f.drop_index();
    f.folder_pause_us = 5000;
    search.generation = 3; search.fresh = false;
    search.slots = {{2, at("media/OLD/Lib/Far.wav")}};
    f.current(3);
    f.post(search);
    while (!f.progress().walking) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    f.current(4); // switched
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!f.idle() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    MovedFiles::Answer stale;
    CHECK(f.idle() && !f.take(stale) && f.indexed()); // the walk finished, its answers dropped
    CHECK(f.progress().looked > 40);
}
} // namespace

int main() {
    try {
        steps();
        index_and_ties();
        plugins();
        worker();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAILED %s\n", e.what());
        fs::remove_all(fs::temp_directory_path() / ("finder-" + std::to_string(::getpid())));
        return 1;
    }
    fs::remove_all(fs::temp_directory_path() / ("finder-" + std::to_string(::getpid())));
    std::puts("file finder ok");
    return 0;
}
