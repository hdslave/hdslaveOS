// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files. Where a project's sample or plug-in is now when it is not at the path the project saved:
// the card under another name, the project's folder moved with its samples, a stock project's sample named without
// its extension, the rest of a kit found in a new folder, and, after the question (shell/find_moved.h), by name
// anywhere on the card and in the internal sample folders. Plain C++, no JUCE: the host tests call it directly.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace melodee {

// Lower case, as exFAT (the card's file system on the MPC) compares names: ASCII letters, and in UTF-8 those of
// Latin-1 and Latin Extended-A.
std::string fold_case(std::string text);

// A slot's saved path as the file it names and what follows it: "<file>.xpm#<pad>" (a pad of an MPC program or
// project, also .xty and .xpj) and "<file>.sf2#<preset>" are looked for by their file, then named again with the
// same "#..." after it. A '#' in a file's own name is part of the name.
struct SavedPath { std::string file, part; };
SavedPath split_saved(const std::string& saved);

// The extensions a name saved without one is tried with (the stock importer can leave it off), in this order.
const std::vector<std::string>& sample_extensions();

// The audio files (and instruments, MPC projects and programs) on the card and in the internal sample folders, by name
// in any case. Built a folder at a time on the finder's thread (step()), so it can be stopped between folders and, a
// large folder, within one. Names are kept in one pool, about 40 bytes a file: some 8 MB at the limit.
class CardIndex {
public:
    static constexpr unsigned max_depth = 12;   // folders deeper are left out
    static constexpr size_t max_files = 200000; // files indexed: past it the walk stops (too_big())
    unsigned depth_limit = max_depth;           // the tests' smaller cards
    size_t file_limit = max_files;
    const std::atomic<bool>* abort = nullptr;   // set: step() stops where it is (Stopped)
    struct File { std::string path; uint64_t bytes = 0; };

    // A new walk: the card's top and all under it (but its boot folder, Melodee/Cache, Melodee/Crashes,
    // Melodee/Plugins, hidden files and folders, and the system's own), then each of `internal` (one not there is passed
    // over).
    void begin(const std::string& card_top, const std::vector<std::string>& internal);
    // More: go on. Done: the whole card (complete()). TooBig: past file_limit, what it holds kept (too_big()). Failed: a
    // folder could not be read (a card pulled out): cleared. Stopped: `abort` set, the walk left where it was.
    enum class Step : uint8_t { More, Done, TooBig, Failed, Stopped };
    Step step();
    bool walking() const { return walking_; }
    bool complete() const { return complete_; }
    bool too_big() const { return too_big_; }
    void clear();
    const std::string& card() const { return card_; }
    size_t looked() const { return looked_; } // entries looked at so far, folders and files
    size_t indexed() const { return entries_.size(); }
    // The files of this name, any case, still there (empty: none).
    std::vector<File> files(const std::string& name) const;
    // A search's slots (their saved paths): matched() counts those with a file of their name met so far.
    void watch(const std::vector<std::string>& saved);
    size_t matched() const { return matched_; }
    size_t bytes_used() const; // what it holds in memory
    // The walk's own steps, open for the tests: a folder (under `parent`, none: a top, its whole path) and a file in it.
    uint32_t add_folder(uint32_t parent, const std::string& name);
    void add_file(uint32_t folder, const std::string& name);
    static constexpr uint32_t none = 0xffffffffu;

private:
    struct Dir { uint32_t parent = none, name = 0; };
    struct Entry { uint32_t name = 0, folder = 0, next = none; };
    struct Walk { uint32_t folder = 0; unsigned depth = 0; bool card_top = false, top = false; };
    uint32_t keep(const std::string& text); // into the pool
    const char* at(uint32_t offset) const { return pool_.data() + offset; }
    std::string dir_path(uint32_t folder) const;
    void rehash(size_t buckets);
    std::vector<char> pool_;
    std::vector<Dir> dirs_;
    std::vector<Entry> entries_;
    std::vector<uint32_t> buckets_;
    std::vector<Walk> stack_;
    std::unordered_map<std::string, std::vector<uint32_t>> watched_; // a name, the slots with it
    std::vector<bool> slot_seen_;
    size_t matched_ = 0;
    std::string card_;
    size_t looked_ = 0;
    bool walking_ = false, complete_ = false, too_big_ = false;
};

// Where a folder of the project's samples went: from the folder a found sample was saved in to the one it was found in,
// both cut back to where they differ (Lib/KitA found as New/KitA: Lib went to New, and Lib/KitB with it).
struct FolderMove { std::string from, to; };
FolderMove folder_move(const std::string& saved, const std::string& found);

// What a search found for one path: the path (empty: not found), the step that found it (1-5), and, not found,
// whether files of that name were there but none could be told from the others (decision 1).
struct FoundFile { std::string path; uint8_t step = 0; bool tie = false; };

// Where a project's sample is now, when `saved` (the slot's path) is not there. `card_top`: the card's top as the
// library found it; `project_file`: the project's own file (.mldp, or the .xpj it was opened from; empty: none);
// `moves`: where this project's other samples were found (folder_move()); `index`: the card's index for step 5,
// nullptr for steps 1-4 only. The first step that finds it wins:
//   1. upstream's repair, card_sample_path (Samples/ at the card's top into Melodee/Samples/);
//   2. the card renamed: /media/<id>/ of the saved path swapped for the card's top (any volume beside the card's);
//   3. beside the project: the saved path's last folder and name, then the name, under the project file's folder
//      (an .xpj's "<stem>_[ProjectData]/<name>" too);
//   4. with the rest of the kit: where `moves` puts the saved path's own folder, only;
//   5. by name in the index (pick_by_name); one not complete (a search stopped) only what pick_by_name takes from part
//      of a card.
// A name without a sample extension tries each of sample_extensions() at each step. The "#..." of a program's pad
// stays on the path found.
FoundFile find_moved_sample(const std::string& saved, const std::string& card_top, const std::string& project_file,
                            const std::vector<FolderMove>& moves, const CardIndex* index);

// Step 5: of `files` (all of the saved path's name), the one that shares the most trailing folders with `saved`,
// then one inside the project file's folder, then one in Melodee/Samples. Tied files that are copies of one file (the
// same size, the same first and last 4 KB) give the first by path; tied files that differ give none (tie set).
// `partial` (the card not all walked): only one sharing a trailing folder with `saved`, or inside the project's
// folder, is taken; a better one may be where the walk did not reach.
FoundFile pick_by_name(const std::string& saved, const std::string& card_top, const std::string& project_file,
                       std::vector<CardIndex::File> files, bool partial = false);

// Where a project's plug-in may be now, when neither `saved` nor upstream's card_plugin_path() of it is there (the
// rack's worker, plugin/plugin_rack.cpp), the best first: the card renamed (and its old top-level Plugins/ into
// Melodee/Plugins/), then the same file or bundle name in Melodee/Plugins up to four folders down, any case (the most
// trailing folders in common, then the shortest path). The rack takes the first that holds the plug-in saved.
std::vector<std::string> find_moved_plugins(const std::string& saved, const std::string& card_top);

} // namespace melodee
