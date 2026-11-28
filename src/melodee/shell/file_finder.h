// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files. Where a project's sample or plug-in is now when it is not at the path the project saved:
// the card under another name, the project's folder moved with its samples, a stock project's sample named without
// its extension, the rest of a kit found in a new folder, and, after the question (shell/find_moved.h), by name
// anywhere on the card and in the internal sample folders. Plain C++, no JUCE: the host tests call it directly.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace melodee {

// ASCII letters in lower case: exFAT, the card's file system on the MPC, ignores case.
std::string fold_case(std::string text);

// A slot's saved path as the file it names and what follows it: "<file>.xpm#<pad>" (a pad of an MPC program or
// project, also .xty and .xpj) and "<file>.sf2#<preset>" are looked for by their file, then named again with the
// same "#..." after it. A '#' in a file's own name is part of the name.
struct SavedPath { std::string file, part; };
SavedPath split_saved(const std::string& saved);

// The extensions a name saved without one is tried with (the stock importer can leave it off), in this order.
const std::vector<std::string>& sample_extensions();

// The audio files (and instruments) on the card and in the internal sample folders, by name in any case, with each
// file's size. Built a folder at a time on the finder's thread (step()), so it can be stopped between folders.
class CardIndex {
public:
    static constexpr unsigned max_depth = 12;
    static constexpr size_t max_entries = 200000;
    unsigned depth_limit = max_depth; // the tests' smaller cards
    size_t entry_limit = max_entries;
    struct File { std::string path; uint64_t bytes = 0; };

    // A new walk: the card's top and all under it (but its boot folder, Melodee/Cache, Melodee/Crashes,
    // Melodee/Plugins, hidden files and folders, and the system's own), then each of `internal`.
    void begin(const std::string& card_top, const std::vector<std::string>& internal);
    enum class Step : uint8_t { More, Done, TooBig };
    Step step(); // the next folder; TooBig past max_entries or max_depth (the index is then cleared)
    bool walking() const { return walking_; }
    bool complete() const { return complete_; }
    void clear();
    const std::string& card() const { return card_; }
    size_t looked() const { return looked_; } // entries looked at so far, folders and files
    size_t folders() const { return folders_seen_; }
    // The files of this name, any case (empty: none).
    std::vector<File> files(const std::string& name) const;
    // Names a search waits for: matched() counts how many have been seen so far, while the walk goes on.
    void watch(const std::vector<std::string>& names);
    size_t matched() const { return seen_.size(); }

private:
    struct Dir { std::string path; unsigned depth = 0; bool card_top = false; };
    struct Entry { uint32_t folder = 0; std::string name; uint64_t bytes = 0; };
    std::vector<Dir> stack_;
    std::vector<std::string> dirs_;
    std::unordered_map<std::string, std::vector<Entry>> names_;
    std::unordered_set<std::string> watched_, seen_;
    std::string card_;
    size_t looked_ = 0, folders_seen_ = 0;
    bool walking_ = false, complete_ = false;
};

// What a search found for one path: the path (empty: not found), the step that found it (1-5), and, not found,
// whether files of that name were there but none could be told from the others (sizes differ: decision 1).
struct FoundFile { std::string path; uint8_t step = 0; bool tie = false; };

// Where a project's sample is now, when `saved` (the slot's path) is not there. `card_top`: the card's top as the
// library found it; `project_file`: the project's own file (.mldp, or the .xpj it was opened from; empty: none);
// `kit_folders`: the folders where this project's other samples were found; `index`: the card's index for step 5,
// nullptr for steps 1-4 only. The first step that finds it wins:
//   1. upstream's repair, card_sample_path (Samples/ at the card's top into Melodee/Samples/);
//   2. the card renamed: /media/<id>/ of the saved path swapped for the card's top (any volume beside the card's);
//   3. beside the project: the saved path's last folder and name, then the name, under the project file's folder
//      (an .xpj's "<stem>_[ProjectData]/<name>" too);
//   4. with the rest of the kit: the name in `kit_folders`;
//   5. by name in the index (pick_by_name), complete or (a search cancelled) as far as it got.
// A name without a sample extension tries each of sample_extensions() at each step. The "#..." of a program's pad
// stays on the path found.
FoundFile find_moved_sample(const std::string& saved, const std::string& card_top, const std::string& project_file,
                            const std::vector<std::string>& kit_folders, const CardIndex* index);

// Step 5: of `files` (all of the saved path's name), the one that shares the most trailing folders with `saved`,
// then one inside the project file's folder, then one in Melodee/Samples. Tied files of one size are copies of one
// file: the first by path. Tied files of different sizes: none (tie set).
FoundFile pick_by_name(const std::string& saved, const std::string& card_top, const std::string& project_file,
                       std::vector<CardIndex::File> files);

// Where a project's plug-in is now, when neither `saved` nor upstream's card_plugin_path() of it is there (the
// rack's worker, plugin/plugin_rack.cpp): the card renamed (and its old top-level Plugins/ into Melodee/Plugins/),
// then the same file or bundle name in Melodee/Plugins up to four folders down, any case (several: the most trailing
// folders in common, then the shortest path). Empty: not found.
std::string find_moved_plugin(const std::string& saved, const std::string& card_top);

} // namespace melodee
