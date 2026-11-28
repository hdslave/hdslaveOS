// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files (shell/file_finder.h).
#include "melodee/shell/file_finder.h"
#include "melodee/model/card_folders.h"
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <tuple>

namespace melodee {

namespace fs = std::filesystem;

std::string fold_case(std::string text) {
    for (auto& ch : text) if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
    return text;
}

SavedPath split_saved(const std::string& saved) {
    const size_t mark = saved.rfind('#');
    if (mark == std::string::npos || mark < 4 || mark + 1 >= saved.size() || saved[mark - 4] != '.'
        || saved.find('/', mark) != std::string::npos)
        return {saved, {}};
    const std::string kind = fold_case(saved.substr(mark - 3, 3));
    if (kind != "xpm" && kind != "xty" && kind != "xpj" && kind != "sf2") return {saved, {}};
    return {saved.substr(0, mark), saved.substr(mark)};
}

const std::vector<std::string>& sample_extensions() {
    static const std::vector<std::string> list {".wav", ".WAV", ".aif", ".aiff", ".flac", ".mp3", ".ogg"};
    return list;
}

namespace {

// What the index holds: what a pad plays (shell/sample_library.cpp is_sample_file) and the instruments a slot can
// name (.sfz, MPC programs, EXS24, SoundFonts, DecentSampler presets).
bool indexed_kind(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    const std::string kind = fold_case(name.substr(dot));
    for (const char* k : {".wav", ".mp3", ".flac", ".ogg", ".oga", ".aif", ".aiff", ".sfz", ".xpm", ".xty", ".exs", ".sf2", ".dspreset"})
        if (kind == k) return true;
    return false;
}
// A name with an extension a sample or an instrument has: tried as it is; else with each of sample_extensions().
bool has_extension(const std::string& name) { return indexed_kind(name); }

bool is_file(const fs::path& path) {
    std::error_code e;
    return fs::is_regular_file(path, e); // follows a link: a linked sample is a sample
}

// `name` in `dir`: as it is, then (no extension) with each extension. Empty: none there.
std::string try_name(const fs::path& dir, const std::string& name) {
    if (dir.empty() || name.empty()) return {};
    if (is_file(dir / name)) return (dir / name).string();
    if (has_extension(name)) return {};
    for (const auto& ext : sample_extensions())
        if (is_file(dir / (name + ext))) return (dir / (name + ext)).string();
    return {};
}
// A whole path, the same way.
std::string try_path(const fs::path& path) { return try_name(path.parent_path(), path.filename().string()); }

// The saved path under the card's top now, where it names another volume beside it: the card mounted as
// /media/<id> is the same card under another <id> (another volume name, another card reader). Empty: not on a volume
// beside the card's, or on the card itself.
std::string renamed(const std::string& saved, const std::string& card_top) {
    const fs::path top(card_top);
    const std::string mounts = top.parent_path().string(); // /media on the MPC
    if (card_top.empty() || mounts.empty() || mounts == "/" || saved.size() <= mounts.size() + 1
        || saved.compare(0, mounts.size(), mounts) != 0 || saved[mounts.size()] != '/')
        return {};
    const size_t end = saved.find('/', mounts.size() + 1);
    if (end == std::string::npos) return {};
    const std::string volume = saved.substr(0, end);
    if (volume == card_top) return {};
    return card_top + saved.substr(end);
}

// The parent folders' names, lower case, the nearest first.
std::vector<std::string> folders_up(const fs::path& file) {
    std::vector<std::string> out;
    for (fs::path at = file.parent_path(); !at.empty() && at.has_filename(); at = at.parent_path()) out.push_back(fold_case(at.filename().string()));
    return out;
}
unsigned trailing_shared(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    unsigned n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n]) ++n;
    return n;
}
bool inside(const std::string& path, const std::string& folder) {
    return !folder.empty() && path.size() > folder.size() + 1 && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/';
}

} // namespace

void CardIndex::clear() {
    stack_.clear(); dirs_.clear(); names_.clear(); seen_.clear(); watched_.clear();
    card_.clear();
    looked_ = folders_seen_ = 0;
    walking_ = complete_ = false;
}

void CardIndex::begin(const std::string& card_top, const std::vector<std::string>& internal) {
    clear();
    card_ = card_top;
    // A stack: the card's top last in, so it and all under it are walked first, then each internal folder.
    for (auto it = internal.rbegin(); it != internal.rend(); ++it)
        if (!it->empty() && *it != card_top) stack_.push_back({*it, 0, false});
    if (!card_top.empty()) stack_.push_back({card_top, 0, true});
    walking_ = true;
}

void CardIndex::watch(const std::vector<std::string>& names) {
    watched_.clear();
    seen_.clear();
    for (const auto& n : names) {
        const auto split = split_saved(n);
        const std::string name = fs::path(split.file).filename().string();
        watched_.insert(fold_case(name));
        if (!has_extension(name)) for (const auto& ext : sample_extensions()) watched_.insert(fold_case(name + ext));
    }
    // Files the index holds already count at once.
    for (const auto& w : watched_) if (names_.count(w)) seen_.insert(w);
}

CardIndex::Step CardIndex::step() {
    if (!walking_) return complete_ ? Step::Done : Step::More;
    if (stack_.empty()) { walking_ = false; complete_ = true; return Step::Done; }
    const Dir dir = stack_.back();
    stack_.pop_back();
    ++folders_seen_;
    const uint32_t folder = uint32_t(dirs_.size());
    dirs_.push_back(dir.path);
    const bool melodee = !card_.empty() && dir.path == card_ + "/Melodee";
    std::error_code error;
    std::vector<Dir> under;
    for (fs::directory_iterator it(dir.path, error), end; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue; // hidden: ._ twins, .Trashes, .Spotlight-V100
        if (++looked_ > entry_limit) { clear(); return Step::TooBig; }
        std::error_code e;
        const auto own = it->symlink_status(e);
        if (e) continue;
        if (fs::is_directory(own)) { // a folder of its own (one linked elsewhere is not walked: no loops)
            if (name == "System Volume Information" || name == "lost+found") continue;
            if (dir.card_top && name == "autorun") continue; // the boot folder
            if (melodee && (name == "Cache" || name == "Crashes" || name == "Plugins")) continue;
            if (dir.depth + 1 > depth_limit) { clear(); return Step::TooBig; } // deeper than it looks: stopped, as a cancel is
            under.push_back({it->path().string(), dir.depth + 1, false});
            continue;
        }
        if (!indexed_kind(name)) continue;
        if (!it->is_regular_file(e) || e) continue; // follows a link to a file
        const uint64_t bytes = it->file_size(e);
        const std::string key = fold_case(name);
        names_[key].push_back({folder, name, e ? 0 : bytes});
        if (watched_.count(key)) seen_.insert(key);
    }
    // In name order, the first walked first: the same card is walked the same way each time.
    std::sort(under.begin(), under.end(), [](const Dir& a, const Dir& b) { return a.path > b.path; });
    for (auto& d : under) stack_.push_back(std::move(d));
    if (stack_.empty()) { walking_ = false; complete_ = true; return Step::Done; }
    return Step::More;
}

std::vector<CardIndex::File> CardIndex::files(const std::string& name) const {
    std::vector<File> out;
    auto it = names_.find(fold_case(name));
    if (it == names_.end()) return out;
    for (const auto& e : it->second) out.push_back({dirs_[e.folder] + "/" + e.name, e.bytes});
    return out;
}

FoundFile pick_by_name(const std::string& saved, const std::string& card_top, const std::string& project_file,
                       std::vector<CardIndex::File> files) {
    FoundFile none;
    if (files.empty()) return none;
    const auto split = split_saved(saved);
    const auto saved_up = folders_up(fs::path(split.file));
    const std::string project_dir = project_file.empty() ? std::string() : fs::path(project_file).parent_path().string();
    const std::string samples = samples_folder(card_top);
    using Key = std::tuple<unsigned, bool, bool>;
    auto key = [&](const CardIndex::File& f) {
        return Key {trailing_shared(saved_up, folders_up(fs::path(f.path))), inside(f.path, project_dir), inside(f.path, samples)};
    };
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    files.erase(std::unique(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.path == b.path; }), files.end());
    Key best = key(files[0]);
    for (const auto& f : files) best = std::max(best, key(f));
    std::vector<const CardIndex::File*> tied;
    for (const auto& f : files) if (key(f) == best) tied.push_back(&f);
    for (const auto* f : tied)
        if (f->bytes != tied[0]->bytes) { none.tie = true; return none; } // two different files: left missing (decision 1)
    return {tied[0]->path + split.part, 5, false}; // one, or copies of one file: the first by path
}

FoundFile find_moved_sample(const std::string& saved, const std::string& card_top, const std::string& project_file,
                            const std::vector<std::string>& kit_folders, const CardIndex* index) {
    FoundFile out;
    if (saved.empty() || saved[0] != '/') return out;
    const auto split = split_saved(saved);
    const fs::path file(split.file);
    const std::string name = file.filename().string();
    auto found = [&](const std::string& path, uint8_t step) { return FoundFile {path + split.part, step, false}; };
    // 1. Upstream's repair (model/card_folders.h): Samples/ at the card's top into Melodee/Samples/ (with the
    // extensions tried, for a name saved without one).
    if (std::string now = card_sample_path(card_top, split.file); now != split.file && is_file(now)) return found(now, 1);
    if (!card_top.empty()) {
        const fs::path inside_old = file.lexically_relative(old_samples_folder(card_top));
        if (!inside_old.empty() && inside_old != "." && *inside_old.begin() != "..")
            if (std::string at = try_path(fs::path(samples_folder(card_top)) / inside_old); !at.empty()) return found(at, 1);
    }
    // 2. The card under another name; and on it, the samples moved into Melodee/Samples since.
    if (std::string swapped = renamed(split.file, card_top); !swapped.empty()) {
        if (std::string at = try_path(swapped); !at.empty()) return found(at, 2);
        if (std::string now = card_sample_path(card_top, swapped); now != swapped)
            if (std::string at = try_path(now); !at.empty()) return found(at, 2);
    }
    if (std::string at = try_path(file); !at.empty()) return found(at, 3); // only its extension was left off
    // 3. Beside the project: its folder moved with its samples.
    if (!project_file.empty()) {
        const fs::path project(project_file), dir = project.parent_path();
        const std::string last = file.parent_path().filename().string();
        if (!last.empty())
            if (std::string at = try_name(dir / last, name); !at.empty()) return found(at, 3);
        if (std::string at = try_name(dir, name); !at.empty()) return found(at, 3);
        if (fold_case(project.extension().string()) == ".xpj")
            if (std::string at = try_name(dir / (project.stem().string() + "_[ProjectData]"), name); !at.empty()) return found(at, 3);
    }
    // 4. With the rest of the kit.
    for (const auto& folder : kit_folders)
        if (std::string at = try_name(folder, name); !at.empty()) return found(at, 4);
    if (!index) return out;
    // 5. By name, anywhere (the files still there: one deleted since the index was made is not named).
    std::vector<CardIndex::File> files = index->files(name);
    if (!has_extension(name))
        for (const auto& ext : sample_extensions()) { auto more = index->files(name + ext); files.insert(files.end(), more.begin(), more.end()); }
    files.erase(std::remove_if(files.begin(), files.end(), [](const CardIndex::File& f) { return !is_file(f.path); }), files.end());
    return pick_by_name(saved, card_top, project_file, std::move(files));
}

std::string find_moved_plugin(const std::string& saved, const std::string& card_top) {
    if (saved.empty() || saved[0] != '/' || card_top.empty()) return {};
    std::error_code e;
    // The card renamed: the saved path under the card's top now, and its old Plugins/ in Melodee/Plugins/.
    if (std::string swapped = renamed(saved, card_top); !swapped.empty()) {
        if (fs::exists(swapped, e)) return swapped;
        const fs::path inside_old = fs::path(swapped).lexically_relative(fs::path(card_top) / "Plugins");
        if (!inside_old.empty() && inside_old != "." && *inside_old.begin() != "..") {
            const fs::path moved = fs::path(card_top) / "Melodee" / "Plugins" / inside_old;
            if (fs::exists(moved, e)) return moved.string();
        }
    }
    // By name in Melodee/Plugins, four folders down at most.
    const fs::path root = fs::path(card_top) / "Melodee" / "Plugins";
    const std::string want = fold_case(fs::path(saved).filename().string());
    if (want.empty()) return {};
    const auto saved_up = folders_up(fs::path(saved));
    std::vector<std::string> found;
    std::vector<std::pair<fs::path, unsigned>> stack {{root, 0}};
    size_t looked = 0;
    while (!stack.empty() && looked < 20000) {
        auto [dir, depth] = stack.back();
        stack.pop_back();
        for (fs::directory_iterator it(dir, e), end; !e && it != end; it.increment(e)) {
            ++looked;
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            if (fold_case(name) == want) { found.push_back(it->path().string()); continue; } // a bundle is not looked into
            std::error_code s;
            const auto own = it->symlink_status(s);
            const std::string kind = fold_case(it->path().extension().string());
            if (!s && fs::is_directory(own) && depth + 1 < 4 && kind != ".vst3" && kind != ".lv2" && kind != ".clap")
                stack.push_back({it->path(), depth + 1});
        }
        e.clear();
    }
    if (found.empty()) return {};
    std::sort(found.begin(), found.end(), [&](const std::string& a, const std::string& b) {
        const unsigned ta = trailing_shared(saved_up, folders_up(fs::path(a))), tb = trailing_shared(saved_up, folders_up(fs::path(b)));
        if (ta != tb) return ta > tb;
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });
    return found.front();
}

} // namespace melodee
