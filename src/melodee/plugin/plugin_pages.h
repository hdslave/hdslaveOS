// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// A plug-in's pages (docs/PLUGIN_PAGES_PLAN.md): named groups of its parameters, laid out for K1-K3 in rows of
// three. The rack takes them from the first source that has them: the plug-in's CLAP remote-controls pages, an
// Open MPC skin's Q-Link pages, its parameter groups (VST3 units, LV2 port groups, CLAP modules), else its list.
// Plain C++, no JUCE: the host tests read it too.
namespace melodee {

struct PluginPage {
    std::string name;      // "" none: the plug-in's one list
    std::vector<int> params; // parameter indices, in order
};

// Pages as the cards take them: each page's parameters in rows of three, its last row filled with -1 (no
// parameter), so a row never holds two pages' parameters. `slots[row * 3 + k]` is what K(k + 1) turns on that row.
struct PluginLayout {
    std::vector<int> slots;
    std::vector<std::string> names;   // each page's name
    std::vector<uint16_t> first_rows; // each page's first row
    unsigned rows() const { return unsigned(slots.size() / 3); }
    unsigned pages() const { return unsigned(names.size()); }
    int at(size_t slot) const { return slot < slots.size() ? slots[slot] : -1; }
    // The page row `row` is on (0 with none).
    unsigned page_of(unsigned row) const {
        unsigned page = 0;
        for (unsigned p = 1; p < first_rows.size() && first_rows[p] <= row; ++p) page = p;
        return page;
    }
    // How many rows page `page` has.
    unsigned rows_of(unsigned page) const {
        if (page >= first_rows.size()) return 0;
        unsigned end = page + 1 < first_rows.size() ? first_rows[page + 1] : rows();
        return end - first_rows[page];
    }
};

// Lays out `pages`: within a page a parameter is taken once (it may be on more than one page); a page left empty
// is dropped.
inline PluginLayout lay_out(const std::vector<PluginPage>& pages) {
    PluginLayout out;
    for (const auto& page : pages) {
        std::vector<int> own;
        for (int p : page.params)
            if (p >= 0 && std::find(own.begin(), own.end(), p) == own.end()) own.push_back(p);
        if (own.empty()) continue;
        out.names.push_back(page.name);
        out.first_rows.push_back(uint16_t(out.rows()));
        out.slots.insert(out.slots.end(), own.begin(), own.end());
        while (out.slots.size() % 3) out.slots.push_back(-1);
    }
    return out;
}

// Each page keeps only the parameters in `allowed` (the ones shown at all: not hidden, not the bypass).
inline void keep_only(std::vector<PluginPage>& pages, const std::vector<int>& allowed) {
    for (auto& page : pages)
        page.params.erase(std::remove_if(page.params.begin(), page.params.end(), [&](int p) {
            return std::find(allowed.begin(), allowed.end(), p) == allowed.end();
        }), page.params.end());
}

// Pages from parameter groups (VST3 units, LV2 port groups, CLAP modules): `groups[i]` is the group of `params[i]`
// as a path, "/" between levels ("" none). With any group: parameters outside every group on a first page MAIN,
// then a page per group in the order the groups first appear, named by the group's last level (its whole path, a
// space between levels, where two groups end the same). With none: one page "" of them all.
inline std::vector<PluginPage> group_pages(const std::vector<int>& params, const std::vector<std::string>& groups) {
    std::vector<std::string> paths;
    for (size_t i = 0; i < params.size(); ++i) {
        const std::string& g = i < groups.size() ? groups[i] : std::string();
        if (!g.empty() && std::find(paths.begin(), paths.end(), g) == paths.end()) paths.push_back(g);
    }
    if (paths.empty()) return {PluginPage {"", params}};
    auto last = [](const std::string& path) {
        auto cut = path.find_last_of('/');
        return cut == std::string::npos ? path : path.substr(cut + 1);
    };
    std::vector<PluginPage> pages(paths.size() + 1);
    pages[0].name = "MAIN";
    for (size_t g = 0; g < paths.size(); ++g) {
        std::string leaf = last(paths[g]);
        bool shared = std::count_if(paths.begin(), paths.end(), [&](const std::string& p) { return last(p) == leaf; }) > 1;
        if (!shared) { pages[g + 1].name = leaf; continue; }
        std::string whole = paths[g];
        std::replace(whole.begin(), whole.end(), '/', ' ');
        pages[g + 1].name = whole;
    }
    for (size_t i = 0; i < params.size(); ++i) {
        const std::string& g = i < groups.size() ? groups[i] : std::string();
        size_t page = g.empty() ? 0 : size_t(std::find(paths.begin(), paths.end(), g) - paths.begin()) + 1;
        pages[page].params.push_back(params[i]);
    }
    if (pages[0].params.empty()) pages.erase(pages.begin());
    return pages;
}

// The pages a plug-in gets (the plan's pick 2): its own pages (`own`: remote-controls or Q-Link pages, already
// kept to `shown`) first, then, where it has parameter groups, the group pages (group_pages()) of the parameters
// its own pages leave out, so nothing is lost (Surge XT's remote-controls pages hold 36 of its 775). A plug-in
// without groups shows only its own pages. With no own pages: group_pages() of them all.
inline std::vector<PluginPage> pages_with_groups(std::vector<PluginPage> own, const std::vector<int>& shown,
                                                 const std::vector<std::string>& groups) {
    own.erase(std::remove_if(own.begin(), own.end(), [](const PluginPage& p) { return p.params.empty(); }), own.end());
    if (own.empty()) return group_pages(shown, groups);
    bool grouped = std::any_of(groups.begin(), groups.end(), [](const std::string& g) { return !g.empty(); });
    if (!grouped) return own;
    std::vector<int> rest;
    std::vector<std::string> rest_groups;
    for (size_t i = 0; i < shown.size(); ++i) {
        bool taken = std::any_of(own.begin(), own.end(), [&](const PluginPage& p) {
            return std::find(p.params.begin(), p.params.end(), shown[i]) != p.params.end();
        });
        if (taken) continue;
        rest.push_back(shown[i]);
        rest_groups.push_back(i < groups.size() ? groups[i] : std::string());
    }
    if (rest.empty()) return own;
    auto more = group_pages(rest, rest_groups);
    if (more.size() == 1 && more[0].name.empty()) more[0].name = "MAIN"; // the rest all outside every group
    own.insert(own.end(), more.begin(), more.end());
    return own;
}

} // namespace melodee
