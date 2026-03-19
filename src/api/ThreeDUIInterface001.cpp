// Consumer-side implementation of P3DUI interface acquisition
// Uses direct DLL export from 3DUI.dll

#include "ThreeDUIInterface001.h"
#include "../log.h"
#include <Windows.h>

namespace P3DUI {

// Function pointer type for direct DLL export
typedef void* (*GetP3DUIInterfaceFunc)(unsigned int revisionNumber);

// Cached interface pointer
static Interface001* g_p3duiInterface = nullptr;

Interface001* GetInterface001() {
    // Return cached interface if already acquired
    if (g_p3duiInterface) {
        return g_p3duiInterface;
    }

    // Get interface via direct DLL export
    HMODULE hModule = GetModuleHandleA("3DUI.dll");
    if (!hModule) {
        spdlog::warn("P3DUI::GetInterface001: 3DUI.dll not loaded");
        return nullptr;
    }

    auto getInterface = reinterpret_cast<GetP3DUIInterfaceFunc>(
        GetProcAddress(hModule, "GetP3DUIInterface")
    );

    if (!getInterface) {
        spdlog::error("P3DUI::GetInterface001: GetP3DUIInterface export not found in 3DUI.dll");
        return nullptr;
    }

    g_p3duiInterface = static_cast<Interface001*>(getInterface(1));

    if (g_p3duiInterface) {
        spdlog::info("P3DUI::GetInterface001: Obtained interface (version {})",
            g_p3duiInterface->GetInterfaceVersion());
    } else {
        spdlog::error("P3DUI::GetInterface001: GetP3DUIInterface returned nullptr");
    }

    return g_p3duiInterface;
}

} // namespace P3DUI
