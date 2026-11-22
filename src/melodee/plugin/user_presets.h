// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "melodee/plugin/plugin_panel.h"
#include <juce_core/juce_core.h>

namespace melodee {

// The card library for a single instrument. Called on the rack's worker.
struct UserPresets {
    juce::File folder;
    juce::String identity;
    UserPresets() = default;
    UserPresets(const std::string& home, const juce::String& id);
    std::vector<std::string> names() const;
    std::string save(const std::string& name, const juce::MemoryBlock& state) const;
    std::string load(const std::string& name, std::vector<uint8_t>& state) const;
};

PluginProgramList with_user_presets(const PluginProgramList& factory, const std::vector<std::string>& names);

} // namespace melodee
