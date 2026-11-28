// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files. The finder's own thread (shell/file_finder.h does the finding): the MIDI thread hands it
// a project's slots, it hands back where each missing one is now, or that it is missing. A check (steps 1-4, at once,
// and step 5 from an index this session already built) after each project is opened; a search (step 5, the card
// walked first when there is no index yet) after the question's SEARCH. The walk goes a folder at a time at idle disk
// priority, so a sample streaming from the card keeps its priority, and a check that comes meanwhile goes first.
// Plain C++, no JUCE: the host tests run it directly. Never on the audio thread; on the MIDI thread only posting and
// taking answers.
#pragma once
#include "melodee/model/chop_slices.h"
#include "melodee/shell/file_finder.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
        bool use_index = true;         // a check: step 5 from a complete index
        bool quiet = false;            // a check that asks nothing after (FORGET undone): its Done says so
    };
    struct Answer {
        enum class Kind : uint8_t { Found, Missing, Done } kind = Kind::Done;
        uint32_t generation = 0;
        uint16_t slot = 0;
        std::string saved, found;      // Found: the path now (the same, for a file copied back)
        SampleChop chop {};            // Found: its slices, read as a project's switch reads them
        bool tie = false;              // Missing: several files of the name, of different sizes
        // Done: what the job was, and came of it.
        bool search = false;
        unsigned found_count = 0, missing_count = 0;
        bool indexed = false;          // a check answered from the index (nothing left to ask)
        bool quiet = false;            // a quiet check's
        bool cancelled = false, too_big = false;
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
    // Any thread: the search stops within a folder (one not begun, at once). What the part of the card walked found is
    // answered; a half-built index is dropped, so it never says a file it did not reach is missing.
    void cancel();
    // Any thread: the index is no longer the card as it is (another card, a file added from the web page, the card
    // back from a computer). Taken in before the next job.
    void drop_index() { drop_.store(true, std::memory_order_relaxed); indexed_.store(false, std::memory_order_relaxed); wake_up(); }
    bool take(Answer& out);
    struct Progress { bool working = false, walking = false; uint32_t looked = 0, matched = 0; };
    Progress progress() const {
        return {working_.load(std::memory_order_relaxed), walking_.load(std::memory_order_relaxed),
                looked_.load(std::memory_order_relaxed), matched_.load(std::memory_order_relaxed)};
    }
    bool indexed() const { return indexed_.load(std::memory_order_relaxed); } // a complete index of this session
    unsigned walks() const { return walks_.load(std::memory_order_relaxed); } // walks begun since start (tests)
    unsigned looked_for() const { return looked_for_.load(std::memory_order_relaxed); } // paths not in place, looked for (tests)
    bool idle() const; // nothing waiting, nothing under way, no answer to take
    // How a found file's slices are read (shell/mpc_wav_slices.h): set before the first job; none, no slices.
    std::function<SampleChop(const std::string&)> read_chop;
    // What the walk waits after each folder, microseconds (tests: a slow card).
    std::atomic<unsigned> folder_pause_us {0};

private:
    void run();
    void wake_up();
    void check(const Job& job);
    void finish_search(const Job& job, bool cancelled);
    void idle_disk(bool idle); // the walk's disk class (idle) or the checks' (usual)
    bool disk_idle = false;
    void answer(Answer a);
    bool stale(const Job& job) const { return job.generation != generation_.load(std::memory_order_relaxed); }
    Answer found(const Job& job, const Slot& s, const std::string& path);

    mutable std::mutex lock;
    std::condition_variable wake;
    std::deque<Job> checks;               // under `lock`
    std::unique_ptr<Job> search_wanted;   // under `lock`: the next search
    std::deque<Answer> answers;           // under `lock`
    bool stop = false, started = false;   // under `lock`
    bool busy = false;                    // under `lock`: the thread has work in hand
    std::thread thread;
    std::atomic<bool> cancel_ {false}, drop_ {false}, indexed_ {false}, working_ {false}, walking_ {false};
    std::atomic<uint32_t> generation_ {0}, looked_ {0}, matched_ {0};
    std::atomic<unsigned> walks_ {0}, looked_for_ {0};
    // The thread's own.
    CardIndex index;
    std::unique_ptr<Job> searching;       // the search the walk is for
    uint32_t kit_generation = 0;
    std::vector<std::string> kit_folders; // where this project's samples were found, most first
    void kit_folder(const std::string& path);
    void start_walk(); // the walk for `searching`, from the start
    void watch();      // its names counted as the walk meets them
};

} // namespace melodee
