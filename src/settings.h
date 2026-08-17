#pragma once

#include <Windows.h>
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

        // [Outfit] section
        m_useOutfitBackend = GetPrivateProfileIntA("Outfit", "bUseOutfitBackend", 1, m_iniPath.c_str()) != 0;
        m_outfitBackendUniqueOnly = GetPrivateProfileIntA("Outfit", "bOutfitBackendUniqueOnly", 1, m_iniPath.c_str()) != 0;
        m_severActionsCompat = GetPrivateProfileIntA("Outfit", "bSeverActionsCompat", 1, m_iniPath.c_str()) != 0;

        spdlog::info("Settings: bEnableModGallery = {}", m_enableModGallery);
        spdlog::info("Settings: bUseOutfitBackend = {}", m_useOutfitBackend);
        spdlog::info("Settings: bOutfitBackendUniqueOnly = {}", m_outfitBackendUniqueOnly);
        spdlog::info("Settings: bSeverActionsCompat = {}", m_severActionsCompat);
    }

    // Accessors
    bool IsModGalleryEnabled() const { return m_enableModGallery; }
    bool IsOutfitBackendEnabled() const { return m_useOutfitBackend; }
    bool IsOutfitBackendUniqueOnly() const { return m_outfitBackendUniqueOnly; }
    bool IsSeverActionsCompatEnabled() const { return m_severActionsCompat; }

private:
    Settings() = default;
    ~Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    std::string m_iniPath;

    // [General]
    bool m_enableModGallery = false;  // Default: disabled

    // [Outfit]
    bool m_useOutfitBackend = true;         // Assign a real Outfit form on lock (SPID handoff)
    bool m_outfitBackendUniqueOnly = true;  // Skip NPCs whose base record is shared between copies
    bool m_severActionsCompat = true;       // Hand locked outfits to SeverActions when installed
};
