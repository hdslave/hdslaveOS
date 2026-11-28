// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files (shell/moved_files.h).
#include "melodee/shell/moved_files.h"
#include <algorithm>
#include <filesystem>
#include <system_error>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace melodee {

namespace fs = std::filesystem;

namespace {
// The finder's thread at the lowest CPU priority; its walk at the idle disk class (a long sample streaming from the card
// is read first), its checks, a few file looks, at the usual class so they come at once. Elsewhere (a Mac host) as it is.
void low_priority() {
#if defined(__linux__)
    setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 19);
#endif
}
void disk_class(bool idle) {
#if defined(__linux__) && defined(SYS_ioprio_set)
    constexpr int who_process = 1, class_idle = 3, class_shift = 13; // linux/ioprio.h: IOPRIO_WHO_PROCESS, IOPRIO_CLASS_IDLE
    syscall(SYS_ioprio_set, who_process, 0, idle ? class_idle << class_shift : 0); // 0: the calling thread; 0: by its CPU priority
#else
    (void)idle;
#endif
}
bool there(const std::string& saved) {
    std::error_code e;
    return fs::exists(split_saved(saved).file, e);
}
} // namespace

MovedFiles::~MovedFiles() {
    {
        std::lock_guard<std::mutex> guard(lock);
        stop = true;
    }
    wake.notify_all();
    if (thread.joinable()) thread.join();
}

void MovedFiles::cancel() {
    {
        std::lock_guard<std::mutex> guard(lock);
        if (search_wanted) { // not begun: it ends here
            Answer a;
            a.search = true; a.cancelled = true; a.generation = search_wanted->generation;
            answers.push_back(std::move(a));
            search_wanted.reset();
            return;
        }
        cancel_.store(true, std::memory_order_relaxed);
    }
    wake.notify_all();
}

void MovedFiles::idle_disk(bool idle) {
    if (idle == disk_idle) return;
    disk_idle = idle;
    disk_class(idle);
}

void MovedFiles::wake_up() {
    { std::lock_guard<std::mutex> guard(lock); }
    wake.notify_all();
}

void MovedFiles::post(Job job) {
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!started) { started = true; thread = std::thread(&MovedFiles::run, this); }
        if (job.search) search_wanted = std::make_unique<Job>(std::move(job));
        else checks.push_back(std::move(job));
    }
    wake.notify_all();
}

bool MovedFiles::take(Answer& out) {
    std::lock_guard<std::mutex> guard(lock);
    if (answers.empty()) return false;
    out = std::move(answers.front());
    answers.pop_front();
    return true;
}

bool MovedFiles::idle() const {
    std::lock_guard<std::mutex> guard(lock);
    return checks.empty() && !search_wanted && answers.empty() && !busy;
}

void MovedFiles::answer(Answer a) {
    std::lock_guard<std::mutex> guard(lock);
    answers.push_back(std::move(a));
}

MovedFiles::Answer MovedFiles::found(const Job& job, const Slot& s, const std::string& path) {
    Answer a;
    a.kind = Answer::Kind::Found;
    a.generation = job.generation;
    a.slot = s.slot;
    a.saved = s.saved;
    a.found = path;
    if (read_chop) a.chop = read_chop(split_saved(path).file);
    kit_folder(path);
    return a;
}

void MovedFiles::kit_folder(const std::string& path) {
    const std::string folder = fs::path(split_saved(path).file).parent_path().string();
    if (std::find(kit_folders.begin(), kit_folders.end(), folder) == kit_folders.end()) kit_folders.push_back(folder);
}

void MovedFiles::check(const Job& job) {
    if (stale(job)) return;
    idle_disk(false);
    if (kit_generation != job.generation) { kit_folders.clear(); kit_generation = job.generation; }
    if (index.complete() && index.card() != job.card) { index.clear(); indexed_.store(false, std::memory_order_relaxed); } // another card
    Answer done;
    done.generation = job.generation;
    std::vector<const Slot*> left;
    for (const auto& s : job.slots) {
        if (stale(job)) return;
        if (there(s.saved)) { // in place: nothing to say (the project plays as ever); a quiet check's loads again
            if (job.quiet) { answer(found(job, s, s.saved)); ++done.found_count; }
            continue;
        }
        looked_for_.fetch_add(1, std::memory_order_relaxed);
        auto r = find_moved_sample(s.saved, job.card, job.origin, kit_folders, nullptr);
        if (r.path.empty()) { left.push_back(&s); continue; }
        answer(found(job, s, r.path));
        ++done.found_count;
    }
    // The rest of a kit, with the folders the others were found in (all of them now).
    const bool use = job.use_index && index.complete();
    for (const Slot* s : left) {
        if (stale(job)) return;
        auto r = find_moved_sample(s->saved, job.card, job.origin, kit_folders, use ? &index : nullptr);
        if (!r.path.empty()) { answer(found(job, *s, r.path)); ++done.found_count; continue; }
        Answer m;
        m.kind = Answer::Kind::Missing;
        m.generation = job.generation; m.slot = s->slot; m.saved = s->saved; m.tie = r.tie;
        answer(std::move(m));
        ++done.missing_count;
    }
    done.indexed = use;
    done.quiet = job.quiet;
    answer(std::move(done));
}

void MovedFiles::finish_search(const Job& job, bool cancelled) {
    if (stale(job)) return;
    idle_disk(false);
    if (kit_generation != job.generation) { kit_folders.clear(); kit_generation = job.generation; }
    Answer done;
    done.search = true;
    done.cancelled = cancelled;
    done.generation = job.generation;
    for (const auto& s : job.slots) {
        if (stale(job)) return;
        FoundFile r;
        if (there(s.saved)) r.path = s.saved; // copied back where it was
        else r = find_moved_sample(s.saved, job.card, job.origin, kit_folders, &index);
        if (!r.path.empty()) { answer(found(job, s, r.path)); ++done.found_count; continue; }
        if (cancelled) continue; // the part walked cannot say what is missing
        Answer m;
        m.kind = Answer::Kind::Missing;
        m.generation = job.generation; m.slot = s.slot; m.saved = s.saved; m.tie = r.tie;
        answer(std::move(m));
        ++done.missing_count;
    }
    answer(std::move(done));
}

void MovedFiles::watch() {
    std::vector<std::string> names;
    if (searching) for (const auto& s : searching->slots) names.push_back(s.saved);
    index.watch(names);
}

void MovedFiles::start_walk() {
    index.begin(searching->card, searching->internal);
    walks_.fetch_add(1, std::memory_order_relaxed);
    watch();
}

void MovedFiles::run() {
    low_priority();
    for (;;) {
        Job job;
        bool have_check = false;
        std::unique_ptr<Job> next_search;
        {
            std::unique_lock<std::mutex> guard(lock);
            for (;;) {
                if (stop) return;
                if (!checks.empty() || search_wanted || searching || index.walking() || cancel_.load(std::memory_order_relaxed)
                    || drop_.load(std::memory_order_relaxed))
                    break;
                busy = false;
                working_.store(false, std::memory_order_relaxed);
                wake.wait(guard);
            }
            busy = true;
            working_.store(true, std::memory_order_relaxed);
            if (!checks.empty()) { job = std::move(checks.front()); checks.pop_front(); have_check = true; }
            else if (search_wanted) next_search = std::move(search_wanted);
        }
        if (drop_.exchange(false)) { // the card changed under the index: walked again when next wanted
            const bool was_walking = index.walking();
            index.clear();
            indexed_.store(false, std::memory_order_relaxed);
            if (was_walking && searching) start_walk();
        }
        if (next_search) { // a new search: instead of one waiting for the walk
            searching = std::move(next_search);
            if (searching->fresh || ((index.complete() || index.walking()) && index.card() != searching->card)) {
                index.clear();
                indexed_.store(false, std::memory_order_relaxed);
            }
            if (!index.complete() && !index.walking()) start_walk();
            else watch();
        }
        if (cancel_.exchange(false)) { // CANCEL: what the part walked found stays; a half-built index cannot say what is missing
            if (searching && index.walking() && !stale(*searching)) finish_search(*searching, true);
            else if (searching) {
                Answer a;
                a.search = true; a.cancelled = true; a.generation = searching->generation;
                answer(std::move(a));
            }
            searching.reset();
            if (index.walking()) index.clear();
            walking_.store(false, std::memory_order_relaxed);
        }
        if (have_check) { check(job); continue; } // a project just opened goes before the walk's next folder
        if (searching && stale(*searching)) searching.reset(); // another project opened: the walk goes on, for the index
        if (searching && !index.walking() && !index.complete()) start_walk();
        if (index.walking()) {
            walking_.store(true, std::memory_order_relaxed);
            idle_disk(true);
            const auto step = index.step();
            looked_.store(uint32_t(index.looked()), std::memory_order_relaxed);
            matched_.store(uint32_t(searching ? index.matched() : 0), std::memory_order_relaxed);
            if (step == CardIndex::Step::TooBig) { // past the limits: stopped as a cancel is, and said so
                walking_.store(false, std::memory_order_relaxed);
                indexed_.store(false, std::memory_order_relaxed);
                if (searching) {
                    Answer a;
                    a.search = true; a.too_big = true; a.generation = searching->generation;
                    answer(std::move(a));
                    searching.reset();
                }
                continue;
            }
            if (step == CardIndex::Step::More) {
                if (const unsigned us = folder_pause_us.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::microseconds(us));
                else std::this_thread::yield();
                continue;
            }
            walking_.store(false, std::memory_order_relaxed);
            indexed_.store(true, std::memory_order_relaxed);
        }
        if (searching && index.complete()) {
            matched_.store(uint32_t(index.matched()), std::memory_order_relaxed);
            finish_search(*searching, false);
            searching.reset();
        }
    }
}

} // namespace melodee
