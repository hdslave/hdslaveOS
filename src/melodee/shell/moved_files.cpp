// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files (shell/moved_files.h).
#include "melodee/shell/moved_files.h"
#include <algorithm>
#include <filesystem>
#include <iterator>
#include <system_error>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace melodee {

namespace fs = std::filesystem;

namespace {
// The finder's thread at the lowest CPU priority. It is started from the MIDI thread, which runs real time
// (SCHED_FIFO): a new thread takes its policy, so the usual one first, then the lowest nice. Elsewhere (a Mac host) as
// it is.
long low_priority() {
#if defined(__linux__)
    sched_param usual {};
    usual.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &usual);
    const long tid = long(syscall(SYS_gettid));
    setpriority(PRIO_PROCESS, id_t(tid), 19);
    return tid;
#else
    return 0;
#endif
}
// Its walk at the idle disk class (a long sample streaming from the card is read first); its checks, a few file
// looks, at the usual best-effort class, so they come at once.
void disk_class(bool idle) {
#if defined(__linux__) && defined(SYS_ioprio_set)
    constexpr int who_process = 1, class_shift = 13, best_effort = 2, idle_class = 3; // linux/ioprio.h
    syscall(SYS_ioprio_set, who_process, 0, idle ? idle_class << class_shift : best_effort << class_shift | 4); // 0: this thread
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
        stop_ = true;
    }
    stopping_.store(true, std::memory_order_relaxed);
    interrupt_.store(true, std::memory_order_relaxed);
    wake.notify_all();
    if (thread.joinable()) thread.join();
}

void MovedFiles::wake_up() {
    { std::lock_guard<std::mutex> guard(lock); }
    wake.notify_all();
}

void MovedFiles::post(Job job) {
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!started_) {
            started_ = true;
            index.file_limit = file_limit;
            index.abort = &interrupt_;
            thread = std::thread(&MovedFiles::run, this);
        }
        if (job.search) search_wanted = std::make_unique<Job>(std::move(job)); // the one waiting, if any, goes below
        else checks_.push_back(std::move(job));
    }
    wake.notify_all();
}

void MovedFiles::cancel() {
    std::unique_ptr<Job> unbegun; // let go of outside the lock
    {
        std::lock_guard<std::mutex> guard(lock);
        if (search_wanted) {
            dropped_search_ = search_wanted->generation + 1; // answered by the thread: cancelled, nothing found
            unbegun = std::move(search_wanted);
        }
        cancel_.store(true, std::memory_order_relaxed);
        interrupt_.store(true, std::memory_order_relaxed);
    }
    wake.notify_all();
}

void MovedFiles::drop_index() {
    drop_.store(true, std::memory_order_relaxed);
    indexed_.store(false, std::memory_order_relaxed);
    wake_up();
}

void MovedFiles::hold(bool away) {
    if (held_.exchange(away) == away) return;
    if (away) cancel(); // sets interrupt_ too: a walk stops within a folder
    else if (!stopping()) interrupt_.store(false, std::memory_order_relaxed);
    wake_up();
}

bool MovedFiles::parked() const {
    std::lock_guard<std::mutex> guard(lock);
    return !busy;
}

bool MovedFiles::take(Answer& out) {
    if (taken_at_ >= taken_.size()) {
        taken_.clear(); // its room kept: what the thread fills next
        taken_at_ = 0;
        std::lock_guard<std::mutex> guard(lock);
        taken_.swap(mailbox_);
    }
    if (taken_at_ >= taken_.size()) return false;
    out = std::move(taken_[taken_at_++]);
    return true;
}

bool MovedFiles::idle() const {
    std::lock_guard<std::mutex> guard(lock);
    return checks_.empty() && !search_wanted && mailbox_.empty() && !busy && !dropped_search_ && taken_at_ >= taken_.size();
}

bool MovedFiles::publish() {
    if (outbox_.empty()) return true;
    std::lock_guard<std::mutex> guard(lock);
    if (!mailbox_.empty()) return false; // the taker has not emptied it yet: again later, nothing allocated here
    mailbox_.swap(outbox_);
    return true;
}

void MovedFiles::idle_disk(bool idle) {
    if (idle == disk_idle) return;
    disk_idle = idle;
    disk_class(idle);
}

MovedFiles::Answer MovedFiles::found(const Job& job, const Slot& s, const std::string& path) {
    if (path.size() >= path_room) return missing(job, s, false); // a slot cannot name it
    Answer a;
    a.kind = Answer::Kind::Found;
    a.generation = job.generation;
    a.slot = s.slot;
    a.saved = s.saved;
    a.found = path;
    const std::string file = split_saved(path).file;
    if (read_chop) a.chop = read_chop(file);
    if (read_rate) a.rate = read_rate(file);
    if (path != s.saved) {
        auto move = folder_move(s.saved, path);
        if (std::none_of(moves_.begin(), moves_.end(), [&](const FolderMove& m) { return m.from == move.from && m.to == move.to; }))
            moves_.push_back(std::move(move));
    }
    return a;
}

MovedFiles::Answer MovedFiles::missing(const Job& job, const Slot& s, bool tie) {
    Answer m;
    m.kind = Answer::Kind::Missing;
    m.generation = job.generation; m.slot = s.slot; m.saved = s.saved; m.tie = tie;
    return m;
}

bool MovedFiles::check(const Job& job) {
    if (stale(job)) return true;
    idle_disk(false);
    if (moves_generation != job.generation) { moves_.clear(); moves_generation = job.generation; }
    if ((index.complete() || index.too_big()) && index.card() != job.card) { index.clear(); indexed_.store(false, std::memory_order_relaxed); } // another card
    // A complete index answers what the quick steps do not; one stopped at the card's size, only what it plainly holds.
    const bool use = job.use_index && (index.complete() || index.too_big());
    Answer done;
    done.generation = job.generation;
    std::vector<size_t> left;
    std::vector<Answer> out; // this job's, given only once it is all done (the card lent meanwhile: again from the start)
    for (size_t n = 0; n < job.slots.size(); ++n) {
        if (stale(job) || stopping()) return true;
        if (held()) return false;
        const auto& s = job.slots[n];
        if (there(s.saved)) { // in place: nothing to say (the project plays as ever); a quiet check's loads again
            if (job.quiet) { out.push_back(found(job, s, s.saved)); ++done.found_count; }
            continue;
        }
        looked_for_.fetch_add(1, std::memory_order_relaxed);
        auto r = find_moved_sample(s.saved, job.card, job.origin, moves_, nullptr);
        if (r.path.empty()) { left.push_back(n); continue; }
        out.push_back(found(job, s, r.path));
        done.found_count += out.back().kind == Answer::Kind::Found;
        done.missing_count += out.back().kind == Answer::Kind::Missing;
    }
    // The rest of a kit, with every folder the others were found in.
    for (size_t n : left) {
        if (stale(job) || stopping()) return true;
        if (held()) return false;
        const auto& s = job.slots[n];
        auto r = find_moved_sample(s.saved, job.card, job.origin, moves_, use ? &index : nullptr);
        out.push_back(r.path.empty() ? missing(job, s, r.tie) : found(job, s, r.path));
        done.found_count += out.back().kind == Answer::Kind::Found;
        done.missing_count += out.back().kind == Answer::Kind::Missing;
    }
    done.indexed = use && index.complete();
    done.quiet = job.quiet;
    for (auto& a : out) answer(std::move(a));
    answer(std::move(done));
    return true;
}

void MovedFiles::finish_search(const Job& job, bool part) {
    if (stale(job)) return;
    idle_disk(false);
    if (moves_generation != job.generation) { moves_.clear(); moves_generation = job.generation; }
    Answer done;
    done.search = true;
    done.too_big = index.too_big();
    done.cancelled = part && !done.too_big;
    done.generation = job.generation;
    for (const auto& s : job.slots) {
        if (stale(job) || stopping()) return;
        if (held()) { done.cancelled = true; break; } // the card lent: nothing more is read
        FoundFile r;
        if (there(s.saved)) r.path = s.saved; // copied back where it was
        else r = find_moved_sample(s.saved, job.card, job.origin, moves_, &index);
        Answer a = r.path.empty() ? missing(job, s, r.tie) : found(job, s, r.path);
        if (a.kind == Answer::Kind::Missing && part) continue; // the part of the card walked cannot say it is missing
        done.found_count += a.kind == Answer::Kind::Found;
        done.missing_count += a.kind == Answer::Kind::Missing;
        answer(std::move(a));
    }
    answer(std::move(done));
}

void MovedFiles::watch() {
    std::vector<std::string> saved;
    if (searching) for (const auto& s : searching->slots) saved.push_back(s.saved);
    index.watch(saved);
}

void MovedFiles::start_walk() {
    index.begin(searching->card, searching->internal);
    walks_.fetch_add(1, std::memory_order_relaxed);
    watch();
}

void MovedFiles::run() {
    tid_.store(low_priority(), std::memory_order_relaxed);
    disk_class(false);
    for (;;) {
        std::unique_ptr<Job> next_search;
        uint32_t dropped = 0;
        {
            std::unique_lock<std::mutex> guard(lock);
            for (;;) {
                if (stop_) return;
                if (!outbox_.empty() && mailbox_.empty()) mailbox_.swap(outbox_);
                const bool work = (!held() && (!checks_.empty() || !paused_.empty() || search_wanted || searching || index.walking()))
                               || cancel_.load(std::memory_order_relaxed) || drop_.load(std::memory_order_relaxed) || dropped_search_;
                if (work) break;
                busy = !outbox_.empty(); // answers still to hand on: not idle
                working_.store(busy, std::memory_order_relaxed);
                if (outbox_.empty()) wake.wait(guard);
                else wake.wait_for(guard, std::chrono::milliseconds(5));
            }
            busy = true;
            working_.store(true, std::memory_order_relaxed);
            if (!held()) {
                inbox_.swap(checks_); // inbox_ is empty here: what it held is done or waits in paused_
                next_search = std::move(search_wanted);
            }
            dropped = dropped_search_;
            dropped_search_ = 0;
        }
        if (dropped) { // a search cancelled before it began
            Answer a;
            a.search = true; a.cancelled = true; a.generation = dropped - 1;
            answer(std::move(a));
        }
        if (drop_.exchange(false)) { // the card changed under the index: walked again when next wanted
            const bool was_walking = index.walking();
            index.clear();
            indexed_.store(false, std::memory_order_relaxed);
            walking_.store(false, std::memory_order_relaxed);
            if (was_walking && searching && !held()) start_walk();
        }
        if (next_search) { // a new search: instead of one waiting for the walk
            searching = std::move(next_search);
            const bool other_card = (index.complete() || index.walking() || index.too_big()) && index.card() != searching->card;
            // FIND MISSING walks anew; a card too big is not walked again this session (another card, or one back from
            // a computer, is).
            if (other_card || (searching->fresh && !index.too_big())) {
                index.clear();
                indexed_.store(false, std::memory_order_relaxed);
            }
            if (!index.complete() && !index.walking() && !index.too_big()) start_walk();
            else watch();
        }
        if (cancel_.exchange(false)) { // CANCEL, or the card lent: the walk ends here
            if (searching && !stale(*searching)) {
                if (!held() && (index.walking() || index.too_big())) finish_search(*searching, true); // what the part walked plainly found
                else {
                    Answer a;
                    a.search = true; a.cancelled = true; a.generation = searching->generation;
                    answer(std::move(a));
                }
            }
            searching.reset();
            if (index.walking()) index.clear(); // half built: it cannot say what is missing
            walking_.store(false, std::memory_order_relaxed);
            if (!held() && !stopping()) interrupt_.store(false, std::memory_order_relaxed);
        }
        if (stopping()) return;
        if (!held()) { // the checks the card's lending cut off first, then the new ones
            if (!paused_.empty()) {
                inbox_.insert(inbox_.begin(), std::make_move_iterator(paused_.begin()), std::make_move_iterator(paused_.end()));
                paused_.clear();
            }
            for (size_t n = 0; n < inbox_.size(); ++n) {
                if (stopping()) return;
                if (!check(inbox_[n])) { // lent meanwhile: this one and the rest wait for the card
                    std::move(inbox_.begin() + long(n), inbox_.end(), std::back_inserter(paused_));
                    break;
                }
            }
            inbox_.clear();
        }
        publish();
        if (held()) continue; // nothing of the card is read
        if (searching && stale(*searching)) searching.reset(); // another project opened: the walk goes on, for the index
        if (searching && !index.walking() && !index.complete() && !index.too_big()) start_walk();
        if (index.walking()) {
            walking_.store(true, std::memory_order_relaxed);
            idle_disk(true);
            const auto step = index.step();
            looked_.store(uint32_t(index.looked()), std::memory_order_relaxed);
            matched_.store(uint32_t(searching ? index.matched() : 0), std::memory_order_relaxed);
            if (step == CardIndex::Step::Stopped) { // a cancel, the card lent or the end: taken in at the top
                if (stopping()) return;
                continue;
            }
            if (step == CardIndex::Step::More) {
                publish();
                if (const unsigned us = folder_pause_us.load(std::memory_order_relaxed)) { // a slow card (tests): woken by what stops it
                    std::unique_lock<std::mutex> guard(lock);
                    wake.wait_for(guard, std::chrono::microseconds(us), [&] {
                        return stop_ || held() || cancel_.load(std::memory_order_relaxed) || !checks_.empty();
                    });
                } else std::this_thread::yield();
                continue;
            }
            walking_.store(false, std::memory_order_relaxed);
            if (step == CardIndex::Step::Failed) { // the card could not be read: nothing found, nothing said missing
                indexed_.store(false, std::memory_order_relaxed);
                if (searching) {
                    Answer a;
                    a.search = true; a.failed = true; a.generation = searching->generation;
                    answer(std::move(a));
                    searching.reset();
                }
                publish();
                continue;
            }
            indexed_.store(step == CardIndex::Step::Done, std::memory_order_relaxed);
        }
        if (searching && (index.complete() || index.too_big())) {
            matched_.store(uint32_t(index.matched()), std::memory_order_relaxed);
            finish_search(*searching, !index.complete());
            searching.reset();
        }
        publish();
    }
}

} // namespace melodee
