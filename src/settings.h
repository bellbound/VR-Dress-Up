#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include "log.h"

class Settings
{
public:
    static Settings* GetSingleton()
    {
        static Settings instance;
        return &instance;
    }

    // Load settings from INI file
    void Load()
    {
        // Build INI path: Data\SKSE\Plugins\DressUpVR.ini
        char pathBuffer[MAX_PATH];
        GetModuleFileNameA(nullptr, pathBuffer, MAX_PATH);
        std::string exePath(pathBuffer);
        std::string dataPath = exePath.substr(0, exePath.rfind('\\')) + "\\Data\\SKSE\\Plugins\\DressUpVR.ini";
        m_iniPath = dataPath;

        spdlog::info("Settings: Loading from '{}'", m_iniPath);

        // [General] section
        m_enableModGallery = GetPrivateProfileIntA("General", "bEnableModGallery", 0, m_iniPath.c_str()) != 0;
        m_enableCategoryGallery = GetPrivateProfileIntA("General", "bEnableCategoryGallery", 1, m_iniPath.c_str()) != 0;

        // [Outfit] section
        m_useOutfitBackend = GetPrivateProfileIntA("Outfit", "bUseOutfitBackend", 1, m_iniPath.c_str()) != 0;
        m_outfitBackendUniqueOnly = GetPrivateProfileIntA("Outfit", "bOutfitBackendUniqueOnly", 1, m_iniPath.c_str()) != 0;
        m_severActionsCompat = GetPrivateProfileIntA("Outfit", "bSeverActionsCompat", 1, m_iniPath.c_str()) != 0;

        // [Reapply] section
        m_reapplyOnExternalChange =
            GetPrivateProfileIntA("Reapply", "bReapplyOnExternalChange", 1, m_iniPath.c_str()) != 0;
        m_reapplyDelayMs = ReadClamped("Reapply", "iReapplyDelayMs", 750, 100, 10000);
        m_reapplySustainedIntervalSec = ReadClamped("Reapply", "iReapplySustainedIntervalSec", 10, 1, 600);
        m_reapplyBurstAllowance = ReadClamped("Reapply", "iReapplyBurstAllowance", 5, 1, 100);
        m_reapplyBackoffSec = ReadClamped("Reapply", "iReapplyBackoffSec", 300, 5, 3600);

        spdlog::info("Settings: bEnableModGallery = {}", m_enableModGallery);
        spdlog::info("Settings: bEnableCategoryGallery = {}", m_enableCategoryGallery);
        spdlog::info("Settings: bUseOutfitBackend = {}", m_useOutfitBackend);
        spdlog::info("Settings: bOutfitBackendUniqueOnly = {}", m_outfitBackendUniqueOnly);
        spdlog::info("Settings: bSeverActionsCompat = {}", m_severActionsCompat);
        spdlog::info("Settings: bReapplyOnExternalChange = {}", m_reapplyOnExternalChange);
        spdlog::info("Settings: iReapplyDelayMs = {}", m_reapplyDelayMs);
        spdlog::info("Settings: iReapplySustainedIntervalSec = {}", m_reapplySustainedIntervalSec);
        spdlog::info("Settings: iReapplyBurstAllowance = {}", m_reapplyBurstAllowance);
        spdlog::info("Settings: iReapplyBackoffSec = {}", m_reapplyBackoffSec);
    }

    // Accessors
    bool IsModGalleryEnabled() const { return m_enableModGallery; }
    bool IsCategoryGalleryEnabled() const { return m_enableCategoryGallery; }
    bool IsOutfitBackendEnabled() const { return m_useOutfitBackend; }
    bool IsOutfitBackendUniqueOnly() const { return m_outfitBackendUniqueOnly; }
    bool IsSeverActionsCompatEnabled() const { return m_severActionsCompat; }

    bool IsReapplyOnExternalChangeEnabled() const { return m_reapplyOnExternalChange; }
    std::int32_t GetReapplyDelayMs() const { return m_reapplyDelayMs; }
    std::int32_t GetReapplySustainedIntervalSec() const { return m_reapplySustainedIntervalSec; }
    std::int32_t GetReapplyBurstAllowance() const { return m_reapplyBurstAllowance; }
    std::int32_t GetReapplyBackoffSec() const { return m_reapplyBackoffSec; }

private:
    Settings() = default;
    ~Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    // A hand-edited INI is the only way these are ever set, so a typo has to land on a
    // usable value rather than on a zero delay or a thousand-second backoff.
    std::int32_t ReadClamped(const char* section, const char* key,
                             std::int32_t fallback, std::int32_t lo, std::int32_t hi) const
    {
        const auto raw = static_cast<std::int32_t>(
            GetPrivateProfileIntA(section, key, fallback, m_iniPath.c_str()));
        const auto clamped = std::clamp(raw, lo, hi);
        if (clamped != raw) {
            spdlog::warn("Settings: {} = {} is out of range [{}, {}], using {}",
                key, raw, lo, hi, clamped);
        }
        return clamped;
    }

    std::string m_iniPath;

    // [General]
    bool m_enableModGallery = false;      // Default: disabled
    bool m_enableCategoryGallery = true;  // Browse by item kind (boots, wigs, ...) across all mods

    // [Outfit]
    bool m_useOutfitBackend = true;         // Assign a real Outfit form on lock (SPID handoff)
    bool m_outfitBackendUniqueOnly = true;  // Skip NPCs whose base record is shared between copies
    bool m_severActionsCompat = true;       // Exclude locked NPCs from the SeverActions outfit system

    // [Reapply] - defending a locked look against whatever else changes it
    bool m_reapplyOnExternalChange = true;      // Put the locked outfit back when something else strips it
    std::int32_t m_reapplyDelayMs = 750;        // Wait this long first, so one burst is one reapply
    std::int32_t m_reapplySustainedIntervalSec = 10;  // Sustained rate we are willing to keep up
    std::int32_t m_reapplyBurstAllowance = 5;   // Reapplies allowed back-to-back before that rate matters
    std::int32_t m_reapplyBackoffSec = 300;     // Stand-down once we conclude we are in a tug-of-war
};
