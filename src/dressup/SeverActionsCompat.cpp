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

    namespace
    {
        // How long to keep asking SeverActions for the preset it is building.
        constexpr int kAcquireAttempts = 6;
        constexpr int kAcquireDelayMs = 1000;

        void AcquireAttempt(RE::FormID actorID, int attempt,
                            std::function<void(RE::BGSOutfit*)> done);

        void GiveUp(int attempt, const char* why, std::function<void(RE::BGSOutfit*)>& done)
        {
            spdlog::info("SeverActionsCompat::AcquirePresetOutfit - {} (attempt {}/{})",
                why, attempt, kAcquireAttempts);
            done(nullptr);
        }

        void Retry(RE::FormID actorID, int attempt, const char* why,
                   std::function<void(RE::BGSOutfit*)> done)
        {
            if (attempt >= kAcquireAttempts) {
                GiveUp(attempt, why, done);
                return;
            }
            spdlog::trace("SeverActionsCompat::AcquirePresetOutfit - {}, retrying", why);
            PapyrusBridge::RunAfterMs(kAcquireDelayMs, [actorID, attempt, done]() mutable {
                AcquireAttempt(actorID, attempt + 1, std::move(done));
            });
        }

        void AcquireAttempt(RE::FormID actorID, int attempt,
                            std::function<void(RE::BGSOutfit*)> done)
        {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorID);
            if (!actor) {
                done(nullptr);
                return;
            }

            // 1. Which slot did SeverActions give this actor?
            PapyrusBridge::CallGlobalInt("SeverActionsNative", "Native_OutfitSlot_GetSlot",
                RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor)),
                [actorID, attempt, done](bool ok, std::int32_t slotIdx) mutable {
                    if (!ok || slotIdx < 0) {
                        Retry(actorID, attempt, "no slot assigned yet", std::move(done));
                        return;
                    }

                    auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorID);
                    auto* dataHandler = RE::TESDataHandler::GetSingleton();
                    auto* questForm = dataHandler ? dataHandler->LookupForm(kQuestID, kPluginName) : nullptr;
                    if (!actor || !questForm) {
                        done(nullptr);
                        return;
                    }

                    // 2. Which preset index is ours?
                    PapyrusBridge::CallMethodInt(questForm, RE::FormType::Quest,
                        "SeverActions_OutfitSlot", "FindPresetIndexByName",
                        RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor),
                                                  RE::BSFixedString(kPresetName)),
                        [actorID, attempt, slotIdx, done](bool ok, std::int32_t presetIdx) mutable {
                            if (!ok || presetIdx < 0) {
                                Retry(actorID, attempt, "preset not built yet", std::move(done));
                                return;
                            }

                            // 3. Hand us the OTFT record behind it.
                            PapyrusBridge::CallGlobalForm("SeverActionsNative",
                                "Native_OutfitSlot_GetOutfitForm", RE::FormType::Outfit,
                                RE::MakeFunctionArguments(static_cast<std::int32_t>(slotIdx),
                                                          static_cast<std::int32_t>(presetIdx)),
                                [actorID, attempt, slotIdx, presetIdx, done](RE::TESForm* form) mutable {
                                    auto* outfit = form ? form->As<RE::BGSOutfit>() : nullptr;
                                    if (!outfit) {
                                        Retry(actorID, attempt, "no outfit record for slot", std::move(done));
                                        return;
                                    }
                                    spdlog::info("SeverActionsCompat::AcquirePresetOutfit - "
                                        "slot {} preset {} -> outfit 0x{:08X}",
                                        slotIdx, presetIdx, outfit->GetFormID());
                                    done(outfit);
                                });
                        });
                });
        }
    }

    void AcquirePresetOutfit(RE::Actor* actor, std::function<void(RE::BGSOutfit*)> done)
    {
        if (!g_active || !actor || !done) {
            if (done) done(nullptr);
            return;
        }
        AcquireAttempt(actor->GetFormID(), 1, std::move(done));
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
