// SPDX-License-Identifier: AGPL-3.0-only
// Melodee's bridge host (Vst2BridgeInstance) against vst2_bridge32 built for this machine and bridge_fixture.so,
// blocks paced in real time (128 frames at 48 kHz). Prints, per scenario, the CPU the host thread and the bridge
// process spend a period, how long processBlock takes, the bridge's wake-ups, and a hash of everything the plug-in
// played (the same before and after a change to the handoff, or the change altered the sound).
//   bridge_wait FIXTURE.so BRIDGE_FOLDER BLOCKS
#include "melodee/plugin/vst2_bridge_instance.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace juce;
using melodee::Vst2BridgeInstance;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 128;
constexpr long periodNs = (long) (block * 1.0e9 / rate);

pid_t childPid()
{
    const auto self = getpid();
    DIR* proc = opendir ("/proc");
    pid_t found = -1;
    while (auto* entry = readdir (proc))
    {
        const pid_t pid = (pid_t) std::atoi (entry->d_name);
        if (pid <= 0)
            continue;
        std::ifstream stat ("/proc/" + std::string (entry->d_name) + "/stat");
        std::string line;
        std::getline (stat, line);
        const auto close = line.rfind (')');
        if (close == std::string::npos)
            continue;
        char state = 0;
        int parent = 0;
        if (std::sscanf (line.c_str() + close + 1, " %c %d", &state, &parent) == 2 && parent == self)
            found = pid;
    }
    closedir (proc);
    return found;
}

// Nanoseconds on a CPU (schedstat) and voluntary context switches (each one a sleep, so a wake-up after it).
struct Usage { uint64_t cpuNs = 0, wakes = 0; };

Usage usageOf (pid_t pid)
{
    Usage usage;
    std::ifstream schedstat ("/proc/" + std::to_string (pid) + "/schedstat");
    schedstat >> usage.cpuNs;
    std::ifstream status ("/proc/" + std::to_string (pid) + "/status");
    for (std::string line; std::getline (status, line);)
        if (line.rfind ("voluntary_ctxt_switches:", 0) == 0)
            usage.wakes = std::strtoull (line.c_str() + 24, nullptr, 10);
    return usage;
}

uint64_t threadCpuNs()
{
    timespec t {};
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t) t.tv_sec * 1000000000ull + (uint64_t) t.tv_nsec;
}

double nowUs()
{
    timespec t {};
    clock_gettime (CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1.0e6 + t.tv_nsec / 1.0e3;
}

void addHash (uint64_t& hash, const AudioBuffer<float>& buffer, int frames)
{
    for (int c = 0; c < buffer.getNumChannels(); ++c)
        for (int i = 0; i < frames; ++i)
        {
            uint32_t bits;
            std::memcpy (&bits, buffer.getReadPointer (c) + i, sizeof (bits));
            hash = (hash ^ bits) * 0x100000001b3ull;
        }
}

enum class Scenario { steady, parts, params };

void run (Vst2BridgeInstance& plugin, pid_t bridge, Scenario scenario, const char* label, int blocks)
{
    AudioBuffer<float> buffer (2, block);
    MidiBuffer midi;
    std::vector<double> times;
    times.reserve ((size_t) blocks);
    uint64_t hash = 0xcbf29ce484222325ull, hostNs = 0;
    float input = 0.0f;
    const auto before = usageOf (bridge);
    timespec next {};
    clock_gettime (CLOCK_MONOTONIC, &next);
    for (int b = 0; b < blocks; ++b)
    {
        next.tv_nsec += periodNs;
        if (next.tv_nsec >= 1000000000L)
            next.tv_nsec -= 1000000000L, ++next.tv_sec;
        clock_nanosleep (CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);

        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < block; ++i)
                buffer.setSample (c, i, (input += 0.001f) > 1.0f ? (input = -1.0f) : input);
        const auto cpu0 = threadCpuNs();
        const auto t0 = nowUs();
        if (scenario == Scenario::parts)
        {
            // An instrument rendered in parts: two calls back to back in one period.
            AudioBuffer<float> first (buffer.getArrayOfWritePointers(), 2, 0, block / 2);
            AudioBuffer<float> second (buffer.getArrayOfWritePointers(), 2, block / 2, block / 2);
            plugin.processBlock (first, midi);
            plugin.processBlock (second, midi);
        }
        else
            plugin.processBlock (buffer, midi);
        times.push_back (nowUs() - t0);
        if (scenario == Scenario::params && b % 17 == 0)
            for (int i = 0; i < 36; ++i) // a card refresh's worth of reads, as PluginRack::panel makes
                (void) plugin.getParameters()[0]->getText (plugin.getParameters()[0]->getValue(), 16);
        hostNs += threadCpuNs() - cpu0;
        addHash (hash, buffer, block);
    }
    const auto after = usageOf (bridge);
    std::sort (times.begin(), times.end());
    double mean = 0.0;
    for (auto t : times)
        mean += t;
    mean /= (double) times.size();
    std::printf ("%-7s host %6.1f us/period  bridge %6.1f us/period  block mean %6.1f us p99 %6.1f us  "
                 "bridge wakes %5" PRIu64 "  hash %016" PRIx64 "%s\n",
                 label, hostNs / 1000.0 / blocks, (after.cpuNs - before.cpuNs) / 1000.0 / blocks, mean,
                 times[(size_t) (times.size() * 0.99)], after.wakes - before.wakes, hash,
                 plugin.bridgeDied() ? "  BRIDGE DIED" : "");
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf (stderr, "usage: bridge_wait FIXTURE.so BRIDGE_FOLDER BLOCKS\n");
        return 2;
    }
    MessageManager::getInstance();
    sched_param priority {};
    priority.sched_priority = bridge::audioPriority;
    const bool realtime = pthread_setschedparam (pthread_self(), SCHED_FIFO, &priority) == 0;

    String error;
    auto plugin = Vst2BridgeInstance::load (File (argv[1]), File (argv[2]), rate, block, error);
    if (plugin == nullptr)
    {
        std::fprintf (stderr, "load failed: %s\n", error.toRawUTF8());
        return 1;
    }
    plugin->prepareToPlay (rate, block);
    const pid_t bridge = childPid();
    const int blocks = std::atoi (argv[3]);
    std::printf ("work %s us, host %s, bridge pid %d\n", std::getenv ("BRIDGE_FIXTURE_WORK_US") ?: "0",
                 realtime ? "SCHED_FIFO" : "normal priority", (int) bridge);

    run (*plugin, bridge, Scenario::steady, "steady", blocks);
    run (*plugin, bridge, Scenario::parts, "parts", blocks);
    run (*plugin, bridge, Scenario::params, "params", blocks);

    // Idle: a loaded plug-in the host sends nothing to (asleep, as the idle gate leaves it).
    const auto before = usageOf (bridge);
    usleep (1000000);
    const auto after = usageOf (bridge);
    std::printf ("idle    bridge %6.1f us/s  bridge wakes %" PRIu64 " in 1 s\n", (after.cpuNs - before.cpuNs) / 1000.0,
                 after.wakes - before.wakes);

    const auto trip = plugin->measureRoundTrips (2000);
    std::printf ("empty round trip mean %.1f us p99 %.1f us max %.1f us\n", trip[0], trip[1], trip[2]);

    // The bridge dies: the state it gives is nothing (the rack keeps the one it had, as for a failed save), never a
    // state of every parameter at 0, which a project saved then, a copy or a reload would put back.
    MemoryBlock alive, dead;
    plugin->getStateInformation (alive);
    kill (bridge, SIGKILL);
    usleep (100000);
    plugin->getStateInformation (dead);
    const bool kept = alive.getSize() > 0 && dead.getSize() == 0 && plugin->bridgeDied();
    std::printf ("dead bridge: state %zu bytes (%zu alive)%s\n", dead.getSize(), alive.getSize(), kept ? "" : "  FAILED");
    plugin->releaseResources();
    plugin.reset();
    MessageManager::deleteInstance();
    return kept ? 0 : 1;
}
