#pragma once

#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESDataHandler.h>
#include <string>
#include <optional>

namespace Persistence {

// Utilities for building stable FormID key strings
// Format: "0x[LocalFormID]~[PluginName]"
// Example: "0x10C0E3~Skyrim.esm"
//
// This format is stable across sessions because:
// - LocalFormID is the form's ID without load order bits (doesn't change)
// - PluginName identifies which mod the form came from (doesn't change)
// - Together they uniquely identify a form regardless of load order
class FormKeyUtil {
public:
    // Build a key string from a TESObjectREFR
    // Returns empty string if ref is null or has no source file
    static std::string BuildFormKey(RE::TESObjectREFR* ref);

    // Build a key string from any TESForm
    static std::string BuildFormKey(RE::TESForm* form);

    // Build a key string from explicit parts
    static std::string BuildFormKey(RE::FormID localFormId, std::string_view pluginName);

    // Parsed key structure
    struct ParsedKey {
        RE::FormID localFormId;
        std::string pluginName;
    };

    // Parse a key string back to parts
    // Returns nullopt if parsing fails
    static std::optional<ParsedKey> ParseFormKey(std::string_view keyString);

    // Try to resolve a key string to a runtime FormID
    // Returns 0 if resolution fails (e.g., plugin not loaded)
    static RE::FormID ResolveToRuntimeFormID(std::string_view keyString);
};

} // namespace Persistence
