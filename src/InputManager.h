#pragma once
#include "VRHookAPI.h"
#include <functional>
#include <vector>

class InputManager
{
public:
	// Return true to consume/block the input, false to let it pass through
	using VrButtonCallback = std::function<bool(bool isLeft, bool isReleased, vr::EVRButtonId buttonId)>;
	using CallbackId = uint32_t;
	static constexpr CallbackId InvalidCallbackId = 0;

	static InputManager* GetSingleton();

	void Initialize();
	void Shutdown();

	bool IsInitialized() const { return m_initialized; }
	bool IsSkyrimVRToolsMissing() const { return m_skyrimVRToolsMissing; }

	// Query if a button is currently pressed
	// buttonMask: VR button mask (e.g., vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger))
	// isLeftHand: which hand to check
	bool IsButtonPressed(uint64_t buttonMask, bool isLeftHand) const {
		int handIndex = isLeftHand ? 0 : 1;
		return (s_lastButtonState[handIndex] & buttonMask) != 0;
	}

	// Register a callback for specific button(s). Returns an ID for removal.
	CallbackId AddVrButtonCallback(uint64_t buttonMask, VrButtonCallback callback);

	// Remove a callback by its ID
	void RemoveVrButtonCallback(CallbackId id);

private:
	InputManager() = default;
	~InputManager() = default;
	InputManager(const InputManager&) = delete;
	InputManager& operator=(const InputManager&) = delete;

	static bool OnControllerStateChanged(
		vr::TrackedDeviceIndex_t unControllerDeviceIndex,
		const vr::VRControllerState_t* pControllerState,
		uint32_t unControllerStateSize,
		vr::VRControllerState_t* pOutputControllerState);

	uint64_t InvokeCallbacks(bool isLeft, bool isReleased, uint64_t changedButtons);
	static const char* GetButtonName(uint64_t buttonMask);

	struct ButtonCallbackEntry {
		CallbackId id;
		uint64_t buttonMask;
		VrButtonCallback callback;
	};

	OpenVRHookManagerAPI* m_hookManager = nullptr;
	vr::IVRSystem* m_vrSystem = nullptr;
	bool m_initialized = false;
	bool m_skyrimVRToolsMissing = false;
	std::vector<ButtonCallbackEntry> m_callbacks;
	CallbackId m_nextCallbackId = 1;  // 0 is InvalidCallbackId

	static uint64_t s_lastButtonState[2];     // Left=0, Right=1
	static uint64_t s_blockedHeldButtons[2];  // Buttons currently blocked while held
};
