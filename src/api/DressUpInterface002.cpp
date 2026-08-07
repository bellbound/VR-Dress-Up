// Provider-side implementation of DressUp interface v002.
//
// v001 stays exactly as it was - its vtable is frozen. This is a separate object
// returned by the same GetDressUpInterface() export when version 2 is requested.

#include "DressUpInterface002.h"

#include <mutex>
#include <string>
#include <vector>

#include <Windows.h>

#include "../dressup/OutfitLockManager.h"
#include "../log.h"
#include "DressUpInterface001.h"

namespace DressUp {

constexpr uint32_t BUILD_NUMBER_002 = 1;

// Shared with the v001 implementation, so both interfaces agree on the toggle.
bool IsMenuOpeningEnabledInternal();
void SetMenuOpeningEnabledInternal(bool enabled);

namespace {

/// Backing store for the borrowed `StringList` results.
///
/// A `std::vector<std::string>` cannot cross a DLL boundary safely - the two sides
/// may be built against different standard libraries. So results are flattened into
/// stable `const char*` storage owned here and handed out as a borrowed view, valid
/// until the next call. Documented on the interface so consumers copy before
/// calling again.
class StringListStorage {
public:
    StringList Set(const std::vector<std::string>& values) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_strings = values;
        m_pointers.clear();
        m_pointers.reserve(m_strings.size());
        for (const auto& value : m_strings) {
            m_pointers.push_back(value.c_str());
        }
        StringList list;
        list.items = m_pointers.empty() ? nullptr : m_pointers.data();
        list.count = static_cast<uint32_t>(m_pointers.size());
        return list;
    }

private:
    std::mutex m_mutex;
    std::vector<std::string> m_strings;
    std::vector<const char*> m_pointers;
};

StringListStorage g_outfitNames;
StringListStorage g_outfitItems;
StringListStorage g_playerItems;

std::vector<std::string> ToVector(const char* const* items, uint32_t count) {
    std::vector<std::string> result;
    if (!items) {
        return result;
    }
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i]) {
            result.emplace_back(items[i]);
        }
    }
    return result;
}

}  // namespace

class Interface002Impl : public Interface002 {
public:
    uint32_t GetVersion() override { return DRESSUP_INTERFACE_VERSION_002; }
    uint32_t GetBuild() override { return BUILD_NUMBER_002; }

    StringList EnumerateOutfits(RE::Actor* actor) override {
        if (!actor) {
            return g_outfitNames.Set({});
        }
        return g_outfitNames.Set(OutfitLockManager::GetSingleton()->EnumerateOutfitNames(actor));
    }

    StringList EnumerateOutfitItems(RE::Actor* actor, const char* outfitName) override {
        if (!actor || !outfitName) {
            return g_outfitItems.Set({});
        }
        return g_outfitItems.Set(
            OutfitLockManager::GetSingleton()->GetOutfitItemFormKeys(actor, outfitName));
    }

    StringList EnumeratePlayerGivenItems(RE::Actor* actor) override {
        if (!actor) {
            return g_playerItems.Set({});
        }
        return g_playerItems.Set(OutfitLockManager::GetSingleton()->GetPlayerGivenFormKeys(actor));
    }

    uint32_t SetOutfitByFormKeys(RE::Actor* actor, const char* outfitName,
                                 const char* const* formKeys, uint32_t count) override {
        if (!actor || !outfitName) {
            return 0;
        }
        return OutfitLockManager::GetSingleton()->SetOutfitFromFormKeys(
            actor, outfitName, ToVector(formKeys, count));
    }

    uint32_t MarkPlayerGivenByFormKeys(RE::Actor* actor, const char* const* formKeys,
                                       uint32_t count) override {
        if (!actor) {
            return 0;
        }
        return OutfitLockManager::GetSingleton()->MarkPlayerGivenFromFormKeys(
            actor, ToVector(formKeys, count));
    }

    uint32_t EnsureOutfitItemsInInventory(RE::Actor* actor, const char* outfitName) override {
        if (!actor || !outfitName) {
            return 0;
        }
        return OutfitLockManager::GetSingleton()->EnsureOutfitItemsInInventory(actor, outfitName);
    }

    bool ApplyOutfitNow(RE::Actor* actor, const char* outfitName, bool unequipOthers) override {
        if (!actor || !outfitName) {
            return false;
        }
        return OutfitLockManager::GetSingleton()->ApplyOutfit(actor, outfitName, unequipOthers);
    }

    void SetOpenDressMenuEnabled(bool enabled) override { SetMenuOpeningEnabledInternal(enabled); }
    bool IsOpenDressMenuEnabled() override { return IsMenuOpeningEnabledInternal(); }

    bool LockActor(RE::Actor* actor) override {
        return actor && OutfitLockManager::GetSingleton()->Lock(actor);
    }
    bool UnlockActor(RE::Actor* actor) override {
        return actor && OutfitLockManager::GetSingleton()->Unlock(actor);
    }
    bool IsActorLocked(RE::Actor* actor) override {
        return actor && OutfitLockManager::GetSingleton()->IsLocked(actor);
    }
};

namespace {
Interface002Impl g_interface002;
}

Interface002* GetInterface002Impl() { return &g_interface002; }

}  // namespace DressUp
