#include "WeaponLockManager.h"

#include "FormKeyUtil.h"
#include "ItemEquipHelper.h"
#include "PapyrusBridge.h"

#include <chrono>
#include <spdlog/spdlog.h>
#include <utility>
#include <vector>

namespace
{
    std::int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // The engine fills empty hands with a hand-to-hand WEAP form, and every creature attack
    // - a wolf's bite, a troll's claws - is one too. Neither is an item anyone can take off,
    // so counting them as weapons means watching an actor we could never satisfy and
    // striking out against a fight that was never happening.
    bool IsRemovableWeapon(RE::TESObjectWEAP* weapon)
    {
        return weapon && weapon->GetWeaponType() != RE::WEAPON_TYPE::kHandToHandMelee;
    }

    std::vector<RE::TESObjectWEAP*> HeldWeapons(RE::Actor* actor)
    {
        auto weapons = ItemEquipHelper::GetEquippedWeapons(actor);
        std::erase_if(weapons, [](RE::TESObjectWEAP* weapon) { return !IsRemovableWeapon(weapon); });
        return weapons;
    }
}

void WeaponLockManager::Initialize()
{
    if (m_initialized) {
        return;
    }

    auto* sourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!sourceHolder) {
        spdlog::error("WeaponLockManager::Initialize - Failed to get ScriptEventSourceHolder");
        return;
    }

    sourceHolder->AddEventSink<RE::TESEquipEvent>(this);
    sourceHolder->AddEventSink<RE::TESActorLocationChangeEvent>(this);
    m_initialized = true;

    spdlog::info("WeaponLockManager::Initialize - Registered for TESEquipEvent and "
        "TESActorLocationChangeEvent");
}

// =============================================================================
// Events
// =============================================================================

RE::BSEventNotifyControl WeaponLockManager::ProcessEvent(
    const RE::TESEquipEvent* a_event,
    RE::BSTEventSource<RE::TESEquipEvent>*)
{
    // This runs on the game thread for every equip in the loaded area, so it is ordered
    // cheapest test first: a flag on the event, then one atomic load, and only then
    // anything that locks a map or resolves a form.
    if (!a_event || !a_event->equipped) {
        return RE::BSEventNotifyControl::kContinue;
    }

    if (m_enforcedCount.load(std::memory_order_relaxed) == 0) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* actor = a_event->actor ? a_event->actor->As<RE::Actor>() : nullptr;
    if (!actor || actor->IsPlayerRef()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_watched.find(actor->GetFormID());
        if (it == m_watched.end() || it->second.surrendered) {
            return RE::BSEventNotifyControl::kContinue;
        }
    }

    // Only now is it worth resolving the form. A shield, a torch and a spell all arrive
    // here as equips and none of them are weapons, so none of them are ours to take away.
    auto* form = RE::TESForm::LookupByID(a_event->baseObject);
    if (!form || !IsRemovableWeapon(form->As<RE::TESObjectWEAP>())) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // We only ever unequip, so an equip is by definition somebody else's doing.
    NoteWeaponEquipped(actor);

    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl WeaponLockManager::ProcessEvent(
    const RE::TESActorLocationChangeEvent* a_event,
    RE::BSTEventSource<RE::TESActorLocationChangeEvent>*)
{
    if (!a_event || !a_event->actor) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // Same trigger the outfit lock uses: the player arriving somewhere is when the NPCs
    // there become ours to look at again.
    auto* actor = a_event->actor->As<RE::Actor>();
    if (!actor || !actor->IsPlayerRef()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    Sweep();

    return RE::BSEventNotifyControl::kContinue;
}

// =============================================================================
// State
// =============================================================================

void WeaponLockManager::Enforce(RE::Actor* actor)
{
    if (!actor || actor->IsPlayerRef()) {
        return;
    }

    const RE::FormID actorID = actor->GetFormID();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& state = m_watched[actorID];
        state.surrendered = false;
        state.strikes = 0;
        state.lastRemovalMs = 0;
        state.drawnPolls = 0;
        if (state.actorKey.empty()) {
            state.actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
        }
        RecountLocked();
    }

    spdlog::info("WeaponLockManager::Enforce - '{}' (0x{:08X}) is to keep their hands empty",
        actor->GetName(), actorID);
}

void WeaponLockManager::StopEnforcing(RE::Actor* actor)
{
    if (!actor) {
        return;
    }

    const RE::FormID actorID = actor->GetFormID();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Erasing is enough to cancel a check in flight: watch IDs are never reused, so the
        // timer will not match anything when it fires.
        if (m_watched.erase(actorID) == 0) {
            return;
        }
        RecountLocked();
    }

    spdlog::info("WeaponLockManager::StopEnforcing - '{}' (0x{:08X}) may hold whatever they like",
        actor->GetName(), actorID);
}

bool WeaponLockManager::IsEnforcing(RE::Actor* actor) const
{
    if (!actor) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_watched.find(actor->GetFormID());
    return it != m_watched.end() && !it->second.surrendered;
}

void WeaponLockManager::NoteMenuWeaponChange(RE::Actor* actor, bool nowHolding)
{
    if (!actor || actor->IsPlayerRef()) {
        return;
    }

    if (nowHolding) {
        StopEnforcing(actor);
        return;
    }

    // They are empty-handed because the player just emptied them - but only if this was the
    // last one. Anything still in the other hand means the player took a weapon off an
    // armed NPC, which is not a request for empty hands.
    if (!HeldWeapons(actor).empty()) {
        return;
    }

    Enforce(actor);
}

void WeaponLockManager::Sweep()
{
    struct Scheduled
    {
        RE::FormID actorID;
        std::uint64_t watch;
    };
    std::vector<Scheduled> scheduled;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [actorID, state] : m_watched) {
            if (state.surrendered || state.watch != 0) {
                continue;
            }
            state.watch = m_nextWatch++;
            state.drawnPolls = 0;
            scheduled.push_back({ actorID, state.watch });
        }
    }

    for (const auto& item : scheduled) {
        ScheduleCheck(item.actorID, item.watch, kSettleMs);
    }

    if (!scheduled.empty()) {
        spdlog::debug("WeaponLockManager::Sweep - Looking at {} NPC(s)", scheduled.size());
    }
}

// =============================================================================
// Enforcement
// =============================================================================

void WeaponLockManager::NoteWeaponEquipped(RE::Actor* actor)
{
    const RE::FormID actorID = actor->GetFormID();
    const char* actorName = actor->GetName();

    std::uint64_t watch = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_watched.find(actorID);
        if (it == m_watched.end() || it->second.surrendered) {
            return;
        }
        auto& state = it->second;

        if (state.lastRemovalMs != 0) {
            if (NowMs() - state.lastRemovalMs <= kStayOffMs) {
                // The last removal did not stick. Enough of those in a row and we are not
                // enforcing a rule, we are just losing an argument in a loop.
                ++state.strikes;
                if (state.strikes > kMaxStrikes) {
                    state.surrendered = true;
                    state.watch = 0;
                    RecountLocked();
                    spdlog::warn("WeaponLockManager - Giving up on '{}' (0x{:08X}): a weapon came "
                        "back within {}s of removal {} times, so something else owns their hands",
                        actorName, actorID, kStayOffMs / 1000, state.strikes);
                    return;
                }
            } else {
                // It stayed off. Whatever is arming them now is a fresh attempt, not the
                // same fight continuing.
                state.strikes = 0;
            }
        }

        if (state.watch != 0) {
            return;  // already waiting on this actor
        }

        state.watch = m_nextWatch++;
        state.drawnPolls = 0;
        watch = state.watch;
    }

    ScheduleCheck(actorID, watch, kSettleMs);
}

void WeaponLockManager::ScheduleCheck(RE::FormID actorID, std::uint64_t watch, std::int32_t delayMs)
{
    // RunAfterMs sleeps off the game thread and hands the body back to the task interface,
    // so RunCheck itself runs where it is safe to read actor state and drive equips.
    PapyrusBridge::RunAfterMs(delayMs, [actorID, watch]() {
        WeaponLockManager::GetSingleton()->RunCheck(actorID, watch);
    });
}

void WeaponLockManager::RunCheck(RE::FormID actorID, std::uint64_t watch)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_watched.find(actorID);
        if (it == m_watched.end() || it->second.surrendered || it->second.watch != watch) {
            return;  // released, given up on, or left behind by a loaded save
        }
    }

    auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorID);
    if (!actor || actor->IsDead()) {
        EndWatch(actorID, watch);
        return;
    }

    auto weapons = HeldWeapons(actor);
    if (weapons.empty()) {
        // They put it away themselves, or the equip never landed. Either way we are done
        // until the next one.
        EndWatch(actorID, watch);
        return;
    }

    // Weapon out: they are using it, and taking it off now is both rude and a good way to
    // strand the animation graph. An unloaded actor is exempt from that courtesy - there is
    // nobody to see the swap, and their drawn flag can sit set indefinitely.
    if (actor->AsActorState()->IsWeaponDrawn() && actor->Is3DLoaded()) {
        std::uint32_t polls = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_watched.find(actorID);
            if (it == m_watched.end() || it->second.watch != watch) {
                return;
            }
            polls = ++it->second.drawnPolls;
        }

        if (polls >= kMaxDrawnPolls) {
            spdlog::info("WeaponLockManager - '{}' (0x{:08X}) has had a weapon drawn for {}s; "
                "dropping the watch until the next sweep",
                actor->GetName(), actorID,
                (static_cast<std::int64_t>(polls) * kDrawnRecheckMs) / 1000);
            EndWatch(actorID, watch);
            return;
        }

        ScheduleCheck(actorID, watch, kDrawnRecheckMs);
        return;
    }

    for (auto* weapon : weapons) {
        ItemEquipHelper::UnequipItem(actor, weapon);
        spdlog::debug("WeaponLockManager - Took '{}' off '{}' (0x{:08X})",
            weapon->GetFullName(), actor->GetName(), actorID);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_watched.find(actorID);
        if (it == m_watched.end() || it->second.watch != watch) {
            return;
        }
        it->second.lastRemovalMs = NowMs();
        it->second.drawnPolls = 0;
        it->second.watch = 0;
    }
}

void WeaponLockManager::EndWatch(RE::FormID actorID, std::uint64_t watch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_watched.find(actorID);
    if (it != m_watched.end() && it->second.watch == watch) {
        it->second.watch = 0;
        it->second.drawnPolls = 0;
    }
}

void WeaponLockManager::RecountLocked()
{
    int count = 0;
    for (const auto& [actorID, state] : m_watched) {
        if (!state.surrendered) {
            ++count;
        }
    }
    m_enforcedCount.store(count, std::memory_order_relaxed);
}

// =============================================================================
// Serialization
// =============================================================================

void WeaponLockManager::OnGameSave(SKSE::SerializationInterface* a_intfc)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    // Only the player's instruction is worth keeping. Strike counts and in-flight watches
    // are this session's business, and a reload is a fair moment to give an NPC we gave up
    // on another chance.
    std::vector<std::string> keys;
    keys.reserve(mgr->m_watched.size());

    for (const auto& [actorID, state] : mgr->m_watched) {
        std::string key = state.actorKey;
        if (key.empty()) {
            if (auto* form = RE::TESForm::LookupByID(actorID)) {
                key = Persistence::FormKeyUtil::BuildFormKey(form);
            }
        }
        if (key.empty()) {
            spdlog::warn("WeaponLockManager::OnGameSave - Actor 0x{:08X} has no source file "
                "(dynamic?); its weapon lock will not survive the save", actorID);
            continue;
        }
        keys.push_back(std::move(key));
    }

    if (!a_intfc->OpenRecord(kRecord, kSerializationVersion)) {
        spdlog::error("WeaponLockManager::OnGameSave - Failed to open record");
        return;
    }

    std::uint32_t count = static_cast<std::uint32_t>(keys.size());
    a_intfc->WriteRecordData(&count, sizeof(count));

    for (const auto& key : keys) {
        std::uint32_t keyLen = static_cast<std::uint32_t>(key.size());
        a_intfc->WriteRecordData(&keyLen, sizeof(keyLen));
        a_intfc->WriteRecordData(key.data(), keyLen);
    }

    spdlog::info("WeaponLockManager::OnGameSave - Saved {} weapon lock(s)", count);
}

void WeaponLockManager::OnPreLoad()
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    // m_nextWatch deliberately keeps climbing: any timer still sleeping from the previous
    // save must not match a state we are about to load.
    mgr->m_watched.clear();
    mgr->RecountLocked();

    spdlog::debug("WeaponLockManager::OnPreLoad - Cleared state");
}

void WeaponLockManager::OnLoadRecord(SKSE::SerializationInterface* a_intfc,
    std::uint32_t type, std::uint32_t version, std::uint32_t length)
{
    if (type != kRecord) {
        return;
    }

    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    if (version != kSerializationVersion) {
        spdlog::warn("WeaponLockManager::OnLoadRecord - Version mismatch: {} vs {}, skipping",
            version, kSerializationVersion);
        if (length > 0) {
            std::vector<char> skipBuffer(length);
            a_intfc->ReadRecordData(skipBuffer.data(), length);
        }
        return;
    }

    std::uint32_t count = 0;
    a_intfc->ReadRecordData(&count, sizeof(count));

    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t keyLen = 0;
        a_intfc->ReadRecordData(&keyLen, sizeof(keyLen));

        std::string key(keyLen, '\0');
        a_intfc->ReadRecordData(key.data(), keyLen);

        // FormKeys are load-order independent, so no SKSE form resolution is involved: a
        // key that does not resolve means its plugin is gone, not that the ID moved.
        const RE::FormID actorID = Persistence::FormKeyUtil::ResolveToRuntimeFormID(key);
        if (actorID == 0) {
            spdlog::warn("WeaponLockManager::OnLoadRecord - Could not resolve '{}', dropping its "
                "weapon lock", key);
            continue;
        }

        mgr->m_watched[actorID].actorKey = key;
    }

    mgr->RecountLocked();

    spdlog::info("WeaponLockManager::OnLoadRecord - Loaded {} weapon lock(s)",
        mgr->m_watched.size());
}

void WeaponLockManager::OnRevert(SKSE::SerializationInterface*)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    spdlog::info("WeaponLockManager::OnRevert - Clearing {} weapon lock(s)", mgr->m_watched.size());
    mgr->m_watched.clear();
    mgr->RecountLocked();
}
