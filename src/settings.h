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

        spdlog::info("Settings: bEnableModGallery = {}", m_enableModGallery);
    }

    // Accessors
    bool IsModGalleryEnabled() const { return m_enableModGallery; }

private:
    Settings() = default;
    ~Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    std::string m_iniPath;

    // [General]
    bool m_enableModGallery = false;  // Default: disabled
};
