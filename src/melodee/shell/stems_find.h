// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <deque>
#include <filesystem>
#include <string>
#include <utility>

// Where the stem separator (shell/stems.h) is on the card. A release keeps it in autorun/melodee/stems; `device.py
// install-sd` in autorun/sample-base/stems (sample-base/stems at the card's top for an install from before). A boot
// menu that puts Melodee in a folder of its own (SimpleBoot, mpc-boot-thingy) keeps it beside the app there, so the
// folder the app runs from is looked at next, then the card's folders three deep, the Melodee folder left out.
namespace melodee {

inline constexpr const char* stems_program_name = "melodee-stems";
// Card folders read at most while looking (each one a few card reads on exFAT, shell/card_warm.h).
inline constexpr unsigned stems_search_folders = 256;
inline constexpr unsigned stems_search_depth = 3;

// The program's path in `folder`/stems, empty when it is not there.
inline std::string stems_in(const std::filesystem::path& folder) {
    std::error_code error;
    const auto path = folder / "stems" / stems_program_name;
    return std::filesystem::is_regular_file(path, error) ? path.string() : std::string();
}

// `card_top`: the card's top folder; `app_folder`: the folder the running program is in (empty: not known). Empty
// when the program is nowhere.
inline std::string find_stems_program(const std::string& card_top, const std::string& app_folder = {}) {
    namespace fs = std::filesystem;
    if (card_top.empty()) return {};
    for (const char* folder : {"autorun/melodee", "autorun/sample-base", "sample-base"})
        if (auto path = stems_in(fs::path(card_top) / folder); !path.empty()) return path;
    if (!app_folder.empty())
        if (auto path = stems_in(app_folder); !path.empty()) return path;
    // The card's folders, nearest first: autorun before the rest, Melodee's own and hidden ones left out.
    std::deque<std::pair<fs::path, unsigned>> left {{fs::path(card_top), 0u}};
    unsigned read = 0;
    while (!left.empty() && read < stems_search_folders) {
        auto [folder, depth] = left.front();
        left.pop_front();
        if (auto path = stems_in(folder); !path.empty()) return path;
        if (depth == stems_search_depth) continue;
        ++read;
        std::error_code error;
        fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error);
        for (; !error && it != fs::directory_iterator(); it.increment(error)) {
            std::error_code kind;
            if (!it->is_directory(kind) || it->is_symlink(kind)) continue;
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.' || (!depth && name == "Melodee")) continue;
            if (!depth && name == "autorun") left.emplace_front(it->path(), depth + 1);
            else left.emplace_back(it->path(), depth + 1);
        }
    }
    return {};
}

} // namespace melodee
