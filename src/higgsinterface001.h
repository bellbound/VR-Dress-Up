#pragma once

// Simplified HIGGS interface for DressUpVR
// Based on activeragdoll's higgsinterface001.h

namespace HiggsPluginAPI {

    struct IHiggsInterface001;

    // Returns an IHiggsInterface001 object compatible with the API shown below
    // This should only be called after SKSE sends kMessage_PostPostLoad to your plugin
    IHiggsInterface001* GetHiggsInterface001(const SKSE::MessagingInterface* messagingInterface);

    struct IHiggsInterface001
    {
        // Gets the HIGGS build number
        virtual unsigned int GetBuildNumber() = 0;

        // Callbacks for when an object is pulled, grabbed, or dropped/thrown
        typedef void(*PulledCallback)(bool isLeft, RE::TESObjectREFR* pulledRefr);
        virtual void AddPulledCallback(PulledCallback callback) = 0;

        typedef void(*GrabbedCallback)(bool isLeft, RE::TESObjectREFR* grabbedRefr);
        virtual void AddGrabbedCallback(GrabbedCallback callback) = 0;

        typedef void(*DroppedCallback)(bool isLeft, RE::TESObjectREFR* droppedRefr);
        virtual void AddDroppedCallback(DroppedCallback callback) = 0;

        typedef void(*StashedCallback)(bool isLeft, RE::TESForm* stashedForm);
        virtual void AddStashedCallback(StashedCallback callback) = 0;

        typedef void(*ConsumedCallback)(bool isLeft, RE::TESForm* consumedForm);
        virtual void AddConsumedCallback(ConsumedCallback callback) = 0;

        typedef void(*CollisionCallback)(bool isLeft, float mass, float separatingVelocity);
        virtual void AddCollisionCallback(CollisionCallback callback) = 0;

        virtual void GrabObject(RE::TESObjectREFR* object, bool isLeft) = 0;
        virtual RE::TESObjectREFR* GetGrabbedObject(bool isLeft) = 0;
        virtual bool IsHandInGrabbableState(bool isLeft) = 0;

        virtual void DisableHand(bool isLeft) = 0;
        virtual void EnableHand(bool isLeft) = 0;
        virtual bool IsDisabled(bool isLeft) = 0;

        virtual void DisableWeaponCollision(bool isLeft) = 0;
        virtual void EnableWeaponCollision(bool isLeft) = 0;
        virtual bool IsWeaponCollisionDisabled(bool isLeft) = 0;

        virtual bool IsTwoHanding() = 0;

        typedef void(*StartTwoHandingCallback)();
        virtual void AddStartTwoHandingCallback(StartTwoHandingCallback callback) = 0;

        typedef void(*StopTwoHandingCallback)();
        virtual void AddStopTwoHandingCallback(StopTwoHandingCallback callback) = 0;

        virtual bool CanGrabObject(bool isLeft) = 0;

        enum class CollisionFilterComparisonResult : uint8_t {
            Continue,
            Collide,
            Ignore,
        };
        typedef CollisionFilterComparisonResult(*CollisionFilterComparisonCallback)(void* collisionFilter, uint32_t filterInfoA, uint32_t filterInfoB);
        virtual void AddCollisionFilterComparisonCallback(CollisionFilterComparisonCallback callback) = 0;

        // Add a callback for right before hkpWorld::stepDeltaTime is called.
        // world is really of type bhkWorld
        typedef void(*PrePhysicsStepCallback)(void* world);
        virtual void AddPrePhysicsStepCallback(PrePhysicsStepCallback callback) = 0;

        virtual uint64_t GetHiggsLayerBitfield() = 0;
        virtual void SetHiggsLayerBitfield(uint64_t bitfield) = 0;

        // Get the hand and weapon rigidbodies that higgs creates. Return type is bhkRigidBody.
        virtual RE::NiObject* GetHandRigidBody(bool isLeft) = 0;
        virtual RE::NiObject* GetWeaponRigidBody(bool isLeft) = 0;

        // Get the currently held rigid body. Return type is actually bhkRigidBody.
        virtual RE::NiObject* GetGrabbedRigidBody(bool isLeft) = 0;

        virtual void ForceWeaponCollisionEnabled(bool isLeft) = 0;
        virtual bool IsHoldingObject(bool isLeft) = 0;
        virtual void GetFingerValues(bool isLeft, float values[5]) = 0;

        typedef void(*NoArgCallback)();
        virtual void AddPreVrikPreHiggsCallback(NoArgCallback callback) = 0;
        virtual void AddPreVrikPostHiggsCallback(NoArgCallback callback) = 0;
        virtual void AddPostVrikPreHiggsCallback(NoArgCallback callback) = 0;
        virtual void AddPostVrikPostHiggsCallback(NoArgCallback callback) = 0;

        // Deprecated
        virtual bool Deprecated1(const std::string_view& name, double& out) = 0;
        virtual bool Deprecated2(const std::string& name, double val) = 0;

        // Get/set the transform of the current grabbed object
        virtual RE::NiTransform GetGrabTransform(bool isLeft) = 0;
        virtual void SetGrabTransform(bool isLeft, const RE::NiTransform& transform) = 0;

        virtual bool GetSettingDouble(const char* name, double& out) = 0;
        virtual bool SetSettingDouble(const char* name, double val) = 0;
    };
}

extern HiggsPluginAPI::IHiggsInterface001* g_higgsInterface;
