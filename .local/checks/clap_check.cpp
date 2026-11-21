// SPDX-License-Identifier: AGPL-3.0-only
// Every plug-in in the given CLAP files, opened as Melodee's rack opens it: the plug-in support pack first
// (ensure_plugin_support: the GUI stubs and the sound libraries the MPC lacks), then ClapInstance::list and load at
// 48 kHz, 128 frames. Each plays one second (noise into an effect, a held chord on an instrument), saves and restores
// its state, and is checked for output that is not a number. "asks" counts the last 32 blocks (the chord let go
// 28 blocks before them) after which it still wanted to run (request_process, request_flush): Melodee wakes a
// sleeping plug-in for each, so one that asks every block never sleeps. One line a plug-in; a last line counts the
// failures.
//   clap_check FILE.clap ...
#include "melodee/plugin/clap_instance.h"
#include "melodee/plugin/plugin_files.h"
#include "melodee/plugin/plugin_support.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

using namespace juce;
using melodee::ClapInstance;

int main (int argc, char** argv)
{
    MessageManager::getInstance();
    for (int i = 0; i < 100 && ! melodee::ensure_plugin_support(); ++i) {}
    std::printf ("support: %s\n", melodee::plugin_support_folder().empty() ? "none" : melodee::plugin_support_folder().c_str());

    constexpr double rate = 48000.0;
    constexpr int block = 128, blocks = 375;
    int failed = 0, passed = 0;
    std::mt19937 random (1);
    std::uniform_real_distribution<float> noise (-0.5f, 0.5f);
    for (int a = 1; a < argc; ++a)
    {
        const File file (argv[a]);
        String error;
        const auto list = ClapInstance::list (file, error);
        if (list.empty())
        {
            std::printf ("FAIL  %s: %s\n", file.getFileName().toRawUTF8(), error.toRawUTF8());
            ++failed;
            continue;
        }
        for (const auto& info : list)
        {
            auto plugin = ClapInstance::load (file, melodee::clap_id_hash (info.id), rate, block, error);
            if (plugin == nullptr)
            {
                std::printf ("FAIL  %s / %s: %s\n", file.getFileName().toRawUTF8(), info.name.c_str(), error.toRawUTF8());
                ++failed;
                continue;
            }
            plugin->prepareToPlay (rate, block);
            AudioBuffer<float> buffer (std::max (2, std::max (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels())), block);
            double total = 0.0, worst = 0.0;
            float peak = 0.0f, keyPeak = 0.0f; // keyPeak: its side-chain key port's (melodee-hdslave), with note 36 too
            bool finite = true;
            const bool key = plugin->hasKeyOutput();
            int asks = 0;
            for (int b = 0; b < blocks; ++b)
            {
                MidiBuffer midi;
                if (info.instrument && b == 0)
                    for (int note : { 48, 52, 55 })
                        midi.addEvent (MidiMessage::noteOn (1, note, 0.8f), 0);
                if (key && b == 0)
                    midi.addEvent (MidiMessage::noteOn (1, 36, 0.8f), 0); // a drum machine's kick (A01)
                if (info.instrument && b == blocks - 60)
                    for (int note : { 48, 52, 55 })
                        midi.addEvent (MidiMessage::noteOff (1, note), 0);
                if (key && b == blocks - 60)
                    midi.addEvent (MidiMessage::noteOff (1, 36), 0);
                for (int c = 0; c < buffer.getNumChannels(); ++c)
                    for (int i = 0; i < block; ++i)
                        buffer.setSample (c, i, info.instrument ? 0.0f : noise (random));
                const auto t0 = std::chrono::steady_clock::now();
                plugin->processBlock (buffer, midi);
                const double us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();
                total += us;
                worst = std::max (worst, us);
                for (int c = 0; c < plugin->getTotalNumOutputChannels() && c < buffer.getNumChannels(); ++c)
                    for (int i = 0; i < block; ++i)
                    {
                        const float s = buffer.getSample (c, i);
                        finite = finite && std::isfinite (s);
                        peak = std::max (peak, std::abs (s));
                    }
                for (int c = 0; key && c < 2; ++c)
                    for (int i = 0; i < plugin->keyFrames(); ++i)
                    {
                        const float s = plugin->keyChannel (c)[i];
                        finite = finite && std::isfinite (s);
                        keyPeak = std::max (keyPeak, std::abs (s));
                    }
                if (b % 32 == 0)
                    plugin->idle();
                if (b >= blocks - 32 && plugin->wantsProcess())
                    ++asks;
            }
            MemoryBlock state;
            plugin->getStateInformation (state);
            plugin->setStateInformation (state.getData(), (int) state.getSize());
            plugin->releaseResources();
            const bool ok = finite;
            (ok ? passed : failed)++;
            std::printf ("%s  %-28s %-34s %s  params %4d  latency %5d  in %d out %d  block mean %7.1f us max %7.1f us  peak %.3f  state %zu B  asks %d/32",
                         ok ? "ok  " : "FAIL", file.getFileName().toRawUTF8(), info.name.c_str(), info.instrument ? "inst" : "fx  ",
                         plugin->getParameters().size(), plugin->getLatencySamples(), plugin->getTotalNumInputChannels(),
                         plugin->getTotalNumOutputChannels(), total / blocks, worst, peak, state.getSize(), asks);
            if (key) std::printf ("  SC key port: peak %.3f", keyPeak);
            std::printf ("\n");
        }
    }
    std::printf ("%d loaded and played, %d failed\n", passed, failed);
    MessageManager::deleteInstance();
    return failed == 0 ? 0 : 1;
}
