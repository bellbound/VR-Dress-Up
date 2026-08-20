#include "KeywordCategoryManager.h"
#include "ArmorModManager.h"
#include "ItemEquipHelper.h"

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{
    constexpr const char* kCategoryDir = "Data\\SKSE\\Plugins\\VRDressup\\Categories";

    std::filesystem::path GetCategoryDirectory()
    {
        char pathBuffer[MAX_PATH]{};
        GetModuleFileNameA(nullptr, pathBuffer, MAX_PATH);
        std::string exePath(pathBuffer);
        return std::filesystem::path(exePath.substr(0, exePath.rfind('\\'))) / kCategoryDir;
    }

    // Lowercase, everything that isn't a letter or digit becomes a separator.
    void SplitWords(const char* text, std::vector<std::string>& out)
    {
        out.clear();
        if (!text) return;

        std::string current;
        for (const char* p = text; *p; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (std::isalnum(c)) {
                current.push_back(static_cast<char>(std::tolower(c)));
            } else if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        }
        if (!current.empty()) out.push_back(current);
    }

    std::string JoinWords(const std::vector<std::string>& words)
    {
        std::string result;
        for (size_t i = 0; i < words.size(); ++i) {
            if (i) result.push_back(' ');
            result += words[i];
        }
        return result;
    }

    std::string NormalizeText(const std::string& text)
    {
        std::vector<std::string> words;
        SplitWords(text.c_str(), words);
        return JoinWords(words);
    }

    // A word ending in "ss" is never a plural, so stripping the s would invent a singular
    // that was never there - which is how "Brass" used to land in the Bras category.
    bool IsPluralS(const std::string& w)
    {
        return w.size() > 3 && w.back() == 's' && w[w.size() - 2] != 's';
    }

    // Every form of an item's name a keyword could reasonably be written as: each word,
    // each word with a plural ending removed, and each adjacent pair of words.
    void CollectTokens(const std::vector<std::string>& words, std::vector<std::string>& out)
    {
        out.clear();

        const auto singulars = [&out](const std::string& w) {
            if (IsPluralS(w)) out.push_back(w.substr(0, w.size() - 1));
            if (w.size() > 4 && w.ends_with("es")) out.push_back(w.substr(0, w.size() - 2));
        };

        for (size_t i = 0; i < words.size(); ++i) {
            out.push_back(words[i]);
            singulars(words[i]);

            if (i + 1 < words.size()) {
                const std::string& next = words[i + 1];
                out.push_back(words[i] + " " + next);
                if (IsPluralS(next)) {
                    out.push_back(words[i] + " " + next.substr(0, next.size() - 1));
                }
                if (next.size() > 4 && next.ends_with("es")) {
                    out.push_back(words[i] + " " + next.substr(0, next.size() - 2));
                }
            }
        }
    }

    // Biped slots are named 30-61 in the CK; the mask packs them from bit 0.
    std::uint32_t ParseSlotMask(const nlohmann::json& parent, const char* key, const std::string& categoryName)
    {
        auto it = parent.find(key);
        if (it == parent.end() || !it->is_array()) return 0;

        std::uint32_t mask = 0;
        for (const auto& entry : *it) {
            if (!entry.is_number_integer()) continue;
            int slot = entry.get<int>();
            if (slot < 30 || slot > 61) {
                spdlog::warn("KeywordCategoryManager: '{}' has out-of-range slot {} in '{}'", categoryName, slot, key);
                continue;
            }
            mask |= 1u << (slot - 30);
        }
        return mask;
    }

    // Sorts each keyword into the bucket that can match it fastest.
    void AddPatterns(const nlohmann::json& parent, const char* key, const std::string& categoryName,
        std::vector<std::string>& phrases, std::vector<std::string>& longPhrases, std::vector<std::string>& prefixes)
    {
        auto it = parent.find(key);
        if (it == parent.end() || !it->is_array()) return;

        for (const auto& entry : *it) {
            if (!entry.is_string()) continue;

            std::string raw = entry.get<std::string>();
            bool isPrefix = !raw.empty() && raw.back() == '*';
            if (isPrefix) raw.pop_back();

            std::string normalized = NormalizeText(raw);
            if (normalized.empty()) continue;

            if (isPrefix) {
                if (normalized.find(' ') != std::string::npos) {
                    spdlog::warn("KeywordCategoryManager: '{}' prefix keyword '{}' must be a single word, ignored",
                        categoryName, normalized);
                    continue;
                }
                prefixes.push_back(normalized);
                continue;
            }

            size_t wordCount = 1 + static_cast<size_t>(std::count(normalized.begin(), normalized.end(), ' '));
            if (wordCount <= 2) {
                phrases.push_back(normalized);
            } else {
                longPhrases.push_back(" " + normalized + " ");
            }
        }
    }

    // "allFromEsp" may be a single plugin name or a list of them. Stored lowercased so the
    // match against TESFile::GetFilename() never depends on how the name was typed.
    void AddPluginNames(const nlohmann::json& parent, const char* key, const std::string& categoryName,
        std::vector<std::string>& out)
    {
        auto it = parent.find(key);
        if (it == parent.end()) return;

        const auto add = [&](const nlohmann::json& entry) {
            if (!entry.is_string()) return;

            std::string name = entry.get<std::string>();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
            if (name.empty()) return;

            if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
        };

        if (it->is_string()) {
            add(*it);
        } else if (it->is_array()) {
            for (const auto& entry : *it) add(entry);
        } else {
            spdlog::warn("KeywordCategoryManager: '{}' has a '{}' that is neither a string nor a list, ignored",
                categoryName, key);
        }
    }

    // True when any of the patterns hits the item, using the same rules as the keyword lists:
    // whole-word (or word-pair) tokens, "foo*" prefixes, and substring for three words or more.
    bool AnyPatternMatches(const std::vector<std::string>& phrases, const std::vector<std::string>& longPhrases,
        const std::vector<std::string>& prefixes, const std::vector<std::string>& words,
        const std::vector<std::string>& tokens, const std::string& paddedName)
    {
        for (const auto& phrase : phrases) {
            if (std::find(tokens.begin(), tokens.end(), phrase) != tokens.end()) return true;
        }

        for (const auto& prefix : prefixes) {
            for (const auto& word : words) {
                if (word.starts_with(prefix)) return true;
            }
        }

        for (const auto& phrase : longPhrases) {
            if (paddedName.find(phrase) != std::string::npos) return true;
        }

        return false;
    }

    bool ParseCategory(const nlohmann::json& node, const std::string& file, KeywordCategoryDef& out)
    {
        if (!node.is_object()) {
            spdlog::warn("KeywordCategoryManager: '{}' contains a non-object category, skipped", file);
            return false;
        }

        out = KeywordCategoryDef{};
        out.name = node.value("name", std::string{});
        if (out.name.empty()) {
            spdlog::warn("KeywordCategoryManager: '{}' has a category without a name, skipped", file);
            return false;
        }

        out.priority = std::clamp(node.value("priority", 50), 0, 100);

        AddPatterns(node, "keywords", out.name, out.phrases, out.longPhrases, out.prefixes);
        AddPatterns(node, "exclude", out.name, out.excludePhrases, out.excludeLongPhrases, out.excludePrefixes);

        AddPluginNames(node, "allFromEsp", out.name, out.allFromEsp);
        AddPatterns(node, "allFromEspExcludeKeywords", out.name,
            out.allFromEspExcludePhrases, out.allFromEspExcludeLongPhrases, out.allFromEspExcludePrefixes);

        if (out.allFromEsp.empty() &&
            !(out.allFromEspExcludePhrases.empty() && out.allFromEspExcludeLongPhrases.empty() &&
              out.allFromEspExcludePrefixes.empty())) {
            spdlog::warn("KeywordCategoryManager: category '{}' in '{}' has allFromEspExcludeKeywords "
                "but no allFromEsp, so the exclusions do nothing", out.name, file);
        }

        out.requireSlots = ParseSlotMask(node, "slots", out.name);
        out.excludeSlots = ParseSlotMask(node, "excludeSlots", out.name);

        if (out.phrases.empty() && out.longPhrases.empty() && out.prefixes.empty() && out.allFromEsp.empty()) {
            spdlog::warn("KeywordCategoryManager: category '{}' in '{}' has neither keywords nor allFromEsp, skipped",
                out.name, file);
            return false;
        }

        return true;
    }

    RE::TESObjectARMO* PickRepresentative(const std::vector<RE::TESObjectARMO*>& items)
    {
        // The plainest name in the bucket makes the most recognisable preview: "Wig"
        // reads as the category, "Ebony Nightingale Wig of Shadows" does not.
        RE::TESObjectARMO* best = nullptr;
        size_t bestLength = 0;

        for (auto* armor : items) {
            if (!armor) continue;

            const char* name = armor->GetFullName();
            if (!name || !*name) continue;

            size_t length = std::strlen(name);
            if (!best || length < bestLength ||
                (length == bestLength && _stricmp(name, best->GetFullName()) < 0)) {
                best = armor;
                bestLength = length;
            }
        }

        return best ? best : (items.empty() ? nullptr : items.front());
    }
}

bool KeywordCategoryManager::LoadDefinitions()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_definitionsLoaded) {
        return !m_defs.empty();
    }
    m_definitionsLoaded = true;

    const auto dir = GetCategoryDirectory();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        spdlog::error("KeywordCategoryManager: category folder '{}' not found", dir.string());
        return false;
    }

    size_t fileCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;

        const auto& path = entry.path();
        if (_stricmp(path.extension().string().c_str(), ".json") != 0) continue;

        const std::string fileName = path.filename().string();
        ++fileCount;

        std::ifstream stream(path);
        if (!stream) {
            spdlog::error("KeywordCategoryManager: could not open '{}'", fileName);
            continue;
        }

        nlohmann::json parsed;
        try {
            stream >> parsed;
        } catch (const std::exception& e) {
            spdlog::error("KeywordCategoryManager: '{}' is not valid JSON: {}", fileName, e.what());
            continue;
        }

        // A file may hold one category or a list of them.
        const nlohmann::json* list = &parsed;
        nlohmann::json wrapper;
        if (parsed.is_object() && parsed.contains("categories")) {
            list = &parsed["categories"];
        } else if (parsed.is_object()) {
            wrapper = nlohmann::json::array({parsed});
            list = &wrapper;
        }

        if (!list->is_array()) {
            spdlog::error("KeywordCategoryManager: '{}' is neither a category nor a list of categories", fileName);
            continue;
        }

        for (const auto& node : *list) {
            KeywordCategoryDef def;
            if (ParseCategory(node, fileName, def)) {
                m_defs.push_back(std::move(def));
            }
        }
    }

    if (m_defs.empty()) {
        spdlog::error("KeywordCategoryManager: no usable categories in {} file(s) under '{}'", fileCount, dir.string());
        return false;
    }

    // Build the pattern -> category lookup once; the matching pass then never
    // walks the category list per item.
    for (std::uint16_t i = 0; i < static_cast<std::uint16_t>(m_defs.size()); ++i) {
        const auto& def = m_defs[i];
        for (const auto& phrase : def.phrases) m_includeIndex[phrase].push_back(i);
        for (const auto& phrase : def.excludePhrases) m_excludeIndex[phrase].push_back(i);
        for (const auto& prefix : def.prefixes) m_prefixEntries.push_back({prefix, i, false});
        for (const auto& prefix : def.excludePrefixes) m_prefixEntries.push_back({prefix, i, true});
        for (const auto& phrase : def.longPhrases) m_longPhraseEntries.push_back({phrase, i, false});
        for (const auto& phrase : def.excludeLongPhrases) m_longPhraseEntries.push_back({phrase, i, true});
        for (const auto& plugin : def.allFromEsp) m_espIndex[plugin].push_back(i);
    }

    spdlog::info("KeywordCategoryManager: loaded {} categories from {} file(s)", m_defs.size(), fileCount);
    return true;
}

size_t KeywordCategoryManager::GetDefinitionCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_defs.size();
}

float KeywordCategoryManager::GetProgress() const
{
    switch (m_state.load()) {
    case KeywordBuildState::NotStarted:          return 0.0f;
    case KeywordBuildState::WaitingForArmorCache: return 0.7f * ArmorModManager::GetSingleton()->GetLoadProgress();
    case KeywordBuildState::Matching:            return 0.7f + 0.3f * m_progressFraction;
    case KeywordBuildState::Complete:            return 1.0f;
    case KeywordBuildState::Failed:              return 0.0f;
    }
    return 0.0f;
}

void KeywordCategoryManager::StartBuildAsync(BuildCompleteCallback callback)
{
    bool alreadyComplete = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const auto state = m_state.load();
        if (state == KeywordBuildState::Complete) {
            alreadyComplete = true;
        } else if (state == KeywordBuildState::WaitingForArmorCache || state == KeywordBuildState::Matching) {
            // Build already running - just ride along with it.
            if (callback) m_callbacks.push_back(std::move(callback));
            return;
        } else {
            if (m_defs.empty()) {
                spdlog::error("KeywordCategoryManager: no categories defined, cannot build");
                m_state = KeywordBuildState::Failed;
            } else {
                if (callback) m_callbacks.push_back(std::move(callback));
                m_state = KeywordBuildState::WaitingForArmorCache;
            }
        }
    }

    if (alreadyComplete) {
        if (callback) callback(true);
        return;
    }

    if (m_state.load() == KeywordBuildState::Failed) {
        if (callback) callback(false);
        return;
    }

    // The mod gallery's scan already deduplicates armor by mesh, so reuse it rather
    // than walking the form array a second time.
    ArmorModManager::GetSingleton()->StartCacheBuildAsync([this](bool success) {
        if (!success) {
            Fail("armor cache build failed");
            return;
        }
        BeginMatching();
    });
}

void KeywordCategoryManager::BeginMatching()
{
    auto armor = ArmorModManager::GetSingleton()->GetAllCachedArmor();

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_buckets.assign(m_defs.size(), Bucket{});
        m_hit.assign(m_defs.size(), 0);
        m_veto.assign(m_defs.size(), 0);
        m_espHit.assign(m_defs.size(), 0);
        m_pendingArmor = std::move(armor);
        m_currentIndex = 0;
        m_progressFraction = 0.0f;
        m_state = KeywordBuildState::Matching;

        spdlog::debug("KeywordCategoryManager: matching {} armor items against {} categories",
            m_pendingArmor.size(), m_defs.size());
    }

    QueueNextBatch();
}

void KeywordCategoryManager::QueueNextBatch()
{
    auto* taskInterface = SKSE::GetTaskInterface();
    if (!taskInterface) {
        Fail("SKSE task interface unavailable");
        return;
    }

    taskInterface->AddTask([this]() { ProcessBatch(); });
}

void KeywordCategoryManager::ProcessBatch()
{
    bool done = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const size_t total = m_pendingArmor.size();
        size_t batchEnd = m_currentIndex + ITEMS_PER_FRAME;
        if (batchEnd > total) batchEnd = total;

        for (; m_currentIndex < batchEnd; ++m_currentIndex) {
            MatchArmor(m_pendingArmor[m_currentIndex]);
        }

        m_progressFraction = total ? static_cast<float>(m_currentIndex) / static_cast<float>(total) : 1.0f;
        done = m_currentIndex >= total;
    }

    if (done) {
        FinishMatching();
    } else {
        QueueNextBatch();
    }
}

void KeywordCategoryManager::MatchArmor(RE::TESObjectARMO* armor)
{
    if (!armor) return;

    const char* name = armor->GetFullName();
    if (!name || !*name) return;

    SplitWords(name, m_words);
    if (m_words.empty()) return;

    std::fill(m_hit.begin(), m_hit.end(), std::uint8_t{0});
    std::fill(m_veto.begin(), m_veto.end(), std::uint8_t{0});
    std::fill(m_espHit.begin(), m_espHit.end(), std::uint8_t{0});

    // Categories that take everything from this item's plugin, name notwithstanding.
    bool anyEspHit = false;
    if (!m_espIndex.empty()) {
        if (auto* file = armor->GetFile(0)) {
            std::string plugin(file->GetFilename());
            std::transform(plugin.begin(), plugin.end(), plugin.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (auto it = m_espIndex.find(plugin); it != m_espIndex.end()) {
                for (auto category : it->second) m_espHit[category] = 1;
                anyEspHit = true;
            }
        }
    }

    CollectTokens(m_words, m_tokens);
    for (const auto& token : m_tokens) {
        if (auto it = m_includeIndex.find(token); it != m_includeIndex.end()) {
            for (auto category : it->second) m_hit[category] = 1;
        }
        if (auto it = m_excludeIndex.find(token); it != m_excludeIndex.end()) {
            for (auto category : it->second) m_veto[category] = 1;
        }
    }

    for (const auto& entry : m_prefixEntries) {
        for (const auto& word : m_words) {
            if (word.starts_with(entry.text)) {
                (entry.exclude ? m_veto : m_hit)[entry.category] = 1;
                break;
            }
        }
    }

    // The allFromEsp exclusions are checked per category below and need this too.
    std::string padded;
    if (!m_longPhraseEntries.empty() || anyEspHit) {
        padded = " " + JoinWords(m_words) + " ";
    }

    for (const auto& entry : m_longPhraseEntries) {
        if (padded.find(entry.text) != std::string::npos) {
            (entry.exclude ? m_veto : m_hit)[entry.category] = 1;
        }
    }

    const auto slots = static_cast<std::uint32_t>(armor->GetSlotMask());

    for (size_t i = 0; i < m_defs.size(); ++i) {
        const auto& def = m_defs[i];

        bool matched = m_hit[i] && !m_veto[i];
        if (matched) {
            if (def.requireSlots && !(slots & def.requireSlots)) matched = false;
            if (def.excludeSlots && (slots & def.excludeSlots)) matched = false;
        }

        // Coming from a listed plugin is a second, independent way in. It ignores the
        // keywords, the exclude list and the slot filters alike - that is what "all from
        // this esp" means - so only allFromEspExcludeKeywords can veto it.
        if (!matched && m_espHit[i]) {
            matched = !AnyPatternMatches(def.allFromEspExcludePhrases, def.allFromEspExcludeLongPhrases,
                def.allFromEspExcludePrefixes, m_words, m_tokens, padded);
        }

        // Either way the item lands here once, so an item both named and listed is not doubled.
        if (matched) m_buckets[i].items.push_back(armor);
    }
}

void KeywordCategoryManager::FinishMatching()
{
    std::vector<BuildCompleteCallback> callbacks;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (size_t i = 0; i < m_buckets.size(); ++i) {
            auto& items = m_buckets[i].items;

            // An item with no mesh at all draws nothing and cannot be picked, so it would
            // only inflate the count shown on the category.
            const size_t matched = items.size();
            std::erase_if(items, [](RE::TESObjectARMO* armor) {
                return ItemEquipHelper::GetModelPath(armor).empty();
            });

            m_buckets[i].representative = PickRepresentative(items);

            if (items.size() != matched) {
                spdlog::trace("KeywordCategoryManager: [{}] {} items ({} matched, {} had no mesh)",
                    m_defs[i].name, items.size(), matched, matched - items.size());
            } else {
                spdlog::trace("KeywordCategoryManager: [{}] {} items", m_defs[i].name, items.size());
            }
        }

        m_pendingArmor.clear();
        m_pendingArmor.shrink_to_fit();
        m_state = KeywordBuildState::Complete;
        m_progressFraction = 1.0f;
        callbacks.swap(m_callbacks);

        size_t nonEmpty = 0;
        for (const auto& bucket : m_buckets) {
            if (!bucket.items.empty()) ++nonEmpty;
        }
        spdlog::info("KeywordCategoryManager: build complete, {}/{} categories have items",
            nonEmpty, m_buckets.size());
    }

    for (auto& callback : callbacks) {
        if (callback) callback(true);
    }
}

void KeywordCategoryManager::Fail(const char* reason)
{
    std::vector<BuildCompleteCallback> callbacks;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = KeywordBuildState::Failed;
        m_pendingArmor.clear();
        callbacks.swap(m_callbacks);
    }

    spdlog::error("KeywordCategoryManager: build failed - {}", reason);

    for (auto& callback : callbacks) {
        if (callback) callback(false);
    }
}

std::vector<KeywordCategoryInfo> KeywordCategoryManager::GetSortedCategories() const
{
    std::vector<KeywordCategoryInfo> result;

    if (!IsReady()) return result;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        result.reserve(m_buckets.size());
        for (size_t i = 0; i < m_buckets.size(); ++i) {
            if (m_buckets[i].items.empty()) continue;

            KeywordCategoryInfo info;
            info.name = m_defs[i].name;
            info.priority = m_defs[i].priority;
            info.itemCount = m_buckets[i].items.size();
            info.representative = m_buckets[i].representative;
            result.push_back(std::move(info));
        }
    }

    std::sort(result.begin(), result.end(), [](const KeywordCategoryInfo& a, const KeywordCategoryInfo& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.name < b.name;
    });

    return result;
}

std::vector<RE::TESObjectARMO*> KeywordCategoryManager::GetItemsForCategory(const std::string& name) const
{
    if (!IsReady()) return {};

    std::lock_guard<std::mutex> lock(m_mutex);

    for (size_t i = 0; i < m_defs.size(); ++i) {
        if (m_defs[i].name == name) return m_buckets[i].items;
    }

    return {};
}
