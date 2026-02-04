#pragma once

#if !defined(TEST_ENVIRONMENT)
#include "RE/Skyrim.h"
#else
#include "../Tests/TestStubs.h"
#endif

namespace VRNodes {

#if !defined(TEST_ENVIRONMENT)

// Get the VR node data from PlayerCharacter (VR-specific API)
// Returns nullptr if not in VR mode
inline RE::VR_NODE_DATA* GetVRNodeData() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return nullptr;
    }
    return player->GetVRNodeData();
}

// Get the left VR controller node (actual tracked position)
// Returns nullptr if not in VR or node not found
inline RE::NiAVObject* GetLeftHand() {
    auto* vrData = GetVRNodeData();
    if (vrData && vrData->LeftWandNode) {
        return vrData->LeftWandNode.get();
    }
    return nullptr;
}

// Get the right VR controller node (actual tracked position)
// Returns nullptr if not in VR or node not found
inline RE::NiAVObject* GetRightHand() {
    auto* vrData = GetVRNodeData();
    if (vrData && vrData->RightWandNode) {
        return vrData->RightWandNode.get();
    }
    return nullptr;
}

// Get the HMD (head-mounted display) node
// Returns nullptr if not in VR or node not found
inline RE::NiAVObject* GetHMD() {
    auto* vrData = GetVRNodeData();
    if (vrData && vrData->UprightHmdNode) {
        return vrData->UprightHmdNode.get();
    }
    return nullptr;
}

// =============================================================================
// Skeleton Bone Accessors (animation-driven, different from VR tracked nodes)
// =============================================================================

// Get the left hand skeleton bone (NPC L Hand [LHnd])
// NOTE: This is the character's animated hand bone, NOT the VR controller position!
// Use GetLeftHand() for the actual VR controller tracking.
inline RE::NiAVObject* GetLeftHandBone() {
    auto* vrData = GetVRNodeData();
    if (vrData && vrData->NPCLHnd) {
        return vrData->NPCLHnd.get();
    }
    return nullptr;
}

// Get the right hand skeleton bone (NPC R Hand [RHnd])
// NOTE: This is the character's animated hand bone, NOT the VR controller position!
// Use GetRightHand() for the actual VR controller tracking.
inline RE::NiAVObject* GetRightHandBone() {
    auto* vrData = GetVRNodeData();
    if (vrData && vrData->NPCRHnd) {
        return vrData->NPCRHnd.get();
    }
    return nullptr;
}

// Get a named node from player skeleton by name
// Returns nullptr if player not available or node not found
inline RE::NiAVObject* GetPlayerNode(std::string_view nodeName) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return nullptr;
    }

    auto* root = player->Get3D();
    if (!root) {
        return nullptr;
    }

    return root->GetObjectByName(nodeName);
}

#else // TEST_ENVIRONMENT - stub implementations for unit tests

inline RE::NiAVObject* GetLeftHand() { return nullptr; }
inline RE::NiAVObject* GetRightHand() { return nullptr; }
inline RE::NiAVObject* GetHMD() { return nullptr; }
inline RE::NiAVObject* GetLeftHandBone() { return nullptr; }
inline RE::NiAVObject* GetRightHandBone() { return nullptr; }
inline RE::NiAVObject* GetPlayerNode(std::string_view) { return nullptr; }

#endif // TEST_ENVIRONMENT

} // namespace VRNodes
