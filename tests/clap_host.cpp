// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Melodee's CLAP host (plugin/clap_instance.h) against tests/clap_fixture.cpp:
// listing a library, notes as CLAP notes and CCs as MIDI, parameters both ways,
// state, latency, the main-thread callback, presets found (with their folders,
// kept in a cache) and loaded, Melodee's plug-in screen, what it is run for when idle, and every call on the right thread.
// Built and run by tests/test_plugin_hosts.py on aarch64 Linux, with Melodee's objects.
#include "melodee/plugin/clap_instance.h"
#include "melodee/plugin/melodee_screen.h"
#include "melodee/plugin/plugin_files.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

using namespace melodee;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

namespace {
constexpr int block = 128;
constexpr double rate = 48000;

// One block through the plug-in: `in` on both channels (an effect's input), `midi` at frame 0.
juce::AudioBuffer<float> run(juce::AudioPluginInstance& p, std::initializer_list<juce::MidiMessage> midi = {}, float in = 0) {
    juce::AudioBuffer<float> buffer(2, block);
    for (int c = 0; c < 2; ++c) for (int f = 0; f < block; ++f) buffer.setSample(c, f, in);
    juce::MidiBuffer events;
    for (auto& m : midi) events.addEvent(m, 0);
    p.processBlock(buffer, events);
    return buffer;
}
float peak(const juce::AudioBuffer<float>& b) { return b.getMagnitude(0, 0, b.getNumSamples()); }
juce::AudioProcessorParameter* param(juce::AudioPluginInstance& p, const char* name) {
    for (auto* x : p.getParameters()) if (x->getName(64) == name) return x;
    return nullptr;
}
void write(const juce::File& f, const char* text) {
    f.getParentDirectory().createDirectory();
    f.replaceWithText(text, false, false, "\n"); // JUCE writes \r\n by default
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: clap_host Test.clap\n"); return 2; }
    juce::File file {juce::String(argv[1])};
    // Held open here too, so its counts can be read once the host has let it go.
    void* fixture = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    auto counts = reinterpret_cast<void (*)(int*, int*, int*)>(dlsym(fixture, "fixture_counts"));
    CHECK(fixture && counts);
    int wrong = 0, callbacks = 0, inits = 0;

    // The picker's view of it.
    auto f = describe_plugin(argv[1]);
    CHECK(f.format == PluginFormat::Clap && f.loadable() && f.library == argv[1] && std::string(format_tag(f.format)) == "CLAP");

    // A .clap drum machine's pad map, read from its library as for a .so (plugin/drum_map.h): the same library
    // with a map appended (describe reads the file's bytes; the copy is never opened).
    {
        auto drums = file.getSiblingFile("Drums.clap");
        CHECK(file.copyFileTo(drums));
        {
            juce::FileOutputStream out(drums);
            out.setPosition(out.getFile().getSize());
            const char* map = "{\"pad_layout\":\"drums\",\"levels\":{\"bd\":{\"name\":\"Kick\",\"note\":36},\"sd\":{\"name\":\"Snare\",\"note\":38}}}";
            out.write(map, std::strlen(map));
        }
        auto d = describe_plugin(drums.getFullPathName().toStdString());
        CHECK(d.format == PluginFormat::Clap && d.drums.size() == 2);
        CHECK(d.drums.size() == 2 && d.drums[0].key == 36 && d.drums[0].name == "Kick" && d.drums[1].key == 38 && d.drums[1].name == "Snare");
        drums.deleteFile();
    }

    juce::String error;
    auto list = ClapInstance::list(file, error);
    CHECK(list.size() == 2);
    CHECK(list.size() == 2 && list[0].name == "Test Sine" && list[0].instrument && list[1].name == "Test Gain" && !list[1].instrument);
    counts(&wrong, &callbacks, &inits);
    CHECK(inits == 0); // listed, then let go

    {
        auto sine = ClapInstance::load(file, 0, rate, block, error); // 0: the first
        CHECK(sine != nullptr);
        if (!sine) return 1;
        juce::PluginDescription d;
        sine->fillInPluginDescription(d);
        CHECK(d.isInstrument && d.name == "Test Sine" && d.pluginFormatName == "CLAP" && d.uniqueId == clap_id_hash("org.melodee.test.sine"));
        CHECK(sine->acceptsMidi() && sine->getTotalNumInputChannels() == 0 && sine->getTotalNumOutputChannels() == 2);
        CHECK(sine->getParameters().size() == 2);
        sine->prepareToPlay(rate, block);
        auto* level = param(*sine, "Level");
        auto* wave = param(*sine, "Wave");
        CHECK(level && wave && std::abs(level->getValue() - 0.5f) < 1e-6f && level->getText(0.5f, 32) == "50 %");
        CHECK(wave && wave->isDiscrete() && wave->getNumSteps() == 3);

        CHECK(peak(run(*sine)) == 0.0f);
        CHECK(!sine->wantsProcess()); // nothing waits for it: the rack may let it sleep (plugin/idle_gate.h)
        // Its second output, the side-chain key (melodee-hdslave): apart from the main output, a 16-frame pulse from
        // the note's first frame; the main output starts its sine at 0 there.
        CHECK(sine->hasKeyOutput() && sine->keyFrames() == block && sine->keyChannel(0) && sine->keyChannel(1));
        auto noted = run(*sine, {juce::MidiMessage::noteOn(1, 69, 1.0f)});
        CHECK(sine->keyFrames() == block && noted.getSample(0, 0) == 0.0f);
        bool pulse = true;
        for (int f = 0; f < block; ++f)
            for (int c = 0; c < 2; ++c) pulse = pulse && sine->keyChannel(c)[f] == (f < 16 ? 1.0f : 0.0f);
        CHECK(pulse);
        CHECK(std::abs(peak(run(*sine)) - 0.5f) < 0.01f); // A at Level 0.5
        bool after = true;
        for (int f = 0; f < block; ++f) after = after && sine->keyChannel(0)[f] == 0.0f;
        CHECK(after && sine->keyFrames() == block);

        level->setValue(1.0f); // the next block hears it
        CHECK(level->getValue() == 1.0f && sine->wantsProcess()); // and is run for it, asleep or not
        CHECK(std::abs(peak(run(*sine)) - 1.0f) < 0.01f);
        CHECK(!sine->wantsProcess());

        run(*sine, {juce::MidiMessage::controllerEvent(1, 1, 64)}); // MIDI: the plug-in sets Level and says so
        CHECK(std::abs(level->getValue() - 64.0f / 127.0f) < 1e-4f);

        juce::MemoryBlock saved;
        sine->getStateInformation(saved);
        level->setValue(0.1f);
        run(*sine);
        sine->setStateInformation(saved.getData(), int(saved.getSize()));
        CHECK(std::abs(level->getValue() - 64.0f / 127.0f) < 1e-4f); // read back from the plug-in

        run(*sine, {juce::MidiMessage::noteOff(1, 69)});
        CHECK(peak(run(*sine)) == 0.0f);
        run(*sine, {juce::MidiMessage::noteOn(1, 60, 1.0f), juce::MidiMessage::noteOn(1, 64, 1.0f)});
        run(*sine, {juce::MidiMessage::allNotesOff(1)}); // as a choke of every note
        CHECK(peak(run(*sine)) == 0.0f);

        // A failed save leaves no state at all, never a bare "MCLP" header a later load would take for one.
        {
            auto failSaves = reinterpret_cast<void (*)(int)>(dlsym(fixture, "fixture_fail_saves"));
            CHECK(failSaves != nullptr);
            juce::MemoryBlock good;
            sine->getStateInformation(good);
            CHECK(good.getSize() > 4);
            failSaves(1);
            juce::MemoryBlock failed;
            sine->getStateInformation(failed);
            failSaves(0);
            CHECK(failed.isEmpty());
        }

        // Silence after a wide session: more played (channel, key) pairs than one block's events hold, every one
        // of them choked within a few blocks (none dropped, none by a wildcard key the fixture would clear on).
        {
            auto chokes = reinterpret_cast<int (*)()>(dlsym(fixture, "fixture_chokes"));
            auto chokesReset = reinterpret_cast<void (*)()>(dlsym(fixture, "fixture_chokes_reset"));
            CHECK(chokes && chokesReset);
            auto blockWith = [&](const juce::MidiBuffer& events) {
                juce::AudioBuffer<float> buffer(2, block);
                buffer.clear();
                juce::MidiBuffer copy(events);
                sine->processBlock(buffer, copy);
                return buffer;
            };
            for (int channel = 1; channel <= 16; ++channel) {
                juce::MidiBuffer notes;
                for (int key = 0; key < 100; ++key) notes.addEvent(juce::MidiMessage::noteOn(channel, key, 1.0f), 0);
                blockWith(notes);
            }
            chokesReset();
            juce::MidiBuffer silence;
            for (int channel = 1; channel <= 16; ++channel) {
                silence.addEvent(juce::MidiMessage::controllerEvent(channel, 120, 0), 0);
                silence.addEvent(juce::MidiMessage::controllerEvent(channel, 123, 0), 0);
            }
            blockWith(silence);
            for (int b = 0; b < 6; ++b) blockWith({});
            CHECK(chokes() == 1600);
            CHECK(sine->droppedEvents() == 0);
            CHECK(peak(run(*sine)) == 0.0f);
        }

        // Preparing again (a new rate, a reload) forgets the keys it had and the chokes still queued for them:
        // the plug-in was deactivated, so nothing of the old session may be choked or released into the new one.
        {
            auto chokes = reinterpret_cast<int (*)()>(dlsym(fixture, "fixture_chokes"));
            auto chokesReset = reinterpret_cast<void (*)()>(dlsym(fixture, "fixture_chokes_reset"));
            CHECK(chokes && chokesReset);
            auto blockWith = [&](const juce::MidiBuffer& events) {
                juce::AudioBuffer<float> buffer(2, block);
                buffer.clear();
                juce::MidiBuffer copy(events);
                sine->processBlock(buffer, copy);
                return buffer;
            };
            for (int channel = 1; channel <= 16; ++channel) {
                juce::MidiBuffer notes;
                for (int key = 0; key < 100; ++key) notes.addEvent(juce::MidiMessage::noteOn(channel, key, 1.0f), 0);
                blockWith(notes);
            }
            juce::MidiBuffer silence;
            for (int channel = 1; channel <= 16; ++channel) {
                silence.addEvent(juce::MidiMessage::controllerEvent(channel, 120, 0), 0);
                silence.addEvent(juce::MidiMessage::controllerEvent(channel, 123, 0), 0);
            }
            blockWith(silence); // one block of the burst: the rest is still queued
            sine->prepareToPlay(rate, block);
            chokesReset();
            for (int b = 0; b < 6; ++b) blockWith({});
            CHECK(chokes() == 0); // nothing queued before the prepare drains after it
            blockWith(silence);
            for (int b = 0; b < 6; ++b) blockWith({});
            CHECK(chokes() == 0); // and the keys played before it are not remembered for a later silence
        }

        sine->idle(); // its request_callback from init()
        counts(&wrong, &callbacks, &inits);
        CHECK(callbacks == 1 && inits == 1);

        // Melodee's plug-in screen (plugin/melodee_screen.h): its picture, its inputs, the parameters it puts on K1-K3.
        auto screen = reinterpret_cast<void (*)(int*, int*, int*, int*, int*, int*)>(dlsym(fixture, "fixture_screen"));
        CHECK(screen != nullptr && sine->hasScreen());
        if (!screen) return 1;
        int paints = 0, drawn = 0, inputs = 0, kind = 0, value = 0, accent = 0;
        std::vector<uint32_t> pixels(size_t(MELODEE_SCREEN_WIDTH) * MELODEE_SCREEN_HEIGHT, 0);
        melodee_screen_canvas canvas {};
        canvas.pixels = pixels.data();
        canvas.width = canvas.stride = MELODEE_SCREEN_WIDTH;
        canvas.height = MELODEE_SCREEN_HEIGHT;
        canvas.accent = 0xff123456u;
        for (auto& k : canvas.knobs) k = {-1, false};
        const size_t below = size_t(MELODEE_SCREEN_WIDTH) * 5; // a pixel under its two bars
        CHECK(sine->paintWanted() && !sine->paintWanted()); // at first, always; then once asked again
        bool changed = false, used = false;
        CHECK(sine->screenPaint(canvas, changed) && changed);
        CHECK(pixels[below] == 0xff810000u);  // Level 64/127: red 129, the cursor on its first row, no inputs yet
        CHECK(pixels[160] == 0xffffffffu && pixels[161] == 0xff810000u && pixels[MELODEE_SCREEN_WIDTH] == 0xff810000u); // Level's bar; K1 unknown
        CHECK(sine->screenPaint(canvas, changed) && !changed); // nothing new
        canvas.whole = true;
        CHECK(sine->screenPaint(canvas, changed) && changed);  // a canvas that lost its picture: all of it again
        canvas.whole = false;
        int32_t knobs[3] {};
        CHECK(sine->screenKnobs(knobs) && knobs[0] == 0 && knobs[1] == 1 && knobs[2] == -1); // Level, Wave, nothing
        melodee_screen_input wheel {MELODEE_SCREEN_WHEEL, 1, false};
        CHECK(sine->screenInput(wheel, used) && used && sine->paintWanted());
        CHECK(!sine->wantsProcess()); // the wheel needs only its main thread
        CHECK(sine->screenKnobs(knobs) && knobs[0] == 1 && knobs[1] == 0 && knobs[2] == -1); // the cursor on Wave
        CHECK(sine->screenPaint(canvas, changed) && changed && pixels[below] == 0xff81ff01u);
        canvas.knobs[0] = {127, true};
        CHECK(sine->screenPaint(canvas, changed) && changed && pixels[2 * MELODEE_SCREEN_WIDTH - 1] == 0xffffffffu); // K1 at the top
        melodee_screen_input push {MELODEE_SCREEN_PUSH, 1, true}, odd {7, 0, false};
        CHECK(sine->screenInput(push, used) && used && sine->screenKnobs(knobs) && knobs[0] == 0);
        CHECK(sine->wantsProcess()); // Push asked to be run (request_process): a sleeping instrument wakes for it
        CHECK(sine->screenInput(odd, used) && !used); // not one it knows
        screen(&paints, &drawn, &inputs, &kind, &value, &accent);
        CHECK(paints == 5 && drawn == 4 && inputs == 3 && kind == 7 && accent == 0x123456);
        level->setValue(0.0f); // through the audio: it asks for a paint from there
        sine->paintWanted(); // what the inputs asked for, taken
        run(*sine);
        CHECK(sine->paintWanted() && !sine->wantsProcess());
        const size_t third = 2 * size_t(MELODEE_SCREEN_WIDTH); // its third row: 16 pixels for each press its audio side took
        CHECK(sine->screenPaint(canvas, changed) && changed && pixels[third + 15] == 0xffffffffu && pixels[third + 16] != 0xffffffffu);
        melodee_screen_input plus {MELODEE_SCREEN_NUDGE, 1, false};
        CHECK(sine->screenInput(plus, used) && used && sine->wantsProcess()); // request_flush: run for it as well
        CHECK(sine->screenPaint(canvas, changed) && pixels[third + 16] != 0xffffffffu); // not taken before a block
        run(*sine);
        CHECK(!sine->wantsProcess() && sine->screenPaint(canvas, changed) && changed);
        CHECK(pixels[third + 31] == 0xffffffffu && pixels[third + 32] != 0xffffffffu);
        sine->releaseResources();
    }

    {
        auto gain = ClapInstance::load(file, clap_id_hash("org.melodee.test.gain"), rate, block, error);
        CHECK(gain != nullptr);
        if (!gain) return 1;
        CHECK(gain->getName() == "Test Gain" && !gain->acceptsMidi() && gain->getTotalNumInputChannels() == 2);
        CHECK(!gain->hasScreen() && !gain->paintWanted()); // no screen of its own: never asked
        gain->prepareToPlay(rate, block);
        CHECK(gain->getLatencySamples() == 32);
        CHECK(std::abs(peak(run(*gain, {}, 0.25f)) - 0.25f) < 1e-6f); // Gain 1
        CHECK(!gain->hasKeyOutput() && gain->keyFrames() == 0 && !gain->keyChannel(0)); // one output: no key
        CHECK(!gain->wantsProcess());
        param(*gain, "Gain")->setValue(0.25f);                          // 0.5 of 0..2
        CHECK(gain->wantsProcess());                                    // an effect too: run for a value set
        CHECK(std::abs(peak(run(*gain, {}, 0.25f)) - 0.125f) < 1e-6f);
        CHECK(!gain->wantsProcess());
    }
    { // Presets: files in folders beside the library, as Surge XT keeps them.
        auto reads = reinterpret_cast<int (*)()>(dlsym(fixture, "fixture_preset_reads"));
        CHECK(reads != nullptr);
        if (!reads) return 1;
        auto dir = file.getParentDirectory(), presets = dir.getChildFile("TestPresets");
        write(presets.getChildFile("Bank.tsp"), "Bank Low\t0.25\nBank High\t0.75\n"); // a file of two, by load key
        write(presets.getChildFile("Leads/Bright.tsp"), "Bright\t0.9\n");
        write(presets.getChildFile("Leads/Soft.tsp"), "Soft\t0.3\t-\n");       // for no plug-in named: anyone's
        write(presets.getChildFile("Leads/Loud.tsp"), "Loud\t1.5\tgain\n");    // Test Gain's
        write(presets.getChildFile("Broken.tsp"), "nothing here\n");             // the provider says no
        write(presets.getChildFile("._Bright.tsp"), "Junk\t1\n");               // a Mac's beside-file: never read
        write(presets.getChildFile("Notes.txt"), "Not\t1\n");                   // not its file type
        auto cache = dir.getChildFile("cache/sine.txt");

        auto sine = ClapInstance::load(file, 0, rate, block, error);
        CHECK(sine != nullptr);
        if (!sine) return 1;
        sine->prepareToPlay(rate, block);
        CHECK(sine->getNumPrograms() == 1 && sine->getProgramName(0).isEmpty()); // none until found
        int before = reads();
        auto found = sine->findPresets(cache);
        CHECK(found.size() == 4);
        if (found.size() != 4) {
            for (const auto& p : found) std::fprintf(stderr, "found \"%s\" in \"%s\" key \"%s\"\n", p.name.c_str(), p.folder.c_str(), p.key.c_str());
            return 1;
        }
        CHECK(found[0].name == "Bank High" && found[0].folder.empty() && found[0].key == "1");
        CHECK(found[1].name == "Bank Low" && found[1].key == "0");
        CHECK(found[2].name == "Bright" && found[2].folder == "Leads" && found[2].key.empty());
        CHECK(found[3].name == "Soft" && found[3].folder == "Leads");
        CHECK(reads() - before == 5 && cache.existsAsFile()); // Bank, Bright, Soft, Loud, Broken
        CHECK(sine->findPresets(cache).size() == 4 && reads() - before == 5); // the second time from the cache

        sine->setPresets(found);
        CHECK(sine->getNumPrograms() == 4 && sine->getProgramName(2) == "Bright");
        auto* level = param(*sine, "Level");
        sine->setCurrentProgram(2);
        CHECK(sine->getCurrentProgram() == 2 && level && std::abs(level->getValue() - 0.9f) < 1e-4f);
        sine->setCurrentProgram(0); // one of a file of several, by its key
        CHECK(sine->getCurrentProgram() == 0 && level && std::abs(level->getValue() - 0.75f) < 1e-4f);

        // A second location: each its own folder, named without the plug-in's name. Only the new and the changed
        // files are read.
        write(dir.getChildFile("TestUser/Mine.tsp"), "Mine\t0.5\n");
        write(presets.getChildFile("Leads/Bright.tsp"), "Brighter\t0.8\n");
        before = reads();
        auto two = sine->findPresets(cache);
        CHECK(two.size() == 5);
        if (two.size() != 5) return 1;
        CHECK(two[0].folder == "Factory" && two[1].folder == "Factory" && two[2].name == "Brighter" && two[2].folder == "Factory/Leads");
        CHECK(two[3].folder == "Factory/Leads" && two[4].name == "Mine" && two[4].folder == "User");
        CHECK(reads() - before == 2);
        sine->releaseResources();

        auto gain = ClapInstance::load(file, clap_id_hash("org.melodee.test.gain"), rate, block, error);
        CHECK(gain != nullptr);
        if (!gain) return 1;
        auto its = gain->findPresets({}); // no cache: every file read
        CHECK(its.size() == 2 && its[0].name == "Loud" && its[1].name == "Soft");
    }
    {
        setenv("MELODEE_TEST_PARAMETER_ONLY", "1", 1);
        auto sine = ClapInstance::load(file, 0, rate, block, error);
        CHECK(sine != nullptr);
        if (!sine) return 1;
        sine->prepareToPlay(rate, block);
        auto* level = param(*sine, "Level");
        auto* wave = param(*sine, "Wave");
        level->setValue(0.75f);
        wave->setValue(1.0f);
        juce::MemoryBlock patch;
        sine->getStateInformation(patch);
        level->setValue(0.25f);
        wave->setValue(0);
        run(*sine);
        sine->setStateInformation(patch.getData(), int(patch.getSize()));
        run(*sine);
        CHECK(level->getValue() == 0.75f && wave->getValue() == 1.0f);
        sine->releaseResources();
        unsetenv("MELODEE_TEST_PARAMETER_ONLY");
    }
    CHECK(ClapInstance::load(file, 12345, rate, block, error) == nullptr && error == "No longer in its library");

    counts(&wrong, &callbacks, &inits);
    CHECK(wrong == 0);  // every call on the thread CLAP gives it
    CHECK(inits == 0);  // every init() matched by a deinit()
    dlclose(fixture);
    if (failures) return 1;
    std::puts("clap ok");
    return 0;
}
