// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files (shell/file_finder.h).
#include "melodee/shell/file_finder.h"
#include "melodee/model/card_folders.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <tuple>

namespace melodee {

namespace fs = std::filesystem;

std::string fold_case(std::string text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 'A' && c <= 'Z') { text[i] = char(c + 32); continue; }
        if (c < 0xc3 || c > 0xc5 || i + 1 >= text.size()) continue;
        const auto d = static_cast<unsigned char>(text[i + 1]);
        if ((d & 0xc0) != 0x80) continue;
        if (c == 0xc3) { // Latin-1: U+00C0-U+00DE but the multiplication sign
            if (d <= 0x9e && d != 0x97) text[i + 1] = char(d + 0x20);
            ++i;
            continue;
        }
        const unsigned cp = (unsigned(c & 0x1f) << 6) | (d & 0x3f); // Latin Extended-A: pairs, the capital first
        unsigned low = cp;
        if (cp <= 0x137) low = cp & 1 ? cp : cp + 1;
        else if (cp >= 0x139 && cp <= 0x148) low = cp & 1 ? cp + 1 : cp;
        else if (cp >= 0x14a && cp <= 0x177) low = cp & 1 ? cp : cp + 1;
        else if (cp == 0x178) low = 0xff; // Y with diaeresis: its small one is Latin-1's
        else if (cp >= 0x179 && cp <= 0x17e) low = cp & 1 ? cp + 1 : cp;
        if (low != cp) { text[i] = char(0xc0 | (low >> 6)); text[i + 1] = char(0x80 | (low & 0x3f)); }
        ++i;
    }
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

// What the index holds: what a pad plays (shell/sample_library.cpp is_sample_file), the instruments a slot can name
// (.sfz, MPC programs, EXS24, SoundFonts, DecentSampler presets) and MPC projects (a pad of one, "<x>.xpj#...").
bool indexed_kind(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    const std::string kind = fold_case(name.substr(dot));
    for (const char* k : {".wav", ".mp3", ".flac", ".ogg", ".oga", ".aif", ".aiff", ".sfz", ".xpm", ".xty", ".xpj", ".exs", ".sf2", ".dspreset"})
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

// Two files of one size are one file copied when their first and last 4 KB are the same too.
bool same_ends(const std::string& a, const std::string& b, uint64_t bytes) {
    constexpr uint64_t piece = 4096;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    char ba[piece], bb[piece];
    for (uint64_t from : {uint64_t(0), bytes > piece ? bytes - piece : uint64_t(0)}) {
        const auto count = std::streamsize(std::min(piece, bytes));
        fa.seekg(std::streamoff(from)); fb.seekg(std::streamoff(from));
        if (!fa.read(ba, count) || !fb.read(bb, count) || std::memcmp(ba, bb, size_t(count))) return false;
    }
    return true;
}

uint32_t name_hash(const std::string& folded) { // FNV-1a
    uint32_t h = 2166136261u;
    for (unsigned char c : folded) { h ^= c; h *= 16777619u; }
    return h;
}
// Room for one more, a quarter more at a time (not twice: the index's lists are its memory).
template <typename V> void room(V& v, size_t more = 1) {
    if (v.size() + more > v.capacity()) v.reserve(std::max(v.size() + more, v.capacity() + v.capacity() / 4 + 256));
}

} // namespace

// ---- the index

void CardIndex::clear() {
    std::vector<char>().swap(pool_);
    std::vector<Dir>().swap(dirs_);
    std::vector<Entry>().swap(entries_);
    std::vector<uint32_t>().swap(buckets_);
    std::vector<Walk>().swap(stack_);
    watched_.clear();
    slot_seen_.clear();
    matched_ = 0;
    card_.clear();
    looked_ = 0;
    walking_ = complete_ = too_big_ = false;
}

uint32_t CardIndex::keep(const std::string& text) {
    room(pool_, text.size() + 1);
    const auto offset = uint32_t(pool_.size());
    pool_.insert(pool_.end(), text.begin(), text.end());
    pool_.push_back('\0');
    return offset;
}

uint32_t CardIndex::add_folder(uint32_t parent, const std::string& name) {
    room(dirs_);
    dirs_.push_back({parent, keep(name)});
    return uint32_t(dirs_.size() - 1);
}

void CardIndex::rehash(size_t count) {
    buckets_.assign(count, none);
    for (uint32_t e = 0; e < entries_.size(); ++e) {
        auto& b = buckets_[name_hash(fold_case(at(entries_[e].name))) & (count - 1)];
        entries_[e].next = b;
        b = e;
    }
}

void CardIndex::add_file(uint32_t folder, const std::string& name) {
    const std::string key = fold_case(name);
    room(entries_);
    if (entries_.size() + 1 > buckets_.size() * 2) rehash(std::max<size_t>(1024, buckets_.size() * 2)); // two a bucket at most
    auto& bucket = buckets_[name_hash(key) & (buckets_.size() - 1)];
    entries_.push_back({keep(name), folder, bucket});
    bucket = uint32_t(entries_.size() - 1);
    if (watched_.empty()) return;
    if (auto it = watched_.find(key); it != watched_.end())
        for (uint32_t s : it->second) if (!slot_seen_[s]) { slot_seen_[s] = true; ++matched_; }
}

std::string CardIndex::dir_path(uint32_t folder) const {
    uint32_t chain[64];
    unsigned n = 0;
    for (uint32_t f = folder; f != none && n < 64; f = dirs_[f].parent) chain[n++] = f;
    std::string path;
    while (n) {
        if (!path.empty()) path += '/';
        path += at(dirs_[chain[--n]].name);
    }
    return path;
}

void CardIndex::begin(const std::string& card_top, const std::vector<std::string>& internal) {
    clear();
    card_ = card_top;
    // A stack: the card's top last in, so it and all under it are walked first, then each internal folder.
    for (auto it = internal.rbegin(); it != internal.rend(); ++it)
        if (!it->empty() && *it != card_top) stack_.push_back({add_folder(none, *it), 0, false, true});
    if (!card_top.empty()) stack_.push_back({add_folder(none, card_top), 0, true, true});
    walking_ = true;
}

void CardIndex::watch(const std::vector<std::string>& saved) {
    watched_.clear();
    slot_seen_.assign(saved.size(), false);
    matched_ = 0;
    for (size_t s = 0; s < saved.size(); ++s) {
        const std::string name = fs::path(split_saved(saved[s]).file).filename().string();
        watched_[fold_case(name)].push_back(uint32_t(s));
        if (!has_extension(name)) for (const auto& ext : sample_extensions()) watched_[fold_case(name + ext)].push_back(uint32_t(s));
    }
    if (buckets_.empty()) return;
    for (const auto& [key, slots] : watched_) // the files the index holds already count at once
        for (uint32_t e = buckets_[name_hash(key) & (buckets_.size() - 1)]; e != none; e = entries_[e].next) {
            if (fold_case(at(entries_[e].name)) != key) continue;
            for (uint32_t s : slots) if (!slot_seen_[s]) { slot_seen_[s] = true; ++matched_; }
            break;
        }
}

CardIndex::Step CardIndex::step() {
    if (!walking_) return complete_ ? Step::Done : too_big_ ? Step::TooBig : Step::More;
    if (stack_.empty()) { walking_ = false; complete_ = true; return Step::Done; }
    const Walk walk = stack_.back();
    stack_.pop_back();
    const std::string path = dir_path(walk.folder);
    const bool melodee = !card_.empty() && path == card_ + "/Melodee";
    std::error_code error;
    fs::directory_iterator it(path, error), end;
    if (error) {
        // An internal folder this MPC has not, or one not to be read: passed over. Anything else (a card pulled out, a
        // read error): the walk cannot say what is on the card.
        const bool absent = error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
        if (error == std::errc::permission_denied || (walk.top && !walk.card_top && absent)) {
            if (stack_.empty()) { walking_ = false; complete_ = true; return Step::Done; }
            return Step::More;
        }
        clear();
        return Step::Failed;
    }
    std::vector<Walk> under;
    unsigned seen = 0;
    for (; !error && it != end; it.increment(error)) {
        if (abort && (++seen & 63) == 0 && abort->load(std::memory_order_relaxed)) return Step::Stopped;
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue; // hidden: ._ twins, .Trashes, .Spotlight-V100
        ++looked_;
        std::error_code e;
        const auto own = it->symlink_status(e);
        if (e) continue;
        if (fs::is_directory(own)) { // a folder of its own (one linked elsewhere is not walked: no loops)
            if (name == "System Volume Information" || name == "lost+found") continue;
            if (walk.card_top && name == "autorun") continue; // the boot folder
            if (melodee && (name == "Cache" || name == "Crashes" || name == "Plugins")) continue;
            if (walk.depth + 1 > depth_limit) continue; // deeper than any kit: left out
            under.push_back({add_folder(walk.folder, name), walk.depth + 1, false, false});
            continue;
        }
        if (!indexed_kind(name)) continue;
        if (!it->is_regular_file(e) || e) continue; // follows a link to a file
        if (entries_.size() >= file_limit) { // as far as it goes: kept, and said
            walking_ = false;
            too_big_ = true;
            std::vector<Walk>().swap(stack_);
            return Step::TooBig;
        }
        add_file(walk.folder, name);
    }
    if (error) { clear(); return Step::Failed; }
    // In name order, the first walked first: the same card is walked the same way each time.
    std::sort(under.begin(), under.end(), [&](const Walk& a, const Walk& b) { return std::strcmp(at(dirs_[a.folder].name), at(dirs_[b.folder].name)) > 0; });
    room(stack_, under.size());
    for (const auto& w : under) stack_.push_back(w);
    if (stack_.empty()) { walking_ = false; complete_ = true; return Step::Done; }
    return Step::More;
}

std::vector<CardIndex::File> CardIndex::files(const std::string& name) const {
    std::vector<File> out;
    if (buckets_.empty()) return out;
    const std::string key = fold_case(name);
    for (uint32_t e = buckets_[name_hash(key) & (buckets_.size() - 1)]; e != none; e = entries_[e].next) {
        if (fold_case(at(entries_[e].name)) != key) continue;
        const std::string path = dir_path(entries_[e].folder) + "/" + at(entries_[e].name);
        std::error_code error;
        if (!fs::is_regular_file(path, error)) continue; // gone since the walk
        const auto bytes = fs::file_size(path, error);
        out.push_back({path, error ? 0 : uint64_t(bytes)});
    }
    return out;
}

size_t CardIndex::bytes_used() const {
    return pool_.capacity() + dirs_.capacity() * sizeof(Dir) + entries_.capacity() * sizeof(Entry) + buckets_.capacity() * sizeof(uint32_t)
         + stack_.capacity() * sizeof(Walk);
}

// ---- finding

FolderMove folder_move(const std::string& saved, const std::string& found) {
    fs::path from = fs::path(split_saved(saved).file).parent_path(), to = fs::path(split_saved(found).file).parent_path();
    while (from.has_filename() && to.has_filename() && from.parent_path() != from && to.parent_path() != to
           && fold_case(from.filename().string()) == fold_case(to.filename().string())) {
        from = from.parent_path();
        to = to.parent_path();
    }
    return {from.string(), to.string()};
}

FoundFile pick_by_name(const std::string& saved, const std::string& card_top, const std::string& project_file,
                       std::vector<CardIndex::File> files, bool partial) {
    FoundFile none;
    const auto split = split_saved(saved);
    const auto saved_up = folders_up(fs::path(split.file));
    const std::string project_dir = project_file.empty() ? std::string() : fs::path(project_file).parent_path().string();
    const std::string samples = samples_folder(card_top);
    using Key = std::tuple<unsigned, bool, bool>;
    auto key = [&](const CardIndex::File& f) {
        return Key {trailing_shared(saved_up, folders_up(fs::path(f.path))), inside(f.path, project_dir), inside(f.path, samples)};
    };
    if (partial) // part of a card: only one that is plainly it (a better one may be where the walk did not reach)
        files.erase(std::remove_if(files.begin(), files.end(), [&](const CardIndex::File& f) {
            const Key k = key(f);
            return std::get<0>(k) == 0 && !std::get<1>(k);
        }), files.end());
    if (files.empty()) return none;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    files.erase(std::unique(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.path == b.path; }), files.end());
    Key best = key(files[0]);
    for (const auto& f : files) best = std::max(best, key(f));
    std::vector<const CardIndex::File*> tied;
    for (const auto& f : files) if (key(f) == best) tied.push_back(&f);
    for (size_t n = 1; n < tied.size(); ++n) // two different files: left missing (decision 1)
        if (tied[n]->bytes != tied[0]->bytes || !same_ends(tied[n]->path, tied[0]->path, tied[0]->bytes)) { none.tie = true; return none; }
    return {tied[0]->path + split.part, 5, false}; // one, or copies of one file: the first by path
}

FoundFile find_moved_sample(const std::string& saved, const std::string& card_top, const std::string& project_file,
                            const std::vector<FolderMove>& moves, const CardIndex* index) {
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
    // 4. With the rest of the kit: where its own folder went, as the others found say (the nearest move that holds it).
    {
        const std::string dir = file.parent_path().string();
        const FolderMove* best = nullptr;
        for (const auto& m : moves)
            if ((dir == m.from || inside(dir, m.from)) && (!best || m.from.size() > best->from.size())) best = &m;
        if (best)
            if (std::string at = try_name(best->to + dir.substr(best->from.size()), name); !at.empty()) return found(at, 4);
    }
    if (!index) return out;
    // 5. By name, anywhere (the files still there: one deleted since the index was made is not named).
    std::vector<CardIndex::File> files = index->files(name);
    if (!has_extension(name))
        for (const auto& ext : sample_extensions()) { auto more = index->files(name + ext); files.insert(files.end(), more.begin(), more.end()); }
    return pick_by_name(saved, card_top, project_file, std::move(files), !index->complete());
}

std::vector<std::string> find_moved_plugins(const std::string& saved, const std::string& card_top) {
    std::vector<std::string> out;
    if (saved.empty() || saved[0] != '/' || card_top.empty()) return out;
    std::error_code e;
    // The card renamed: the saved path under the card's top now, and its old Plugins/ in Melodee/Plugins/.
    if (std::string swapped = renamed(saved, card_top); !swapped.empty()) {
        if (fs::exists(swapped, e)) out.push_back(swapped);
        const fs::path inside_old = fs::path(swapped).lexically_relative(fs::path(card_top) / "Plugins");
        if (!inside_old.empty() && inside_old != "." && *inside_old.begin() != "..") {
            const fs::path moved = fs::path(card_top) / "Melodee" / "Plugins" / inside_old;
            if (fs::exists(moved, e)) out.push_back(moved.string());
        }
    }
    // By name in Melodee/Plugins, four folders down at most.
    const fs::path root = fs::path(card_top) / "Melodee" / "Plugins";
    const std::string want = fold_case(fs::path(saved).filename().string());
    if (want.empty()) return out;
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
    std::sort(found.begin(), found.end(), [&](const std::string& a, const std::string& b) {
        const unsigned ta = trailing_shared(saved_up, folders_up(fs::path(a))), tb = trailing_shared(saved_up, folders_up(fs::path(b)));
        if (ta != tb) return ta > tb;
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });
    for (auto& f : found) if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(std::move(f));
    return out;
}

} // namespace melodee
