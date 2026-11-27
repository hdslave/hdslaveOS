// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/model/project_file.h"
#include <cstdio>
#include <filesystem>
#include <string>

// A card mounts at /media/<its volume id>, and a project, a bank file or the settings name their files with that
// folder in front. On another card (a new one with the files copied over, or the same files on a card formatted
// again) that folder is not there: the files are looked for at the same place on the card in now. A path on the
// card in now, off the mounts (the internal drive), relative, or there as saved stays as it is.
namespace melodee {

inline std::string on_card_now(const std::string& saved, const std::string& card_top) {
    namespace fs = std::filesystem;
    if (card_top.empty() || saved.empty() || saved[0] != '/') return saved;
    fs::path top = fs::path(card_top).lexically_normal();
    if (top.filename().empty()) top = top.parent_path(); // "/media/card/"
    const fs::path mounts = top.parent_path();           // "/media"
    if (mounts.empty() || mounts == top.root_path()) return saved;
    const fs::path inside = fs::path(saved).lexically_normal().lexically_relative(mounts);
    if (inside.empty() || *inside.begin() == "." || *inside.begin() == "..") return saved; // not on a mount
    auto part = inside.begin();
    if (mounts / *part == top) return saved; // on the card in now
    fs::path rest;
    for (++part; part != inside.end(); ++part) rest /= *part;
    if (rest.empty()) return saved;
    std::error_code e;
    if (fs::exists(saved, e)) return saved; // that card or drive is in too
    return (top / rest).string();
}

// A project's sample files and plug-ins, saved on another card: on the card in now. True when one moved.
inline bool samples_on_card_now(Project& project, const std::string& card_top) {
    if (card_top.empty()) return false;
    bool moved = false;
    for (size_t n = 0; n < project.samples.size(); ++n) {
        if (!project.samples[n].used()) continue; // read without making a block
        const std::string now = on_card_now(project.samples[n].path, card_top);
        if (now == project.samples[n].path || now.size() >= sizeof(SampleRef::path)) continue;
        std::snprintf(project.samples.edit(n).path, sizeof(SampleRef::path), "%s", now.c_str());
        moved = true;
    }
    for (auto& ref : project.plugins) {
        if (!ref.used()) continue;
        const std::string now = on_card_now(ref.path, card_top);
        if (now == ref.path || now.size() >= sizeof(ref.path)) continue;
        std::snprintf(ref.path, sizeof(ref.path), "%s", now.c_str());
        moved = true;
    }
    return moved;
}

// A bank file's samples, saved on another card: on the card in now.
inline void samples_on_card_now(BankFile& bank, const std::string& card_top) {
    if (card_top.empty()) return;
    for (auto& ref : bank.samples) {
        if (!ref.used()) continue;
        const std::string now = on_card_now(ref.path, card_top);
        if (now != ref.path && now.size() < sizeof(ref.path)) std::snprintf(ref.path, sizeof(ref.path), "%s", now.c_str());
    }
}

} // namespace melodee
