#include "PapyrusBridge.h"
#include <spdlog/spdlog.h>

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
