#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>

// Manages persistent gallery state:
// - Which mod categories have been pushed to the front by user interaction
// - Uses mod name strings (stable across sessions, unlike FormIDs)
class GalleryStateManager
{
public:
    static constexpr std::uint32_t kSerializationVersion = 1;
    static constexpr std::uint32_t kRecord = '5GLY';  // 5 + GaLlerY state

    static GalleryStateManager* GetSingleton()
    {
        static GalleryStateManager instance;
        return &instance;
    }

    // Push a mod to the front of the ordering (called when user interacts with a category)
    // This will be applied on future menu opens, not immediately
    void PushModToFront(const std::string& modName)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Remove if already in list (we'll add to front)
        auto it = std::find(m_frontMods.begin(), m_frontMods.end(), modName);
        if (it != m_frontMods.end()) {
            m_frontMods.erase(it);
        }

        // Add to front
        m_frontMods.insert(m_frontMods.begin(), modName);

        spdlog::info("GalleryStateManager: Pushed '{}' to front (total front mods: {})",
            modName, m_frontMods.size());
    }

    // Check if a mod is in the front list
    bool IsFrontMod(const std::string& modName) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::find(m_frontMods.begin(), m_frontMods.end(), modName) != m_frontMods.end();
    }

    // Get the front index for sorting (0 = first, -1 = not in front list)
    // Lower index = more recent interaction = should appear earlier
    int GetFrontIndex(const std::string& modName) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::find(m_frontMods.begin(), m_frontMods.end(), modName);
        if (it != m_frontMods.end()) {
            return static_cast<int>(std::distance(m_frontMods.begin(), it));
        }
        return -1;  // Not in front list
    }

    // Get all front mods (for debugging/display)
    std::vector<std::string> GetFrontMods() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_frontMods;
    }

    // === Serialization (SKSE cosave) ===

    static void OnGameSave(SKSE::SerializationInterface* a_intfc)
    {
        auto* mgr = GetSingleton();
        std::lock_guard<std::mutex> lock(mgr->m_mutex);

        if (!a_intfc->OpenRecord(kRecord, kSerializationVersion)) {
            spdlog::error("GalleryStateManager: Failed to open record for save");
            return;
        }

        // Write count
        uint32_t count = static_cast<uint32_t>(mgr->m_frontMods.size());
        a_intfc->WriteRecordData(&count, sizeof(count));

        // Write each mod name (length-prefixed)
        for (const auto& modName : mgr->m_frontMods) {
            uint32_t len = static_cast<uint32_t>(modName.size());
            a_intfc->WriteRecordData(&len, sizeof(len));
            a_intfc->WriteRecordData(modName.data(), len);
        }

        spdlog::info("GalleryStateManager: Saved {} front mods", count);
    }

    static void OnGameLoad(SKSE::SerializationInterface* a_intfc)
    {
        auto* mgr = GetSingleton();
        std::lock_guard<std::mutex> lock(mgr->m_mutex);

        std::uint32_t type, version, length;
        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type != kRecord) {
                continue;
            }

            if (version != kSerializationVersion) {
                spdlog::warn("GalleryStateManager: Skipping record with version {} (expected {})",
                    version, kSerializationVersion);
                continue;
            }

            mgr->m_frontMods.clear();

            // Read count
            uint32_t count = 0;
            if (!a_intfc->ReadRecordData(&count, sizeof(count))) {
                spdlog::error("GalleryStateManager: Failed to read count");
                return;
            }

            // Read each mod name
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t len = 0;
                if (!a_intfc->ReadRecordData(&len, sizeof(len))) {
                    spdlog::error("GalleryStateManager: Failed to read string length at index {}", i);
                    return;
                }

                std::string modName(len, '\0');
                if (!a_intfc->ReadRecordData(modName.data(), len)) {
                    spdlog::error("GalleryStateManager: Failed to read string data at index {}", i);
                    return;
                }

                mgr->m_frontMods.push_back(modName);
            }

            spdlog::info("GalleryStateManager: Loaded {} front mods", count);
        }
    }

    static void OnRevert(SKSE::SerializationInterface*)
    {
        auto* mgr = GetSingleton();
        std::lock_guard<std::mutex> lock(mgr->m_mutex);
        mgr->m_frontMods.clear();
        spdlog::info("GalleryStateManager: Cleared on revert");
    }

private:
    GalleryStateManager() = default;
    ~GalleryStateManager() = default;
    GalleryStateManager(const GalleryStateManager&) = delete;
    GalleryStateManager& operator=(const GalleryStateManager&) = delete;

    // Mods pushed to front by user interaction (most recent first)
    std::vector<std::string> m_frontMods;

    mutable std::mutex m_mutex;
};
