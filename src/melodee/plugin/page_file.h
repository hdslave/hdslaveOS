// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/plugin/drum_map.h"
#include "melodee/plugin/plugin_pages.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A page file (docs/PLUGIN_PAGES.md): JSON naming a plug-in and the pages Melodee shows for it, from the card's
// Melodee/Plugin Pages folder, the plug-in's own package or Melodee's library (plugin/page_library.h). The rack reads
// it at load (plugin/plugin_rack.cpp); a file that does not read or match is passed over for the next source.
// Plain C++, no JUCE: the host tests read it too.
namespace melodee {

// A parameter as a file names it: by its name as the plug-in gives it, or by the format's own ID (JUCE's
// getParameterID(): the CLAP param id, VST3 ParamID, VST2 index, LV2 symbol); a label for its card where the
// plug-in's name is long.
struct PageFileParam {
    std::string name, id, label;
};
struct PageFilePage {
    std::string name;
    std::vector<PageFileParam> params;
    bool rest = false;              // then every parameter no page of the file names, in the plug-in's order
    PagePicture picture = PagePicture::None;
    std::vector<PageFileParam> parts; // the picture's parameters in the kind's order (plugin_pages.h PagePictureSlots)
};
struct PageFile {
    // Which plug-in: any of its CLAP id, VST2 unique ID (four letters, or a number), LV2 URI, or its name as Melodee
    // shows it (with its vendor, where given). VST3 plug-ins go by name.
    std::string clap, vst2, lv2, name, vendor;
    std::vector<PageFilePage> pages;
};
// What the rack knows of a loaded plug-in, to match a file against.
struct PluginIdentity {
    std::string clap, lv2, name, vendor;
    int32_t vst2 = 0;               // its unique ID; 0 none
};

namespace page_file_detail {
using drum_map_detail::Reader;
// A parameter: "Name", or {"name": "Name", "label": "SHORT"}, or {"id": "1234"}.
inline bool param(Reader& r, PageFileParam& out) {
    if (r.peek('"')) return r.string(&out.name);
    if (!r.peek('{')) return false;
    return r.object([&](const std::string& key) {
        if ((key == "name" || key == "label" || key == "id") && r.peek('"'))
            return r.string(key == "name" ? &out.name : key == "label" ? &out.label : &out.id);
        if (key == "id") { long v = 0; if (!r.number(&v)) return false; out.id = std::to_string(v); return true; }
        return r.skip();
    });
}
template <class Each> bool array(Reader& r, Each each) {
    if (!r.peek('[')) return r.skip();
    if (++r.depth > 32) return false;
    r.eat('[');
    if (!r.eat(']')) {
        do { if (!each()) return false; } while (r.eat(','));
        if (!r.eat(']')) return false;
    }
    --r.depth;
    return true;
}
} // namespace page_file_detail

// Reads `json` into `out`; false with `why` (a few words) when it is not a page file Melodee can use.
inline bool read_page_file(std::string_view json, PageFile& out, std::string& why) {
    using namespace page_file_detail;
    out = {};
    Reader r {json};
    long version = 0;
    bool ok = r.object([&](const std::string& key) {
        if (key == "melodee-pages") return r.number(&version) || r.skip();
        if (key == "plugin") {
            return r.object([&](const std::string& k) {
                if (k == "vst2" && !r.peek('"')) { long v = 0; if (!r.number(&v)) return r.skip(); out.vst2 = std::to_string(v); return true; }
                std::string* field = k == "clap" ? &out.clap : k == "vst2" ? &out.vst2 : k == "lv2" ? &out.lv2
                                   : k == "name" ? &out.name : k == "vendor" ? &out.vendor : nullptr;
                return field && r.peek('"') ? r.string(field) : r.skip();
            });
        }
        if (key != "pages") return r.skip();
        return array(r, [&] {
            if (!r.peek('{')) return r.skip();
            PageFilePage page;
            bool read = r.object([&](const std::string& k) {
                if (k == "name" && r.peek('"')) return r.string(&page.name);
                if (k == "rest") { page.rest = r.word("true"); return page.rest || r.skip(); }
                if (k == "params")
                    return array(r, [&] { PageFileParam p; if (!param(r, p)) return r.skip(); page.params.push_back(p); return true; });
                if (k != "picture" || !r.peek('{')) return r.skip();
                std::string kind;
                std::vector<std::pair<std::string, PageFileParam>> named; // the picture's parts by their role
                std::vector<PageFileParam> listed;                          // value: "params"
                bool pic = r.object([&](const std::string& part) {
                    if (part == "kind" && r.peek('"')) return r.string(&kind);
                    if (part == "params")
                        return array(r, [&] { PageFileParam p; if (!param(r, p)) return r.skip(); listed.push_back(p); return true; });
                    if (part == "bands")
                        return array(r, [&] {
                            if (!r.peek('{')) return r.skip();
                            PageFileParam band[3];
                            bool b = r.object([&](const std::string& role) {
                                int n = role == "freq" ? 0 : role == "gain" ? 1 : role == "q" ? 2 : -1;
                                return n >= 0 ? param(r, band[n]) || r.skip() : r.skip();
                            });
                            for (auto& p : band) listed.push_back(p);
                            return b;
                        });
                    PageFileParam p;
                    if (!param(r, p)) return r.skip();
                    named.push_back({part, p});
                    return true;
                });
                if (!pic) return false;
                auto role = [&](const char* name) {
                    for (auto& [k2, p] : named) if (k2 == name) return p;
                    return PageFileParam {};
                };
                if (kind == "envelope") { page.picture = PagePicture::Envelope; page.parts = {role("attack"), role("decay"), role("sustain"), role("release")}; }
                else if (kind == "filter") { page.picture = PagePicture::Filter; page.parts = {role("cutoff"), role("resonance")}; }
                else if (kind == "value") { page.picture = PagePicture::Value; listed.resize(std::min<size_t>(listed.size(), 3)); page.parts = listed; }
                else if (kind == "eq") { page.picture = PagePicture::Eq; listed.resize(std::min<size_t>(listed.size(), PagePictureSlots::most)); page.parts = listed; }
                else if (kind == "scope") page.picture = PagePicture::Scope;
                return true; // an unknown kind: no picture
            });
            if (!read) return false;
            out.pages.push_back(std::move(page));
            return true;
        });
    });
    if (!ok) { why = "not JSON Melodee can read"; return false; }
    if (version < 1) { why = "no \"melodee-pages\": 1"; return false; }
    if (version > 1) { why = "version " + std::to_string(version) + " is newer than this Melodee"; return false; }
    if (out.clap.empty() && out.vst2.empty() && out.lv2.empty() && out.name.empty()) { why = "names no plug-in"; return false; }
    if (out.pages.empty()) { why = "has no pages"; return false; }
    return true;
}

// A VST2 unique ID as files write it: its four letters ("NAMp"), else the number.
inline std::string vst2_code(int32_t id) {
    std::string code;
    for (int shift = 24; shift >= 0; shift -= 8) {
        char c = char((uint32_t(id) >> shift) & 0xff);
        if (c < 32 || c > 126) return std::to_string(id);
        code.push_back(c);
    }
    return code;
}

// Whether `file` is for the plug-in `who`: its CLAP id, VST2 ID, LV2 URI, or else its name (and vendor, where the
// file gives one).
inline bool page_file_matches(const PageFile& file, const PluginIdentity& who) {
    if (!file.clap.empty() && file.clap == who.clap) return true;
    if (!file.vst2.empty() && who.vst2 && (file.vst2 == vst2_code(who.vst2) || file.vst2 == std::to_string(who.vst2))) return true;
    if (!file.lv2.empty() && file.lv2 == who.lv2) return true;
    if (file.name.empty() || file.name != who.name) return false;
    return file.vendor.empty() || file.vendor == who.vendor;
}

// The file's pages as parameter indices (plugin_pages.h): `shown` the plug-in's parameters any page may show, `names`
// and `ids` every parameter's name and ID by index. A parameter the plug-in does not have is left out and said in
// `problems` (one line each); a page left empty is dropped by lay_out().
inline std::vector<PluginPage> resolve_page_file(const PageFile& file, const std::vector<int>& shown, const std::vector<std::string>& names,
                                                 const std::vector<std::string>& ids, std::vector<std::string>& problems) {
    auto lookup = [&](const PageFileParam& p) { // its index among `shown`, -1 none
        for (int s : shown) {
            size_t n = size_t(s);
            if (!p.id.empty() ? n < ids.size() && ids[n] == p.id : !p.name.empty() && n < names.size() && names[n] == p.name) return s;
        }
        return -1;
    };
    auto missing = [&](const PageFileParam& p) {
        if (p.id.empty() && p.name.empty()) return;
        std::string line = "no parameter " + (p.id.empty() ? "\"" + p.name + "\"" : "with id " + p.id);
        if (std::find(problems.begin(), problems.end(), line) == problems.end()) problems.push_back(line);
    };
    std::vector<int> named;
    for (const auto& page : file.pages)
        for (const auto& p : page.params) {
            int i = lookup(p);
            if (i >= 0) named.push_back(i); else missing(p);
        }
    std::vector<PluginPage> out;
    for (const auto& page : file.pages) {
        PluginPage made;
        made.name = page.name;
        for (const auto& p : page.params)
            if (int i = lookup(p); i >= 0) { made.params.push_back(i); made.labels.push_back(p.label); }
        if (page.rest)
            for (int s : shown)
                if (std::find(named.begin(), named.end(), s) == named.end()) { made.params.push_back(s); made.labels.push_back(""); }
        made.picture = page.picture;
        for (const auto& part : page.parts) {
            int i = lookup(part);
            if (i < 0) missing(part);
            made.picture_params.push_back(i);
        }
        out.push_back(std::move(made));
    }
    return out;
}

} // namespace melodee
