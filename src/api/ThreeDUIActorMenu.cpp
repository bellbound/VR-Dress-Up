// Consumer-side implementation of P3DUI ActorMenu interface acquisition
// Uses direct DLL export from 3DUI.dll

#include "ThreeDUIActorMenu.h"
#include "../log.h"
#include <Windows.h>

namespace P3DUI {

// Function pointer type for direct DLL export
typedef void* (*GetP3DUIActorMenuInterfaceFunc)(unsigned int revisionNumber);

// Cached interface pointer
static ActorMenuInterface* g_actorMenuInterface = nullptr;

ActorMenuInterface* GetActorMenuInterface() {
    // Return cached interface if already acquired
    if (g_actorMenuInterface) {
        return g_actorMenuInterface;
    }

    // Get interface via direct DLL export
    HMODULE hModule = GetModuleHandleA("3DUI.dll");
    if (!hModule) {
        spdlog::warn("P3DUI::GetActorMenuInterface: 3DUI.dll not loaded");
        return nullptr;
    }

    auto getInterface = reinterpret_cast<GetP3DUIActorMenuInterfaceFunc>(
        GetProcAddress(hModule, "GetP3DUIActorMenuInterface")
    );

    if (!getInterface) {
        spdlog::error("P3DUI::GetActorMenuInterface: GetP3DUIActorMenuInterface export not found in 3DUI.dll");
        spdlog::error("P3DUI::GetActorMenuInterface: Make sure you have the latest version of 3DUI");
        return nullptr;
    }

    g_actorMenuInterface = static_cast<ActorMenuInterface*>(getInterface(1));

    if (g_actorMenuInterface) {
        spdlog::info("P3DUI::GetActorMenuInterface: Obtained ActorMenu interface (version {})",
            g_actorMenuInterface->GetInterfaceVersion());
    } else {
        spdlog::error("P3DUI::GetActorMenuInterface: GetP3DUIActorMenuInterface returned nullptr");
    }

    return g_actorMenuInterface;
}

} // namespace P3DUI
