// SPDX-License-Identifier: AGPL-3.0-only
// A VST2 effect for bridge_wait.cpp: each block it works for BRIDGE_FIXTURE_WORK_US microseconds (a plug-in's own
// DSP time), then writes half its input plus a 441 Hz tone at its one parameter's level. Built for the machine the
// check runs on, so the bridge's handoff can be measured where no armv7 code runs.
#include "melodee/plugin/vst2_abi.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
vst2::Effect effect {};
double phase = 0.0;
int workUs = 0;
float level = 0.25f;

intptr_t dispatcher (vst2::Effect*, int32_t opcode, int32_t, intptr_t, void* ptr, float)
{
    const auto copy = [ptr] (const char* text) { if (ptr != nullptr) std::strcpy ((char*) ptr, text); return (intptr_t) 1; };
    switch (opcode)
    {
        case vst2::op::getEffectName: return copy ("Bridge fixture");
        case vst2::op::getVendorString: return copy ("Melodee checks");
        case vst2::op::getParamName: return copy ("Level");
        case vst2::op::getParamDisplay:
            if (ptr != nullptr)
                std::snprintf ((char*) ptr, 8, "%.3f", level);
            return 1;
        default: return 0;
    }
}

void setParameter (vst2::Effect*, int32_t, float value) { level = value; }
float getParameter (vst2::Effect*, int32_t) { return level; }

void process (vst2::Effect*, float** inputs, float** outputs, int32_t frames)
{
    if (workUs > 0)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds (workUs);
        while (std::chrono::steady_clock::now() < until) {}
    }
    for (int32_t i = 0; i < frames; ++i)
    {
        const float tone = (float) std::sin (phase) * level;
        phase += 2.0 * M_PI * 441.0 / 48000.0;
        if (phase > 2.0 * M_PI)
            phase -= 2.0 * M_PI;
        outputs[0][i] = inputs[0][i] * 0.5f + tone;
        outputs[1][i] = inputs[1][i] * 0.5f - tone;
    }
}
} // namespace

extern "C" __attribute__ ((visibility ("default"))) vst2::Effect* VSTPluginMain (vst2::HostCallback)
{
    if (const char* work = std::getenv ("BRIDGE_FIXTURE_WORK_US"))
        workUs = std::atoi (work);
    effect.magic = vst2::magic;
    effect.dispatcher = dispatcher;
    effect.setParameter = setParameter;
    effect.getParameter = getParameter;
    effect.numPrograms = 1;
    effect.numParams = 1;
    effect.numInputs = 2;
    effect.numOutputs = 2;
    effect.flags = vst2::flag::canReplace;
    effect.uniqueId = ('B' << 24) | ('r' << 16) | ('F' << 8) | 'x';
    effect.version = 1;
    effect.processReplacing = process;
    return &effect;
}
