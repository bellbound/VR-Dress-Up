#include "PapyrusBridge.h"

#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>

// Windows.h turns GetObject into GetObjectA, and something in the PCH pulls it back in
// after Variable.h has already undone it.
#undef GetObject

namespace PapyrusBridge
{
    namespace
    {
        // Adapts a std::function onto the VM's result callback interface.
        class IntCallback : public RE::BSScript::IStackCallbackFunctor
        {
        public:
            explicit IntCallback(IntResult fn) : m_fn(std::move(fn)) {}

            void operator()(RE::BSScript::Variable a_result) override
            {
                if (!m_fn) return;
                if (a_result.IsInt()) {
                    m_fn(true, a_result.GetSInt());
                } else {
                    m_fn(false, 0);
                }
            }

            bool CanSave() const override { return false; }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            IntResult m_fn;
        };

        // Same, for a Bool return.
        class BoolCallback : public RE::BSScript::IStackCallbackFunctor
        {
        public:
            explicit BoolCallback(BoolResult fn) : m_fn(std::move(fn)) {}

            void operator()(RE::BSScript::Variable a_result) override
            {
                if (!m_fn) return;
                if (a_result.IsBool()) {
                    m_fn(true, a_result.GetBool());
                } else {
                    m_fn(false, false);
                }
            }

            bool CanSave() const override { return false; }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            BoolResult m_fn;
        };

        // Same, for a call whose return value is a form.
        class FormCallback : public RE::BSScript::IStackCallbackFunctor
        {
        public:
            FormCallback(RE::FormType formType, FormResult fn)
                : m_formType(formType), m_fn(std::move(fn)) {}

            void operator()(RE::BSScript::Variable a_result) override
            {
                if (!m_fn) return;

                RE::TESForm* form = nullptr;
                if (a_result.IsObject() && !a_result.IsNoneObject()) {
                    if (auto object = a_result.GetObject()) {
                        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
                        auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
                        if (policy) {
                            // The VMTypeID overload, spelled out: FormType converts to
                            // VMTypeID implicitly, so the two overloads are ambiguous.
                            void* raw = policy->GetObjectForHandle(
                                static_cast<RE::VMTypeID>(m_formType), object->GetHandle());
                            form = static_cast<RE::TESForm*>(raw);
                        }
                    }
                }
                m_fn(form);
            }

            bool CanSave() const override { return false; }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            RE::FormType m_formType;
            FormResult   m_fn;
        };

        RE::BSScript::IVirtualMachine* GetVM()
        {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                spdlog::error("PapyrusBridge - No VirtualMachine");
            }
            return vm;
        }

        // A dispatch with no interest in the result still needs a callback slot.
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> NullCallback()
        {
            return RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>{};
        }
    }

    bool CallActorSetOutfit(RE::Actor* actor, RE::BGSOutfit* outfit, bool sleepOutfit)
    {
        if (!actor || !outfit) {
            spdlog::error("PapyrusBridge::CallActorSetOutfit - Null actor or outfit");
            return false;
        }

        auto* vm = GetVM();
        if (!vm) return false;

        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            spdlog::error("PapyrusBridge::CallActorSetOutfit - No object handle policy");
            return false;
        }

        const auto handle = policy->GetHandleForObject(
            static_cast<RE::VMTypeID>(RE::FormType::ActorCharacter), actor);
        if (handle == policy->EmptyHandle()) {
            spdlog::warn("PapyrusBridge::CallActorSetOutfit - No VM handle for '{}'", actor->GetName());
            return false;
        }

        auto callback = NullCallback();
        auto* args = RE::MakeFunctionArguments(
            static_cast<RE::BGSOutfit*>(outfit), static_cast<bool>(sleepOutfit));

        const bool ok = vm->DispatchMethodCall(handle, "Actor", "SetOutfit", args, callback);
        if (!ok) {
            spdlog::warn("PapyrusBridge::CallActorSetOutfit - Dispatch failed for '{}'", actor->GetName());
        }
        return ok;
    }

    bool CallGlobal(const char* className, const char* fnName,
                    RE::BSScript::IFunctionArguments* args)
    {
        auto* vm = GetVM();
        if (!vm) return false;

        auto callback = NullCallback();
        const bool ok = vm->DispatchStaticCall(className, fnName, args, callback);
        if (!ok) {
            spdlog::warn("PapyrusBridge::CallGlobal - {}.{} dispatch failed", className, fnName);
        }
        return ok;
    }

    bool CallGlobalInt(const char* className, const char* fnName,
                       RE::BSScript::IFunctionArguments* args, IntResult result)
    {
        auto* vm = GetVM();
        if (!vm) return false;

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new IntCallback(std::move(result)));
        const bool ok = vm->DispatchStaticCall(className, fnName, args, callback);
        if (!ok) {
            spdlog::warn("PapyrusBridge::CallGlobalInt - {}.{} dispatch failed", className, fnName);
        }
        return ok;
    }

    bool CallGlobalBool(const char* className, const char* fnName,
                        RE::BSScript::IFunctionArguments* args, BoolResult result)
    {
        auto* vm = GetVM();
        if (!vm) return false;

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new BoolCallback(std::move(result)));
        const bool ok = vm->DispatchStaticCall(className, fnName, args, callback);
        if (!ok) {
            spdlog::warn("PapyrusBridge::CallGlobalBool - {}.{} dispatch failed", className, fnName);
        }
        return ok;
    }

    bool CallMethodInt(RE::TESForm* target, RE::FormType targetType,
                       const char* scriptName, const char* fnName,
                       RE::BSScript::IFunctionArguments* args, IntResult result)
    {
        if (!target) {
            spdlog::error("PapyrusBridge::CallMethodInt - Null target for {}.{}", scriptName, fnName);
            return false;
        }

        auto* vm = GetVM();
        if (!vm) return false;

        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) return false;

        const auto handle = policy->GetHandleForObject(static_cast<RE::VMTypeID>(targetType), target);
        if (handle == policy->EmptyHandle()) {
            spdlog::warn("PapyrusBridge::CallMethodInt - No VM handle for {}.{}", scriptName, fnName);
            return false;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new IntCallback(std::move(result)));
        const bool ok = vm->DispatchMethodCall(handle, scriptName, fnName, args, callback);
        if (!ok) {
            spdlog::warn("PapyrusBridge::CallMethodInt - {}.{} dispatch failed", scriptName, fnName);
        }
        return ok;
    }

    bool CallMethod(RE::TESForm* target, RE::FormType targetType,
                    const char* scriptName, const char* fnName,
                    RE::BSScript::IFunctionArguments* args)
    {
        if (!target) {
            spdlog::error("PapyrusBridge::CallMethod - Null target for {}.{}", scriptName, fnName);
            return false;
        }

        auto* vm = GetVM();
        if (!vm) return false;

        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) return false;

        const auto handle = policy->GetHandleForObject(static_cast<RE::VMTypeID>(targetType), target);
        if (handle == policy->EmptyHandle()) {
            spdlog::warn("PapyrusBridge::CallMethod - No VM handle for {}.{}", scriptName, fnName);
            return false;
        }

        auto callback = NullCallback();
        const bool ok = vm->DispatchMethodCall(handle, scriptName, fnName, args, callback);
        if (!ok) {
            spdlog::warn("PapyrusBridge::CallMethod - {}.{} dispatch failed", scriptName, fnName);
        }
        return ok;
    }

    RE::TESQuest* FindQuestWithScript(const char* scriptName)
    {
        if (!scriptName || !*scriptName) return nullptr;

        auto* vm = GetVM();
        if (!vm) return nullptr;

        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) return nullptr;

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return nullptr;

        const auto questType = static_cast<RE::VMTypeID>(RE::FormType::Quest);

        for (auto* form : dataHandler->GetFormArray<RE::TESQuest>()) {
            auto* quest = form ? form->As<RE::TESQuest>() : nullptr;
            if (!quest) continue;

            const auto handle = policy->GetHandleForObject(questType, quest);
            if (handle == policy->EmptyHandle()) continue;

            RE::BSTSmartPointer<RE::BSScript::Object> object;
            if (vm->FindBoundObject(handle, scriptName, object) && object) {
                return quest;
            }
        }

        return nullptr;
    }

    bool CallGlobalForm(const char* className, const char* fnName, RE::FormType formType,
                        RE::BSScript::IFunctionArguments* args, FormResult result)
    {
        auto* vm = GetVM();
        if (!vm) return false;

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(
            new FormCallback(formType, std::move(result)));
        const bool ok = vm->DispatchStaticCall(className, fnName, args, callback);
        if (!ok) {
            spdlog::warn("PapyrusBridge::CallGlobalForm - {}.{} dispatch failed", className, fnName);
        }
        return ok;
    }

    void RunAfterMs(std::int32_t delayMs, std::function<void()> fn)
    {
        if (!fn) return;

        std::thread([delayMs, fn = std::move(fn)]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask(std::move(fn));
            }
        }).detach();
    }

    void SendModEvent(const std::string& eventName, const std::string& strArg,
                      float numArg, RE::TESForm* sender)
    {
        auto* source = SKSE::GetModCallbackEventSource();
        if (!source) {
            spdlog::error("PapyrusBridge::SendModEvent - No mod callback event source");
            return;
        }

        SKSE::ModCallbackEvent event{
            RE::BSFixedString(eventName.c_str()),
            RE::BSFixedString(strArg.c_str()),
            numArg,
            sender
        };

        source->SendEvent(&event);
        spdlog::info("PapyrusBridge::SendModEvent - Sent '{}' strArg='{}'", eventName, strArg);
    }
}
