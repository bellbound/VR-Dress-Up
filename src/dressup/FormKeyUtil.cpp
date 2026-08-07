#include "FormKeyUtil.h"
#include "../log.h"
#include <RE/T/TESFile.h>
#include <RE/T/TESDataHandler.h>
#include <fmt/format.h>
#include <charconv>

namespace Persistence {

std::string FormKeyUtil::BuildFormKey(RE::TESObjectREFR* ref)
{
    if (!ref) {
        return "";
    }
    return BuildFormKey(static_cast<RE::TESForm*>(ref));
}

std::string FormKeyUtil::BuildFormKey(RE::TESForm* form)
{
    if (!form) {
        return "";
    }

    // Get the originating file (index 0 = first/primary source file)
    auto* file = form->GetFile(0);
    if (!file) {
        spdlog::trace("FormKeyUtil: Form {:08X} has no source file (dynamic form?)",
            form->GetFormID());
        return "";
    }

    RE::FormID localId = form->GetLocalFormID();
    std::string_view pluginName = file->GetFilename();

    return BuildFormKey(localId, pluginName);
}

std::string FormKeyUtil::BuildFormKey(RE::FormID localFormId, std::string_view pluginName)
{
    // Format: "0x[hex]~[name]"
    return fmt::format("0x{:X}~{}", localFormId, pluginName);
}

std::optional<FormKeyUtil::ParsedKey> FormKeyUtil::ParseFormKey(std::string_view keyString)
{
    // Expected format: "0x[hex]~[pluginName]"
    // Example: "0x10C0E3~Skyrim.esm"

    if (keyString.size() < 4) {  // Minimum: "0x0~X"
        return std::nullopt;
    }

    // Must start with "0x"
    if (keyString.substr(0, 2) != "0x") {
        return std::nullopt;
    }

    // Find the tilde separator
    size_t tildePos = keyString.find('~');
    if (tildePos == std::string_view::npos || tildePos <= 2) {
        return std::nullopt;
    }

    // Extract hex portion (after "0x", before "~")
    std::string_view hexPart = keyString.substr(2, tildePos - 2);
    if (hexPart.empty()) {
        return std::nullopt;
    }

    // Parse hex to FormID
    RE::FormID localFormId = 0;
    auto result = std::from_chars(hexPart.data(), hexPart.data() + hexPart.size(),
                                   localFormId, 16);
    if (result.ec != std::errc{} || result.ptr != hexPart.data() + hexPart.size()) {
        return std::nullopt;
    }

    // Extract plugin name (after "~")
    std::string_view pluginName = keyString.substr(tildePos + 1);
    if (pluginName.empty()) {
        return std::nullopt;
    }

    return ParsedKey{ localFormId, std::string(pluginName) };
}

RE::FormID FormKeyUtil::ResolveToRuntimeFormID(std::string_view keyString)
{
    auto parsed = ParseFormKey(keyString);
    if (!parsed) {
        spdlog::warn("FormKeyUtil: Failed to parse key string: {}", keyString);
        return 0;
    }

    // Special case: DYNAMIC forms are runtime-created objects
    // Their FormID in the key IS already the runtime FormID (0xFF range)
    if (parsed->pluginName == "DYNAMIC") {
        spdlog::trace("FormKeyUtil: Resolved DYNAMIC form key {} to {:08X}",
            keyString, parsed->localFormId);
        return parsed->localFormId;
    }

    // Find the plugin file by name
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        spdlog::error("FormKeyUtil: TESDataHandler not available");
        return 0;
    }

    const RE::TESFile* file = nullptr;

    // Search in loaded mods
    for (auto* mod : dataHandler->files) {
        if (mod && mod->GetFilename() == parsed->pluginName) {
            file = mod;
            break;
        }
    }

    if (!file) {
        spdlog::warn("FormKeyUtil: Plugin not loaded: {}", parsed->pluginName);
        return 0;
    }

    // Resolve through TESDataHandler rather than synthesising the runtime FormID
    // by hand.
    //
    // The previous implementation branched on `TESFile::IsLight()` and built
    // `0xFE000000 | (smallFileCompileIndex << 12)`. `IsLight()` reports the
    // plugin's own kSmallFile header flag regardless of whether the *runtime*
    // supports ESLs - and Skyrim VR (1.4.15) has no ESL space at all. On VR an
    // ESL-flagged plugin occupies an ordinary compile-index slot, so that
    // synthesis produced an ID that resolves to nothing.
    //
    // That mattered a great deal here: a null lookup makes SavedArmorItem::GetArmor()
    // return nullptr, ApplyOutfit treats the item as "no longer valid", and it is
    // then **permanently removed from the stored outfit**. On VR, every ESL-sourced
    // armour piece was being silently deleted from saved outfits.
    //
    // TESDataHandler::LookupForm contains the REL::Module::IsVR() branch and gets
    // this right on both runtimes.
    auto* form = dataHandler->LookupForm(parsed->localFormId, file->GetFilename());
    if (!form) {
        spdlog::warn("FormKeyUtil: {} not found in {}", parsed->localFormId, parsed->pluginName);
        return 0;
    }
    return form->GetFormID();
}

} // namespace Persistence
