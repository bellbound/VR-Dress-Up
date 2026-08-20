#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../log.h"

// A category as written in a JSON file under Data\SKSE\Plugins\VRDressup\Categories.
// Patterns are matched against the item's display name, word by word, so "bra" hits
// "Silk Bra" but not "Bracer". Plurals are handled automatically.
struct KeywordCategoryDef
{
    std::string name;
    int priority = 50;                          // 0-100, higher shows first in the row

    std::vector<std::string> phrases;           // one or two words, whole-word match
    std::vector<std::string> longPhrases;       // three or more words, substring match
    std::vector<std::string> prefixes;          // keyword written "foo*": any word starting with foo

    std::vector<std::string> excludePhrases;    // a hit here vetoes the item for this category
    std::vector<std::string> excludeLongPhrases;
    std::vector<std::string> excludePrefixes;

    // Every item from these plugins joins the category whatever its name says. Lowercased
    // filenames, e.g. "ks hairdo's.esp". The keyword lists above are ignored for them -
    // only allFromEspExclude* can keep one out, which is why it is a separate list from
    // the exclude* above.
    std::vector<std::string> allFromEsp;
    std::vector<std::string> allFromEspExcludePhrases;
    std::vector<std::string> allFromEspExcludeLongPhrases;
    std::vector<std::string> allFromEspExcludePrefixes;

    std::uint32_t requireSlots = 0;             // 0 = no constraint, else item must use one of these biped slots
    std::uint32_t excludeSlots = 0;
};

// What the gallery row needs to draw one category (mirrors ModCategoryInfo).
struct KeywordCategoryInfo
{
    std::string name;
    int priority = 50;
    size_t itemCount = 0;
    RE::TESObjectARMO* representative = nullptr;
};

enum class KeywordBuildState : std::uint8_t
{
    NotStarted = 0,
    WaitingForArmorCache,   // ArmorModManager is still scanning
    Matching,               // running names against the category patterns
    Complete,
    Failed
};

// Buckets every armor in the game into name-keyword categories ("Boots", "Wigs", ...).
// Nothing is read from disk and nothing is scanned until the category gallery is opened
// for the first time. Reuses ArmorModManager's deduplicated armor cache.
class KeywordCategoryManager
{
public:
    static constexpr size_t ITEMS_PER_FRAME = 2000;   // matching is cheap, so larger batches than the mod scan

    using BuildCompleteCallback = std::function<void(bool success)>;

    static KeywordCategoryManager* GetSingleton()
    {
        static KeywordCategoryManager instance;
        return &instance;
    }

    // Read the category JSONs. Safe to call repeatedly - only the first call touches disk.
    // Returns false when no usable definition was found.
    bool LoadDefinitions();
    size_t GetDefinitionCount() const;

    // Build the item lists. Waits for the shared armor cache first, then matches over
    // several frames. The callback runs on the main thread when the lists are ready.
    void StartBuildAsync(BuildCompleteCallback callback = nullptr);

    KeywordBuildState GetBuildState() const { return m_state.load(); }
    bool IsReady() const { return m_state.load() == KeywordBuildState::Complete; }
    float GetProgress() const;

    // Non-empty categories, highest priority first, ties broken by name.
    std::vector<KeywordCategoryInfo> GetSortedCategories() const;

    // Items matched into one category. Empty if unknown or not built yet.
    std::vector<RE::TESObjectARMO*> GetItemsForCategory(const std::string& name) const;

private:
    KeywordCategoryManager() = default;
    ~KeywordCategoryManager() = default;
    KeywordCategoryManager(const KeywordCategoryManager&) = delete;
    KeywordCategoryManager& operator=(const KeywordCategoryManager&) = delete;

    // One category's matched items, parallel to m_defs.
    struct Bucket
    {
        std::vector<RE::TESObjectARMO*> items;
        RE::TESObjectARMO* representative = nullptr;
    };

    // A pattern that can't be answered by the token lookup, plus the category it belongs to.
    struct PatternEntry
    {
        std::string text;
        std::uint16_t category = 0;
        bool exclude = false;
    };

    void BeginMatching();
    void QueueNextBatch();
    void ProcessBatch();
    void MatchArmor(RE::TESObjectARMO* armor);
    void FinishMatching();
    void Fail(const char* reason);
    void InvokeCallbacks(bool success);

    std::atomic<KeywordBuildState> m_state{KeywordBuildState::NotStarted};
    std::atomic<float> m_progressFraction{0.0f};

    bool m_definitionsLoaded = false;
    std::vector<KeywordCategoryDef> m_defs;

    // Pattern -> categories, so one pass over an item's words answers every category at once.
    std::unordered_map<std::string, std::vector<std::uint16_t>> m_includeIndex;
    std::unordered_map<std::string, std::vector<std::uint16_t>> m_excludeIndex;
    std::vector<PatternEntry> m_prefixEntries;
    std::vector<PatternEntry> m_longPhraseEntries;

    // Lowercased plugin filename -> categories that take everything from it.
    std::unordered_map<std::string, std::vector<std::uint16_t>> m_espIndex;

    std::vector<Bucket> m_buckets;

    // Matching pass state
    std::vector<RE::TESObjectARMO*> m_pendingArmor;
    size_t m_currentIndex = 0;
    std::vector<BuildCompleteCallback> m_callbacks;

    // Scratch buffers reused per item to keep the matching pass allocation-free
    std::vector<std::string> m_words;
    std::vector<std::string> m_tokens;
    std::vector<std::uint8_t> m_hit;
    std::vector<std::uint8_t> m_veto;
    std::vector<std::uint8_t> m_espHit;

    mutable std::mutex m_mutex;
};
