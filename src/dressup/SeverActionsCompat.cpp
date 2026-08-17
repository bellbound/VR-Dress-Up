#include "SeverActionsCompat.h"
#include "PapyrusBridge.h"
#include "../settings.h"

#include <Windows.h>
#include <spdlog/spdlog.h>

namespace SeverActionsCompat
{
    namespace
    {
        bool g_active = false;

        std::string ActorArg(RE::Actor* actor, const char* preset)
        {
            // SeverActions decodes "actorName|arg"; the sender form is authoritative
            // and the name is only its fuzzy fallback.
            const char* name = actor ? actor->GetName() : "";
            return std::string(name ? name : "") + "|" + preset;
        }
    }

    void Initialize()
    {
        g_active = false;

        if (!Settings::GetSingleton()->IsSeverActionsCompatEnabled()) {
            spdlog::info("SeverActionsCompat::Initialize - Disabled by bSeverActionsCompat");
            return;
        }

        if (!GetModuleHandleA("SeverActionsNative.dll")) {
            spdlog::info("SeverActionsCompat::Initialize - SeverActionsNative.dll not loaded");
            return;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler || !dataHandler->LookupModByName("SeverActions.esp")) {
            spdlog::info("SeverActionsCompat::Initialize - SeverActions.esp not in load order");
            return;
        }

        g_active = true;
        spdlog::info("SeverActionsCompat::Initialize - detected, handing locked outfits to preset '{}'",
            kPresetName);
    }

    bool IsActive()
    {
        return g_active;
    }

    void HandOffOutfit(RE::Actor* actor)
    {
        if (!g_active || !actor) return;

        spdlog::info("SeverActionsCompat::HandOffOutfit - Handing '{}' (0x{:08X}) to SeverActions",
            actor->GetName(), actor->GetFormID());

        PapyrusBridge::SendModEvent("SeverActions_PrismaSavePreset",
            ActorArg(actor, kPresetName), 0.0f, actor);
    }

    void ReleaseOutfit(RE::Actor* actor)
    {
        if (!g_active || !actor) return;

        spdlog::info("SeverActionsCompat::ReleaseOutfit - Releasing '{}' (0x{:08X})",
            actor->GetName(), actor->GetFormID());

        PapyrusBridge::SendModEvent("SeverActions_PrismaDeletePreset",
            ActorArg(actor, kPresetName), 0.0f, actor);
    }

    void SuspendLock(RE::Actor* actor)
    {
        if (!g_active || !actor) return;
        PapyrusBridge::CallGlobal("SeverActionsNativeExt", "Native_Outfit_SuspendLock",
            RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor)));
    }

    void ResumeLock(RE::Actor* actor)
    {
        if (!g_active || !actor) return;
        PapyrusBridge::CallGlobal("SeverActionsNativeExt", "Native_Outfit_ResumeLock",
            RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor)));
    }
}
