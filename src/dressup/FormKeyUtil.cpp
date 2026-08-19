#include "FormKeyUtil.h"
#include "../log.h"
#include <RE/T/TESFile.h>
#include <RE/T/TESDataHandler.h>
#include <fmt/format.h>
#include <charconv>
#include <cstdint>

namespace Persistence {

namespace {

// The plugin a runtime FormID's own index bits name, and the local id left over
// once those bits are stripped.
//
// The exact inverse of TESDataHandler::LookupFormID, which is what
// ResolveToRuntimeFormID goes through - deliberately, so a key built here always
// reads back as the form it was built from.
//
// Walks handler->files, the same list LookupModByName walks, rather than
// GetLoadedMods(): on VR that accessor is a hard-coded struct offset.
//
// Whether ESL space exists is asked of the *runtime*, not of IsVR(). Stock Skyrim
// VR 1.4.15 has none, so an 0xFE top byte there is a plain compile index of 254 -
// a real slot in a load order that big. SkyrimVRESL gives VR a genuine
// light-plugin collection, and branching on IsVR() would be wrong on one install
// or the other; GetLoadedLightModCount() answers the question actually being asked
// on both.
const RE::TESFile* OwningFile(RE::TESDataHandler* handler, RE::FormID formId,
                              RE::FormID& localId)
{
    if ((formId & 0xFF000000u) == 0xFE000000u && handler->GetLoadedLightModCount() > 0) {
        const auto smallIndex = static_cast<std::uint16_t>((formId & 0x00FFF000u) >> 12);
        localId = formId & 0x00000FFFu;
        for (const auto* file : handler->files) {
            if (file && file->compileIndex == 0xFE && file->smallFileCompileIndex == smallIndex) {
                return file;
            }
        }
        return nullptr;
    }

    const auto index = static_cast<std::uint8_t>((formId & 0xFF000000u) >> 24);
    localId = formId & 0x00FFFFFFu;
    for (const auto* file : handler->files) {
        if (file && file->compileIndex == index) {
            return file;
        }
    }
    return nullptr;
}

}  // namespace

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

    const RE::FormID formId = form->GetFormID();

    // A dynamic form is an allocator value private to one save. It names nothing
    // in another one, so it gets no key at all.
    if ((formId & 0xFF000000u) == 0xFF000000u) {
        spdlog::trace("FormKeyUtil: Form {:08X} is dynamic (no source plugin)", formId);
        return "";
    }

    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        spdlog::error("FormKeyUtil: TESDataHandler not available");
        return "";
    }

    // Deliberately *not* sourceFiles / GetFile(0). That array lists plugins that
    // touch the form, and for an overridden record it can hold only the overriding
    // one - the plugin that never declared this id. Base records keep an ordered
    // source list so they came out right; references do not, which is why this only
    // ever bit reference keys.
    //
    // Measured in DressUpVR.log 2026-08-19: three vanilla NPCs were keyed off the
    // plugin that merely overrides them - 0x000C3B2B and 0x000E1BA9 (Skyrim.esm)
    // as '0xC3B2B~3DNPC.esp' and '0xE1BA9~3DNPC.esp', 0x000198AD as
    // '0x198AD~AX ValSerano.esp'. Every one came back on load as
    // "FormKeyUtil: <id> not found in <plugin>", because those local ids exist in
    // no such plugin. Save-Migration hit the identical key on Keeper Carcette.
    //
    // The FormID's own index bits are not a hint, they are the answer: the engine
    // builds a runtime id by pasting the owning plugin's load order index onto its
    // local id, and that is what the in-game console reports.
    RE::FormID localId = 0;
    const auto* file = OwningFile(handler, formId, localId);
    if (!file) {
        spdlog::warn("FormKeyUtil: no loaded plugin holds the index of form {:08X}", formId);
        return "";
    }

    return BuildFormKey(localId, file->GetFilename());
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
        spdlog::warn("FormKeyUtil: 0x{:X} not found in {}", parsed->localFormId, parsed->pluginName);
        return 0;
    }
    return form->GetFormID();
}

} // namespace Persistence
