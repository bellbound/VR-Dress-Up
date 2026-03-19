#pragma once

// =============================================================================
// 3D UI Plugin Interface v001
// =============================================================================
// This header is designed to be copied to other SKSE projects.
// Provides a VR-ready 3D UI system with projectile-based rendering.
//
// Quick Start:
//   1. Get interface after PostPostLoad: auto* api = P3DUI::GetInterface001();
//   2. Create a root: auto* root = api->CreateRoot(RootConfig::Default("myMenu", "MyModName"));
//   3. Add containers: auto* wheel = api->CreateScrollWheel(ScrollWheelConfig::Default("items"));
//   4. Add items: auto* sword = api->CreateElement(ElementConfig::Default("sword"));
//   5. Add to hierarchy: wheel->AddChild(sword); root->AddChild(wheel);
//   6. Show: root->SetVisible(true);
//   7. Handle events via the EventCallback set in RootConfig
//
// All types use virtual methods for ABI stability across DLL boundaries.
// Only POD types in structs - no std:: types.
//
// Thread Safety:
//   All API calls must be made from the game's main thread.
//   The EventCallback is invoked on the main thread during the frame update.
//
// Pointer Lifetime:
//   Event::source and Event::sourceID are only valid for the duration of the
//   EventCallback invocation. Do not store these pointers.
//
// Ownership:
//   All created objects are owned by the implementation. Callers receive
//   pointers but do not own the memory. Use SetVisible(false) to hide/recycle.
//
// Config Struct Versioning:
//   All config structs have a structSize field as the first member.
//   Always use the Default() factory which sets this correctly.
//   This allows future fields to be added without breaking ABI.
// =============================================================================

#include <cstdint>

// Forward declaration for Skyrim types (consumer must have CommonLib)
namespace RE {
    class NiAVObject;
}

namespace P3DUI {

// =============================================================================
// Forward Declarations
// =============================================================================

struct Positionable;
struct Element;
struct Text;
struct Container;
struct ScrollableContainer;
struct Root;
struct Interface001;

// =============================================================================
// Enums
// =============================================================================

// Note on enum Unknown values:
// Using 0xFFFFFFFF ensures new enum values can be added in the middle without
// breaking switch statements in consumer code. Consumers should handle Unknown
// gracefully (ignore or use default behavior) rather than assuming exhaustive matching.

enum class FacingMode : uint32_t {
    None     = 0,       // No automatic rotation
    Full     = 1,       // Full 3D facing toward anchor (formerly FacePlayer/FaceHMD)
    YawOnly  = 2,       // Only rotate around vertical axis
    _Unknown = 0xFFFFFFFF
};

// VR anchor types for positioning relative to VR hardware
enum class VRAnchorType : uint32_t {
    None      = 0,    // No VR anchor (use world position)
    HMD       = 1,    // Head-mounted display
    LeftHand  = 2,    // Left controller
    RightHand = 3,    // Right controller
    _Unknown  = 0xFFFFFFFF
};

// =============================================================================
// Grid Layout Enums - Fill Direction and Origin (Anchor) Point
// =============================================================================
// These enums are DECOUPLED:
// - Fill direction: Controls how items are ordered/indexed in the grid
// - Origin: Controls where the grid's (0,0,0) point is relative to content

// Vertical fill direction - how rows are ordered
enum class VerticalFill : uint32_t {
    TopToBottom = 0,    // Row 0 at top, subsequent rows go downward
    BottomToTop = 1,    // Row 0 at bottom, subsequent rows go upward
    _Unknown = 0xFFFFFFFF
};

// Horizontal fill direction - how columns are ordered
enum class HorizontalFill : uint32_t {
    LeftToRight = 0,    // Column 0 at left, subsequent columns go rightward
    RightToLeft = 1,    // Column 0 at right, subsequent columns go leftward
    _Unknown = 0xFFFFFFFF
};

// Vertical origin (anchor) position - where local (0,0,0) is vertically
enum class VerticalOrigin : uint32_t {
    Top    = 0,    // Grid's top edge is at Z=0
    Center = 1,    // Grid's vertical center is at Z=0
    Bottom = 2,    // Grid's bottom edge is at Z=0
    _Unknown = 0xFFFFFFFF
};

// Horizontal origin (anchor) position - where local (0,0,0) is horizontally
enum class HorizontalOrigin : uint32_t {
    Left   = 0,    // Grid's left edge is at X=0
    Center = 1,    // Grid's horizontal center is at X=0
    Right  = 2,    // Grid's right edge is at X=0
    _Unknown = 0xFFFFFFFF
};

// =============================================================================
// Event System
// =============================================================================

enum class EventType : uint32_t {
    HoverEnter   = 0,     // Hand entered hover threshold
    HoverExit    = 1,     // Hand exited hover threshold
    GrabStart    = 2,     // Grab button pressed while hovering
    GrabEnd      = 3,     // Grab button released
    ActivateDown = 4,     // Activation button pressed while hovering
    ActivateUp   = 5,     // Activation button released
    _Unknown     = 0xFFFFFFFF
};

struct Event {
    uint32_t structSize;            // Must be sizeof(Event) - for ABI versioning
    EventType type;                 // What happened
    Positionable* source;           // Item that triggered the event
    const char* sourceID;           // ID string of the source item
    RE::NiAVObject* handNode;       // VR hand node involved (may be null)
    bool isLeftHand;                // Which hand triggered the event
};

// Callback type for receiving all events from a Root hierarchy
// Return true to consume the event, false to let it propagate
typedef bool (*EventCallback)(const Event* event);

// =============================================================================
// Configuration Structures (POD - use static Default() for defaults)
// =============================================================================

struct RootConfig {
    uint32_t structSize;            // Must be sizeof(RootConfig) - for ABI versioning
    const char* id;                 // Required: unique identifier for this root
    const char* modId;              // Required: mod identifier for logging (e.g., "DressUpVR")
    bool interactive;               // Enable VR interaction (hover, grab, activate)
    EventCallback eventCallback;    // Receives all events from this hierarchy
    uint64_t activationButtonMask;  // VR button mask for activation
    uint64_t grabButtonMask;        // VR button mask for grab
    float hoverThreshold;           // Distance for hover detection (exit uses 1.01x for hysteresis)

    static RootConfig Default(const char* id, const char* modId) {
        RootConfig c{};
        c.structSize = sizeof(RootConfig);
        c.id = id;
        c.modId = modId;
        c.interactive = true;
        c.eventCallback = nullptr;
        // Default to VR trigger/grip buttons (OpenVR: k_EButton_SteamVR_Trigger=33, k_EButton_Grip=2)
        c.activationButtonMask = (1ULL << 33);  // SteamVR Trigger
        c.grabButtonMask = (1ULL << 2);         // Grip
        c.hoverThreshold = 10.0f;
        return c;
    }
};

struct ScrollWheelConfig {
    uint32_t structSize;            // Must be sizeof(ScrollWheelConfig) - for ABI versioning
    const char* id;                 // Required: unique identifier
    float itemSpacing;              // Distance between items within a ring
    float ringSpacing;              // Distance between concentric rings
    float firstRingSpacing;         // Distance from center to first ring (0 = use ringSpacing)

    static ScrollWheelConfig Default(const char* id) {
        ScrollWheelConfig c{};
        c.structSize = sizeof(ScrollWheelConfig);
        c.id = id;
        c.itemSpacing = 8.0f;
        c.ringSpacing = 10.0f;
        c.firstRingSpacing = 15.0f;
        return c;
    }
};

struct WheelConfig {
    uint32_t structSize;            // Must be sizeof(WheelConfig) - for ABI versioning
    const char* id;                 // Required: unique identifier
    float itemSpacing;              // Target distance between items within a ring
    float ringSpacing;              // Distance between concentric rings

    static WheelConfig Default(const char* id) {
        WheelConfig c{};
        c.structSize = sizeof(WheelConfig);
        c.id = id;
        c.itemSpacing = 15.0f;
        c.ringSpacing = 15.0f;
        return c;
    }
};

// Configuration for column-major grid with horizontal scrolling
// Items fill vertically first (top-to-bottom), then wrap to next column
struct ColumnGridConfig {
    uint32_t structSize;            // Must be sizeof(ColumnGridConfig) - for ABI versioning
    const char* id;                 // Required: unique identifier
    float columnSpacing;            // Horizontal distance between columns
    float rowSpacing;               // Vertical distance between rows
    uint32_t numRows;               // Number of rows per column (items wrap to next column after this)
    float visibleWidth;             // Width of visible area in game units (default 50)

    static ColumnGridConfig Default(const char* id) {
        ColumnGridConfig c{};
        c.structSize = sizeof(ColumnGridConfig);
        c.id = id;
        c.columnSpacing = 10.0f;
        c.rowSpacing = 10.0f;
        c.numRows = 1;
        c.visibleWidth = 1000.0f;
        return c;
    }
};

// Configuration for row-major grid with vertical scrolling
// Items fill horizontally first (left-to-right), then wrap to next row
struct RowGridConfig {
    uint32_t structSize;            // Must be sizeof(RowGridConfig) - for ABI versioning
    const char* id;                 // Required: unique identifier
    float columnSpacing;            // Horizontal distance between columns
    float rowSpacing;               // Vertical distance between rows
    uint32_t numColumns;            // Number of columns per row (items wrap to next row after this)
    float visibleHeight;            // Height of visible area in game units (default 50)

    static RowGridConfig Default(const char* id) {
        RowGridConfig c{};
        c.structSize = sizeof(RowGridConfig);
        c.id = id;
        c.columnSpacing = 10.0f;
        c.rowSpacing = 10.0f;
        c.numColumns = 1;
        c.visibleHeight = 50.0f;
        return c;
    }
};

struct ElementConfig {
    uint32_t structSize;            // Must be sizeof(ElementConfig) - for ABI versioning
    const char* id;                 // Required: unique identifier
    const char* modelPath;          // Path to .nif model (null for texture mode or formID mode)
    const char* texturePath;        // Path to .dds texture (null for model mode)
    const wchar_t* tooltip;         // Tooltip text (optional) - use L"text" or game strings directly
    float scale;                    // Scale multiplier
    float rotationPitch;            // Rotation offset in degrees (X axis)
    float rotationRoll;             // Rotation offset in degrees (Y axis)
    float rotationYaw;              // Rotation offset in degrees (Z axis)
    FacingMode facingMode;          // Automatic rotation behavior
    uint32_t formID;                // Optional: FormID for auto model/corrections (e.g., armor or weapon)
                                    // scale/rotation corrections are automatically applied based on bounds.
                                    // Supports TESObjectARMO, TESObjectWEAP, and everything inheriting TESBoundObject.
    bool isAnchorHandle;            // If true, this element acts can be grabbed to reposition the menu
    float hoverThreshold;           // Per-element hover threshold override (<= 0 means use root's default)
    float smoothingFactor;          // Transform smoothing speed (higher = more responsive, 15 = default)

    static ElementConfig Default(const char* id) {
        ElementConfig c{};
        c.structSize = sizeof(ElementConfig);
        c.id = id;
        c.modelPath = nullptr;
        c.texturePath = nullptr;
        c.tooltip = nullptr;
        c.scale = 1.0f;
        c.rotationPitch = 0.0f;
        c.rotationRoll = 0.0f;
        c.rotationYaw = 0.0f;
        c.facingMode = FacingMode::None;
        c.formID = 0;
        c.isAnchorHandle = false;
        c.hoverThreshold = -1.0f;   // Use root's default
        c.smoothingFactor = 15.0f;
        return c;
    }
};

struct TextConfig {
    uint32_t structSize;            // Must be sizeof(TextConfig) - for ABI versioning
    const char* id;                 // Required: unique identifier
    const wchar_t* text;            // Text to display - use L"text" or game strings directly
    float scale;                    // Text scale
    FacingMode facingMode;          // Automatic rotation behavior

    static TextConfig Default(const char* id) {
        TextConfig c{};
        c.structSize = sizeof(TextConfig);
        c.id = id;
        c.text = nullptr;
        c.scale = 1.0f;
        c.facingMode = FacingMode::None;
        return c;
    }
};

// =============================================================================
// Positionable - Base interface for all scene graph nodes
// =============================================================================

struct Positionable {
    // === Identity ===
    virtual const char* GetID() = 0;

    // === Local Transform (relative to parent) ===
    virtual void SetLocalPosition(float x, float y, float z) = 0;
    virtual void GetLocalPosition(float& x, float& y, float& z) = 0;

    // === World Transform (computed from hierarchy) ===
    virtual void GetWorldPosition(float& x, float& y, float& z) = 0;

    // === Visibility ===
    // SetVisible(false) hides/recycles the node. Use this instead of destruction.
    virtual void SetVisible(bool visible) = 0;
    virtual bool IsVisible() = 0;

    // === Hierarchy ===
    virtual Positionable* GetParent() = 0;

    // === Reserved for future expansion (preserves vtable layout) ===
    virtual void _positionable_reserved1() {}
    virtual void _positionable_reserved2() {}
    virtual void _positionable_reserved3() {}
    virtual void _positionable_reserved4() {}
    virtual void _positionable_reserved5() {}
    virtual void _positionable_reserved6() {}
    virtual void _positionable_reserved7() {}
    virtual void _positionable_reserved8() {}
    virtual void _positionable_reserved9() {}
    virtual void _positionable_reserved10() {}

protected:
    virtual ~Positionable() = default;
};

// =============================================================================
// Element - 3D model or textured icon in the scene
// =============================================================================

struct Element : Positionable {
    // === Visual Configuration ===
    virtual void SetModel(const char* nifPath) = 0;
    virtual const char* GetModel() = 0;
    virtual void SetTexture(const char* ddsPath) = 0;
    virtual const char* GetTexture() = 0;
    virtual void SetTooltip(const wchar_t* text) = 0;
    virtual const wchar_t* GetTooltip() = 0;

    // === Transform ===
    virtual void SetScale(float scale) = 0;
    virtual float GetScale() = 0;
    virtual void SetFacingMode(FacingMode mode) = 0;
    virtual FacingMode GetFacingMode() = 0;

    // === Haptic Feedback ===
    // When false, no controller vibration on interactions with this element
    virtual void SetUseHapticFeedback(bool enabled) = 0;
    virtual bool GetUseHapticFeedback() = 0;

    // === Activateable ===
    // When false, no haptic pulses AND no hover scale animation
    // Still tracks hover state and sends events (for non-interactive display elements)
    virtual void SetActivateable(bool activateable) = 0;
    virtual bool IsActivateable() = 0;

    // === Background Projectile ===
    // Optional secondary model rendered at same position (e.g., for glow effects, panels)
    // Background is non-interactive and follows primary's visibility lifecycle
    virtual void SetBackgroundModel(const char* nifPath) = 0;
    virtual void SetBackgroundScale(float scale) = 0;
    virtual void ClearBackground() = 0;

    // === Label Text ===
    // Optional text rendered below the element (e.g., item names, descriptions)
    // Label follows element's visibility lifecycle and is positioned relative to center
    virtual void SetLabelText(const wchar_t* text) = 0;
    virtual const wchar_t* GetLabelText() = 0;
    virtual void SetLabelTextScale(float scale) = 0;
    virtual float GetLabelTextScale() = 0;
    virtual void SetLabelTextVisible(bool visible) = 0;
    virtual bool IsLabelTextVisible() = 0;
    // Offset from element center in local coordinates (default: {0, 0, -10} = below)
    virtual void SetLabelOffset(float x, float y, float z) = 0;
    virtual void ClearLabelText() = 0;

    // === Reserved for future expansion ===
    virtual void _element_reserved1() {}
    virtual void _element_reserved2() {}
    virtual void _element_reserved3() {}
    virtual void _element_reserved4() {}
    virtual void _element_reserved5() {}
    virtual void _element_reserved6() {}
    virtual void _element_reserved7() {}
    virtual void _element_reserved8() {}
    virtual void _element_reserved9() {}
    virtual void _element_reserved10() {}
};

// =============================================================================
// Text - Floating text display in the scene
// =============================================================================

struct Text : Positionable {
    virtual void SetText(const wchar_t* text) = 0;
    virtual const wchar_t* GetText() = 0;
    virtual void SetScale(float scale) = 0;
    virtual void SetFacingMode(FacingMode mode) = 0;

    // === Reserved for future expansion ===
    virtual void _text_reserved1() {}
    virtual void _text_reserved2() {}
    virtual void _text_reserved3() {}
    virtual void _text_reserved4() {}
    virtual void _text_reserved5() {}
    virtual void _text_reserved6() {}
    virtual void _text_reserved7() {}
    virtual void _text_reserved8() {}
    virtual void _text_reserved9() {}
    virtual void _text_reserved10() {}
};

// =============================================================================
// Container - Holds and arranges child Positionables
// =============================================================================

struct Container : Positionable {
    // === Child Management ===
    virtual void AddChild(Positionable* child) = 0;
    virtual void SetChildren(Positionable** children, uint32_t count) = 0;
    virtual void Clear() = 0;
    virtual uint32_t GetChildCount() = 0;
    virtual Positionable* GetChildAt(uint32_t index) = 0;

    // === Haptic Feedback ===
    // When false, suppresses haptic pulses for events bubbling through this container
    virtual void SetUseHapticFeedback(bool enabled) = 0;
    virtual bool GetUseHapticFeedback() = 0;

    // === Reserved for future expansion ===
    virtual void _container_reserved1() {}
    virtual void _container_reserved2() {}
    virtual void _container_reserved3() {}
    virtual void _container_reserved4() {}
    virtual void _container_reserved5() {}
    virtual void _container_reserved6() {}
    virtual void _container_reserved7() {}
    virtual void _container_reserved8() {}
    virtual void _container_reserved9() {}
    virtual void _container_reserved10() {}
};

// =============================================================================
// ScrollableContainer - Container with scroll position control
// =============================================================================
// Used for grid containers that support scrolling:
// - ColumnGrid: Column-major fill (top-to-bottom), horizontal scrolling
// - RowGrid: Row-major fill (left-to-right), vertical scrolling
// Provides methods to query and control the scroll position.

struct ScrollableContainer : Container {
    // === Scroll Position ===
    // Get current scroll position as normalized value (0.0 = start, 1.0 = end)
    virtual float GetScrollPosition() = 0;

    // Set scroll position as normalized value (0.0 = start, 1.0 = end)
    // Values are clamped to valid range [0.0, 1.0]
    virtual void SetScrollPosition(float position) = 0;

    // === Scroll Navigation ===
    // Scroll to make a specific child index visible (centered in view if possible)
    // index: child index (0-based). Out of range values are clamped.
    virtual void ScrollToChild(uint32_t index) = 0;

    // Reset scroll to the start position (equivalent to SetScrollPosition(0.0f))
    virtual void ResetScroll() = 0;

    // === Fill Direction (for grid layouts) ===
    // Controls the order in which items are laid out.
    // - verticalFill: TopToBottom fills rows downward, BottomToTop fills upward
    // - horizontalFill: LeftToRight fills columns rightward, RightToLeft fills leftward
    virtual void SetFillDirection(VerticalFill verticalFill, HorizontalFill horizontalFill) = 0;

    // === Origin/Anchor Point (for grid layouts) ===
    // Controls where the grid's local (0,0,0) point is relative to its content.
    // This is INDEPENDENT from fill direction - you can fill top-to-bottom but
    // anchor at the center, for example.
    // - verticalOrigin: Top/Center/Bottom - which vertical edge/center is at Z=0
    // - horizontalOrigin: Left/Center/Right - which horizontal edge/center is at X=0
    virtual void SetOrigin(VerticalOrigin verticalOrigin, HorizontalOrigin horizontalOrigin) = 0;

    // === Reserved for future expansion ===
    virtual void _scrollable_reserved1() {}
    virtual void _scrollable_reserved2() {}
    virtual void _scrollable_reserved3() {}
    virtual void _scrollable_reserved4() {}
    virtual void _scrollable_reserved5() {}
    virtual void _scrollable_reserved6() {}
    virtual void _scrollable_reserved7() {}
    virtual void _scrollable_reserved8() {}
    virtual void _scrollable_reserved9() {}
    virtual void _scrollable_reserved10() {}
};

// =============================================================================
// Root - Top-level container with VR interaction support
// =============================================================================

struct Root : Container {
    // === Lookup ===
    // Find a node by ID anywhere in the hierarchy (recursive search)
    // Returns nullptr if not found. IDs should be unique within the hierarchy.
    virtual Positionable* Find(const char* id) = 0;

    // === Facing Configuration ===
    // Controls how the entire menu rotates to face the player
    virtual void SetFacingMode(FacingMode mode) = 0;

    // === VR Anchor ===
    // Set the VR device this root is anchored to and faces toward
    // HMD: menu follows head position and faces toward it
    // LeftHand/RightHand: menu follows hand (useful during positioning)
    virtual void SetVRAnchor(VRAnchorType anchor) = 0;

    // === Grab/Positioning ===
    // Start: anchors menu to hand for initial placement
    // End: fixes menu at current position relative to facing anchor
    virtual void StartPositioning(bool isLeftHand) = 0;
    virtual void EndPositioning() = 0;
    virtual bool IsGrabbing() = 0;

    // === Tooltips ===
    // Enable/disable tooltip display when hovering elements in this root (default: true)
    // Tooltips appear on the back of the VR hand when hovering interactive elements.
    // Disabling is useful for menus where tooltips would be redundant or distracting.
    virtual void SetTooltipsEnabled(bool enabled) = 0;
    virtual bool GetTooltipsEnabled() = 0;

    // === Convenience Methods ===
    // Shows the menu at the specified hand's current position.
    // Equivalent to: SetVisible(true); StartPositioning(isLeftHand); EndPositioning();
    // The menu appears at the hand and then stays fixed at that world position.
    inline void ShowAtHand(bool isLeftHand) {
        SetVisible(true);
        StartPositioning(isLeftHand);
        EndPositioning();
    }

    // === Reserved for future expansion ===
    virtual void _root_reserved1() {}
    virtual void _root_reserved2() {}
    virtual void _root_reserved3() {}
    virtual void _root_reserved4() {}
    virtual void _root_reserved5() {}
    virtual void _root_reserved6() {}
    virtual void _root_reserved7() {}
    virtual void _root_reserved8() {}
    virtual void _root_reserved9() {}
    virtual void _root_reserved10() {}
};

// =============================================================================
// Interface001 - Main entry point
// =============================================================================

// Interface version for compatibility checking
// Consumers should verify: api->GetInterfaceVersion() == P3DUI_INTERFACE_VERSION
constexpr uint32_t P3DUI_INTERFACE_VERSION = 1;

struct Interface001 {
    // === Version ===
    // Returns P3DUI_INTERFACE_VERSION (1 for this interface)
    virtual uint32_t GetInterfaceVersion() = 0;
    // Returns implementation build number (increments with each release)
    virtual uint32_t GetBuildNumber() = 0;

    // === Root Management ===
    // Creates a new root container. Returns nullptr on failure.
    // Logs error if ID is already registered.
    virtual Root* CreateRoot(const RootConfig& config) = 0;
    // Get an existing root by ID. Returns nullptr if not found.
    virtual Root* GetRoot(const char* id) = 0;

    // === Factory Methods ===
    // Create nodes. Caller must add to a container via AddChild().
    virtual Element* CreateElement(const ElementConfig& config) = 0;
    virtual Text* CreateText(const TextConfig& config) = 0;
    virtual Container* CreateScrollWheel(const ScrollWheelConfig& config) = 0;
    virtual Container* CreateWheel(const WheelConfig& config) = 0;

    // === Scrollable Grid Containers ===
    // Column-major grid: items fill top-to-bottom, then next column. Scrolls horizontally.
    virtual ScrollableContainer* CreateColumnGrid(const ColumnGridConfig& config) = 0;
    // Row-major grid: items fill left-to-right, then next row. Scrolls vertically.
    virtual ScrollableContainer* CreateRowGrid(const RowGridConfig& config) = 0;

    // === Input State Query ===
    // Returns true if a hand is currently hovering over any 3DUI element.
    // Use this from other mods to suppress your own input handling when 3DUI has focus.
    // Checks ALL visible roots.
    // leftHand: check left hand hover state
    // anyHand: if true, checks both hands (returns true if either is hovering)
    virtual bool IsHovering(bool leftHand, bool anyHand) { return false; }

    // Returns the currently hovered element for the specified hand, or nullptr if none.
    // Checks ALL visible roots.
    // isLeftHand: which hand to query
    virtual Positionable* GetHoveredItem(bool isLeftHand) { return nullptr; }

    // === Reserved for future expansion ===
    virtual void _interface_reserved1() {}
    virtual void _interface_reserved2() {}
    virtual void _interface_reserved3() {}
    virtual void _interface_reserved4() {}
    virtual void _interface_reserved5() {}
    virtual void _interface_reserved6() {}
    virtual void _interface_reserved7() {}
    virtual void _interface_reserved8() {}
    virtual void _interface_reserved9() {}
    virtual void _interface_reserved10() {}
};

// =============================================================================
// Interface Accessor
// =============================================================================

// Returns the P3DUI interface. Call after SKSE PostPostLoad message.
// First call initializes the 3D UI subsystems.
// Returns nullptr if initialization fails.
Interface001* GetInterface001();

} // namespace P3DUI
