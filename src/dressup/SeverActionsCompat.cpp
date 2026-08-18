#include "SeverActionsCompat.h"
#include "PapyrusBridge.h"
#include "FormKeyUtil.h"
#include "../settings.h"

#include <Windows.h>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_set>
#include <vector>

namespace SeverActionsCompat
{
    namespace
    {
        bool g_active = false;

        std::mutex                      g_mutex;
        std::unordered_set<std::string> g_excludedByUs;  // actor FormKeys

        void DispatchSetExcluded(RE::Actor* actor, bool excluded)
        {
            PapyrusBridge::CallGlobal("SeverActionsNative", "Native_SetOutfitExcluded",
                RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor),
                                          static_cast<bool>(excluded)));
        }

        // The native setter only writes the flag. SeverActions' own exclusion path also
        // clears the legacy StorageUtil lock mirror, and it does that from Papyrus, off
        // this event - so send it rather than leave a stale mirror behind.
        void ClearLegacyLockMirror(RE::Actor* actor)
        {
            PapyrusBridge::SendModEvent("SeverActions_OutfitExcluded",
                std::to_string(actor->GetFormID()) + "|", 0.0f, actor);
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
        if (!dataHandler || !dataHandler->LookupModByName(kPluginName)) {
            spdlog::info("SeverActionsCompat::Initialize - SeverActions.esp not in load order");
            return;
        }

        g_active = true;
        spdlog::info("SeverActionsCompat::Initialize - detected, locked NPCs will be excluded "
            "from its outfit system while we hold them");
    }

    void TakeOver(RE::Actor* actor)
    {
        if (!g_active || !actor) return;

        const std::string actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
        if (actorKey.empty()) {
            // Nothing to persist against, so we could never tell whose exclusion it was.
            spdlog::info("SeverActionsCompat::TakeOver - '{}' has no source file (dynamic "
                "actor?), leaving the SeverActions outfit system alone", actor->GetName());
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_excludedByUs.count(actorKey) > 0) return;  // already ours
        }

        const RE::FormID actorID = actor->GetFormID();

        // Read before write: an exclusion the player set themselves is not ours to undo
        // on unlock, so in that case we take no ownership of it.
        PapyrusBridge::CallGlobalBool("SeverActionsNative", "Native_GetOutfitExcluded",
            RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor)),
            [actorID, actorKey](bool ok, bool alreadyExcluded) {
                auto* target = RE::TESForm::LookupByID<RE::Actor>(actorID);
                if (!target) return;

                if (ok && alreadyExcluded) {
                    spdlog::info("SeverActionsCompat::TakeOver - '{}' is already outfit-excluded "
                        "in SeverActions, leaving that as the player set it", target->GetName());
                    return;
                }
                if (!ok) {
                    spdlog::warn("SeverActionsCompat::TakeOver - Native_GetOutfitExcluded gave no "
                        "answer for '{}', assuming not excluded", target->GetName());
                }

                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_excludedByUs.insert(actorKey);
                }

                DispatchSetExcluded(target, true);
                ClearLegacyLockMirror(target);

                spdlog::info("SeverActionsCompat::TakeOver - '{}' (0x{:08X}) excluded from the "
                    "SeverActions outfit system for the duration of the lock",
                    target->GetName(), target->GetFormID());
            });
    }

    void Release(RE::Actor* actor)
    {
        if (!g_active || !actor) return;

        const std::string actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
        if (actorKey.empty()) return;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_excludedByUs.erase(actorKey) == 0) return;  // not ours to hand back
        }

        DispatchSetExcluded(actor, false);

        spdlog::info("SeverActionsCompat::Release - '{}' (0x{:08X}) handed back to the "
            "SeverActions outfit system", actor->GetName(), actor->GetFormID());
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

    void ResumeLockAfterMs(RE::Actor* actor, std::int32_t delayMs)
    {
        if (!g_active || !actor) return;

        const RE::FormID actorID = actor->GetFormID();
        PapyrusBridge::RunAfterMs(delayMs, [actorID]() {
            if (auto* target = RE::TESForm::LookupByID<RE::Actor>(actorID)) {
                ResumeLock(target);
            }
        });
    }

    // =============================================================================
    // Serialization
    // =============================================================================

    void OnGameSave(SKSE::SerializationInterface* a_intfc)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (!a_intfc->OpenRecord(kRecord, kSerializationVersion)) {
            spdlog::error("SeverActionsCompat::OnGameSave - Failed to open record");
            return;
        }

        std::uint32_t count = static_cast<std::uint32_t>(g_excludedByUs.size());
        a_intfc->WriteRecordData(&count, sizeof(count));

        for (const auto& actorKey : g_excludedByUs) {
            std::uint32_t len = static_cast<std::uint32_t>(actorKey.size());
            a_intfc->WriteRecordData(&len, sizeof(len));
            if (len > 0) {
                a_intfc->WriteRecordData(actorKey.data(), len);
            }
        }

        spdlog::info("SeverActionsCompat::OnGameSave - Saved {} outfit exclusion(s)", count);
    }

    void OnPreLoad()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_excludedByUs.clear();
    }

    void OnLoadRecord(SKSE::SerializationInterface* a_intfc,
        std::uint32_t type, std::uint32_t version, std::uint32_t length)
    {
        if (type != kRecord) return;

        if (version > kSerializationVersion) {
            spdlog::warn("SeverActionsCompat::OnLoadRecord - Incompatible version {} (expected {}), skipping",
                version, kSerializationVersion);
            if (length > 0) {
                std::vector<char> skipBuffer(length);
                a_intfc->ReadRecordData(skipBuffer.data(), length);
            }
            return;
        }

        std::uint32_t count = 0;
        a_intfc->ReadRecordData(&count, sizeof(count));

        std::lock_guard<std::mutex> lock(g_mutex);

        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t len = 0;
            a_intfc->ReadRecordData(&len, sizeof(len));

            std::string actorKey;
            if (len > 0) {
                actorKey.resize(len);
                a_intfc->ReadRecordData(actorKey.data(), len);
            }

            if (!actorKey.empty()) {
                g_excludedByUs.insert(std::move(actorKey));
            }
        }

        spdlog::info("SeverActionsCompat::OnLoadRecord - Loaded {}/{} outfit exclusion(s)",
            g_excludedByUs.size(), count);
    }

    void OnRevert(SKSE::SerializationInterface*)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_excludedByUs.clear();
    }
}
