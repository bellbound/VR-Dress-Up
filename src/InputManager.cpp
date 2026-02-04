#include "InputManager.h"
#include "log.h"
#include <algorithm>

uint64_t InputManager::s_lastButtonState[2] = {0, 0};
uint64_t InputManager::s_blockedHeldButtons[2] = {0, 0};

InputManager* InputManager::GetSingleton()
{
	static InputManager instance;
	return &instance;
}

void InputManager::Initialize()
{
	if (m_initialized) {
		spdlog::warn("InputManager already initialized");
		return;
	}

	m_hookManager = RequestOpenVRHookManagerObject();
	if (!m_hookManager) {
		spdlog::error("Failed to get OpenVRHookManagerAPI - is SkyrimVRTools installed?");
		m_skyrimVRToolsMissing = true;
		return;
	}

	m_vrSystem = m_hookManager->GetVRSystem();
	if (!m_vrSystem) {
		spdlog::error("Failed to get IVRSystem from hook manager");
		return;
	}

	m_hookManager->RegisterControllerStateCB(&InputManager::OnControllerStateChanged);
	m_initialized = true;

	spdlog::info("InputManager initialized with raw OpenVR hook API");
}

void InputManager::Shutdown()
{
	if (!m_initialized) {
		return;
	}

	if (m_hookManager) {
		m_hookManager->UnregisterControllerStateCB(&InputManager::OnControllerStateChanged);
		m_hookManager = nullptr;
	}

	m_callbacks.clear();
	m_vrSystem = nullptr;
	m_initialized = false;
	s_blockedHeldButtons[0] = 0;
	s_blockedHeldButtons[1] = 0;
	spdlog::info("InputManager shut down");
}

InputManager::CallbackId InputManager::AddVrButtonCallback(uint64_t buttonMask, VrButtonCallback callback)
{
	CallbackId id = m_nextCallbackId++;
	m_callbacks.push_back({id, buttonMask, std::move(callback)});
	spdlog::info("Added VR button callback {} for mask 0x{:X}", id, buttonMask);
	return id;
}

void InputManager::RemoveVrButtonCallback(CallbackId id)
{
	if (id == InvalidCallbackId) {
		return;
	}

	auto it = std::find_if(m_callbacks.begin(), m_callbacks.end(),
		[id](const ButtonCallbackEntry& entry) { return entry.id == id; });

	if (it != m_callbacks.end()) {
		spdlog::info("Removed VR button callback {} for mask 0x{:X}", id, it->buttonMask);
		m_callbacks.erase(it);
	}
}

uint64_t InputManager::InvokeCallbacks(bool isLeft, bool isReleased, uint64_t changedButtons)
{
	uint64_t blockedButtons = 0;

	// IMPORTANT: Make a copy of callbacks before iterating!
	// Callbacks may register new callbacks (e.g., opening a menu registers its interaction callback).
	// Modifying m_callbacks during iteration would invalidate iterators and crash.
	auto callbacksCopy = m_callbacks;

	// Iterate through each bit that changed to find individual button IDs
	for (int buttonId = 0; buttonId < 64; ++buttonId) {
		uint64_t mask = 1ULL << buttonId;
		if (changedButtons & mask) {
			// This button changed - invoke all matching callbacks
			for (const auto& entry : callbacksCopy) {
				if (entry.buttonMask & mask) {
					if (entry.callback(isLeft, isReleased, static_cast<vr::EVRButtonId>(buttonId))) {
						blockedButtons |= mask;
					}
				}
			}
		}
	}

	return blockedButtons;
}

const char* InputManager::GetButtonName(uint64_t buttonMask)
{
	// Common VR controller buttons
	if (buttonMask & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) return "ApplicationMenu";
	if (buttonMask & vr::ButtonMaskFromId(vr::k_EButton_Grip)) return "Grip";
	if (buttonMask & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) return "Touchpad";
	if (buttonMask & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) return "Trigger";
	if (buttonMask & vr::ButtonMaskFromId(vr::k_EButton_A)) return "A";
	if (buttonMask & vr::ButtonMaskFromId(vr::k_EButton_System)) return "System";
	return "Unknown";
}

bool InputManager::OnControllerStateChanged(
	vr::TrackedDeviceIndex_t unControllerDeviceIndex,
	const vr::VRControllerState_t* pControllerState,
	uint32_t unControllerStateSize,
	vr::VRControllerState_t* pOutputControllerState)
{
	auto* instance = GetSingleton();
	if (!instance->m_vrSystem) {
		return false;
	}

	vr::TrackedDeviceIndex_t leftController = instance->m_vrSystem->GetTrackedDeviceIndexForControllerRole(
		vr::ETrackedControllerRole::TrackedControllerRole_LeftHand);
	vr::TrackedDeviceIndex_t rightController = instance->m_vrSystem->GetTrackedDeviceIndexForControllerRole(
		vr::ETrackedControllerRole::TrackedControllerRole_RightHand);

	int handIndex = -1;
	bool isLeft = false;

	if (unControllerDeviceIndex == leftController) {
		handIndex = 0;
		isLeft = true;
	} else if (unControllerDeviceIndex == rightController) {
		handIndex = 1;
		isLeft = false;
	}

	if (handIndex < 0) {
		return false;  // Unknown device
	}

	uint64_t currentButtons = pControllerState->ulButtonPressed;
	uint64_t lastButtons = s_lastButtonState[handIndex];

	// Debug: log full state when it changes
	if (currentButtons != lastButtons) {
		spdlog::info("[{}] State change: 0x{:X} -> 0x{:X}", isLeft ? "Left" : "Right", lastButtons, currentButtons);
	}

	// Detect newly pressed buttons (bits that are now 1 but were 0)
	uint64_t newlyPressed = currentButtons & ~lastButtons;
	// Detect newly released buttons (bits that were 1 but are now 0)
	uint64_t newlyReleased = lastButtons & ~currentButtons;

	if (newlyPressed) {
		spdlog::info("[{}] Button PRESSED: {} (mask: 0x{:X})", isLeft ? "Left" : "Right", GetButtonName(newlyPressed), newlyPressed);
		uint64_t blocked = instance->InvokeCallbacks(isLeft, false, newlyPressed);
		s_blockedHeldButtons[handIndex] |= blocked;  // Remember blocked buttons while held
	}

	if (newlyReleased) {
		spdlog::info("[{}] Button RELEASED: {} (mask: 0x{:X})", isLeft ? "Left" : "Right", GetButtonName(newlyReleased), newlyReleased);
		instance->InvokeCallbacks(isLeft, true, newlyReleased);
		s_blockedHeldButtons[handIndex] &= ~newlyReleased;  // Stop blocking on release
	}

	// Block consumed buttons from reaching the game - every frame while held
	if (s_blockedHeldButtons[handIndex] && pOutputControllerState) {
		pOutputControllerState->ulButtonPressed &= ~s_blockedHeldButtons[handIndex];
	}

	s_lastButtonState[handIndex] = currentButtons;

	return s_blockedHeldButtons[handIndex] != 0;
}
