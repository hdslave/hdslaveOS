// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files. The finder's own thread (shell/file_finder.h does the finding): the MIDI thread hands it
// a project's slots, it hands back where each missing one is now, or that it is missing. A check (steps 1-4, at once,
// and step 5 from an index this session already built) after each project is opened; a search (step 5, the card
// walked first when there is no index yet) after the question's SEARCH. The thread runs at the lowest CPU priority
// whatever thread started it (the MIDI thread is real time); the walk goes a folder at a time at idle disk priority,
// so a sample streaming from the card keeps its priority, and a check that comes meanwhile goes first. Plain C++, no
// JUCE: the host tests run it directly. Never on the audio thread; the MIDI thread only posts and takes answers, and
// its lock is never held across an allocation on this thread's side.
#pragma once
#include "melodee/model/chop_slices.h"
#include "melodee/shell/file_finder.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace melodee {

class MovedFiles {
public:
    struct Slot { uint16_t slot = 0; std::string saved; };
    struct Job {
        bool search = false;           // after SEARCH (or Always, or FIND MISSING), else a check
        uint32_t generation = 0;       // the project's (FindMoved): work and answers for another are dropped
        std::string origin, card;      // the project's own file (or the .xpj), the card's top
        std::vector<std::string> internal; // a search: the internal sample folders, walked after the card
        std::vector<Slot> slots;
        bool fresh = false;            // a search: the index of this session dropped first (FIND MISSING)
        bool use_index = true;         // a check: step 5 from the index
        bool quiet = false;            // a check that asks nothing after (FORGET undone): its Done says so
    };
    struct Answer {
        enum class Kind : uint8_t { Found, Missing, Done } kind = Kind::Done;
        uint32_t generation = 0;
        uint16_t slot = 0;
        std::string saved, found;      // Found: the path now (the same, for a file copied back)
        SampleChop chop {};            // Found: its slices, read as a project's switch reads them
        unsigned rate = 0;             // Found: the file's sample rate (read_rate; 0 not known)
        bool tie = false;              // Missing: several files of the name, none to tell from the others
        // Done: what the job was, and came of it.
        bool search = false;
        unsigned found_count = 0, missing_count = 0;
        bool indexed = false;          // a check answered from a complete index (nothing left to ask)
        bool quiet = false;            // a quiet check's
        // A search that did not walk the whole card (CANCEL, the card lent): what it found is in, nothing said missing.
        bool cancelled = false;
        bool too_big = false;          // the card past CardIndex::max_files: as cancelled, and so for the session
        bool failed = false;           // the card could not be read: nothing found or said missing
    };

    MovedFiles() = default;
    ~MovedFiles();
    MovedFiles(const MovedFiles&) = delete;
    MovedFiles& operator=(const MovedFiles&) = delete;

    // MIDI thread. A job (the thread starts at the first); a search replaces one not begun.
    void post(Job job);
    // MIDI thread: the generation work is for now; a job or a search for another is dropped (a walk under way goes
    // on, so the next search finds the index there).
    void current(uint32_t generation) { generation_.store(generation, std::memory_order_relaxed); wake_up(); }
    // Any thread: the search stops (within a folder); what the part walked found plainly is answered (pick_by_name,
    // partial), nothing is said missing, and the half-built index is dropped.
    void cancel();
    // Any thread: the index is no longer the card as it is (another card, a file added from the web page, the card
    // back from a computer). Taken in before the next job; a card too big is looked at again.
    void drop_index();
    // Any thread (the service thread, SD card access): while `away` nothing of the card is read. A search stops as
    // CANCEL stops it, but finds nothing more; a check under way waits and goes on after. parked(): nothing is reading
    // now (the card may be lent).
    void hold(bool away);
    bool parked() const;
    // MIDI thread (one taker): the answers come in order.
    bool take(Answer& out);
    struct Progress { bool working = false, walking = false; uint32_t looked = 0, matched = 0; };
    Progress progress() const {
        return {working_.load(std::memory_order_relaxed), walking_.load(std::memory_order_relaxed),
                looked_.load(std::memory_order_relaxed), matched_.load(std::memory_order_relaxed)};
    }
    bool indexed() const { return indexed_.load(std::memory_order_relaxed); } // a complete index of this session
    unsigned walks() const { return walks_.load(std::memory_order_relaxed); } // walks begun since start (tests)
    unsigned looked_for() const { return looked_for_.load(std::memory_order_relaxed); } // paths not in place, looked for (tests)
    long thread_id() const { return tid_.load(std::memory_order_relaxed); } // the thread's (Linux), 0 before it starts (tests)
    bool idle() const; // nothing waiting, nothing under way, no answer to take
    // How a found file's slices and sample rate are read (shell/mpc_wav_slices.h, shell/mpc_project.h): set before the
    // first job; none, no slices or rate.
    std::function<SampleChop(const std::string&)> read_chop;
    std::function<unsigned(const std::string&)> read_rate;
    // What the walk waits after each folder, microseconds (tests: a slow card).
    std::atomic<unsigned> folder_pause_us {0};
    size_t file_limit = CardIndex::max_files; // before the first job (tests)
    static constexpr size_t path_room = 256;  // a slot's path, as SampleRef holds it: one longer is not taken

private:
    void run();
    void wake_up();
    bool check(const Job& job);                  // false: held (the card lent) before it was done
    void finish_search(const Job& job, bool part);
    void idle_disk(bool idle); // the walk's disk class (idle) or the checks' (usual)
    bool disk_idle = false;
    void answer(Answer a) { outbox_.push_back(std::move(a)); }
    bool publish();                              // the outbox into the mailbox, when the taker has emptied it
    bool stale(const Job& job) const { return job.generation != generation_.load(std::memory_order_relaxed); }
    bool stopping() const { return stopping_.load(std::memory_order_relaxed); }
    bool held() const { return held_.load(std::memory_order_relaxed); }
    Answer found(const Job& job, const Slot& s, const std::string& path);
    Answer missing(const Job& job, const Slot& s, bool tie);

    mutable std::mutex lock;
    std::condition_variable wake;
    std::vector<Job> checks_;             // under `lock`
    std::unique_ptr<Job> search_wanted;   // under `lock`: the next search
    std::vector<Answer> mailbox_;         // under `lock`: answers for the taker
    uint32_t dropped_search_ = 0;         // under `lock`: a search cancelled before it began (its generation + 1)
    bool stop_ = false, started_ = false; // under `lock`
    bool busy = false;                    // under `lock`: the thread has work in hand
    std::thread thread;
    std::atomic<bool> cancel_ {false}, drop_ {false}, indexed_ {false}, working_ {false}, walking_ {false}, held_ {false};
    std::atomic<bool> stopping_ {false}, interrupt_ {false}; // interrupt_: CardIndex::abort (cancel, hold, stop)
    std::atomic<uint32_t> generation_ {0}, looked_ {0}, matched_ {0};
    std::atomic<unsigned> walks_ {0}, looked_for_ {0};
    std::atomic<long> tid_ {0};
    // The taker's own.
    std::vector<Answer> taken_;
    size_t taken_at_ = 0;
    // The thread's own.
    std::vector<Job> inbox_, paused_;     // checks taken; checks the card's lending cut off, to do again
    std::vector<Answer> outbox_;
    CardIndex index;
    std::unique_ptr<Job> searching;       // the search the walk is for
    uint32_t moves_generation = 0;
    std::vector<FolderMove> moves_;       // where this project's samples were found
    void start_walk(); // the walk for `searching`, from the start
    void watch();      // its slots counted as the walk meets their names
};

} // namespace melodee
