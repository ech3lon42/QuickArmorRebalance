#pragma once

#ifdef _MSC_VER
    #define _CRT_SECURE_NO_WARNINGS
#endif


#include <unordered_set>

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#undef small

#include "SimpleIni.h"

#include "rapidjson/document.h"

#define TOML_EXCEPTIONS 0
#include "toml++/toml.hpp"

#include <rapidjson/fwd.h>

#define IMGUI_ENABLE_FREETYPE
#include <imgui.h>

using namespace std::literals;

#define PLUGIN_NAME "QuickArmorRebalance"

#include "logger.h"

#define FORMAT_HEX_FORMID "0x{:x}"

inline RE::FormID GetFullId(const RE::TESFile* file, RE::FormID id) {
    return ((RE::FormID)file->compileIndex << 24) | (file->smallFileCompileIndex << 12) | id;
}

inline RE::FormID GetFileId(const RE::TESForm* f) {
    auto file = f->GetFile(0);
    if (!file) {
        // Dynamic item without source file - return full formID
        return f->GetFormID();
    }
    auto id = f->GetLocalFormID();
    return file->IsLight() ? id & 0xfff : id & 0xffffff;
}

inline std::string QARFormID(const RE::TESForm* form) {
    auto file = form->GetFile(0);
    if (!file) {
        // Dynamic item without source file
        return std::format("<dynamic>:0x{:x}", form->GetFormID());
    }
    return std::format("{}:0x{:x}", file->fileName, GetFileId(form));
}

// Lookup a form from a QARFormID string (format: "ModName.esp:0xFormID" or "<dynamic>:0xFormID")
inline RE::TESForm* LookupForm(const std::string& formIDStr) {
    if (formIDStr.empty()) return nullptr;

    // Find the separator
    auto colonPos = formIDStr.find(':');
    if (colonPos == std::string::npos) return nullptr;

    std::string fileName = formIDStr.substr(0, colonPos);
    std::string idStr = formIDStr.substr(colonPos + 1);

    // Parse the hex form ID
    RE::FormID localFormID = 0;
    try {
        // Skip "0x" prefix if present
        if (idStr.size() > 2 && idStr[0] == '0' && (idStr[1] == 'x' || idStr[1] == 'X')) {
            idStr = idStr.substr(2);
        }
        localFormID = std::stoul(idStr, nullptr, 16);
    } catch (...) {
        return nullptr;
    }

    // Handle dynamic items
    if (fileName == "<dynamic>") {
        return RE::TESForm::LookupByID(localFormID);
    }

    // Lookup the mod file
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return nullptr;

    auto modFile = dataHandler->LookupModByName(fileName);
    if (!modFile) return nullptr;

    // Compute the full form ID
    RE::FormID fullFormID = GetFullId(modFile, localFormID);
    return RE::TESForm::LookupByID(fullFormID);
}

// Templated version that casts to the desired type
template <class T>
inline T* LookupForm(const std::string& formIDStr) {
    auto form = LookupForm(formIDStr);
    return form ? form->As<T>() : nullptr;
}

namespace QuickArmorRebalance {
    template <class T>
    void write_thunk_call() {
        auto& trampoline = SKSE::GetTrampoline();
        REL::Relocation<std::uintptr_t> hook{T::id, T::offset};
        T::func = trampoline.write_call<5>(hook.address(), T::thunk);
    }

    template <class T>
    void HookVirtualFunction() {
        auto vtbl = (std::uintptr_t*)T::id.address();

        T::func = vtbl[T::offset.offset()];
        REL::safe_write((std::uintptr_t)&vtbl[T::offset.offset()], (std::uintptr_t)T::thunk);
    }

    extern std::mt19937 RNG;

    inline float RNG_f() { return (float)((double)RNG() / (double)RNG.max()); }
}
