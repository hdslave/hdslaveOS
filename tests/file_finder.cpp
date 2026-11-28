// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files on a card made in a temporary folder (src/melodee/shell/file_finder.h, moved_files.h):
// each step of finding a sample, the index's walk, its limits and its memory, a plug-in moved, and the finder's thread
// (checks, searches, CANCEL, a project opened mid-search, the index kept for the session, the card lent, its priority).
#include "melodee/shell/file_finder.h"
#include "melodee/shell/moved_files.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif

using namespace melodee;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__ ":") + std::to_string(__LINE__) + ": " #x); } while (false)

namespace {
fs::path root;
void file(const fs::path& path, size_t bytes = 16, char fill = 'x') {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, fill);
}
bool wait_until(std::function<bool()> done, int seconds = 20) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
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
    // 4. With the rest of the kit: where its own folder went, as another sample of the project found says.
    file(card + "/Kits/Mine/Perc.wav");
    r = find_moved_sample(at("media/X/Gone/Perc.wav"), card, "", {}, nullptr);
    CHECK(r.path.empty());
    r = find_moved_sample(at("media/X/Gone/Perc.wav"), card, "", {folder_move(at("media/X/Gone/Tom.wav"), card + "/Kits/Mine/Tom.wav")}, nullptr);
    CHECK(r.path == card + "/Kits/Mine/Perc.wav" && r.step == 4);
    // Two kits of the same names moved together: KitA's sample found says where Lib went, so KitB's Kick is KitB's
    // (never KitA's Kick, of the same name); with KitB not there, missing.
    file(card + "/New/KitA/Kick.wav", 10);
    file(card + "/New/KitA/Snare.wav", 10);
    const auto moved_a = folder_move(at("media/X/Lib/KitA/Snare.wav"), card + "/New/KitA/Snare.wav");
    CHECK(moved_a.from == at("media/X/Lib") && moved_a.to == card + "/New");
    CHECK(find_moved_sample(at("media/X/Lib/KitB/Kick.wav"), card, "", {moved_a}, nullptr).path.empty());
    file(card + "/New/KitB/Kick.wav", 20);
    r = find_moved_sample(at("media/X/Lib/KitB/Kick.wav"), card, "", {moved_a}, nullptr);
    CHECK(r.path == card + "/New/KitB/Kick.wav" && r.step == 4);
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
    // A tie of copies of one file (one size, the same first and last 4 KB): the first by path. Of different sizes, or
    // of one size but different, left missing.
    file(card + "/Copies/One/Ride.wav", 9000);
    file(card + "/Copies/Two/Ride.wav", 9000);
    file(card + "/Diff/One/Crash.wav", 30);
    file(card + "/Diff/Two/Crash.wav", 31);
    file(card + "/Same/One/China.wav", 9000, 'a');
    file(card + "/Same/Two/China.wav", 9000, 'b');
    // An MPC project is indexed too: a pad of one ("<x>.xpj#...") is found by its file.
    file(card + "/Stock/Song.xpj", 40);
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
    r = find_moved_sample(at("media/OLD/China.wav"), card, "", {}, &index);
    CHECK(r.path.empty() && r.tie);
    CHECK(find_moved_sample(at("media/OLD/Song.xpj#2.1"), card, "", {}, &index).path == card + "/Stock/Song.xpj#2.1");
    CHECK(find_moved_sample(at("media/OLD/Factory/Bell.wav"), card, "", {}, &index).path == at("internal/Samples/Factory/Bell.wav"));
    CHECK(find_moved_sample(at("media/OLD/LONG.WAV"), card, "", {}, &index).path == card + "/Links/Long.wav"); // any case
    // Among ties, one in the project's folder, then one in Melodee/Samples.
    file(card + "/Proj/Snare.wav", 5);
    file(card + "/Melodee/Samples/Snare.wav", 6);
    file(card + "/Else/Snare.wav", 7);
    index = walked(card);
    CHECK(find_moved_sample(at("media/OLD/Snare.wav"), card, card + "/Proj/P.mldp", {}, &index).path == card + "/Proj/Snare.wav");
    CHECK(find_moved_sample(at("media/OLD/Snare.wav"), card, card + "/Other/P.mldp", {}, &index).path == card + "/Melodee/Samples/Snare.wav");
    // A search's "found" counts its slots, not the names met: two slots of one name are two; one slot of a name saved
    // without its extension, met as .wav and as .aif, is one.
    file(card + "/Ext/Tom.wav", 5);
    file(card + "/Ext/Tom.aif", 5);
    index = walked(card);
    index.watch({at("media/OLD/A/Snare.wav"), at("media/OLD/B/Snare.wav"), at("media/OLD/C/Snare.wav"), at("media/OLD/D/Tom"),
                 at("media/OLD/Nowhere.wav")});
    CHECK(index.matched() == 4); // (names met: snare.wav, tom.wav, tom.aif)
    // A name saved without its extension, by name with one.
    CHECK(find_moved_sample(at("media/OLD/x_[ProjectData]/KICK_06"), card, "", {}, &index).path == card + "/Melodee/Samples/KICK_06.wav");
    // Part of a card (a search stopped): only a file that is plainly it, sharing a folder with the saved path or in the
    // project's folder, is taken.
    {
        std::vector<CardIndex::File> one {{card + "/Else/Snare.wav", 7}};
        CHECK(pick_by_name(at("media/OLD/Snare.wav"), card, "", one).path == card + "/Else/Snare.wav");
        CHECK(pick_by_name(at("media/OLD/Snare.wav"), card, "", one, true).path.empty());
        CHECK(pick_by_name(at("media/OLD/Else/Snare.wav"), card, "", one, true).path == card + "/Else/Snare.wav");
        CHECK(pick_by_name(at("media/OLD/Snare.wav"), card, card + "/Else/P.mldp", one, true).path == card + "/Else/Snare.wav");
    }
    // The limits. Past the files it may hold it stops, what it holds kept (TooBig); only the files indexed count.
    CardIndex small;
    small.file_limit = 5;
    small.begin(card, {});
    CardIndex::Step s;
    while ((s = small.step()) == CardIndex::Step::More) {}
    CHECK(s == CardIndex::Step::TooBig && small.too_big() && !small.complete() && small.indexed() == 5 && small.looked() > 5);
    // A folder deeper than the limit is left out; the rest is walked.
    CardIndex shallow;
    shallow.depth_limit = 1;
    shallow.begin(card, {});
    while ((s = shallow.step()) == CardIndex::Step::More) {}
    CHECK(s == CardIndex::Step::Done && shallow.complete() && shallow.files("snare.wav").size() == 2); // not Melodee/Samples'
    // A folder that cannot be read (the card pulled out) fails the walk: nothing kept.
    {
        CardIndex pulled;
        pulled.begin(card, {});
        CHECK(pulled.step() == CardIndex::Step::More); // the top listed, its folders to come
        fs::remove_all(card + "/Proj");
        while ((s = pulled.step()) == CardIndex::Step::More) {}
        CHECK(s == CardIndex::Step::Failed && !pulled.complete() && pulled.indexed() == 0);
        file(card + "/Proj/Snare.wav", 5);
    }
    // A file gone since the index was made is not named: the slot stays missing.
    fs::remove(card + "/Else/Snare.wav");
    fs::remove(card + "/Proj/Snare.wav");
    fs::remove(card + "/Melodee/Samples/Snare.wav");
    CHECK(find_moved_sample(at("media/OLD/Snare.wav"), card, card + "/Proj/P.mldp", {}, &index).path.empty());
}

std::string first_plugin(const std::string& saved, const std::string& card) {
    const auto found = find_moved_plugins(saved, card);
    return found.empty() ? std::string() : found.front();
}

void plugins() {
    fresh("plugins");
    const std::string card = at("media/CARD");
    // The hdslave plug-ins moved into Melodee/Plugins/hdslave/: by name, any case.
    file(card + "/Melodee/Plugins/hdslave/Anvil.clap");
    CHECK(first_plugin(card + "/Melodee/Plugins/Anvil.clap", card) == card + "/Melodee/Plugins/hdslave/Anvil.clap");
    CHECK(first_plugin(card + "/Melodee/Plugins/ANVIL.CLAP", card) == card + "/Melodee/Plugins/hdslave/Anvil.clap");
    // A VST2 folder; a bundle is not looked into.
    fs::create_directories(card + "/Melodee/Plugins/Vendor/Synth VST");
    file(card + "/Melodee/Plugins/Vendor/Synth VST/synth.so");
    CHECK(first_plugin(card + "/Melodee/Plugins/Synth VST", card) == card + "/Melodee/Plugins/Vendor/Synth VST");
    // Several: the most trailing folders in common, then the shortest path.
    file(card + "/Melodee/Plugins/a/b/Dup.clap");
    file(card + "/Melodee/Plugins/c/Dup.clap");
    CHECK(first_plugin(card + "/Melodee/Plugins/x/Dup.clap", card) == card + "/Melodee/Plugins/c/Dup.clap");
    CHECK(first_plugin(card + "/Melodee/Plugins/b/Dup.clap", card) == card + "/Melodee/Plugins/a/b/Dup.clap");
    CHECK(find_moved_plugins(card + "/Melodee/Plugins/x/Dup.clap", card).size() == 2); // both, for the rack to try in turn
    // Four folders down at most.
    file(card + "/Melodee/Plugins/1/2/3/4/Deep.clap");
    CHECK(first_plugin(card + "/Melodee/Plugins/Deep.clap", card).empty());
    // The card renamed.
    file(card + "/Melodee/Plugins/Own/Lead.clap");
    CHECK(first_plugin(at("media/OLD/Melodee/Plugins/Own/Lead.clap"), card) == card + "/Melodee/Plugins/Own/Lead.clap");
    // A plug-in replaced by another one is not guessed.
    CHECK(first_plugin(card + "/Melodee/Plugins/Gone.clap", card).empty());
}

void worker() {
    fresh("worker");
    const std::string card = at("media/CARD");
    for (int n = 0; n < 4; ++n) file(card + "/Kit/S" + std::to_string(n) + ".wav", 10);
    file(card + "/Melodee/Samples/Far.wav", 10);
    for (int d = 0; d < 40; ++d) file(card + "/Noise/D" + std::to_string(d) + "/n.txt");
    const std::string far = at("media/OLD/Lib/Samples/Far.wav"); // only the index finds it (sharing "Samples")
    MovedFiles f;
    // Every file in place: a Done and nothing else, and no walk.
    MovedFiles::Job job;
    job.generation = 1; job.card = card;
    for (int n = 0; n < 4; ++n) job.slots.push_back({uint16_t(n), card + "/Kit/S" + std::to_string(n) + ".wav"});
    f.current(1);
    f.post(job);
    auto got = answers(f);
    CHECK(got.size() == 1 && done(got).found_count == 0 && done(got).missing_count == 0 && f.walks() == 0 && f.looked_for() == 0);
#if defined(__linux__)
    { // The thread runs at the usual policy and the lowest priority, whatever its starter's (the MIDI thread is SCHED_FIFO;
      // here a SCHED_BATCH one, which any user may set).
        MovedFiles g;
        g.current(1);
        sched_param none {};
        CHECK(sched_setscheduler(0, SCHED_BATCH, &none) == 0);
        g.post(job);
        CHECK(wait_until([&] { return g.thread_id() != 0; }));
        sched_setscheduler(0, SCHED_OTHER, &none);
        std::ifstream in("/proc/self/task/" + std::to_string(g.thread_id()) + "/stat");
        std::string line;
        std::getline(in, line);
        std::istringstream fields(line.substr(line.rfind(')') + 2)); // field 3 on
        std::vector<std::string> v;
        for (std::string w; fields >> w;) v.push_back(w);
        CHECK(v.size() > 38 && v[38] == "0" && v[16] == "19"); // policy (field 41) SCHED_OTHER, nice (field 19) 19
        answers(g);
    }
#endif
    // A renamed card (steps 1-4 at once), one sample only the index knows, one gone.
    job.slots = {{0, at("media/OLD/Kit/S0.wav")}, {1, at("media/OLD/Kit/S1.wav")}, {2, far}, {3, at("media/OLD/Gone.wav")}};
    f.post(job);
    got = answers(f);
    CHECK(of(got, 0, MovedFiles::Answer::Kind::Found) && of(got, 0, MovedFiles::Answer::Kind::Found)->found == card + "/Kit/S0.wav");
    CHECK(of(got, 1, MovedFiles::Answer::Kind::Found));
    CHECK(of(got, 2, MovedFiles::Answer::Kind::Missing) && of(got, 3, MovedFiles::Answer::Kind::Missing));
    CHECK(done(got).missing_count == 2 && !done(got).indexed && f.walks() == 0); // no search without being asked
    // CANCEL of a search not begun yet: it ends at once, with no walk.
    MovedFiles::Job search = job;
    search.search = true;
    search.slots = {{2, far}, {3, at("media/OLD/Gone.wav")}};
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
        CHECK(cancelled == 1 && !f.progress().walking && f.walks() == 0);
    }
    // SEARCH, then CANCEL within a folder, once the part walked has met it: taken (it shares a folder with its saved
    // path), and nothing is said missing (the rest of the card was never walked). "found" counts slots, not names.
    f.post(search);
    CHECK(wait_until([&] { return f.progress().matched >= 1; }, 10));
    CHECK(f.progress().walking && f.progress().matched == 1);
    f.cancel();
    auto asked = std::chrono::steady_clock::now();
    got = answers(f);
    CHECK(done(got).cancelled && !f.indexed() && f.walks() == 1);
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::milliseconds(500)); // within a folder or so
    CHECK(of(got, 2, MovedFiles::Answer::Kind::Found) && !of(got, 3, MovedFiles::Answer::Kind::Missing) && done(got).found_count == 1);
    // The card lent mid-walk (hold): the search stops with nothing found or said missing, nothing more is read; a check
    // posted meanwhile waits, then is done once the card is back.
    f.post(search);
    CHECK(wait_until([&] { return f.progress().walking; }));
    f.hold(true);
    got = answers(f);
    CHECK(done(got).cancelled && done(got).found_count == 0 && got.size() == 1);
    CHECK(wait_until([&] { return f.parked(); }, 2));
    f.post(job);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MovedFiles::Answer none;
    CHECK(!f.take(none) && f.parked()); // nothing while lent
    f.hold(false);
    got = answers(f);
    CHECK(of(got, 0, MovedFiles::Answer::Kind::Found) && of(got, 3, MovedFiles::Answer::Kind::Missing));
    CHECK(of(got, 2, MovedFiles::Answer::Kind::Found)); // by step 4 now: where the search found its folder went
    // SEARCH again: walked again from the start, then found by the index.
    f.folder_pause_us = 0;
    const unsigned walks_before = f.walks();
    f.post(search);
    got = answers(f);
    CHECK(f.walks() == walks_before + 1 && f.indexed() && !done(got).cancelled);
    CHECK(of(got, 2, MovedFiles::Answer::Kind::Found) && of(got, 2, MovedFiles::Answer::Kind::Found)->found == card + "/Melodee/Samples/Far.wav");
    CHECK(of(got, 3, MovedFiles::Answer::Kind::Missing) && done(got).found_count == 1 && done(got).missing_count == 1);
    // A second project: its check answered from the index at once, no walk.
    job.generation = 2;
    f.current(2);
    job.slots = {{7, at("media/OTHER/X/Far.wav")}, {8, at("media/OTHER/Gone.wav")}};
    f.post(job);
    got = answers(f);
    CHECK(of(got, 7, MovedFiles::Answer::Kind::Found) && of(got, 8, MovedFiles::Answer::Kind::Missing) && done(got).indexed && f.walks() == walks_before + 1);
    // A file copied back where it was, after the index dropped (FIND MISSING: fresh): found where it was saved.
    file(at("media/OTHER/Gone.wav"), 3);
    search.generation = 2;
    search.fresh = true;
    search.slots = {{8, at("media/OTHER/Gone.wav")}};
    f.post(search);
    got = answers(f);
    CHECK(of(got, 8, MovedFiles::Answer::Kind::Found) && of(got, 8, MovedFiles::Answer::Kind::Found)->found == at("media/OTHER/Gone.wav") && f.walks() == walks_before + 2);
    // A path a slot cannot hold (256 characters or more): not found.
    {
        std::string deep = card + "/Long";
        while (deep.size() < 300) deep += "/" + std::string(40, 'd');
        file(deep + "/Far2.wav", 10);
        f.drop_index();
        job.slots = {{9, at("media/OLD/Far2.wav")}};
        search.fresh = false;
        search.slots = job.slots;
        f.post(search);
        got = answers(f);
        CHECK(of(got, 9, MovedFiles::Answer::Kind::Missing) && done(got).found_count == 0);
        fs::remove_all(card + "/Long");
    }
    // Another project opened mid-search: nothing of the old one's comes, and the walk goes on for the index.
    f.drop_index();
    f.folder_pause_us = 5000;
    search.generation = 3; search.fresh = false;
    search.slots = {{2, far}};
    f.current(3);
    f.post(search);
    CHECK(wait_until([&] { return f.progress().walking; }));
    f.current(4); // switched
    CHECK(wait_until([&] { return f.idle(); }));
    MovedFiles::Answer stale;
    CHECK(f.idle() && !f.take(stale) && f.indexed()); // the walk finished, its answers dropped
    CHECK(f.progress().looked > 40);
}

// A sample found by name alone, in a folder of another name, says nothing of where its kit went: the next sample of
// its saved folder is not taken from that folder by step 4, but by its own name, where its folders match.
void guessed_move() {
    fresh("guess");
    const std::string card = at("media/CARD");
    file(card + "/Melodee/Samples/909/Kick.wav", 10);
    file(card + "/Melodee/Samples/909/Snare.wav", 11);
    file(card + "/New/808/Snare.wav", 12);
    MovedFiles f;
    f.current(1);
    MovedFiles::Job search;
    search.search = true; search.generation = 1; search.card = card;
    search.slots = {{0, at("media/OLD/Drums/808/Kick.wav")}, {1, at("media/OLD/Drums/808/Snare.wav")}};
    f.post(search);
    auto got = answers(f);
    CHECK(of(got, 0, MovedFiles::Answer::Kind::Found) && of(got, 0, MovedFiles::Answer::Kind::Found)->found == card + "/Melodee/Samples/909/Kick.wav");
    CHECK(of(got, 1, MovedFiles::Answer::Kind::Found) && of(got, 1, MovedFiles::Answer::Kind::Found)->found == card + "/New/808/Snare.wav");
}

// The MIDI thread never waits on the finder's lock: held by the finder, take() says "nothing yet" at once.
void take_never_waits() {
    fresh("wait");
    MovedFiles f;
    f.current(1);
    std::atomic<bool> held {false}, done {false};
    std::thread finder([&] {
        auto guard = f.hold_lock();
        held = true;
        while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    CHECK(wait_until([&] { return held.load(); }));
    const auto from = std::chrono::steady_clock::now();
    std::thread timeout([&] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); done = true; });
    MovedFiles::Answer a;
    CHECK(!f.take(a));
    const auto took = std::chrono::steady_clock::now() - from;
    done = true;
    timeout.join();
    finder.join();
    CHECK(took < std::chrono::milliseconds(50));
}

void too_big_and_end() {
    fresh("big");
    const std::string card = at("media/CARD");
    for (int n = 0; n < 30; ++n) file(card + "/A/F" + std::to_string(n) + ".wav", 10);
    file(card + "/A/Mine/Hit.wav", 10);
    file(card + "/Z/Hit.wav", 11);
    // Past the files it may hold: a search stops as a cancel does (what it plainly found taken, nothing said missing),
    // says so, and the card is not walked again this session.
    MovedFiles f;
    f.file_limit = 20;
    f.current(1);
    MovedFiles::Job search;
    search.search = true; search.generation = 1; search.card = card;
    search.slots = {{0, at("media/OLD/Mine/Hit.wav")}, {1, at("media/OLD/Nowhere.wav")}};
    f.post(search);
    auto got = answers(f);
    CHECK(done(got).too_big && !done(got).cancelled && f.walks() == 1);
    CHECK(!of(got, 1, MovedFiles::Answer::Kind::Missing));
    search.fresh = true; // FIND MISSING too
    f.post(search);
    got = answers(f);
    CHECK(done(got).too_big && f.walks() == 1);
    f.drop_index(); // another card, one back from a computer: walked again
    search.fresh = false;
    f.post(search);
    got = answers(f);
    CHECK(done(got).too_big && f.walks() == 2);
    // A folder that cannot be read mid-walk (the card pulled out): the search says so, finds nothing, says nothing
    // missing, and keeps no index.
    {
        for (int d = 0; d < 20; ++d) file(card + "/W/D" + std::to_string(d) + "/x.txt");
        MovedFiles h;
        h.folder_pause_us = 20000;
        h.current(1);
        MovedFiles::Job job = search;
        job.slots = {{0, at("media/OLD/W/Nowhere.wav")}};
        h.post(job);
        CHECK(wait_until([&] { return h.progress().looked >= 55; })); // W listed, its last folder not walked yet
        fs::remove_all(card + "/W/D9");
        h.folder_pause_us = 0;
        got = answers(h);
        CHECK(done(got).failed && got.size() == 1 && !h.indexed());
        fs::remove_all(card + "/W");
    }
    // The end comes within a folder, however slow the card.
    auto g = std::make_unique<MovedFiles>();
    g->folder_pause_us = 3000000;
    g->current(1);
    g->post(search);
    CHECK(wait_until([&] { return g->progress().walking; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto ending = std::chrono::steady_clock::now();
    g.reset();
    CHECK(std::chrono::steady_clock::now() - ending < std::chrono::milliseconds(500));
}

void fold_and_memory() {
    // exFAT ignores case in Latin-1 and Latin Extended-A too.
    CHECK(fold_case("\xc3\x80" "BC\xc3\x89") == "\xc3\xa0" "bc\xc3\xa9");  // ÀBCÉ
    CHECK(fold_case("\xc3\x97") == "\xc3\x97");                          // × stays
    CHECK(fold_case("\xc4\x80\xc5\x81") == "\xc4\x81\xc5\x82");          // ĀŁ
    CHECK(fold_case("\xc5\xb8") == "\xc3\xbf");                          // Ÿ
    CHECK(fold_case("\xc5\xbd") == "\xc5\xbe");                          // Ž
    // At the limit, the index holds its names in some 10 MB or less (names of 24 characters, 100 files a folder).
    CardIndex index;
    const uint32_t top = index.add_folder(CardIndex::none, "/media/CARD");
    for (unsigned d = 0; d < CardIndex::max_files / 100; ++d) {
        const uint32_t folder = index.add_folder(top, "Folder " + std::to_string(d));
        for (unsigned n = 0; n < 100; ++n) {
            char name[32];
            std::snprintf(name, sizeof(name), "Sample_%06u_%05u.wav", d, n);
            index.add_file(folder, name);
        }
    }
    const size_t bytes = index.bytes_used();
    std::printf("index at the limit: %zu files, %.1f MB\n", index.indexed(), bytes / 1048576.0);
    CHECK(index.indexed() == CardIndex::max_files && bytes < size_t(10) << 20);
}
} // namespace

int main() {
    try {
        steps();
        index_and_ties();
        plugins();
        worker();
        too_big_and_end();
        fold_and_memory();
        guessed_move();
        take_never_waits();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAILED %s\n", e.what());
        fs::remove_all(fs::temp_directory_path() / ("finder-" + std::to_string(::getpid())));
        return 1;
    }
    fs::remove_all(fs::temp_directory_path() / ("finder-" + std::to_string(::getpid())));
    std::puts("file finder ok");
    return 0;
}
