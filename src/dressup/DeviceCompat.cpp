#include "DeviceCompat.h"
#include "PapyrusBridge.h"

#include <atomic>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <spdlog/spdlog.h>

namespace DeviceCompat
{
    namespace
    {
        // Keyword every inventory device carries, and nothing else does.
        constexpr const char* kInventoryDeviceKeyword = "zad_InventoryDevice";

        // Quest devices. Absent from stock DD assets, used by the DD-based quest mods.
        constexpr const char* kQuestItemKeyword = "zad_QuestItem";

        // Rendered devices are identified by any keyword under this prefix - collected at
        // runtime rather than listed here, so devices added by DD Expansion, Contraptions
        // or a third-party pack are covered without us tracking their keyword lists.
        constexpr std::string_view kTypeKeywordPrefix = "zad_Devious";

        // The Papyrus script that owns DD's public equip/remove API.
        constexpr const char* kLibsScript = "zadlibs";

        struct State
        {
            bool                             resolved = false;
            RE::BGSKeyword*                  inventoryDevice = nullptr;
            RE::BGSKeyword*                  questItem = nullptr;
            std::unordered_set<RE::FormID>   typeKeywords;
        };

        State      g_state;
        std::mutex g_stateMutex;

        // The classification calls read g_state without the lock, on the game thread, for
        // every armour equip event. Release/acquire on this flag is what makes that safe:
        // it is stored once, last, after every write to g_state, so a thread that sees it
        // set also sees the keywords behind it.
        std::atomic<bool> g_installed{false};

        // The quest carrying zadlibs. Found by asking the VM which quest has the script
        // bound, rather than by FormID, so a DD update or a rebuilt ESM cannot break it.
        // Resolved lazily: script instances only exist once a save is loaded.
        std::atomic<RE::FormID> g_libsQuest{0};

        bool HasKeyword(RE::TESObjectARMO* armor, RE::BGSKeyword* keyword)
        {
            if (!armor || !keyword) return false;
            for (std::uint32_t i = 0; i < armor->numKeywords; ++i) {
                if (armor->keywords[i] == keyword) return true;
            }
            return false;
        }

        bool HasTypeKeyword(RE::TESObjectARMO* armor)
        {
            if (!armor) return false;
            for (std::uint32_t i = 0; i < armor->numKeywords; ++i) {
                auto* keyword = armor->keywords[i];
                if (keyword && g_state.typeKeywords.contains(keyword->GetFormID())) {
                    return true;
                }
            }
            return false;
        }

        RE::TESQuest* ResolveLibsQuest()
        {
            if (const auto cached = g_libsQuest.load(std::memory_order_relaxed); cached != 0) {
                if (auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(cached)) {
                    return quest;
                }
                g_libsQuest.store(0, std::memory_order_relaxed);
            }

            auto* quest = PapyrusBridge::FindQuestWithScript(kLibsScript);
            if (!quest) {
                // Not an error on its own: nothing is bound before a save is loaded, and
                // the next call retries.
                return nullptr;
            }

            g_libsQuest.store(quest->GetFormID(), std::memory_order_relaxed);
            spdlog::info("DeviceCompat - Found '{}' on quest 0x{:08X}", kLibsScript, quest->GetFormID());
            return quest;
        }
    }

    void Initialize()
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_state.resolved) return;
        g_state.resolved = true;

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            spdlog::warn("DeviceCompat::Initialize - No TESDataHandler");
            return;
        }

        for (auto* form : dataHandler->GetFormArray<RE::BGSKeyword>()) {
            auto* keyword = form ? form->As<RE::BGSKeyword>() : nullptr;
            if (!keyword) continue;

            const char* editorID = keyword->GetFormEditorID();
            if (!editorID || !*editorID) continue;

            const std::string_view name{editorID};
            if (name == kInventoryDeviceKeyword) {
                g_state.inventoryDevice = keyword;
            } else if (name == kQuestItemKeyword) {
                g_state.questItem = keyword;
            } else if (name.starts_with(kTypeKeywordPrefix)) {
                g_state.typeKeywords.insert(keyword->GetFormID());
            }
        }

        // The inventory-device keyword is the load-bearing one: without it we cannot tell
        // the two halves apart, and guessing is worse than standing down.
        if (!g_state.inventoryDevice) {
            spdlog::info("DeviceCompat - Devious Devices not installed, device handling disabled");
            return;
        }

        g_installed.store(true, std::memory_order_release);
        spdlog::info("DeviceCompat - Devious Devices detected: {} device-type keyword(s), quest keyword {}",
            g_state.typeKeywords.size(), g_state.questItem ? "found" : "absent");
    }

    bool IsInstalled()
    {
        return g_installed.load(std::memory_order_acquire);
    }

    bool IsInventoryDevice(RE::TESObjectARMO* armor)
    {
        if (!IsInstalled() || !armor) return false;
        return HasKeyword(armor, g_state.inventoryDevice);
    }

    bool IsRenderedDevice(RE::TESObjectARMO* armor)
    {
        if (!IsInstalled() || !armor) return false;
        if (HasKeyword(armor, g_state.inventoryDevice)) return false;
        if (!HasTypeKeyword(armor)) return false;

        // The name is the tiebreaker against DD's two pre-4.0 leftovers - zad_piercingNSoul
        // and zad_piercingVSoul - which carry a type keyword but are standalone named
        // armour with no script and no inventory half. Those behave like ordinary clothing
        // and must keep doing so; every real rendered device is nameless.
        const char* name = armor->GetFullName();
        return !name || !*name;
    }

    bool IsDevice(RE::TESObjectARMO* armor)
    {
        return IsInventoryDevice(armor) || IsRenderedDevice(armor);
    }

    bool IsQuestDevice(RE::TESObjectARMO* armor)
    {
        if (!IsInstalled() || !armor || !g_state.questItem) return false;
        return HasKeyword(armor, g_state.questItem);
    }

    bool Equip(RE::Actor* actor, RE::TESObjectARMO* inventoryDevice, bool force)
    {
        if (!actor || !inventoryDevice) return false;
        if (!IsInventoryDevice(inventoryDevice)) return false;

        auto* quest = ResolveLibsQuest();
        if (!quest) {
            spdlog::warn("DeviceCompat::Equip - '{}' is bound to no quest yet, cannot lock '{}' onto '{}'",
                kLibsScript, inventoryDevice->GetFullName(), actor->GetName());
            return false;
        }

        // LockDevice(actor akActor, armor deviceInventory, bool force)
        auto* args = RE::MakeFunctionArguments(
            static_cast<RE::Actor*>(actor),
            static_cast<RE::TESObjectARMO*>(inventoryDevice),
            static_cast<bool>(force));

        const bool ok = PapyrusBridge::CallMethod(quest, RE::FormType::Quest,
            kLibsScript, "LockDevice", args);

        spdlog::debug("DeviceCompat::Equip - {} zadlibs.LockDevice('{}') for '{}'",
            ok ? "Dispatched" : "Failed to dispatch", inventoryDevice->GetFullName(), actor->GetName());
        return ok;
    }

    bool Unequip(RE::Actor* actor, RE::TESObjectARMO* inventoryDevice)
    {
        if (!actor || !inventoryDevice) return false;
        if (!IsInventoryDevice(inventoryDevice)) return false;

        if (IsQuestDevice(inventoryDevice)) {
            spdlog::debug("DeviceCompat::Unequip - '{}' on '{}' is a quest device; leaving it locked on",
                inventoryDevice->GetFullName(), actor->GetName());
            return false;
        }

        auto* quest = ResolveLibsQuest();
        if (!quest) {
            spdlog::warn("DeviceCompat::Unequip - '{}' is bound to no quest yet, cannot unlock '{}' from '{}'",
                kLibsScript, inventoryDevice->GetFullName(), actor->GetName());
            return false;
        }

        // UnlockDevice(actor akActor, armor deviceInventory, armor deviceRendered,
        //              keyword zad_DeviousDevice, bool destroyDevice, bool genericonly)
        //
        // The rendered device and the keyword are optional speed-ups DD looks up itself
        // when they are None, and we have no way to read them from the item's script
        // properties. destroyDevice stays false: the device goes back to being an ordinary
        // inventory item so it can be put on again. genericonly stays false so a
        // zad_BlockGeneric device still comes off - the player asked for it explicitly.
        auto* args = RE::MakeFunctionArguments(
            static_cast<RE::Actor*>(actor),
            static_cast<RE::TESObjectARMO*>(inventoryDevice),
            static_cast<RE::TESObjectARMO*>(nullptr),
            static_cast<RE::BGSKeyword*>(nullptr),
            static_cast<bool>(false),
            static_cast<bool>(false));

        const bool ok = PapyrusBridge::CallMethod(quest, RE::FormType::Quest,
            kLibsScript, "UnlockDevice", args);

        spdlog::debug("DeviceCompat::Unequip - {} zadlibs.UnlockDevice('{}') for '{}'",
            ok ? "Dispatched" : "Failed to dispatch", inventoryDevice->GetFullName(), actor->GetName());
        return ok;
    }
}
