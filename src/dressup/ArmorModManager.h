#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <vector>
#include <string>
#include <string_view>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <functional>
#include <atomic>
#include <mutex>
#include "GalleryStateManager.h"
#include "../log.h"

// The base game's own plugins. Gallery lists sort these last: someone browsing for
// something to wear has already seen every vanilla piece a hundred times, so the
// mod-added items are what they came for.
inline bool IsVanillaPlugin(std::string_view pluginName)
{
    static constexpr std::string_view kVanilla[] = {
        "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm"
    };

    for (auto candidate : kVanilla) {
        if (candidate.size() != pluginName.size()) continue;

        if (std::equal(candidate.begin(), candidate.end(), pluginName.begin(), [](char lower, char actual) {
                return lower == static_cast<char>(std::tolower(static_cast<unsigned char>(actual)));
            })) {
            return true;
        }
    }

    return false;
}

inline bool IsVanillaArmor(RE::TESObjectARMO* armor)
{
    auto* file = armor ? armor->GetFile(0) : nullptr;
    return file && IsVanillaPlugin(file->GetFilename());
}

// Cached mod category info (lightweight, built at startup)
struct ModCategoryInfo
{
    std::string modName;                    // Plugin filename (e.g., "Skyrim.esm")
    RE::TESObjectARMO* representativeArmor; // For display in gallery
    size_t itemCount;                       // Number of armor items
    bool addsNPCs;                          // Whether this mod adds NPCs
};

// Loading state for async operations
enum class CacheLoadState : uint8_t
{
    NotStarted = 0,
    LoadingNPCs,        // Scanning NPC array for mod detection
    LoadingArmor,       // Scanning armor array
    BuildingCategories, // Building category list from grouped data
    Complete,
    Failed
};

// Manages armor mod enumeration, caching, and sorting for the gallery
// Uses async multi-frame processing to avoid hitches
class ArmorModManager
{
public:
    static constexpr size_t MIN_ARMOR_COUNT = 3;       // Minimum armor items to show mod in gallery
    static constexpr size_t SMALL_MOD_THRESHOLD = 8;   // Mods with fewer items pushed to end
    static constexpr size_t LARGE_MOD_THRESHOLD = 50;  // Mods with more items pushed back
    static constexpr size_t ITEMS_PER_FRAME = 500;     // Items to process per frame (tune for performance)

    using CacheCompleteCallback = std::function<void(bool success)>;

    static ArmorModManager* GetSingleton()
    {
        static ArmorModManager instance;
        return &instance;
    }

    // Start async cache building (call on first gallery open)
    // Returns immediately, cache builds over multiple frames
    // Callback is called on completion (on main thread). Several callers may wait on the
    // same build - each one's callback fires.
    void StartCacheBuildAsync(CacheCompleteCallback callback = nullptr)
    {
        bool alreadyComplete = false;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (m_loadState == CacheLoadState::Complete) {
                alreadyComplete = true;
            } else if (m_loadState != CacheLoadState::NotStarted && m_loadState != CacheLoadState::Failed) {
                // Build already running - ride along with it
                if (callback) m_completionCallbacks.push_back(std::move(callback));
                return;
            } else {
                spdlog::info("ArmorModManager: Starting async category cache build...");

                if (callback) m_completionCallbacks.push_back(std::move(callback));
                m_categoryCache.clear();
                m_armorByMod.clear();
                m_npcModCache.clear();
                m_loadState = CacheLoadState::LoadingNPCs;
                m_currentIndex = 0;

                // Queue first batch
                QueueNextBatch();
                return;
            }
        }  // mutex released here

        // Invoke callback AFTER releasing mutex to avoid deadlock
        if (alreadyComplete && callback) {
            callback(true);
        }
    }

    // Get current loading state
    CacheLoadState GetLoadState() const { return m_loadState.load(); }

    // Check if cache is ready for use
    bool IsCacheReady() const { return m_loadState == CacheLoadState::Complete; }

    // Legacy compatibility - check if cache is built (same as IsCacheReady)
    bool IsCacheBuilt() const { return IsCacheReady(); }

    // Get loading progress (0.0 - 1.0)
    float GetLoadProgress() const
    {
        auto state = m_loadState.load();
        switch (state) {
            case CacheLoadState::NotStarted: return 0.0f;
            case CacheLoadState::LoadingNPCs: return 0.1f + (0.2f * m_progressFraction);
            case CacheLoadState::LoadingArmor: return 0.3f + (0.5f * m_progressFraction);
            case CacheLoadState::BuildingCategories: return 0.8f + (0.2f * m_progressFraction);
            case CacheLoadState::Complete: return 1.0f;
            case CacheLoadState::Failed: return 0.0f;
        }
        return 0.0f;
    }

    // Get sorted categories for display (uses cached data, fast)
    // Returns empty if cache not ready
    std::vector<ModCategoryInfo> GetSortedCategories()
    {
        if (!IsCacheReady()) {
            return {};
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<ModCategoryInfo> result = m_categoryCache;
        auto* galleryState = GalleryStateManager::GetSingleton();

        // Sort by criteria:
        // 1. Front mods (user interaction history) - most recent first
        // 2. Standard mods (8-50 items, no NPCs)
        // 3. Large mods (>50 items) - pushed back
        // 4. Small mods (<8 items) - pushed to end
        // 5. Mods that add NPCs - also pushed back

        std::sort(result.begin(), result.end(), [galleryState](const ModCategoryInfo& a, const ModCategoryInfo& b) {
            int frontIndexA = galleryState->GetFrontIndex(a.modName);
            int frontIndexB = galleryState->GetFrontIndex(b.modName);

            // Both are front mods - sort by front index (lower = more recent = first)
            if (frontIndexA >= 0 && frontIndexB >= 0) {
                return frontIndexA < frontIndexB;
            }

            // Only A is front mod - A comes first
            if (frontIndexA >= 0) return true;
            // Only B is front mod - B comes first
            if (frontIndexB >= 0) return false;

            // Neither is front mod - apply default sorting
            int priorityA = GetDefaultPriority(a);
            int priorityB = GetDefaultPriority(b);

            if (priorityA != priorityB) {
                return priorityA < priorityB;
            }

            // Same priority - sort alphabetically
            return a.modName < b.modName;
        });

        return result;
    }

    // Get armor items for a specific mod (uses cached data - instant!)
    // Returns empty if cache not ready or mod not found
    std::vector<RE::TESObjectARMO*> GetArmorForMod(const std::string& modName)
    {
        if (!IsCacheReady()) {
            return {};
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_armorByMod.find(modName);
        if (it == m_armorByMod.end()) {
            return {};
        }

        // Return copy of cached armor list (already sorted during cache build)
        return it->second;
    }

    // Legacy compatibility - calls GetArmorForMod
    std::vector<RE::TESObjectARMO*> LoadArmorForMod(const std::string& modName)
    {
        return GetArmorForMod(modName);
    }

    // Every deduplicated armor in the cache, regardless of source mod.
    // Used by the keyword categories so the form array is only ever scanned once.
    std::vector<RE::TESObjectARMO*> GetAllCachedArmor() const
    {
        if (!IsCacheReady()) {
            return {};
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        size_t total = 0;
        for (const auto& [modName, armors] : m_armorByMod) {
            total += armors.size();
        }

        std::vector<RE::TESObjectARMO*> result;
        result.reserve(total);
        for (const auto& [modName, armors] : m_armorByMod) {
            result.insert(result.end(), armors.begin(), armors.end());
        }
        return result;
    }

    // Get category count (for UI display)
    size_t GetCategoryCount() const
    {
        if (!IsCacheReady()) return 0;
        return m_categoryCache.size();
    }

private:
    ArmorModManager() = default;
    ~ArmorModManager() = default;
    ArmorModManager(const ArmorModManager&) = delete;
    ArmorModManager& operator=(const ArmorModManager&) = delete;

    // Queue next batch of work on the main thread
    void QueueNextBatch()
    {
        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            spdlog::error("ArmorModManager: Failed to get SKSE task interface");
            m_loadState = CacheLoadState::Failed;
            return;
        }

        taskInterface->AddTask([this]() {
            ProcessBatch();
        });
    }

    // Process one batch of items (called on main thread via task)
    void ProcessBatch()
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            spdlog::error("ArmorModManager: Failed to get TESDataHandler");
            m_loadState = CacheLoadState::Failed;

            std::vector<CacheCompleteCallback> callbacks;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                callbacks.swap(m_completionCallbacks);
            }
            for (auto& callback : callbacks) {
                if (callback) callback(false);
            }
            return;
        }

        auto state = m_loadState.load();

        switch (state) {
            case CacheLoadState::LoadingNPCs:
                ProcessNPCBatch(dataHandler);
                break;

            case CacheLoadState::LoadingArmor:
                ProcessArmorBatch(dataHandler);
                break;

            case CacheLoadState::BuildingCategories:
                ProcessCategoryBatch();
                break;

            default:
                // Done or error state
                break;
        }
    }

    // Process batch of NPC forms to detect which mods add NPCs
    void ProcessNPCBatch(RE::TESDataHandler* dataHandler)
    {
        auto& npcArray = dataHandler->GetFormArray<RE::TESNPC>();
        size_t total = npcArray.size();
        size_t processed = 0;

        for (size_t i = m_currentIndex; i < total && processed < ITEMS_PER_FRAME; ++i, ++processed) {
            auto* npc = npcArray[i];
            if (!npc || npc->IsPlayer()) continue;

            auto* file = npc->GetFile(0);
            if (!file) continue;

            // Use string_view for comparison to avoid allocation
            std::string_view filename = file->GetFilename();
            m_npcModCache.emplace(filename);
        }

        m_currentIndex += processed;
        m_progressFraction = static_cast<float>(m_currentIndex) / static_cast<float>(total);

        if (m_currentIndex >= total) {
            // Done with NPCs, move to armor
            spdlog::info("ArmorModManager: Found {} mods that add NPCs", m_npcModCache.size());
            m_loadState = CacheLoadState::LoadingArmor;
            m_currentIndex = 0;
            m_progressFraction = 0.0f;
        }

        QueueNextBatch();
    }

    // Check if armor has an enchantment
    static bool IsArmorEnchanted(RE::TESObjectARMO* armor)
    {
        if (!armor) return false;
        auto* enchantable = armor->As<RE::TESEnchantableForm>();
        return enchantable && enchantable->formEnchanting;
    }

    // Process batch of armor forms
    void ProcessArmorBatch(RE::TESDataHandler* dataHandler)
    {
        auto& armorArray = dataHandler->GetFormArray<RE::TESObjectARMO>();
        size_t total = armorArray.size();
        size_t processed = 0;

        std::lock_guard<std::mutex> lock(m_mutex);

        for (size_t i = m_currentIndex; i < total && processed < ITEMS_PER_FRAME; ++i, ++processed) {
            auto* armor = armorArray[i];
            if (!armor) continue;

            // Skip armor without a name (often invisible/placeholder items)
            const char* name = armor->GetFullName();
            if (!name || !*name) continue;

            // Get source mod
            auto* file = armor->GetFile(0);
            if (!file) continue;

            std::string modName(file->GetFilename());

            // Get model path for deduplication
            // Many mods have enchanted variants that share the same mesh
            const char* modelPath = armor->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
            if (modelPath && *modelPath) {
                std::string modelKey(modelPath);

                // Check if we've already seen this mesh for this mod
                auto& seenMeshes = m_seenMeshesByMod[modName];
                auto existingIt = seenMeshes.find(modelKey);

                if (existingIt != seenMeshes.end()) {
                    // We already have an armor with this mesh
                    // Prefer unenchanted version: if current is unenchanted, try to replace enchanted one
                    bool currentIsEnchanted = IsArmorEnchanted(armor);

                    if (!currentIsEnchanted) {
                        // Current armor is unenchanted - check if stored one is enchanted and replace it
                        auto& armorList = m_armorByMod[modName];
                        for (size_t j = 0; j < armorList.size(); ++j) {
                            const char* existingPath = armorList[j]->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
                            if (existingPath && modelKey == existingPath && IsArmorEnchanted(armorList[j])) {
                                // Replace enchanted with unenchanted
                                armorList[j] = armor;
                                break;
                            }
                        }
                    }
                    // Either way, skip adding a new entry (we either replaced or kept existing)
                    continue;
                }

                seenMeshes.insert(modelKey);
            }

            m_armorByMod[modName].push_back(armor);
        }

        m_currentIndex += processed;
        m_progressFraction = static_cast<float>(m_currentIndex) / static_cast<float>(total);

        if (m_currentIndex >= total) {
            // Done with armor, build categories
            size_t totalArmor = 0;
            for (const auto& [modName, armors] : m_armorByMod) {
                totalArmor += armors.size();
            }
            spdlog::info("ArmorModManager: Grouped {} unique armor items into {} mods (after deduplication)",
                totalArmor, m_armorByMod.size());

            // Clear temporary deduplication data
            m_seenMeshesByMod.clear();

            m_loadState = CacheLoadState::BuildingCategories;
            m_currentIndex = 0;
            m_progressFraction = 0.0f;

            // Pre-sort armor lists by rating while we're at it
            for (auto& [modName, armors] : m_armorByMod) {
                std::sort(armors.begin(), armors.end(), [](RE::TESObjectARMO* a, RE::TESObjectARMO* b) {
                    return a->armorRating > b->armorRating;
                });
            }
        }

        QueueNextBatch();
    }

    // Build category info from grouped armor data
    void ProcessCategoryBatch()
    {
        bool cacheComplete = false;
        std::vector<CacheCompleteCallback> callbacksToInvoke;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // Convert map to vector for indexed iteration
            if (m_modNameList.empty()) {
                m_modNameList.reserve(m_armorByMod.size());
                for (const auto& [modName, armors] : m_armorByMod) {
                    if (armors.size() >= MIN_ARMOR_COUNT) {
                        m_modNameList.push_back(modName);
                    }
                }
            }

            size_t total = m_modNameList.size();
            size_t processed = 0;

            for (size_t i = m_currentIndex; i < total && processed < ITEMS_PER_FRAME; ++i, ++processed) {
                const std::string& modName = m_modNameList[i];
                const auto& armors = m_armorByMod[modName];

                ModCategoryInfo info;
                info.modName = modName;
                info.itemCount = armors.size();
                info.addsNPCs = m_npcModCache.count(modName) > 0;
                info.representativeArmor = SelectRepresentativeArmor(modName, armors);

                m_categoryCache.push_back(info);
            }

            m_currentIndex += processed;
            m_progressFraction = static_cast<float>(m_currentIndex) / static_cast<float>(total);

            if (m_currentIndex >= total) {
                // All done!
                m_loadState = CacheLoadState::Complete;
                m_modNameList.clear();  // Free temporary list

                spdlog::info("ArmorModManager: Cache complete! {} categories, {} total armor items",
                    m_categoryCache.size(),
                    std::accumulate(m_armorByMod.begin(), m_armorByMod.end(), size_t(0),
                        [](size_t sum, const auto& pair) { return sum + pair.second.size(); }));

                cacheComplete = true;
                callbacksToInvoke.swap(m_completionCallbacks);
            }
        }  // mutex released here

        if (cacheComplete) {
            // Invoke callbacks AFTER releasing the mutex to avoid deadlock!
            // A callback may call GetSortedCategories() which needs the mutex.
            for (auto& callback : callbacksToInvoke) {
                if (callback) callback(true);
            }
            return;
        }

        QueueNextBatch();
    }

    // Try to get a hardcoded representative armor for known base game mods
    RE::TESObjectARMO* GetHardcodedRepresentative(const std::string& modName)
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return nullptr;

        // Hardcoded representatives for base game mods
        if (modName == "Skyrim.esm") {
            return dataHandler->LookupForm<RE::TESObjectARMO>(0x00012E4D, modName);
        }
        if (modName == "Dawnguard.esm") {
            return dataHandler->LookupForm<RE::TESObjectARMO>(0x01989E, modName);
        }
        if (modName == "Dragonborn.esm") {
            return dataHandler->LookupForm<RE::TESObjectARMO>(0x037B88, modName);
        }

        return nullptr;
    }

    // Helper to check if a string contains a substring (case-insensitive)
    static bool ContainsIgnoreCase(const char* str, const char* substr)
    {
        if (!str || !substr) return false;
        std::string haystack(str);
        std::string needle(substr);
        // Convert both to lowercase for comparison
        std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
        std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
        return haystack.find(needle) != std::string::npos;
    }

    // Select representative armor for a mod (hardcoded > helmet > body > hands > any)
    // For helmets, prefers items with "Helmet" in name, then "Mask", then any head slot
    RE::TESObjectARMO* SelectRepresentativeArmor(const std::string& modName, const std::vector<RE::TESObjectARMO*>& items)
    {
        // First check for hardcoded representative
        auto* hardcoded = GetHardcodedRepresentative(modName);
        if (hardcoded) {
            return hardcoded;
        }

        // Fall back to automatic selection
        // For head slot: prefer "Helmet" > "Mask" > any head slot
        RE::TESObjectARMO* helmetNamed = nullptr;   // Contains "Helmet" in name
        RE::TESObjectARMO* maskNamed = nullptr;     // Contains "Mask" in name
        RE::TESObjectARMO* headSlot = nullptr;      // Any head/hair slot item
        RE::TESObjectARMO* body = nullptr;
        RE::TESObjectARMO* hands = nullptr;
        RE::TESObjectARMO* best = nullptr;

        float helmetNamedRating = 0.0f;
        float maskNamedRating = 0.0f;
        float headSlotRating = 0.0f;
        float bodyRating = 0.0f;
        float handsRating = 0.0f;
        float bestRating = 0.0f;

        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        using SlotType = std::underlying_type_t<Slot>;

        for (auto* armor : items) {
            if (!armor) continue;

            SlotType slots = static_cast<SlotType>(armor->GetSlotMask());
            float rating = static_cast<float>(armor->armorRating);
            const char* name = armor->GetFullName();

            bool isHeadSlot = (slots & static_cast<SlotType>(Slot::kHead)) != 0 ||
                              (slots & static_cast<SlotType>(Slot::kHair)) != 0;
            bool isBody = (slots & static_cast<SlotType>(Slot::kBody)) != 0;
            bool isHands = (slots & static_cast<SlotType>(Slot::kHands)) != 0;

            // Check for head slot items with preferred names
            if (isHeadSlot) {
                if (ContainsIgnoreCase(name, "Helmet") && rating > helmetNamedRating) {
                    helmetNamed = armor;
                    helmetNamedRating = rating;
                } else if (ContainsIgnoreCase(name, "Mask") && rating > maskNamedRating) {
                    maskNamed = armor;
                    maskNamedRating = rating;
                } else if (rating > headSlotRating) {
                    headSlot = armor;
                    headSlotRating = rating;
                }
            }
            if (isBody && rating > bodyRating) {
                body = armor;
                bodyRating = rating;
            }
            if (isHands && rating > handsRating) {
                hands = armor;
                handsRating = rating;
            }
            if (rating > bestRating) {
                best = armor;
                bestRating = rating;
            }
        }

        // Priority: helmet(named) > mask(named) > head slot > body > hands > any
        if (helmetNamed) return helmetNamed;
        if (maskNamed) return maskNamed;
        if (headSlot) return headSlot;
        if (body) return body;
        if (hands) return hands;
        return best;
    }

    // Get default sorting priority (lower = appears earlier)
    static int GetDefaultPriority(const ModCategoryInfo& info)
    {
        if (info.itemCount < SMALL_MOD_THRESHOLD) return 3;  // Small mods at end
        if (info.itemCount > LARGE_MOD_THRESHOLD) return 2;  // Large mods pushed back
        if (info.addsNPCs) return 1;                         // NPC mods pushed back
        return 0;                                            // Standard mods first
    }

    // === State ===
    std::atomic<CacheLoadState> m_loadState{CacheLoadState::NotStarted};
    std::atomic<float> m_progressFraction{0.0f};
    size_t m_currentIndex = 0;
    std::vector<CacheCompleteCallback> m_completionCallbacks;

    // Temporary list for category building iteration
    std::vector<std::string> m_modNameList;

    // Temporary deduplication tracking (cleared after armor processing)
    std::unordered_map<std::string, std::unordered_set<std::string>> m_seenMeshesByMod;

    // === Cached Data (protected by mutex) ===
    mutable std::mutex m_mutex;

    // Category info for gallery display
    std::vector<ModCategoryInfo> m_categoryCache;

    // Armor items grouped by mod (cached to avoid re-scanning!)
    std::unordered_map<std::string, std::vector<RE::TESObjectARMO*>> m_armorByMod;

    // Set of mod names that add NPCs
    std::unordered_set<std::string> m_npcModCache;
};
