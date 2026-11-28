// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/card_folders.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// 16 Levels FILE's folders (issue 65): the card's .progression files (shell/progression_file.h list_progressions,
// listed once) shown a folder at a time, as a file's place: under Melodee/Progressions as it sits there (Jazz/ii-V-I),
// under <card>/Progressions in Progressions/, under either Expansions folder in Expansions/. The folders in view come
// first, by name, then its files in the list's order. Pure, for the shell and the tests.
namespace melodee {

// A file's place in FILE's folders, its .progression left on.
inline std::string prog_place(const std::string& card, const std::string& path) {
    auto under = [&](const std::string& folder) -> std::string {
        if (folder.empty() || path.size() <= folder.size() + 1 || path.compare(0, folder.size(), folder) || path[folder.size()] != '/') return {};
        return path.substr(folder.size() + 1);
    };
    if (auto own = under(card + "/Melodee/Progressions"); !own.empty()) return own;
    if (auto top = under(card + "/Progressions"); !top.empty()) return "Progressions/" + top;
    if (auto ex = under(samples_folder(card) + "/Expansions"); !ex.empty()) return "Expansions/" + ex;
    if (auto old = under(old_samples_folder(card) + "/Expansions"); !old.empty()) return "Expansions/" + old;
    auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1); // elsewhere: at the top
}

// A row of the folder in view: a folder (file -1) or a file (its place in the list).
struct ProgRow {
    std::string name; // the folder's name, or the file's without .progression
    int file = -1;
};

// The rows of `folder` ("" the top; "Jazz", "Expansions/Organs") of the places, which are the list's in its order.
inline std::vector<ProgRow> prog_folder_rows(const std::vector<std::string>& places, const std::string& folder) {
    std::vector<ProgRow> folders, files;
    const std::string prefix = folder.empty() ? std::string() : folder + "/";
    for (size_t n = 0; n < places.size(); ++n) {
        const auto& place = places[n];
        if (place.compare(0, prefix.size(), prefix)) continue;
        const std::string rest = place.substr(prefix.size());
        auto slash = rest.find('/');
        if (slash != std::string::npos) {
            std::string name = rest.substr(0, slash);
            if (std::none_of(folders.begin(), folders.end(), [&](const ProgRow& r) { return r.name == name; })) folders.push_back({name, -1});
            continue;
        }
        std::string name = rest;
        auto dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0) name.resize(dot);
        files.push_back({name, int(n)});
    }
    auto lower = [](std::string s) { for (auto& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch))); return s; };
    std::stable_sort(folders.begin(), folders.end(), [&](const ProgRow& a, const ProgRow& b) { return lower(a.name) < lower(b.name); });
    folders.insert(folders.end(), files.begin(), files.end());
    return folders;
}

// The folder a place sits in ("" the top).
inline std::string prog_place_folder(const std::string& place) {
    auto slash = place.rfind('/');
    return slash == std::string::npos ? std::string() : place.substr(0, slash);
}
// The folder above ("" from one at the top).
inline std::string prog_folder_up(const std::string& folder) { return prog_place_folder(folder); }
// The last name of a folder, for the header ("" the top).
inline std::string prog_folder_name(const std::string& folder) {
    auto slash = folder.rfind('/');
    return slash == std::string::npos ? folder : folder.substr(slash + 1);
}

} // namespace melodee
