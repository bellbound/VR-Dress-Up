# DressUpVR Pre-Release Code Review

**Date:** December 21, 2025
**Review Type:** Comprehensive Pre-Release Stability Audit
**Focus Areas:** Stability, Crash Prevention, User Experience, Mod Compatibility, Error Handling

---


---

## 1. Thread Safety and Race Conditions

### CRITICAL Issues

#### 1.1 VR Update Callback Runs on Different Thread
**Files:** `src/widget/DriverUpdateManager.cpp:53-75`
**Impact:** Data race on game objects between VR compositor and main thread

The `OnVRUpdate` callback registered with OpenVR runs on the compositor's thread, NOT the main game thread. This callback calls:
- `driver->Update()` → `projectile->Update()` → `m_gameProjectile.ApplyTransform()`

Meanwhile, the main thread via `ProjectileHook` also calls `ApplyTransform()`. This creates a **data race on the same NiAVObject and Projectile data** without synchronization.

```cpp
// VR Thread (compositor):
void DriverUpdateManager::OnVRUpdate(void* unused) {
    GetSingleton().Update(deltaTime);  // Calls ApplyTransform
}

// Main Thread (game hook):
void OnProjectileUpdate(RE::Projectile* proj, float delta) {
    controlledProj->m_gameProjectile.ApplyTransform();  // Same call!
}
```

**Fix:** Queue transform updates to main thread via SKSE task queue, or add mutex protection to `ApplyTransform()`.

---

---

#### 1.3 Weak_ptr Capture Without Mutex Protection
**Files:** `src/projectile/ProjectileSubsystem.cpp:264`

The SKSE task lambda captures `weak_ptr` without holding mutex, but the mutex is only held during registration—not during `FireProjectileFor()`:

```cpp
std::weak_ptr<ControlledProjectile> weakProj = m_projectiles[uuid];  // No mutex here!
task->AddTask([...weakProj...]{...});
```

**Fix:** Acquire mutex before accessing `m_projectiles`:
```cpp
std::weak_ptr<ControlledProjectile> weakProj;
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_projectiles.find(uuid);
    if (it == m_projectiles.end()) return false;
    weakProj = it->second;
}
```

---

### HIGH Issues
---

#### 1.5 InteractionController Global State Not Thread-Safe
**Files:** `src/widget/InteractionController.cpp:12, 46-48`

`g_registeredControllers` vector modified without mutex protection.

**Fix:** Add `std::mutex g_controllerMutex` and lock before modifications.

---

### MEDIUM Issues

- **Non-Atomic Generation Increment** - Increment before compare_exchange for ordering

---

## 2. Error Handling and Crash Prevention

### CRITICAL Issues

---

#### 2.3 Null Pointer Race in GameProjectile::PreventDestruction
**Files:** `src/projectile/GameProjectile.cpp:294`

Game can destroy projectile between null check and dereference (TOCTOU race).

**Fix:** Cache pointer locally:
```cpp
RE::Projectile* proj = m_projectile;
if (!proj) return;
auto* baseObj = proj->GetBaseObject();
```

---

#### 2.4 Weak Pointer Expiry in Async Task
**Files:** `src/projectile/ProjectileSubsystem.cpp:270-323`

`proj` is checked at line 271 but could expire before line 323.

**Fix:** Re-check before final use:
```cpp
if (!proj) {
    spdlog::warn("ControlledProjectile expired before bind");
    return;
}
```


## 4. Mod Compatibility


---

#### 4.2 Hardcoded Skyrim VR Version
**Files:** `src/projectile/ProjectileHook.cpp:7-12`

Offset `0x016F93A8` is hardcoded for Skyrim VR 1.4.15. No version checking.

**Fix:** Add runtime version validation:
```cpp
if (runtime->GetVersion() != SKSE::RUNTIME_VR_1_4_15) {
    spdlog::error("DressUpVR requires Skyrim VR 1.4.15");
    return false;
}
```

---

### HIGH Issues

- **OpenVR Hook API Not Validated** - SkyrimVRTools required but not checked

---

### MEDIUM Issues

- **VR Headset Button Mapping** - Assumes SteamVR controllers
- **Form Pool Exhaustion** - 44-projectile limit not documented

- **No CommonLibSSE-NG Version Check**

---

## 5. Initialization and Shutdown

### CRITICAL Issues



---

<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
### HIGH Issues

- **Null Subsystem Passed to Initialize** - Early exit before registering hooks
- **FormManager::AcquireForm Before Init** - Returns -1, causes OOB access

---

### MEDIUM Issues

- **VR System Unavailable During Init** - Clear hookManager on failure
- **Lazy Init Race Condition** - TooltipTextDisplayManager in multithreaded context
- **Double Event Sink Registration** - OutfitLockManager on reload
- **Re-initialization Not Supported** - Subsystems can't reinit after shutdown

---

## 6. UX Edge Cases and Robustness


#### 6.2 Zero-Scale Edge Case
**Files:** `src/projectile/ControlledProjectile.h:108-113`

Scroll can set items to scale 0.0f, and worldScale calculation can produce permanent zero.

**Fix:**
```cpp
return std::max(finalScale, 0.0001f);  // Prevent complete zero scale
```

---

#### 6.3 Rapid Menu Open/Close Race
**Files:** `src/DressupMenuManager.h:119-194`

No guard against immediate re-opening during close operation.

**Fix:** Add `m_isTransitioning` state flag.

---

### HIGH Issues

- **VR Node Null During Menu** - Check HMD/hand nodes before opening
- **Scroll Offset Invalid After Item Changes** - Validate after refresh
- **Hover Debounce Stale Tooltips** - Cancel debounce if item deleted

---

### MEDIUM Issues

- **Empty Item List** - Show "No items" message
- **Single Item Display** - Works but could be clearer
- **Grab During Scroll** - End existing scroll before starting new
- **Rapid Player Movement** - Clamp unreasonable scroll distances
- **Rotation Gimbal Lock** - Document limitation
- **Billboard Heading Drift** - Normalize to [-π, π]

---

## Recommendations

### Immediate (Before Release)

1. **Fix VR thread safety** - This is the most dangerous issue
2. **Add vtable hook conflict detection** - Log warning if other mods hook same function
3. **Add shutdown handlers** - Prevent crashes on game exit
4. **Fix division by zero** - Simple validation prevents crashes
5. **Add version checking** - Graceful failure on unsupported game versions

### Short-Term (First Patch)

1. Clean up orphaned registry entries
2. Add NaN/infinity validation to math operations
3. Fix rapid menu toggle race conditions
4. Add explicit dependency documentation

### Long-Term

1. Coordinate with SpellWheelVR on hook sharing
2. Implement save data migration
3. Add performance monitoring
4. Create comprehensive test suite

---

## Testing Checklist

- [ ] Long-running session (2+ hours) - verify no memory growth
- [ ] Rapid visibility toggle (100 toggles/second)
- [ ] VR controller disconnect/reconnect during menu
- [ ] Save/load during menu open
- [ ] Load save without ever opening menu
- [ ] Test with SpellWheelVR installed
- [ ] Test with HIGGS disabled
- [ ] Empty inventory categories
- [ ] Large inventory (100+ items)
- [ ] Game freeze/resume during menu
- [ ] Teleport while scrolling

---

## Conclusion

The DressUpVR codebase shows excellent engineering practices with proper RAII, smart pointer usage, atomic operations for thread safety, and defensive programming. However, the **VR compositor thread racing with the game thread** is a critical architectural issue that must be resolved before release.

The mod will likely work well for most users in most scenarios, but edge cases around rapid operations, long sessions, and mod conflicts could cause crashes. Addressing the Critical and High severity issues will ensure a stable release.

**Overall Assessment:** Ready for release after addressing Critical issues.

---

*Generated by Claude Code Review - December 21, 2025*
