#include "OutfitFormBackend.h"
#include "PapyrusBridge.h"
#include "SeverActionsCompat.h"
#include "DeviceCompat.h"
#include "FormKeyUtil.h"
#include "../settings.h"

#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

namespace
{
    std::int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

void OutfitFormBackend::Initialize()
{
    if (m_initialized) {
        spdlog::warn("OutfitFormBackend::Initialize - Already initialized");
        return;
    }
    m_initialized = true;

    if (!Settings::GetSingleton()->IsOutfitBackendEnabled()) {
        spdlog::info("OutfitFormBackend::Initialize - Disabled by bUseOutfitBackend");
        return;
    }

    ResolveNativePool();
}

void OutfitFormBackend::ResolveNativePool()
{
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        spdlog::error("OutfitFormBackend::ResolveNativePool - No TESDataHandler");
        return;
    }

    if (!dataHandler->LookupModByName(kPluginName)) {
        spdlog::warn("OutfitFormBackend::ResolveNativePool - '{}' is not in the load order, "
            "SPID handoff disabled", kPluginName);
        return;
    }

    // Resolved through LookupForm rather than a hand-built FormID. VRDressUp.esp is
    // ESL-flagged, and synthesising 0xFE000000 | (idx << 12) does not work on VR -
    // see the note in FormKeyUtil::ResolveToRuntimeFormID.
    if (auto* blank = dataHandler->LookupForm(kBlankOutfitID, kPluginName)) {
        m_blankOutfit = blank->As<RE::BGSOutfit>();
    }

    m_pool.clear();
    m_pool.reserve(kPoolSize);
    for (std::size_t i = 0; i < kPoolSize; ++i) {
        auto* form = dataHandler->LookupForm(
            kFirstOutfitID + static_cast<RE::FormID>(i), kPluginName);
        auto* outfit = form ? form->As<RE::BGSOutfit>() : nullptr;
        if (!outfit) {
            spdlog::warn("OutfitFormBackend::ResolveNativePool - Missing outfit record #{}", i);
            break;
        }
        m_pool.push_back(outfit);
    }

    spdlog::info("OutfitFormBackend::ResolveNativePool - Indexed {}/{} outfit records from '{}'",
        m_pool.size(), kPoolSize, kPluginName);
}

bool OutfitFormBackend::IsAvailable() const
{
    return !m_pool.empty();
}

bool OutfitFormBackend::IsEligible(RE::Actor* actor) const
{
    if (!actor || actor->IsPlayerRef()) return false;

    auto* npc = actor->GetActorBase();
    if (!npc) return false;

    if (Settings::GetSingleton()->IsOutfitBackendUniqueOnly() && !npc->IsUnique()) {
        spdlog::info("OutfitFormBackend::IsEligible - '{}' has a shared base record, skipping "
            "(every copy of that NPC would change with it)", actor->GetName());
        return false;
    }

    return true;
}

RE::BGSOutfit* OutfitFormBackend::OutfitForIndex(std::size_t index) const
{
    return index < m_pool.size() ? m_pool[index] : nullptr;
}

OutfitFormBackend::Assignment* OutfitFormBackend::AssignLocked(const std::string& actorKey)
{
    auto it = m_assignments.find(actorKey);
    Assignment* existing = (it != m_assignments.end()) ? &it->second : nullptr;
    if (existing && existing->poolIndex < m_pool.size()) {
        return existing;
    }

    // Either a new actor, or one holding no usable slot: an entry made while the pool was
    // full, or one from a co-save written when the record could come from another mod and
    // an entry of our own did not need a slot at all.
    std::vector<bool> taken(m_pool.size(), false);
    for (const auto& [key, assignment] : m_assignments) {
        if (&assignment != existing && assignment.poolIndex < taken.size()) {
            taken[assignment.poolIndex] = true;
        }
    }

    std::size_t slot = kNoPoolSlot;
    for (std::size_t i = 0; i < taken.size(); ++i) {
        if (!taken[i]) {
            slot = i;
            break;
        }
    }

    if (slot == kNoPoolSlot) {
        // The entry is still created - it carries the original outfit and the item set -
        // but with no record to assign, Apply will not dispatch for this actor.
        spdlog::warn("OutfitFormBackend::AssignLocked - Outfit pool exhausted ({} slots)",
            m_pool.size());
    }

    if (existing) {
        existing->poolIndex = slot;
        existing->lastApplied.clear();  // different record: the next Apply has to dispatch
        return existing;
    }

    Assignment fresh;
    fresh.poolIndex = slot;
    return &m_assignments.emplace(actorKey, std::move(fresh)).first->second;
}

bool OutfitFormBackend::Fill(RE::BGSOutfit* outfit, const std::vector<std::string>& formKeys) const
{
    if (!outfit) return false;

    outfit->outfitItems.clear();

    for (const auto& formKey : formKeys) {
        const RE::FormID runtimeID = Persistence::FormKeyUtil::ResolveToRuntimeFormID(formKey);
        if (runtimeID == 0) continue;

        auto* form = RE::TESForm::LookupByID(runtimeID);
        if (!form) continue;

        // Devious Devices items are deliberately left out.
        //
        // This record only exists to tell SPID the NPC is taken; the engine also *applies*
        // it, adding a fresh copy of every listed item on each cell load. For ordinary
        // clothing that is a harmless no-op, but a second copy of a device in the same
        // inventory is a state DD documents as broken - its own removal path cannot tell
        // which copy is worn. The lock's reapply puts devices back through DD instead.
        if (auto* armor = form->As<RE::TESObjectARMO>(); armor && DeviceCompat::IsDevice(armor)) {
            spdlog::trace("OutfitFormBackend::Fill - Skipping Devious Device '{}'", formKey);
            continue;
        }

        outfit->outfitItems.push_back(form);
    }

    spdlog::trace("OutfitFormBackend::Fill - '{}' now holds {}/{} item(s)",
        outfit->GetFormEditorID(), outfit->outfitItems.size(), formKeys.size());

    return !outfit->outfitItems.empty();
}

bool OutfitFormBackend::IsApplying() const
{
    return NowMs() < m_applyUntilMs.load(std::memory_order_relaxed);
}

void OutfitFormBackend::DispatchSetOutfit(RE::Actor* actor, RE::BGSOutfit* outfit,
    bool guardInventory)
{
    SeverActionsCompat::SuspendLock(actor);

    // Snapshotted before the call, because the point of the guard is to compare against
    // what the record held when the engine was handed it.
    std::vector<RE::FormID> guarded;
    if (guardInventory && outfit) {
        guarded.reserve(outfit->outfitItems.size());
        for (auto* item : outfit->outfitItems) {
            if (item) guarded.push_back(item->GetFormID());
        }
    }
    const RE::FormID actorID = actor ? actor->GetFormID() : 0;

    m_applyUntilMs.store(NowMs() + kApplySettleMs, std::memory_order_relaxed);
    PapyrusBridge::CallActorSetOutfit(actor, outfit, false);

    // Held across the same window we ignore our own equip events for. SetOutfit's
    // implicit UnequipAll reaches Papyrus well after this call returns, so resuming here
    // would hand the SeverActions debounce exactly the trigger the suspend is for.
    SeverActionsCompat::ResumeLockAfterMs(actor, kApplySettleMs);

    if (!guarded.empty() && actorID != 0) {
        // Behind the same window: the removals ride in on the engine's queue and are not
        // done when this returns.
        PapyrusBridge::RunAfterMs(static_cast<std::int32_t>(kApplySettleMs),
            [actorID, guarded = std::move(guarded)]() {
                VerifyInventoryAfterDispatch(actorID, guarded);
            });
    }
}

// Put back anything the outfit change took out of the actor's inventory.
//
// An outfit change strips the items the previous application added, and whether it adds
// them back depends on engine state we do not control - notably it does nothing at all
// when the outfit form is the one the actor already had. Both halves of that are the
// engine's business; ours is that the player's NPC must not quietly lose gear over it.
//
// A backstop, not the fix. Apply no longer re-dispatches for a content change, which is
// what was driving the loss; this catches the dispatches that do still have to happen -
// the first assignment, and the re-assignment after a load.
void OutfitFormBackend::VerifyInventoryAfterDispatch(RE::FormID actorID,
    const std::vector<RE::FormID>& itemIDs)
{
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorID);
    if (!actor) return;

    const auto counts = actor->GetInventoryCounts();

    std::uint32_t restored = 0;
    for (const RE::FormID itemID : itemIDs) {
        auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(itemID);
        if (!armor) continue;

        const auto found = counts.find(static_cast<RE::TESBoundObject*>(armor));
        if (found != counts.end() && found->second > 0) continue;

        actor->AddObjectToContainer(armor, nullptr, 1, nullptr);
        ++restored;
        spdlog::warn("OutfitFormBackend - the outfit change took '{}' (0x{:08X}) out of "
            "'{}'s inventory; put it back", armor->GetFullName(), itemID, actor->GetName());
    }

    if (restored > 0) {
        spdlog::warn("OutfitFormBackend - restored {} item(s) the engine removed behind a "
            "SetOutfit on '{}'", restored, actor->GetName());
    }
}

bool OutfitFormBackend::Apply(RE::Actor* actor, const std::vector<std::string>& formKeys)
{
    if (!IsAvailable() || !IsEligible(actor)) return false;

    // An empty set is a real outfit - "wear nothing" - and goes through like any other.
    // Returning early here used to leave the record holding whatever the previous set was,
    // so an NPC undressed to nothing was re-dressed out of their own record on the next
    // cell reset, and a fresh one was never handed to SPID at all.

    const std::string actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
    if (actorKey.empty()) {
        spdlog::warn("OutfitFormBackend::Apply - '{}' has no source file (dynamic actor?), skipping",
            actor->GetName());
        return false;
    }

    std::vector<std::string> sortedKeys = formKeys;
    std::sort(sortedKeys.begin(), sortedKeys.end());

    auto* npc = actor->GetActorBase();
    RE::BGSOutfit* outfit = nullptr;
    bool needsDispatch = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* assignment = AssignLocked(actorKey);
        if (!assignment) return false;

        outfit = OutfitForIndex(assignment->poolIndex);
        if (!outfit) {
            spdlog::warn("OutfitFormBackend::Apply - No outfit record available for '{}'",
                actor->GetName());
            return false;
        }

        if (assignment->originalOutfitKey.empty() && npc->defaultOutfit &&
            npc->defaultOutfit != outfit) {
            assignment->originalOutfitKey =
                Persistence::FormKeyUtil::BuildFormKey(npc->defaultOutfit);
        }

        // Dispatch only to put our record on an actor who is not already wearing it.
        //
        // A change to the item set is deliberately NOT a reason to dispatch. Fill rewrites
        // the record in place and the engine reads it fresh every time it applies an
        // outfit, so a new item set is already live for everything that comes later - a
        // cell load, SPID, the lock's own reapply. Re-dispatching cannot make it any more
        // live, because the form identity has not changed and the engine has nothing to
        // react to.
        //
        // What it does instead is destroy gear. An outfit change makes the engine strip
        // the items the previous application put in the inventory, and with the same form
        // on both sides of the change it does not add them back. Every piece that left the
        // list between two dispatches was deleted out of the actor that way. Verified
        // against Daegon: her Elven Sentry Cuirass and Sabatons went out of the list on
        // the 14:27:03 dispatch and left her inventory with it, after which no click could
        // equip them - EquipObject on an item an actor does not have is a silent no-op,
        // so the wheel reported the equip and nothing ever happened.
        needsDispatch = (npc->defaultOutfit != outfit);
        assignment->lastApplied = sortedKeys;
    }

    Fill(outfit, formKeys);

    if (!needsDispatch) {
        spdlog::trace("OutfitFormBackend::Apply - '{}' already wears '{}'; rewrote it in place "
            "to {} item(s)", actor->GetName(), outfit->GetFormEditorID(),
            outfit->outfitItems.size());
        return true;
    }

    spdlog::info("OutfitFormBackend::Apply - Assigning '{}' ({} item(s)) to '{}' (0x{:08X})",
        outfit->GetFormEditorID(), outfit->outfitItems.size(),
        actor->GetName(), actor->GetFormID());

    DispatchSetOutfit(actor, outfit, true);
    return true;
}

bool OutfitFormBackend::Restore(RE::Actor* actor)
{
    if (!IsAvailable() || !actor) return false;

    const std::string actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
    if (actorKey.empty()) return false;

    std::string originalKey;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_assignments.find(actorKey);
        if (it == m_assignments.end()) return false;
        originalKey = it->second.originalOutfitKey;
        m_assignments.erase(it);
    }

    RE::BGSOutfit* original = nullptr;
    if (!originalKey.empty()) {
        const RE::FormID runtimeID = Persistence::FormKeyUtil::ResolveToRuntimeFormID(originalKey);
        if (auto* form = RE::TESForm::LookupByID(runtimeID)) {
            original = form->As<RE::BGSOutfit>();
        }
    }

    if (!original) {
        // Nothing sensible to hand back. Leaving our record in place keeps the NPC
        // dressed; blanking it would strip them.
        spdlog::info("OutfitFormBackend::Restore - No original outfit recorded for '{}', "
            "leaving the current one in place", actor->GetName());
        return false;
    }

    spdlog::info("OutfitFormBackend::Restore - Returning '{}' to outfit '{}'",
        actor->GetName(), original->GetFormEditorID());

    DispatchSetOutfit(actor, original);
    return true;
}

void OutfitFormBackend::ReapplyAll()
{
    if (!IsAvailable()) return;

    struct Pending
    {
        RE::Actor*               actor;
        RE::BGSOutfit*           outfit;
        std::vector<std::string> keys;
    };

    std::vector<Pending> pending;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [actorKey, assignment] : m_assignments) {
            const RE::FormID actorID = Persistence::FormKeyUtil::ResolveToRuntimeFormID(actorKey);
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorID);
            if (!actor) continue;

            auto* outfit = OutfitForIndex(assignment.poolIndex);
            if (!outfit) continue;

            pending.push_back({actor, outfit, assignment.lastApplied});
        }
    }

    spdlog::info("OutfitFormBackend::ReapplyAll - Rebuilding {} outfit record(s)", pending.size());

    for (auto& entry : pending) {
        // outfitItems is memory-only, so every record comes back from a load empty.
        Fill(entry.outfit, entry.keys);

        auto* npc = entry.actor->GetActorBase();
        if (npc && npc->defaultOutfit == entry.outfit) {
            continue;  // still assigned, nothing to dispatch
        }

        spdlog::debug("  - Re-assigning '{}' to '{}'",
            entry.outfit->GetFormEditorID(), entry.actor->GetName());
        DispatchSetOutfit(entry.actor, entry.outfit, true);
    }
}

void OutfitFormBackend::Forget(RE::Actor* actor)
{
    if (!actor) return;

    const std::string actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
    if (actorKey.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_assignments.erase(actorKey);
}

// =============================================================================
// Serialization
// =============================================================================

void OutfitFormBackend::OnGameSave(SKSE::SerializationInterface* a_intfc)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    if (!a_intfc->OpenRecord(kRecord, kSerializationVersion)) {
        spdlog::error("OutfitFormBackend::OnGameSave - Failed to open record");
        return;
    }

    std::uint32_t count = static_cast<std::uint32_t>(mgr->m_assignments.size());
    a_intfc->WriteRecordData(&count, sizeof(count));

    const auto writeString = [a_intfc](const std::string& value) {
        std::uint32_t len = static_cast<std::uint32_t>(value.size());
        a_intfc->WriteRecordData(&len, sizeof(len));
        if (len > 0) {
            a_intfc->WriteRecordData(value.data(), len);
        }
    };

    for (const auto& [actorKey, assignment] : mgr->m_assignments) {
        writeString(actorKey);

        std::uint32_t poolIndex = static_cast<std::uint32_t>(assignment.poolIndex);
        a_intfc->WriteRecordData(&poolIndex, sizeof(poolIndex));

        writeString(assignment.originalOutfitKey);

        std::uint32_t itemCount = static_cast<std::uint32_t>(assignment.lastApplied.size());
        a_intfc->WriteRecordData(&itemCount, sizeof(itemCount));
        for (const auto& itemKey : assignment.lastApplied) {
            writeString(itemKey);
        }
    }

    spdlog::info("OutfitFormBackend::OnGameSave - Saved {} outfit assignment(s)", count);
}

void OutfitFormBackend::OnPreLoad()
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);
    mgr->m_assignments.clear();
}

void OutfitFormBackend::OnLoadRecord(SKSE::SerializationInterface* a_intfc,
    std::uint32_t type, std::uint32_t version, std::uint32_t length)
{
    if (type != kRecord) return;

    // v1 and v3 records have the same layout; v2 carries one extra byte per assignment,
    // the borrowed-record flag, which is read and discarded below.
    if (version < kMinReadableVersion || version > kSerializationVersion) {
        spdlog::warn("OutfitFormBackend::OnLoadRecord - Incompatible version {} (readable range {}..{}), skipping",
            version, kMinReadableVersion, kSerializationVersion);
        if (length > 0) {
            std::vector<char> skipBuffer(length);
            a_intfc->ReadRecordData(skipBuffer.data(), length);
        }
        return;
    }

    const auto readString = [a_intfc](std::string& out) {
        std::uint32_t len = 0;
        a_intfc->ReadRecordData(&len, sizeof(len));
        out.clear();
        if (len > 0) {
            out.resize(len);
            a_intfc->ReadRecordData(out.data(), len);
        }
    };

    std::uint32_t count = 0;
    a_intfc->ReadRecordData(&count, sizeof(count));

    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    for (std::uint32_t i = 0; i < count; ++i) {
        std::string actorKey;
        readString(actorKey);

        Assignment assignment;

        std::uint32_t poolIndex = 0;
        a_intfc->ReadRecordData(&poolIndex, sizeof(poolIndex));
        // kNoPoolSlot is size_t(-1) and was written truncated to 32 bits, so it has to be
        // widened back to the sentinel rather than read as index 0xFFFFFFFF.
        assignment.poolIndex = (poolIndex == UINT32_MAX) ? kNoPoolSlot : poolIndex;

        readString(assignment.originalOutfitKey);

        std::uint32_t itemCount = 0;
        a_intfc->ReadRecordData(&itemCount, sizeof(itemCount));
        assignment.lastApplied.reserve(itemCount);
        for (std::uint32_t j = 0; j < itemCount; ++j) {
            std::string itemKey;
            readString(itemKey);
            assignment.lastApplied.push_back(std::move(itemKey));
        }

        if (version == 2) {
            // The borrowed-record flag. Records are no longer borrowed, but the byte is
            // in the stream and has to come out of it before the next assignment reads.
            std::uint8_t usedExternal = 0;
            a_intfc->ReadRecordData(&usedExternal, sizeof(usedExternal));
            if (usedExternal != 0) {
                spdlog::debug("  - Assignment previously used another mod's outfit record; "
                    "it will be re-assigned one from our own pool");
            }
        }

        // Read first, decide afterwards - the record has to be consumed in full even
        // when the entry is dropped, or every following one reads at the wrong offset.
        if (actorKey.empty()) {
            spdlog::warn("  - Dropping outfit assignment with no actor key");
            continue;
        }

        mgr->m_assignments.emplace(std::move(actorKey), std::move(assignment));
    }

    spdlog::info("OutfitFormBackend::OnLoadRecord - Loaded {}/{} outfit assignment(s)",
        mgr->m_assignments.size(), count);
}

void OutfitFormBackend::OnRevert(SKSE::SerializationInterface*)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);
    mgr->m_assignments.clear();
}
