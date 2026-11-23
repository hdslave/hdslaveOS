// SPDX-License-Identifier: AGPL-3.0-only
#include "melodee/plugin/user_presets.h"

namespace melodee {
namespace {
constexpr int signature = 0x31504448; // HDP1
constexpr int max_state_bytes = 64 * 1024 * 1024;
uint32_t checksum(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}
constexpr auto extension = ".hdpreset";
bool valid_name(const juce::String& name) {
    return name.isNotEmpty() && !name.startsWithChar('.') && juce::File::createLegalFileName(name) == name;
}
}

UserPresets::UserPresets(const std::string& home, const juce::String& id) : identity(id) {
    if (!home.empty())
        folder = juce::File(home).getChildFile("User Presets").getChildFile(juce::String::toHexString(id.hashCode64()));
}

std::vector<std::string> UserPresets::names() const {
    std::vector<std::string> out;
    if (folder == juce::File()) return out;
    for (const auto& file : folder.findChildFiles(juce::File::findFiles | juce::File::ignoreHiddenFiles, false, "*.hdpreset")) {
        auto name = file.getFileNameWithoutExtension();
        if (valid_name(name)) out.push_back(name.toStdString());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string UserPresets::save(const std::string& name, const juce::MemoryBlock& state) const {
    if (!valid_name(name)) return "Choose another preset name";
    if (folder == juce::File() || !folder.getParentDirectory().getParentDirectory().isDirectory()) return "No SD card for presets";
    if (state.isEmpty()) return "Plug-in has no preset state";
    if (state.getSize() > max_state_bytes) return "Preset exceeds 64 MB";
    auto file = folder.getChildFile(juce::String(name) + extension);
    if (file.exists()) return "Preset name already used";
    if (folder.createDirectory().failed()) return "Cannot create preset folder";
    juce::TemporaryFile pending(file, juce::TemporaryFile::useHiddenFile);
    {
        juce::FileOutputStream out(pending.getFile());
        if (out.failedToOpen() || !out.writeInt(signature) || !out.writeString(identity)
            || !out.writeInt(int(state.getSize())) || !out.writeInt(int(checksum(state.getData(), state.getSize())))
            || !out.write(state.getData(), state.getSize())) return "Cannot write preset";
        out.flush();
        if (out.getStatus().failed()) return "Cannot write preset";
    }
    return pending.overwriteTargetFileWithTemporary() ? "" : "Cannot finish preset";
}

std::string UserPresets::load(const std::string& name, std::vector<uint8_t>& state) const {
    if (folder == juce::File() || !valid_name(name)) return "Preset unavailable";
    auto in = folder.getChildFile(juce::String(name) + extension).createInputStream();
    if (!in) return "Cannot open preset";
    // Compare the exact header without reading an unbounded string from the card.
    juce::MemoryOutputStream header;
    header.writeInt(signature);
    header.writeString(identity);
    juce::MemoryBlock found;
    if (in->readIntoMemoryBlock(found, ssize_t(header.getDataSize())) != header.getDataSize()
        || std::memcmp(found.getData(), header.getData(), header.getDataSize())) return "Wrong plug-in or invalid preset";
    int size = in->readInt();
    uint32_t expected = uint32_t(in->readInt());
    if (size <= 0 || size > max_state_bytes || in->getNumBytesRemaining() != size) return "Incomplete preset";
    state.resize(size_t(size));
    if (in->read(state.data(), size) != size) { state.clear(); return "Cannot read preset"; }
    if (checksum(state.data(), state.size()) != expected) { state.clear(); return "Damaged preset"; }
    return {};
}

PluginProgramList with_user_presets(const PluginProgramList& factory, const std::vector<std::string>& names) {
    if (names.empty()) return factory;
    std::vector<PluginProgram> list;
    list.reserve(factory.size() + names.size());
    for (const auto& p : factory) list.push_back(p);
    for (size_t n = 0; n < names.size(); ++n) list.push_back({-2 - int(n), names[n], "User Presets"});
    return PluginProgramList(std::move(list));
}
} // namespace melodee
