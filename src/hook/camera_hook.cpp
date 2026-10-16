#include "camera_hook.h"
#include "logging.h"
#include "opengl_proxy.h"
#include "vr_config.h"
#include "vr_hand_renderer.h"
#include "vr_action_map.h"
#include "combat_stance.h"
#include "combat_guard.h"
#include "combat_strike.h"
#include "input_hold.h"
#include "aim_pull.h"
#include "dialogue_navigation.h"
#include "gesture_tap.h"
#include <algorithm>
#include <cmath>

namespace RiddickVR {

// GH #33: forward decls (defined below with the input helpers).
static void EnsureGameHwnd();
static bool IsGameForeground();

CameraHook& CameraHook::Instance() {
    static CameraHook s_instance;
    return s_instance;
}

// Adaptive physical crouching, two-handed stabilization, and decoupled aim state (ADR 0004, 0005, 0008)
// Track C #17 step 2: posture state (standing height, crouch/prone, lean base)
// hoisted out of CameraHook::Update into an explicit struct.
struct PostureState {
    float standingEyeHeight = 0.0f;
    bool isPhysicallyCrouched = false;
    bool isDeepProne = false;
    float torsoBaseX = 0.0f;
    float torsoBaseZ = 0.0f;
    bool hasTorsoBase = false;
    bool isStealthPeeking = false;
};
static PostureState s_posture;
// Track C #17 step 4 (GH #17): NanoMED cartridge/palm state hoisted out of
// CameraHook::Update section 2b into an explicit struct (no behavior change).
struct NanoMedState {
    bool holdingCartridge = false;
    bool palmActive = false;
    DWORD lastTouchTime = 0;
    DWORD lastDrawTime = 0;
    DWORD lastInsertTime = 0;
};
static NanoMedState s_nanomed;
static bool s_twoHandedStabilizationActive = false;
static bool s_physicalBlockEngaged = false;
static bool s_isAimingOrBlocking = false;
static RiddickVRTrackedPose s_stabilizedAimPose{};
static bool s_hasStabilizedPose = false;

static CombatStanceState s_combatStance;

// GH #62: front-of-HMD block/guard envelope, sourced from the INI (head-local
// metres; +Y up, -Z forward). Shared by the guard-pose detector and the strike
// gate so a swing thrown from a raised guard is no longer masked.
static GuardEnvelope GetGuardEnvelope() {
    GuardEnvelope env;
    env.yMin = g_vrConfig.guardYMin;
    env.yMax = g_vrConfig.guardYMax;
    env.zMin = g_vrConfig.guardZMin;
    env.zMax = g_vrConfig.guardZMax;
    env.maxHandSpan = g_vrConfig.guardMaxHandSpan;
    return env;
}

// GH #65: melee strike thresholds, sourced from the INI (see combat_strike.h).
// The classifier is shared + unit-tested (`combat_strike_thresholds`).
static StrikeThresholds GetStrikeThresholds() {
    StrikeThresholds t;
    t.forwardVz = g_vrConfig.strikeForwardVz;
    t.forwardMaxZ = g_vrConfig.strikeForwardMaxZ;
    t.hookVx = g_vrConfig.strikeHookVx;
    t.hookVz = g_vrConfig.strikeHookVz;
    t.uppercutVy = g_vrConfig.strikeUppercutVy;
    t.uppercutMaxY = g_vrConfig.strikeUppercutMaxY;
    t.uppercutVz = g_vrConfig.strikeUppercutVz;
    t.crossVx = g_vrConfig.strikeCrossVx;
    return t;
}


bool CameraHook::Initialize() {
    if (m_initialized) return true;

    m_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    VR_LOG_INFO("Initializing CameraHook. Game module base: 0x%p", (void*)m_moduleBase);
    EnsureGameHwnd(); // GH #33: cache game HWND for the foreground guard

    LoadVRConfig(); // Track C #10: riddick_vr.ini (snap/deadzone/comfort/seated/scale/southpaw/debounce)
    if (g_vrConfig.standingEyeHeight > 0.2f) {
        s_posture.standingEyeHeight = g_vrConfig.standingEyeHeight;
        VR_LOG_INFO("Standing eye height overridden from INI: %.2fm", s_posture.standingEyeHeight);
    }

    if (ScanForCameraSignature()) {
        VR_LOG_INFO("Camera structure identified in game memory!");
    } else {
        VR_LOG_INFO("Dynamic camera matrix detachment & 6DOF injection active.");
    }

    m_initialized = true;
    return true;
}

bool CameraHook::ScanForCameraSignature() {
    return false; // Dynamic matrix intercept active
}

static HWND s_gameHwnd = nullptr;

// GH #33: Foreground-focus guard. Input injection only reaches the game when
// its window is foreground; fail-open while the HWND is still unknown (tests).
static void EnsureGameHwnd() {
    if ((!s_gameHwnd || !IsWindow(s_gameHwnd)) && GetForegroundWindow()) {
        // Only latch once: the hook is injected into the game process, so the
        // foreground window at init time is the game window.
        if (!s_gameHwnd) s_gameHwnd = GetForegroundWindow();
    }
}

static bool IsGameForeground() {
    if (!s_gameHwnd || !IsWindow(s_gameHwnd)) return true;
    return GetForegroundWindow() == s_gameHwnd;
}

// GH #68: never inject input while the OpenXR simulator harness is driving.
static bool SimModeNoInput() {
    static int cached = -1;
    if (cached < 0) {
        char buf[8] = {};
        cached = (GetEnvironmentVariableA("RIDDICKVR_SIM", buf, sizeof(buf)) > 0) ? 1 : 0;
    }
    return cached == 1;
}

// GH #33: Central SendInput-based mouse helpers (migration target for the
// legacy mouse_event call sites). Relative moves; foreground-guarded.
static void SendMouseMove(int dx, int dy) {
    if (SimModeNoInput()) return; // GH #68
    if ((dx == 0 && dy == 0) || !IsGameForeground()) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

// requireForeground=false is only for releasing a held button (GH #65), so a
// focus change can never leave a button stuck down.
static void SendMouseClick(bool leftButton, bool isDown, bool requireForeground = true) {
    if (SimModeNoInput()) return; // GH #68
    if (requireForeground && !IsGameForeground()) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = leftButton
        ? (isDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP)
        : (isDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
    SendInput(1, &in, sizeof(INPUT));
}

// GH #32: central haptic dispatch queue (frame-local arbiter). Gestures
// request pulses via HapticRequest(side, amplitude, priority, tag) instead
// of writing vrState directly; Update() publishes once at the end: per
// side the highest-priority request wins, ties break to max amplitude.
// Decay applies to the published (carried) values only, at the top of
// Update. This ends hook-side last-writer-wins clobbering.
enum : int {
    kHapticAmbient = 30,   // ambient / crouch / footstep beds
    kHapticUi = 50,        // UI / menu clicks + general mechanical feedback
    kHapticLadder = 70,    // ladder / mantle / climb
    kHapticReload = 80,    // reload / NanoMED scanner
    kHapticFinisher = 100  // finisher / melee impacts
};

struct HapticArbiter {
    int prio[2];
    float amp[2];
    void Reset() { prio[0] = prio[1] = -1; amp[0] = amp[1] = 0.0f; }
    void Request(int side, float a, int p) {
        if (side < 0 || side > 1) return;
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        if (p > prio[side] || (p == prio[side] && a > amp[side])) {
            prio[side] = p;
            amp[side] = a;
        }
    }
    bool HasRequest(int side) const { return side >= 0 && side <= 1 && prio[side] >= 0; }
    float Winner(int side) const { return amp[side]; }
};

static HapticArbiter s_hapticArbiter;

// GH #32: frame-local dispatch entry point. `side`: 0 = left, 1 = right.
// `tag` is an optional call-site label for future diagnostics.
inline void HapticRequest(int side, float amplitude, int priority, const char* tag = nullptr) {
    (void)tag;
    s_hapticArbiter.Request(side, amplitude, priority);
}

// GH #32: legacy max-merge helper, superseded by the arbiter above.
// Retained so any out-of-Update writers still max-merge instead of clobbering.
inline void MergeHaptic(float& slot, float v) {
    if (v > slot) slot = v;
}

static void SendKey(WORD vk, bool isDown, bool requireForeground = true) {
    if (SimModeNoInput()) return; // GH #68: never inject desktop input under the XRSIM harness
    if (requireForeground && !IsGameForeground()) return;
    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);

    // 1. DirectInput 8 Scan Code hardware event
    INPUT inScancode = {};
    inScancode.type = INPUT_KEYBOARD;
    inScancode.ki.wVk = 0;
    inScancode.ki.wScan = static_cast<WORD>(scan);
    inScancode.ki.dwFlags = KEYEVENTF_SCANCODE | (isDown ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &inScancode, sizeof(INPUT));

    // 2. Standard Virtual Key event
    INPUT inVk = {};
    inVk.type = INPUT_KEYBOARD;
    inVk.ki.wVk = vk;
    inVk.ki.wScan = static_cast<WORD>(scan);
    inVk.ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &inVk, sizeof(INPUT));

    // 3. Win32 legacy keybd_event fallback
    keybd_event(static_cast<BYTE>(vk), static_cast<BYTE>(scan), isDown ? 0 : KEYEVENTF_KEYUP, 0);

    // 4. Direct window message posting to game window
    if (s_gameHwnd && IsWindow(s_gameHwnd)) {
        LPARAM lParam = (scan << 16) | 1;
        if (!isDown) lParam |= (1 << 30) | (1 << 31);
        PostMessageA(s_gameHwnd, isDown ? WM_KEYDOWN : WM_KEYUP, vk, lParam);
        if (isDown && (vk == VK_RETURN || vk == 'E' || vk == 'e' || vk == VK_ESCAPE || vk == VK_BACK || vk == VK_TAB || vk == VK_SPACE)) {
            PostMessageA(s_gameHwnd, WM_CHAR, vk, lParam);
        }
    }
}

static void SendKeyTap(WORD vk) {
    SendKey(vk, true);
    Sleep(15);
    SendKey(vk, false);
}

// GH #65: held clicks. A DOWN+UP pair inside one frame is often missed by the
// engine's polled input, so the DOWN goes out now and the UP is sent from the
// top of a later CameraHook::Update (FlushDueHeldInputs).
static InputHoldScheduler s_inputHold;
static bool s_loggedSimNoInput = false;

static unsigned long MeleeHoldMs() {
    return static_cast<unsigned long>(g_vrConfig.meleeClickHoldMs > 0 ? g_vrConfig.meleeClickHoldMs : 0);
}

// Tap the left (melee) or right mouse button, held for MeleeClickHoldMs.
static void SendMouseTap(bool left) {
    const unsigned long hold = MeleeHoldMs();
    if (hold == 0) { // legacy instant down+up
        SendMouseClick(left, true);
        SendMouseClick(left, false);
        return;
    }
    const HeldInput kind = left ? HeldInput::MouseLeft : HeldInput::MouseRight;
    if (s_inputHold.Press(kind, 0, GetTickCount(), hold)) {
        SendMouseClick(left, true);
    }
}

// Hold a direction/modifier key slightly longer than the click it accompanies.
static void SendKeyHeld(WORD vk) {
    const unsigned long hold = MeleeHoldMs() + 20;
    if (s_inputHold.Press(HeldInput::Key, vk, GetTickCount(), hold)) {
        SendKey(vk, true);
    }
}

static void ReleaseHeld(const HeldRelease& r) {
    switch (r.kind) {
        case HeldInput::MouseLeft: SendMouseClick(true, false, false); break;
        case HeldInput::MouseRight: SendMouseClick(false, false, false); break;
        case HeldInput::Key: SendKey(r.vk, false, false); break;
    }
}

static void FlushDueHeldInputs(unsigned long now) {
    HeldRelease due[InputHoldScheduler::kCapacity];
    const int n = s_inputHold.CollectDue(now, due, InputHoldScheduler::kCapacity);
    for (int i = 0; i < n; ++i) ReleaseHeld(due[i]);
}

static void FlushHeldInputs() {
    HeldRelease all[InputHoldScheduler::kCapacity];
    const int n = s_inputHold.CollectAll(all, InputHoldScheduler::kCapacity);
    for (int i = 0; i < n; ++i) ReleaseHeld(all[i]);
}

static void SendAction(VRAction action, bool isDown) {
    WORD key = LegacyKeyForAction(action);
    if (key) SendKey(key, isDown);
}

static void SendActionTap(VRAction action) {
    WORD key = LegacyKeyForAction(action);
    if (key) SendKeyTap(key);
}
// NOTE (merge A+B+C): SendKey/SendKeyTap stay guarded here (GH #33); gesture_tap.{h,cpp}
// provides debounced SendGestureTap which routes through its own SendKey copy -
// foreground guard is ported there post-merge (see gesture_tap.cpp).

// VR-First 1:1 Physical Interaction Anchor State (ADR 0406)
// Prevents engine action cutscene cameras (usethirdperson = 1 during NanoMED healing and ladders)
// from yanking the player out of 1:1 first person.
static bool s_vrFirstActionActive = false;
static float s_firstPersonAnchor[3] = {0.0f, 0.0f, 0.0f};
static bool s_hasFirstPersonAnchor = false;

void CameraHook::StabilizeViewMatrix(float* outView, const float* inView, const RiddickVRSharedState& vrState, int eyePass) {
    if (!inView || !outView) return;

    const float* eyeMat = (eyePass == 2 /* RightEye */) ? vrState.rightEyeView.m : vrState.leftEyeView.m;

    if (!m_decoupleRoll && !m_decouplePitch) {
        // Direct multiplication without horizon stabilization
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                outView[c * 4 + r] =
                    inView[0 * 4 + r] * eyeMat[c * 4 + 0] +
                    inView[1 * 4 + r] * eyeMat[c * 4 + 1] +
                    inView[2 * 4 + r] * eyeMat[c * 4 + 2] +
                    inView[3 * 4 + r] * eyeMat[c * 4 + 3];
            }
        }
        return;
    }

    // Static orientation state for horizon stabilization
    static uint32_t s_lastFrameIndex = 0xFFFFFFFF;
    static float s_currentYaw = 0.0f;
    static float s_lastValidYaw = 0.0f;
    static bool s_hasInitializedYaw = false;

    // Only update yaw once per stereo frame (share exact same stabilized base between left and right eyes)
    if (vrState.frameIndex != s_lastFrameIndex || !s_hasInitializedYaw || vrState.shouldRecenter) {
        s_lastFrameIndex = vrState.frameIndex;

        // Extract horizontal forward heading from inView (-Z camera column in OpenGL)
        float fwdX = -inView[2];
        float fwdZ = -inView[10];
        float lenSq = fwdX * fwdX + fwdZ * fwdZ;

        float targetYaw = s_lastValidYaw;
        if (lenSq >= 0.001f) {
            // Well-defined horizontal heading
            targetYaw = atan2f(fwdX, fwdZ);
            s_lastValidYaw = targetYaw;
        } else {
            // Gimbal singularity guard: looking nearly straight up or down (e.g. vertical ladder climbs,
            // looking directly up a ventilation shaft). Smoothly retain last known valid yaw.
            targetYaw = s_lastValidYaw;
        }

        if (!s_hasInitializedYaw || vrState.shouldRecenter) {
            s_currentYaw = targetYaw;
            s_hasInitializedYaw = true;
        } else {
            // Calculate shortest angular difference on circle [-pi, +pi]
            float deltaYaw = targetYaw - s_currentYaw;
            while (deltaYaw > 3.14159265f) deltaYaw -= 6.2831853f;
            while (deltaYaw < -3.14159265f) deltaYaw += 6.2831853f;

            // Horizon angular rate limiting / smoothing during sudden animation transitions:
            // Prevents jarring camera snaps and disorienting rotational jerks when mounting ladders,
            // crawling into vents, or executing contextual stealth takedown animations.
            constexpr float MAX_YAW_STEP = 0.20f; // ~11.5 deg per frame (~690 deg/sec at 60 FPS)
            if (deltaYaw > MAX_YAW_STEP) {
                deltaYaw = MAX_YAW_STEP;
            } else if (deltaYaw < -MAX_YAW_STEP) {
                deltaYaw = -MAX_YAW_STEP;
            }

            s_currentYaw += deltaYaw;
            while (s_currentYaw > 3.14159265f) s_currentYaw -= 6.2831853f;
            while (s_currentYaw < -3.14159265f) s_currentYaw += 6.2831853f;
        }
    }

    float cosY = cosf(s_currentYaw);
    float sinY = sinf(s_currentYaw);

    // Construct perfectly horizontal base matrix (zero roll and pitch, purely yaw-stabilized)
    float stabilizedBase[16] = {0};
    stabilizedBase[0] = cosY;
    stabilizedBase[1] = 0.0f;
    stabilizedBase[2] = -sinY;
    stabilizedBase[3] = 0.0f;

    stabilizedBase[4] = 0.0f;
    stabilizedBase[5] = 1.0f;
    stabilizedBase[6] = 0.0f;
    stabilizedBase[7] = 0.0f;

    stabilizedBase[8] = sinY;
    stabilizedBase[9] = 0.0f;
    stabilizedBase[10] = cosY;
    stabilizedBase[11] = 0.0f;

    // Retain full 3D neck/camera translation from inView, with anti-pullback stabilization (ADR 0406)
    if (s_vrFirstActionActive && s_hasFirstPersonAnchor) {
        float dx = inView[12] - s_firstPersonAnchor[0];
        float dy = inView[13] - s_firstPersonAnchor[1];
        float dz = inView[14] - s_firstPersonAnchor[2];
        float distSq = dx * dx + dy * dy + dz * dz;
        // If distance suddenly exceeds 0.65m (characteristic of 3rd person action cutscene pullback), anchor to 1st person
        if (distSq > 0.4225f) {
            stabilizedBase[12] = s_firstPersonAnchor[0];
            stabilizedBase[13] = inView[13]; // Retain vertical translation height
            stabilizedBase[14] = s_firstPersonAnchor[2];
        } else {
            stabilizedBase[12] = inView[12];
            stabilizedBase[13] = inView[13];
            stabilizedBase[14] = inView[14];
            s_firstPersonAnchor[0] = inView[12];
            s_firstPersonAnchor[1] = inView[13];
            s_firstPersonAnchor[2] = inView[14];
        }
    } else {
        stabilizedBase[12] = inView[12];
        stabilizedBase[13] = inView[13];
        stabilizedBase[14] = inView[14];
        s_firstPersonAnchor[0] = inView[12];
        s_firstPersonAnchor[1] = inView[13];
        s_firstPersonAnchor[2] = inView[14];
        s_hasFirstPersonAnchor = true;
    }
    stabilizedBase[15] = 1.0f;

    // Combine stabilized base with 6DOF eye pose
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            outView[c * 4 + r] =
                stabilizedBase[0 * 4 + r] * eyeMat[c * 4 + 0] +
                stabilizedBase[1 * 4 + r] * eyeMat[c * 4 + 1] +
                stabilizedBase[2 * 4 + r] * eyeMat[c * 4 + 2] +
                stabilizedBase[3 * 4 + r] * eyeMat[c * 4 + 3];
        }
    }
}

// Transform a world-space point into HMD local space (where +X is right, +Y is up, -Z is forward)
static RiddickVRVector3 WorldToHmdLocal(const RiddickVRVector3& worldPos, const RiddickVRTrackedPose& hmdPose) {
    float dx = worldPos.x;
    float dy = worldPos.y;
    float dz = worldPos.z;

    if (hmdPose.isValid && (fabsf(hmdPose.position.x) > 0.001f || fabsf(hmdPose.position.y) > 0.5f || fabsf(hmdPose.position.z) > 0.001f)) {
        dx -= hmdPose.position.x;
        dy -= hmdPose.position.y;
        dz -= hmdPose.position.z;

        // Inverse quaternion rotation: q_inv = (-qx, -qy, -qz, qw)
        float qx = -hmdPose.orientation.x;
        float qy = -hmdPose.orientation.y;
        float qz = -hmdPose.orientation.z;
        float qw = hmdPose.orientation.w;

        float norm = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
        if (norm > 0.0001f) {
            qx /= norm;
            qy /= norm;
            qz /= norm;
            qw /= norm;

            float tx = 2.0f * (qy * dz - qz * dy);
            float ty = 2.0f * (qz * dx - qx * dz);
            float tz = 2.0f * (qx * dy - qy * dx);

            float rx = dx + qw * tx + (qy * tz - qz * ty);
            float ry = dy + qw * ty + (qz * tx - qx * tz);
            float rz = dz + qw * tz + (qx * ty - qy * tx);

            // Track C #13: world scale multiplier on interaction volumes.
            float ws = GetWorldScale();
            return { rx * ws, ry * ws, rz * ws };
        }
    }

    float wsFallback = GetWorldScale(); // Track C #13
    return { dx * wsFallback, dy * wsFallback, dz * wsFallback };
}

// Adaptive physical crouching, two-handed stabilization, and decoupled aim state (ADR 0004, 0005, 0008)
// (state variables live at file top, next to includes).
// GH #35: single eyeshine source of truth, toggled by all emitters (Button Y,
// temple tap, EVA visor pull) and published once per frame at the end of Update.
static bool s_eyeshineActive = false;
// ---------------------------------------------------------------------------
// Track C #17, FIRST STEP ONLY: begin decomposing CameraHook::Update.
// Update currently owns ~478 function statics across sections 0-10. Full
// split plan (follow-ups, one section per change, statics -> explicit state):
//   0 Head-look/mouse emulation -> UpdateHeadLook (head_look.cpp)
//   1 Crouch/height/lean        -> UpdatePosture (posture.cpp)
//   2 Ladders/mantle            -> UpdateClimbing (climbing.cpp)
//   2b NanoMED cartridge/palm   -> UpdateNanoMED (nanomed.cpp)
//   3 Locomotion deadzone       -> UpdateLocomotion (locomotion.cpp)
//   4 Snap/smooth turn (+4b)    -> UpdateLocomotionTurn (locomotion.cpp)
//   5 Melee gestures            -> UpdateMelee (melee.cpp, per-weapon tables)
//   6 Reload/holster/grenade    -> UpdateReloadGestures (reload.cpp)
//   7 Eyeshine/flashlight       -> UpdateTacticalGear (tactical.cpp)
//   8 Aim/block/fire            -> UpdateAimAndFire (aim.cpp)
//   9 Grips/throw/drag          -> UpdateGrips (grips.cpp)
//   10 Buttons/menus            -> UpdateButtons (buttons.cpp)
// First step (this change, same file, no behavior change): three static
// helpers with explicit state structs for locomotion intent, reload
// detection, and NanoMED volumes. Call sites below delegate the pure
// detection math; side effects (keys/haptics/logs) stay in Update.
// ---------------------------------------------------------------------------
struct LocomotionIntent { bool fwd = false; bool back = false; bool left = false; bool right = false; };

static LocomotionIntent ComputeLocomotionIntent(float stickLX, float stickLY, float deadzone) {
    LocomotionIntent out;
    float moveMag = sqrtf(stickLX * stickLX + stickLY * stickLY);
    if (moveMag >= deadzone) {
        float dirX = stickLX / moveMag;
        float dirY = stickLY / moveMag;
        // Angular deadband around forward/backward axis (same thresholds as inline code).
        constexpr float FORWARD_THRESHOLD = 0.35f;
        constexpr float STRAFE_THRESHOLD = 0.42f;
        if (dirY > FORWARD_THRESHOLD) out.fwd = true;
        else if (dirY < -FORWARD_THRESHOLD) out.back = true;
        if (dirX > STRAFE_THRESHOLD) out.right = true;
        else if (dirX < -STRAFE_THRESHOLD) out.left = true;
    }
    return out;
}

struct ReloadGestureRequest { bool cellEject = false; bool magInsert = false; bool waistPouch = false; };

static ReloadGestureRequest DetectReloadGesture(float handDist, float offGrip, float offVelY, float offLocalY, float offLocalX) {
    ReloadGestureRequest out;
    out.cellEject = (handDist < 0.18f && offGrip > 0.50f && offVelY < -0.70f);
    out.magInsert = (handDist < 0.18f && offGrip > 0.50f && offVelY > 0.35f);
    out.waistPouch = (offLocalY < -0.38f && offLocalX > -0.35f && offLocalX < -0.05f && offGrip > 0.50f);
    return out;
}

struct NanoMedVolumes { bool inReceiver = false; bool leftContact = false; bool rightContact = false; };

static NanoMedVolumes DetectNanoMEDContact(const RiddickVRVector3& domLocal,
                                           const RiddickVRVector3& leftLocal, const RiddickVRVector3& rightLocal,
                                           bool leftPalmFlat, bool rightPalmFlat) {
    NanoMedVolumes out;
    out.inReceiver = (domLocal.z >= -0.62f && domLocal.z <= -0.20f) &&
                     (domLocal.y >= 0.05f && domLocal.y <= 0.42f) &&
                     (fabsf(domLocal.x) < 0.30f);
    out.leftContact = leftPalmFlat && (leftLocal.z >= -0.65f && leftLocal.z <= -0.22f) &&
                      (leftLocal.y >= -0.25f && leftLocal.y <= 0.25f) && (fabsf(leftLocal.x) < 0.28f);
    out.rightContact = rightPalmFlat && (rightLocal.z >= -0.65f && rightLocal.z <= -0.22f) &&
                       (rightLocal.y >= -0.25f && rightLocal.y <= 0.25f) && (fabsf(rightLocal.x) < 0.28f);
    return out;
}

// Track C #17 step 4 (GH #17): section 2b NanoMED cartridge/palm detection.
// Pure detection: updates st, returns edge intents; Update owns
// SendGestureTap/HapticRequest/logs. Same volumes (via DetectNanoMEDContact),
// cooldowns, and haptic cadence as the inline block it replaces.
struct NanoMedIntent {
    bool cartridgeDrawn = false;
    bool cartridgeReturned = false;
    bool cartridgeInserted = false;
    bool palmEngaged = false; // edge: debounced 'E' tap + log
    bool palmActive = false;  // level: continuous haptic cadence
    bool leftContact = false;
    bool rightContact = false;
    float pulseAmp = 0.0f;
};

static NanoMedIntent UpdateNanoMED(NanoMedState& st, DWORD now,
                                   const RiddickVRVector3& domHandLocal, float domHandVelY, float domGrip,
                                   const RiddickVRVector3& leftLocal, const RiddickVRVector3& rightLocal,
                                   bool leftPalmFlat, bool rightPalmFlat, bool physicalBlockEngaged) {
    NanoMedIntent out;
    NanoMedVolumes vol = DetectNanoMEDContact(domHandLocal, leftLocal, rightLocal, leftPalmFlat, rightPalmFlat);
    out.leftContact = vol.leftContact;
    out.rightContact = vol.rightContact;

    // A. Hip holster cartridge draw / return.
    if (!st.holdingCartridge) {
        if (domHandLocal.y < -0.38f && domGrip > 0.60f && (now - st.lastDrawTime > 1200)) {
            st.holdingCartridge = true;
            st.lastDrawTime = now;
            out.cartridgeDrawn = true;
        }
    } else {
        // Return cartridge to holster if grip released away from station.
        if (domGrip < 0.30f && (domHandLocal.z > -0.20f || domHandLocal.y < 0.0f)) {
            st.holdingCartridge = false;
            out.cartridgeReturned = true;
        }
    }

    // B. Downward cartridge insertion into overhead station receiver slot.
    if (st.holdingCartridge) {
        if (vol.inReceiver && domHandVelY < -0.22f && (now - st.lastInsertTime > 1500)) {
            st.holdingCartridge = false;
            st.lastInsertTime = now;
            out.cartridgeInserted = true;
        }
    }

    // C. Biometric scanner palm contact.
    bool wantPalmActive = (vol.leftContact || vol.rightContact) && !st.holdingCartridge && !physicalBlockEngaged;
    if (wantPalmActive) {
        if (!st.palmActive && (now - st.lastTouchTime > 1200)) {
            st.lastTouchTime = now;
            out.palmEngaged = true;
        }
        st.palmActive = true;
        out.palmActive = true;
        // Continuous restorative nanite injection haptic cadence (120 Hz pulse oscillation).
        out.pulseAmp = 0.35f + 0.15f * sinf(static_cast<float>(now) * 0.015f);
    } else {
        st.palmActive = false;
        out.palmActive = false;
    }
    return out;
}

// Track C #17 step 2: posture/crouch/height/recenter + lean/peek detection.
// Pure detection: updates st, returns edge intents; Update owns logs/keys/haptics.
struct PostureIntent {
    bool recenterCalibrated = false;
    bool baselineSnappedUp = false; // GH #49: fast-up recovery (was latched low)
    bool initialCalibrated = false;
    bool crouchChanged = false;
    bool crouchEngaged = false;
    float crouchY = 0.0f;
    float crouchStandingY = 0.0f;
    float crouchDrop = 0.0f;
    bool proneChanged = false;
    bool proneEngaged = false;
    float proneY = 0.0f;
    float proneDrop = 0.0f;
    bool peekChanged = false;
    bool peekEngaged = false;
    float leanRight = 0.0f;
    float leanForward = 0.0f;
};

static PostureIntent UpdatePosture(PostureState& st, bool hmdValid,
                                   float currentY, float hmdVelY,
                                   float hmdX, float hmdZ,
                                   bool recenterEdge, bool seated, float seatedEyeOffset,
                                   float lastHmdYaw) {
    PostureIntent out;
    // Reset calibration on recenter edge (GH #37) + seated offset (Track C #11).
    if (recenterEdge && hmdValid) {
        st.standingEyeHeight = currentY + (seated ? seatedEyeOffset : 0.0f); // Track C #11 + GH #37 edge
        out.recenterCalibrated = true;
    }

    if (hmdValid) {
        // Initial calibration (GH #49): latch only a plausible STATIONARY
        // standing height. Latching a reach-up spike or a crouch as the
        // baseline is what stuck players crouched permanently.
        if (st.standingEyeHeight <= 0.001f && currentY > 0.1f) {
            bool stationary = fabsf(hmdVelY) < 0.25f;
            bool plausible = (currentY >= 1.20f && currentY <= 2.05f) || seated;
            if (stationary && plausible) {
                st.standingEyeHeight = currentY + (seated ? seatedEyeOffset : 0.0f); // Track C #11
                out.initialCalibrated = true;
            }
        }

        // GH #49: fast-up recovery. If the user is plainly TALLER than the
        // baseline while stationary, the baseline was latched low - snap up
        // instead of slow-creeping (peak tracking below only moves 0.2%/frame).
        if (st.standingEyeHeight > 0.2f && !st.isPhysicallyCrouched &&
            (currentY - st.standingEyeHeight) > 0.05f && fabsf(hmdVelY) < 0.25f) {
            st.standingEyeHeight = currentY;
            out.baselineSnappedUp = true;
        }

        // ADR 0377: Peak tracking with Jump Immunity & Vertical Velocity Gating
        // Only adapt standing height when stationary and upright (|Vy| < 0.12 m/s).
        // Adapt gradually via exponential smoothing so jumping or head bob never latches a false floor!
        if (!st.isPhysicallyCrouched && currentY > st.standingEyeHeight && fabsf(hmdVelY) < 0.12f) {
            st.standingEyeHeight += 0.002f * (currentY - st.standingEyeHeight);
        }

        // Calculate height drop from calibrated standing height
        float heightDrop = st.standingEyeHeight - currentY;
        float heightRatio = (st.standingEyeHeight > 0.2f) ? (currentY / st.standingEyeHeight) : 1.0f;

        // Crouch activation criteria:
        // Ducks > 28cm below standing height OR falls below 78% of standing height
        bool wantCrouch = false;
        if (st.standingEyeHeight > 0.2f) {
            if (st.isPhysicallyCrouched) {
                // Hysteresis release buffer: remain crouched until rising within 18cm of standing height
                wantCrouch = (heightDrop > 0.18f);
            } else {
                wantCrouch = (heightDrop > 0.28f) || (heightRatio < 0.78f);
            }
        } else {
            // Fallback for zero-baseline tracking spaces
            constexpr float FALLBACK_CROUCH_THRESHOLD = -0.28f;
            constexpr float FALLBACK_RELEASE_THRESHOLD = -0.18f;
            wantCrouch = st.isPhysicallyCrouched ? (currentY < FALLBACK_RELEASE_THRESHOLD) : (currentY < FALLBACK_CROUCH_THRESHOLD);
        }
        if (seated) wantCrouch = false; // Track C #11: no physical auto-crouch while seated

        if (wantCrouch != st.isPhysicallyCrouched) {
            st.isPhysicallyCrouched = wantCrouch;
            out.crouchChanged = true;
            out.crouchEngaged = wantCrouch;
            out.crouchY = currentY;
            out.crouchStandingY = st.standingEyeHeight;
            out.crouchDrop = heightDrop;
        }

        // Deep Prone / Laser Tripwire Limbo Evasion (ADR 0030)
        bool wantProne = false;
        if (st.standingEyeHeight > 0.2f) {
            wantProne = (heightDrop > 0.48f) || (heightRatio < 0.65f);
        } else {
            wantProne = (currentY < -0.45f);
        }
        if (seated) wantProne = false; // Track C #11: seated players never auto-prone

        if (wantProne != st.isDeepProne) {
            st.isDeepProne = wantProne;
            out.proneChanged = true;
            out.proneEngaged = wantProne;
            out.proneY = currentY;
            out.proneDrop = heightDrop;
        }
    }

    if (hmdValid) {
        if (!st.hasTorsoBase || recenterEdge) {
            st.torsoBaseX = hmdX;
            st.torsoBaseZ = hmdZ;
            st.hasTorsoBase = true;
            st.isStealthPeeking = false;
        } else {
            // Horizontal displacement from torso base
            float dispX = hmdX - st.torsoBaseX;
            float dispZ = hmdZ - st.torsoBaseZ;
            float horizDist = sqrtf(dispX * dispX + dispZ * dispZ);

            // Decompose into lateral (right/left) and longitudinal (forward/back) lean using stabilized heading
            float cosY = cosf(lastHmdYaw);
            float sinY = sinf(lastHmdYaw);
            // Right vector is (cosY, -sinY), Forward vector is (-sinY, -cosY)
            float leanRight = dispX * cosY - dispZ * sinY;
            float leanForward = -dispX * sinY - dispZ * cosY;

            // Lateral corner peek detection threshold: 15cm lateral head shift
            bool wantPeek = (fabsf(leanRight) > 0.15f);
            if (wantPeek != st.isStealthPeeking) {
                st.isStealthPeeking = wantPeek;
                out.peekChanged = true;
                out.peekEngaged = wantPeek;
                out.leanRight = leanRight;
                out.leanForward = leanForward;
            }

            // Exceeding 38cm roomscale lean: softly slide torso origin along with player
            constexpr float MAX_LEAN_RADIUS = 0.38f;
            if (horizDist > MAX_LEAN_RADIUS) {
                float excess = horizDist - MAX_LEAN_RADIUS;
                float dirX = dispX / horizDist;
                float dirZ = dispZ / horizDist;
                st.torsoBaseX += dirX * excess;
                st.torsoBaseZ += dirZ * excess;
            }
        }
    }
    return out;
}

// GH #17 step 3: turn handling extracted (snap/smooth + mode toggle).
// No behavior change: same thresholds, hysteresis, INI wiring, guards.
enum VRTurnMode {
    VR_TURN_SNAP = 0,    // Discrete 45° snap turns with zero pre-snap crawl
    VR_TURN_SMOOTH = 1   // Continuous rotation with cubic acceleration curve
};

struct TurnState {
    VRTurnMode mode = VR_TURN_SNAP;
    bool snapArmed = true;
    bool prevToggle = false;
    float smoothAccum = 0.0f;
};

static TurnState s_turn;

static void UpdateTurn(TurnState& st, uint32_t btns, RiddickVRSharedState& vrState) {
    // Mode Toggle: Left Grip + Right Stick Click toggles turn mode
    bool stickRightClick = (btns & RIDDICK_VR_BTN_STICK_RIGHT) != 0;
    bool leftGripHeld = vrState.leftController.grip > 0.5f;
    if (stickRightClick && leftGripHeld && !st.prevToggle) {
        st.mode = (st.mode == VR_TURN_SNAP) ? VR_TURN_SMOOTH : VR_TURN_SNAP;
        VR_LOG_INFO("Turning Mode Toggled: %s", (st.mode == VR_TURN_SNAP) ? "SNAP TURN (45 deg)" : "SMOOTH TURN (Cubic Curve)");
        HapticRequest(1, ((st.mode == VR_TURN_SNAP) ? 0.40f : 0.25f), kHapticUi);
    }
    st.prevToggle = (stickRightClick && leftGripHeld);

    float stickX = vrState.rightController.thumbstickX;

    if (st.mode == VR_TURN_SNAP) {
        // Pure Snap Turn: require deflection > 0.70f, trigger clean single 45° mouse delta
        // CRITICAL: Absolutely no smooth mouse move during ramp-up or return to center!
        constexpr float SNAP_THRESHOLD = 0.70f;
        constexpr float RESET_THRESHOLD = 0.30f;

        if (fabsf(stickX) > SNAP_THRESHOLD) {
            if (st.snapArmed) {
                st.snapArmed = false;
                int snapDelta = static_cast<int>(350.0f * (g_vrConfig.snapAngleDeg / 45.0f)) * ((stickX > 0.0f) ? 1 : -1); // Track C #10 INI angle
                SendMouseMove(snapDelta, 0); // GH #33: SendInput helper
                HapticRequest(1, 0.25f, kHapticUi); // GH #32: crisp tactile click on snap
            }
        } else if (fabsf(stickX) < RESET_THRESHOLD) {
            st.snapArmed = true;
        }
    } else {
        // Pure Smooth Turn: continuous rotation with progressive cubic curve and sub-pixel accumulation
        // Zero truncation stutter or sudden snaps at stick limits
        const float SMOOTH_DEADZONE = g_vrConfig.turnDeadzone; // Track C #10 (default 0.20)
        if (fabsf(stickX) > SMOOTH_DEADZONE) {
            float norm = (fabsf(stickX) - SMOOTH_DEADZONE) / (1.0f - SMOOTH_DEADZONE);
            float curvedSpeed = norm * norm * g_vrConfig.smoothTurnSpeed; // Track C #10 INI (default 20.0)
            // GH #23: guard corrupt stick input before accumulation.
            if (curvedSpeed != curvedSpeed || fabsf(curvedSpeed) > 1e6f) {
                curvedSpeed = 0.0f;
                st.smoothAccum = 0.0f;
            }
            st.smoothAccum += std::copysign(curvedSpeed, stickX);
            if (st.smoothAccum > 500.0f) st.smoothAccum = 500.0f;
            if (st.smoothAccum < -500.0f) st.smoothAccum = -500.0f;
            int deltaX = static_cast<int>(st.smoothAccum);
            if (deltaX != 0) {
                st.smoothAccum -= deltaX;
                SendMouseMove(deltaX, 0); // GH #33: SendInput helper
            }
        } else {
            st.smoothAccum = 0.0f;
        }
    }
}

// GH #17 step 5: grips/throw/body-drag detection extracted (section 9:
// interactions, physical throwing, body dragging, climbing/reload overlap).
// Pure detection: updates st, returns edge intents; Update owns
// SendKey/SendKeyTap/mouse_event/HapticRequest/logs. Same thresholds,
// velocities, and cooldowns as the inline block it replaces.
struct GripsState {
    bool leftGripDown = false;
    bool rightGripDown = false;
    DWORD lastDiceRattleTime = 0;
    DWORD lastCardDealTime = 0;
    DWORD lastAshThrowTime = 0;
    DWORD lastFanScatterTime = 0;
    DWORD lastFanClenchBurstTime = 0;
    DWORD fanClenchStartTime = 0;
    bool handsWereClenchedChest = false;
    DWORD lastEyeRecoveryTime = 0;
    DWORD lastDragPulseTime = 0;
    bool inspectingItem = false;
    bool isThroatGripping = false;
    DWORD lastShakedownPulseTime = 0;
    bool isChokeholding = false;
    DWORD lastChokeStruggleTime = 0;
    DWORD lastBunkSearchTime = 0;
    DWORD lastBunkConcealTime = 0;
};
static GripsState s_grips;

struct GripsIntent {
    bool leftGrab = false;   // press edge: SendKey('E', true) + latch haptic
    bool leftThrow = false;  // release edge with forward flick: click + log + haptic
    bool leftDrop = false;   // release edge without flick: SendKey('E', false)
    float throwSpeed = 0.0f;
    RiddickVRVector3 throwVel{};
    bool diceRattle = false;
    bool cardDeal = false;
    float cardVx = 0.0f;
    float cardVz = 0.0f;
    bool ashThrow = false;
    float ashVy = 0.0f;
    float ashVz = 0.0f;
    bool fanScatter = false;
    float fanLx = 0.0f;
    float fanRx = 0.0f;
    bool fanClenchEngaged = false; // clench-start edge: friction haptic
    bool fanBurst = false;
    float burstLx = 0.0f;
    float burstRx = 0.0f;
    bool eyeRecovery = false;
    bool eyeHeadShake = false; // true: head shake reason; false: forearm wipe
    bool dragPulse = false;
    bool inspectChanged = false;
    bool inspectEngaged = false;
    float inspectY = 0.0f;
    bool throatEngaged = false;
    float throatX = 0.0f;
    float throatY = 0.0f;
    float throatZ = 0.0f;
    bool shakedown = false;
    float shakeVelZ = 0.0f;
    bool throatReleased = false;
    bool chokeEngaged = false;
    bool chokePulse = false; // periodic struggle tick (300 ms cadence)
    bool chokeFinisher = false;
    float chokeVx = 0.0f;
    bool chokeReleased = false;
    bool bunkSearch = false;
    float bunkSweepVx = 0.0f;
    bool bunkConceal = false;
    float concealVz = 0.0f;
    bool rightChanged = false;
    bool rightDown = false; // new level for SendKey('R', down)
};

static GripsIntent UpdateGrips(GripsState& st, DWORD now,
                               float leftGrip, float rightGrip, float rightTrigger,
                               const RiddickVRVector3& leftLocal, const RiddickVRVector3& rightLocal,
                               const RiddickVRVector3& lVel, const RiddickVRVector3& rVel,
                               float lSpeed, float stickLX, float stickLY,
                               bool isClimbingActive, bool twoHandedStabilizationActive,
                               bool physicalBlockEngaged, bool meleeFiredThisFrame,
                               bool crouched, bool deepProne, bool peeking,
                               float lastHmdDeltaYaw) {
    GripsIntent out;

    // Left Grip -> Grab / Secondary Interact ('E') / Physical Object Throwing / Body Dragging
    bool wantLeftGrip = !isClimbingActive && !twoHandedStabilizationActive && (leftGrip > 0.5f);
    if (wantLeftGrip != st.leftGripDown) {
        if (!wantLeftGrip && st.leftGripDown) {
            // Evaluated at moment of release (ADR 0382: Forward Trajectory Vector Gating)
            if (lVel.z < -0.65f && lSpeed > 1.10f && !meleeFiredThisFrame) { // GH #31: melee has priority
                out.leftThrow = true;
                out.throwSpeed = lSpeed;
                out.throwVel = lVel;
            } else {
                out.leftDrop = true;
            }
        } else {
            out.leftGrab = true;
        }
        st.leftGripDown = wantLeftGrip;
    }

    // Inmate Gambling Dice Shaking & Roll Throwing (ADR 0045)
    bool cuppedLeftHand = (leftGrip > 0.40f && leftGrip < 0.85f);
    if (cuppedLeftHand && lSpeed > 0.70f && !physicalBlockEngaged && (now - st.lastDiceRattleTime > 120)) {
        st.lastDiceRattleTime = now;
        out.diceRattle = true;
    }

    // Interactive Inmate Gambling Card Flipping & Dealing (ADR 0075)
    bool cardHandProximity = (rightLocal.y > -0.45f && rightLocal.y < -0.10f) &&
                             (rightLocal.z > -0.40f && rightLocal.z < -0.15f);
    bool cardPinchGrip = (rightGrip > 0.40f && rightGrip < 0.75f) && (rightTrigger < 0.30f);
    bool cardFlick = (fabsf(rVel.x) > 0.60f || rVel.z < -0.65f);
    if (cardHandProximity && cardPinchGrip && cardFlick && !physicalBlockEngaged && (now - st.lastCardDealTime > 300)) {
        st.lastCardDealTime = now;
        out.cardDeal = true;
        out.cardVx = rVel.x;
        out.cardVz = rVel.z;
    }

    // Inmate Pit Brawl Dirt / Ash Throw Blind (ADR 0065)
    bool ashThrowFlick = (crouched || deepProne) &&
                         (rightGrip < 0.40f && rightTrigger < 0.40f) &&
                         (rVel.y > 0.80f && rVel.z < -0.80f);
    if (ashThrowFlick && !meleeFiredThisFrame && (now - st.lastAshThrowTime > 1200)) { // GH #31
        st.lastAshThrowTime = now;
        out.ashThrow = true;
        out.ashVy = rVel.y;
        out.ashVz = rVel.z;
    }

    // Inmate Pit Brawl Sand / Ash Throw Two-Handed Fan Scatter (ADR 0085)
    bool bothHandsLowSand = (crouched || deepProne) &&
                            (leftLocal.y < -0.40f && rightLocal.y < -0.40f) &&
                            (leftGrip < 0.45f && rightGrip < 0.45f);
    bool fanScatterOutward = (lVel.x < -0.80f && rVel.x > 0.80f) && (lVel.y > 0.40f || rVel.y > 0.40f);
    if (bothHandsLowSand && fanScatterOutward && !meleeFiredThisFrame && (now - st.lastFanScatterTime > 1400)) { // GH #31
        st.lastFanScatterTime = now;
        out.fanScatter = true;
        out.fanLx = lVel.x;
        out.fanRx = rVel.x;
    }

    // Inmate Pit Brawl Dirt / Ash Two-Handed Fan Clench & Burst (ADR 0104)
    float dxHandsFan = leftLocal.x - rightLocal.x;
    float dyHandsFan = leftLocal.y - rightLocal.y;
    float dzHandsFan = leftLocal.z - rightLocal.z;
    float distHandsFan = sqrtf(dxHandsFan * dxHandsFan + dyHandsFan * dyHandsFan + dzHandsFan * dzHandsFan);

    bool handsAtChest = (leftLocal.y > -0.25f && leftLocal.y < 0.05f) &&
                        (rightLocal.y > -0.25f && rightLocal.y < 0.05f) &&
                        (distHandsFan < 0.18f);
    bool bothFanGripsClenched = (leftGrip > 0.60f && rightGrip > 0.60f);

    if (handsAtChest && bothFanGripsClenched && !physicalBlockEngaged) {
        if (!st.handsWereClenchedChest) {
            st.handsWereClenchedChest = true;
            st.fanClenchStartTime = now;
            out.fanClenchEngaged = true;
        }
    } else if (st.handsWereClenchedChest) {
        // Evaluate burst if fists were clenched together recently
        if (now - st.fanClenchStartTime < 800) {
            bool explosiveFanOutward = (lVel.x < -1.10f && rVel.x > 1.10f);
            if (explosiveFanOutward && !meleeFiredThisFrame && (now - st.lastFanClenchBurstTime > 2000)) { // GH #31
                st.lastFanClenchBurstTime = now;
                out.fanBurst = true;
                out.burstLx = lVel.x;
                out.burstRx = rVel.x;
            }
        }
        if (!bothFanGripsClenched || distHandsFan > 0.35f) {
            st.handsWereClenchedChest = false;
        }
    }

    // Inmate Pit Brawl Dirt / Ash Eye Recovery Blink Gesture (ADR 0073)
    bool rightWipingEyes = (rightLocal.z > -0.22f && rightLocal.z < -0.05f) &&
                           (rightLocal.y > -0.05f && rightLocal.y < 0.10f) &&
                           (fabsf(rVel.x) > 0.50f);
    bool leftWipingEyes = (leftLocal.z > -0.22f && leftLocal.z < -0.05f) &&
                          (leftLocal.y > -0.05f && leftLocal.y < 0.10f) &&
                          (fabsf(lVel.x) > 0.50f);
    bool rapidHeadShake = (fabsf(lastHmdDeltaYaw) > 0.15f);

    if ((rightWipingEyes || leftWipingEyes || rapidHeadShake) && (now - st.lastEyeRecoveryTime > 1200)) {
        st.lastEyeRecoveryTime = now;
        out.eyeRecovery = true;
        out.eyeHeadShake = rapidHeadShake;
    }

    // Kinematic Body / Heavy Object Drag Resistance (ADR 0020)
    if (st.leftGripDown && (crouched || peeking)) {
        float moveMagnitude = fabsf(stickLX) + fabsf(stickLY);
        if (moveMagnitude > 0.25f && (now - st.lastDragPulseTime > 450)) {
            st.lastDragPulseTime = now;
            out.dragPulse = true;
        }
    }

    // Diegetic Collectible Inspection: Cigarette Packs & Bounty Cards (ADR 0029)
    bool wantInspect = st.leftGripDown && (leftLocal.y > -0.05f && leftLocal.z > -0.32f);
    if (wantInspect != st.inspectingItem) {
        st.inspectingItem = wantInspect;
        out.inspectChanged = true;
        out.inspectEngaged = wantInspect;
        out.inspectY = leftLocal.y;
    }

    // Interactive Guard Interrogation: Physical Throat-Grip Shakedown (ADR 0032)
    bool inThroatZone = st.leftGripDown && (leftLocal.z > -0.60f && leftLocal.z < -0.22f) &&
                                        (leftLocal.y > -0.25f && leftLocal.y < 0.15f) &&
                                        (fabsf(leftLocal.x) < 0.28f);
    if (inThroatZone) {
        if (!st.isThroatGripping) {
            st.isThroatGripping = true;
            out.throatEngaged = true;
            out.throatX = leftLocal.x;
            out.throatY = leftLocal.y;
            out.throatZ = leftLocal.z;
        }
        // Physical Forward/Back Shake: |lVel.z| > 0.70 m/s
        if (fabsf(lVel.z) > 0.70f && (now - st.lastShakedownPulseTime > 350)) {
            st.lastShakedownPulseTime = now;
            out.shakedown = true;
            out.shakeVelZ = lVel.z;
        }
    } else {
        if (st.isThroatGripping) {
            st.isThroatGripping = false;
            out.throatReleased = true;
        }
    }

    // Guard Subdue Chokehold & Silent Neck Snap (ADR 0047)
    bool inChokeholdZone = (crouched || peeking || deepProne) &&
                           st.leftGripDown && (leftLocal.z > -0.35f && leftLocal.z < -0.15f) &&
                           (leftLocal.y > -0.20f && leftLocal.y < 0.10f) &&
                           (fabsf(leftLocal.x) < 0.25f);
    if (inChokeholdZone) {
        if (!st.isChokeholding) {
            st.isChokeholding = true;
            out.chokeEngaged = true;
        }
        if (now - st.lastChokeStruggleTime > 300) {
            st.lastChokeStruggleTime = now;
            out.chokePulse = true;
        }
        // Cervical Twist Finisher: rapid lateral jerk of right hand (|rVel.x| > 0.80 m/s)
        if ((fabsf(rVel.x) > 0.80f || fabsf(rVel.y) > 0.80f) && !meleeFiredThisFrame) { // GH #31: melee has priority
            st.isChokeholding = false;
            out.chokeFinisher = true;
            out.chokeVx = rVel.x;
        }
    } else {
        if (st.isChokeholding) {
            st.isChokeholding = false;
            out.chokeReleased = true;
        }
    }

    // Inmate Bunk Mattress Stash Search (ADR 0050)
    bool leftHoldingMattress = (crouched || deepProne) &&
                               st.leftGripDown && (leftLocal.y > -0.55f && leftLocal.y < -0.25f);
    bool rightSweepingUnder = (rightLocal.z < -0.30f) && (fabsf(rVel.x) > 0.35f);
    if (leftHoldingMattress && rightSweepingUnder && (now - st.lastBunkSearchTime > 900)) {
        st.lastBunkSearchTime = now;
        out.bunkSearch = true;
        out.bunkSweepVx = rVel.x;
    }

    // Inmate Bunk Shiv Sheath Concealment Under Mattress Frame (ADR 0110)
    bool leftLiftingMattress = (crouched || deepProne) &&
                               st.leftGripDown && (leftLocal.y > -0.45f && leftLocal.y < -0.15f);
    bool rightThrustingIntoSlat = (rightLocal.z > -0.42f && rightLocal.z < -0.14f) && (rVel.z < -0.65f);
    if (leftLiftingMattress && rightThrustingIntoSlat && (now - st.lastBunkConcealTime > 1500)) {
        st.lastBunkConcealTime = now;
        out.bunkConceal = true;
        out.concealVz = rVel.z;
    }

    // Right Grip -> Weapon Reload ('R') (suppressed during ladder climb)
    bool wantRightGrip = !isClimbingActive && (rightGrip > 0.5f);
    if (wantRightGrip != st.rightGripDown) {
        st.rightGripDown = wantRightGrip;
        out.rightChanged = true;
        out.rightDown = wantRightGrip;
    }

    return out;
}

// GH #17 step 6: buttons/menus detection extracted (section 10: face buttons,
// dual-stick ejection, right-stick recenter gate, menu edge).
// Pure detection: no SendKey/SendMouseClick/HapticRequest/logs/vrState writes;
// Update owns all side effects plus the s_prevButtons assignment and the
// haptic arbiter publish block (both untouched). Same edges/gates as the
// inline block this replaces.
struct ButtonsState {
};
static ButtonsState s_buttons;

struct ButtonsIntent {
    bool aChanged = false;
    bool aDown = false;
    bool bChanged = false;
    bool bDown = false;
    bool xPressed = false;
    bool menuEdge = false;
    bool dualStickPressed = false;
    bool dualGrip = false;
    bool recenterPressed = false;
};

static ButtonsIntent UpdateButtons(ButtonsState& st, uint32_t btns, uint32_t prevBtns,
                                   float leftGrip, float rightGrip) {
    (void)st;
    ButtonsIntent out;
    bool btnA = (btns & RIDDICK_VR_BTN_A) != 0;
    bool prevBtnA = (prevBtns & RIDDICK_VR_BTN_A) != 0;
    out.aChanged = (btnA != prevBtnA);
    out.aDown = btnA;
    bool btnB = (btns & RIDDICK_VR_BTN_B) != 0;
    bool prevBtnB = (prevBtns & RIDDICK_VR_BTN_B) != 0;
    out.bChanged = (btnB != prevBtnB);
    out.bDown = btnB;
    bool btnX = (btns & RIDDICK_VR_BTN_X) != 0;
    bool prevBtnX = (prevBtns & RIDDICK_VR_BTN_X) != 0;
    out.xPressed = (btnX && !prevBtnX);
    // GH #20: single edge-triggered Menu handler (computed once here); a Menu
    // edge in the same frame suppresses the dual-stick ESC so VK_ESCAPE fires once.
    out.menuEdge = ((btns & RIDDICK_VR_BTN_MENU) != 0) && ((prevBtns & RIDDICK_VR_BTN_MENU) == 0);
    bool stickLeft = (btns & RIDDICK_VR_BTN_STICK_LEFT) != 0;
    bool stickRight = (btns & RIDDICK_VR_BTN_STICK_RIGHT) != 0;
    bool prevStickLeft = (prevBtns & RIDDICK_VR_BTN_STICK_LEFT) != 0;
    bool prevStickRight = (prevBtns & RIDDICK_VR_BTN_STICK_RIGHT) != 0;
    bool dualStick = stickLeft && stickRight;
    bool prevDualStick = prevStickLeft && prevStickRight;
    out.dualStickPressed = dualStick && !prevDualStick && !out.menuEdge;
    out.dualGrip = (leftGrip > 0.55f && rightGrip > 0.55f);
    // Right Thumbstick Click recenter gate: grip held means the click toggles
    // snap/smooth turn mode instead of resetting the view.
    bool recenterGripHeld = leftGrip > 0.5f;
    out.recenterPressed = stickRight && !prevStickRight && !recenterGripHeld;
    return out;
}

// GH #17 step 7: head-look/mouse-emulation math extracted (section 0:
// Singularity-Free Relative Quaternion Head Tracking + decoupled controller
// aim steering + recoil kick + ADS corridor).
// Pure math: updates st, returns mouse deltas + ADS edge intent; Update owns
// SendMouseMove/HapticRequest/logs. Same quaternion math, deadzones,
// sensitivities, GH #23 guard/clamps, and recoil decay as the inline block.
struct HeadLookState {
    bool hasPrevHmdOrientation = false;
    float prevHmdX = 0.0f;
    float prevHmdY = 0.0f;
    float prevHmdZ = 0.0f;
    float prevHmdW = 1.0f;
    bool hasPrevCtrlAim = false;
    float prevCtrlYaw = 0.0f;
    float prevCtrlPitch = 0.0f;
    float accumulatedLookX = 0.0f;
    float accumulatedLookY = 0.0f;
    float lastHmdYaw = 0.0f;
    float lastHmdPitch = 0.0f;
    float lastHmdDeltaYaw = 0.0f;
    float recoilKickPitch = 0.0f;
    DWORD adsEnterStartTime = 0;
    bool adsHapticFired = false;
    bool isPhysicalADS = false;
};
static HeadLookState s_headlook;

struct HeadLookIntent {
    int moveX = 0;
    int moveY = 0;
    bool adsAcquired = false; // edge: tactile detent haptic on sight picture acquisition
};

// ADR 0376 weapon slot state (file scope: head-look section 0 reads it,
// holster section 6c writes it). 1: Primary, 2: Sidearm, 3: Melee/Blades,
// 4: Ordnance, 5: Heavy.
static int s_activeWeaponSlot = 1;

static HeadLookIntent UpdateHeadLook(HeadLookState& st, DWORD now,
                                     const RiddickVRTrackedPose& hmdPose,
                                     const RiddickVRTrackedPose& ctrlPose,
                                     bool recenterEdge, bool isReloading, bool aimHeld,
                                     bool physicalBlockEngaged, bool twoHandedActive,
                                     bool southpaw, bool meleeEquipped, bool guardPoseLike) {
    HeadLookIntent out;
    if (!hmdPose.isValid) {
        st.hasPrevHmdOrientation = false;
        st.hasPrevCtrlAim = false;
        return out;
    }

    float qx = hmdPose.orientation.x;
    float qy = hmdPose.orientation.y;
    float qz = hmdPose.orientation.z;
    float qw = hmdPose.orientation.w;

    float norm = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
    if (norm <= 0.0001f) return out;
    qx /= norm;
    qy /= norm;
    qz /= norm;
    qw /= norm;

    if (!st.hasPrevHmdOrientation || recenterEdge) {
        st.prevHmdX = qx;
        st.prevHmdY = qy;
        st.prevHmdZ = qz;
        st.prevHmdW = qw;
        st.hasPrevCtrlAim = false;
        st.accumulatedLookX = 0.0f;
        st.accumulatedLookY = 0.0f;
        st.hasPrevHmdOrientation = true;
        return out;
    }

    // ADR 0409: Compute relative rotation in head-local space: Delta Q = Q_prev^-1 * Q_curr
    // Eliminates all gimbal lock singularities, division-by-zero, and runaway pitch divergence
    float w1 = st.prevHmdW, x1 = st.prevHmdX, y1 = st.prevHmdY, z1 = st.prevHmdZ;
    float w2 = qw, x2 = qx, y2 = qy, z2 = qz;

    float dw = w1 * w2 + x1 * x2 + y1 * y2 + z1 * z2;
    float dx = w1 * x2 - x1 * w2 - y1 * z2 + z1 * y2;
    float dy = w1 * y2 + x1 * z2 - y1 * w2 - z1 * x2;
    float dz = w1 * z2 - x1 * y2 + y1 * x2 - z1 * w2;

    if (dw < 0.0f) {
        dx = -dx;
        dy = -dy;
        dz = -dz;
        dw = -dw;
    }

    // Small-angle approximation: theta ~= 2 * sin(theta/2)
    float deltaYaw = 2.0f * dy;   // Pure local neck yaw around Y
    float deltaPitch = 2.0f * dx; // Pure local head pitch around X
    // deltaRoll = 2.0f * dz; // Pure local roll around Z (completely decoupled)

    st.prevHmdX = qx;
    st.prevHmdY = qy;
    st.prevHmdZ = qz;
    st.prevHmdW = qw;
    st.lastHmdDeltaYaw = deltaYaw;

    // Decoupled Motion Controller Aim Steering:
    // Extract relative orientation between HMD and weapon aim controller: Q_rel = Q_hmd^-1 * Q_ctrl
    float deltaCtrlYaw = 0.0f;
    float deltaCtrlPitch = 0.0f;
    float pullYaw = 0.0f;
    float pullPitch = 0.0f;
    float wPull = 0.0f;
    bool isActivelyAiming = false;

    // ADR 0383: Physical 6DOF Aim-Down-Sights (ADS) Eye-Relief Alignment & Parallax Mitigation
    RiddickVRVector3 ctrlLocal = WorldToHmdLocal(ctrlPose.position, hmdPose);
    if (southpaw) ctrlLocal.x = -ctrlLocal.x; // Track C #14: mirror into right-eye corridor frame
    // Dominant eye sight line corridor:
    // Y: [-0.18m, 0.08m] (chin to eye level)
    // Z: [-0.36m, -0.10m] (10cm to 36cm eye relief)
    // X: [0.02m, 0.22m] (dominant right eye alignment corridor)
    bool inAdsCorridor = (ctrlLocal.y >= -0.18f && ctrlLocal.y <= 0.08f) &&
                         (ctrlLocal.z >= -0.36f && ctrlLocal.z <= -0.10f) &&
                         (ctrlLocal.x >= 0.02f && ctrlLocal.x <= 0.22f);

    // GH #50/#51: the fist guard sits inside this corridor; never latch ADS from it.
    if (inAdsCorridor && !isReloading && !physicalBlockEngaged && !guardPoseLike) {
        if (st.adsEnterStartTime == 0) {
            st.adsEnterStartTime = now;
        } else if (now - st.adsEnterStartTime > 50) { // 50ms hold to confirm ADS
            st.isPhysicalADS = true;
            if (!st.adsHapticFired) {
                out.adsAcquired = true; // call site: tactile detent on sight picture acquisition
                st.adsHapticFired = true;
            }
        }
    } else {
        // Spatial exit hysteresis: requires moving clearly outside the corridor
        if (ctrlLocal.y < -0.24f || ctrlLocal.z < -0.42f || ctrlLocal.x < -0.05f || ctrlLocal.x > 0.28f) {
            st.isPhysicalADS = false;
            st.adsEnterStartTime = 0;
            st.adsHapticFired = false;
        }
    }

    if (ctrlPose.isValid && !physicalBlockEngaged && !isReloading) {
        float cx = ctrlPose.orientation.x;
        float cy = ctrlPose.orientation.y;
        float cz = ctrlPose.orientation.z;
        float cw = ctrlPose.orientation.w;
        float cNorm = sqrtf(cx * cx + cy * cy + cz * cz + cw * cw);
        if (cNorm > 0.0001f) {
            cx /= cNorm; cy /= cNorm; cz /= cNorm; cw /= cNorm;

            // Inverse HMD quaternion: q_inv = (-qx, -qy, -qz, qw)
            float invHmdX = -qx;
            float invHmdY = -qy;
            float invHmdZ = -qz;
            float invHmdW = qw;

            // Q_rel = Q_hmd^-1 * Q_ctrl
            float relW = invHmdW * cw - invHmdX * cx - invHmdY * cy - invHmdZ * cz;
            float relX = invHmdW * cx + invHmdX * cw + invHmdY * cz - invHmdZ * cy;
            float relY = invHmdW * cy - invHmdX * cz + invHmdY * cw + invHmdZ * cx;
            float relZ = invHmdW * cz + invHmdX * cy - invHmdY * cx + invHmdZ * cw;

            float relNorm = sqrtf(relW * relW + relX * relX + relY * relY + relZ * relZ);
            if (relNorm > 0.0001f) {
                relW /= relNorm; relX /= relNorm; relY /= relNorm; relZ /= relNorm;

                // Relative forward vector (rotating (0, 0, -1) by Q_rel):
                // fwdX = -2(relW * relY + relX * relZ)
                // fwdY =  2(relW * relX - relY * relZ)
                // fwdZ = -(1 - 2(relX^2 + relY^2))
                float fwdX = -2.0f * (relW * relY + relX * relZ);
                float fwdY =  2.0f * (relW * relX - relY * relZ);
                float fwdZ = -(1.0f - 2.0f * (relX * relX + relY * relY));

                // Relative controller aim angles in HMD view space
                float ctrlYaw = atan2f(2.0f * (relW * relY + relZ * relX), 1.0f - 2.0f * (relX * relX + relY * relY));
                float sinCtrlPitch = fwdY;
                float ctrlPitch = 0.0f;
                if (fabsf(sinCtrlPitch) >= 1.0f) {
                    ctrlPitch = copysignf(1.5707963f, sinCtrlPitch);
                } else {
                    ctrlPitch = asinf(sinCtrlPitch);
                }

                // ADR 0374: Quest Controller Ergonomic Barrel Pitch Calibration
                // Natural wrist resting angle offsets the raw aim ray ~10.5 degrees downward
                constexpr float ERGONOMIC_BARREL_PITCH_OFFSET = -0.1832f; // -10.5 deg in radians
                ctrlPitch += ERGONOMIC_BARREL_PITCH_OFFSET;

                // Apply transient muzzle climb recoil kick (exponential recovery over ~120ms)
                ctrlPitch += st.recoilKickPitch;
                st.recoilKickPitch *= 0.82f;

                if (!st.hasPrevCtrlAim) {
                    st.prevCtrlYaw = ctrlYaw;
                    st.prevCtrlPitch = ctrlPitch;
                    st.hasPrevCtrlAim = true;
                } else {
                    deltaCtrlYaw = ctrlYaw - st.prevCtrlYaw;
                    while (deltaCtrlYaw > 3.14159265f) deltaCtrlYaw -= 6.2831853f;
                    while (deltaCtrlYaw < -3.14159265f) deltaCtrlYaw += 6.2831853f;

                    deltaCtrlPitch = ctrlPitch - st.prevCtrlPitch;

                    st.prevCtrlYaw = ctrlYaw;
                    st.prevCtrlPitch = ctrlPitch;

                    // GH #50/#51: DELTA-based aim steering (aim_pull.h). A steady arm
                    // injects nothing; melee and the boxer-guard pose never steer.
                    AimPullInputs pullIn;
                    pullIn.deltaYaw = deltaCtrlYaw;
                    pullIn.deltaPitch = deltaCtrlPitch;
                    pullIn.isPhysicalADS = st.isPhysicalADS;
                    pullIn.twoHanded = twoHandedActive;
                    pullIn.aimHeld = aimHeld;
                    pullIn.physicalBlock = physicalBlockEngaged;
                    pullIn.reloading = isReloading;
                    pullIn.meleeEquipped = meleeEquipped;
                    pullIn.guardPoseLike = guardPoseLike;
                    const AimPullResult pull = ComputeAimPull(pullIn);
                    pullYaw = pull.pullYaw;
                    pullPitch = pull.pullPitch;
                    isActivelyAiming = pull.active;
                    wPull = pull.weight;
                }
            }
        }
    } else {
        st.hasPrevCtrlAim = false;
    }

    // ADR 0409: Singularity-Free Relative Quaternion Head Tracking Look & Controller Aim Deflection
    // 1:1 Head tracking orientation in head-local space drives in-game heading and pitch smoothly
    // without Euler gimbal lock or runaway spinning.
    constexpr float HMD_LOOK_DEADZONE = 0.0004f; // ~0.02 deg deadband filter
    float cleanDeltaYaw = (fabsf(deltaYaw) > HMD_LOOK_DEADZONE) ? deltaYaw : 0.0f;
    float cleanDeltaPitch = (fabsf(deltaPitch) > HMD_LOOK_DEADZONE) ? deltaPitch : 0.0f;

    constexpr float SENSITIVITY_YAW = 445.6f;   // fallback; live value from INI below
    constexpr float SENSITIVITY_PITCH = 445.6f; // fallback; live value from INI below

    float mouseDeltaX = -cleanDeltaYaw * g_vrConfig.lookSensitivityYaw;   // GH #45: INI-calibrated gain
    float mouseDeltaY = -cleanDeltaPitch * g_vrConfig.lookSensitivityPitch; // (default preserves legacy feel)
    (void)SENSITIVITY_YAW; (void)SENSITIVITY_PITCH;

    if (isActivelyAiming && fabsf(wPull) > 0.001f) {
        constexpr float CONTROLLER_AIM_SENSITIVITY = 120.0f;
        mouseDeltaX += -pullYaw * wPull * CONTROLLER_AIM_SENSITIVITY;
        mouseDeltaY += -pullPitch * wPull * CONTROLLER_AIM_SENSITIVITY;
    }

    // GH #23: NaN/Inf guard + clamp (ADR 0409 look deltas). A corrupt
    // quaternion/velocity must never inject runaway mouse deltas.
    bool badLook = (mouseDeltaX != mouseDeltaX) || (mouseDeltaY != mouseDeltaY) ||
                   (fabsf(mouseDeltaX) > 1e6f) || (fabsf(mouseDeltaY) > 1e6f);
    if (badLook) {
        mouseDeltaX = 0.0f;
        mouseDeltaY = 0.0f;
        st.accumulatedLookX = 0.0f;
        st.accumulatedLookY = 0.0f;
    } else {
        if (mouseDeltaX > 500.0f) mouseDeltaX = 500.0f;
        if (mouseDeltaX < -500.0f) mouseDeltaX = -500.0f;
        if (mouseDeltaY > 500.0f) mouseDeltaY = 500.0f;
        if (mouseDeltaY < -500.0f) mouseDeltaY = -500.0f;
    }

    st.accumulatedLookX += mouseDeltaX;
    st.accumulatedLookY += mouseDeltaY;
    if (st.accumulatedLookX > 500.0f) st.accumulatedLookX = 500.0f;
    if (st.accumulatedLookX < -500.0f) st.accumulatedLookX = -500.0f;
    if (st.accumulatedLookY > 500.0f) st.accumulatedLookY = 500.0f;
    if (st.accumulatedLookY < -500.0f) st.accumulatedLookY = -500.0f;

    out.moveX = static_cast<int>(st.accumulatedLookX);
    out.moveY = static_cast<int>(st.accumulatedLookY);
    if (out.moveX != 0 || out.moveY != 0) {
        st.accumulatedLookX -= out.moveX;
        st.accumulatedLookY -= out.moveY;
    }
    return out;
}

// GH #17 step 8: tactical weapon reload gestures extracted (section 6:
// off-hand receiver/waist/cell-eject via DetectReloadGesture, plus 6a
// shiv-sharpening hone, 6a-2 whittle, 6a-3 spork, 6a-4 flare, 6a-5 syringe).
// Pure detection: updates st, returns edge intents; Update owns
// SendGestureTap/SendKeyTap/SendMouseClick/HapticRequest/logs. Same volumes,
// velocities, cooldowns, and stroke counts as the inline block it replaces.
struct ReloadState {
    DWORD lastReloadGestureTime = 0;
    DWORD lastHoneStrokeTime = 0;
    DWORD lastWhittleStrokeTime = 0;
    int whittleStrokeCount = 0;
    DWORD lastSporkStrokeTime = 0;
    int sporkStrokeCount = 0;
    DWORD lastFlareIgniteTime = 0;
    DWORD lastSyringeInjectTime = 0;
};
static ReloadState s_reload;

struct ReloadIntent {
    bool cellEject = false;
    float cellEjectVelY = 0.0f;
    float cellEjectDist = 0.0f;
    bool manualReload = false;
    bool manualIsMagInsert = false;
    float manualDist = 0.0f;
    float manualOffY = 0.0f;
    bool honeStroke = false;
    float honeRelVelY = 0.0f;
    float honeDist = 0.0f;
    bool whittleStroke = false;
    int whittleCount = 0;
    float whittleSpeedX = 0.0f;
    bool whittleComplete = false;
    bool sporkStroke = false;
    int sporkCount = 0;
    float sporkSpeedX = 0.0f;
    bool sporkComplete = false;
    bool flareIgnited = false;
    float flareShearY = 0.0f;
    float flareShearX = 0.0f;
    bool syringeInjected = false;
    float syringeX = 0.0f;
    float syringeY = 0.0f;
    float syringeZ = 0.0f;
};

static ReloadIntent UpdateReload(ReloadState& st, DWORD now,
                                 const RiddickVRVector3& leftLocal, const RiddickVRVector3& rightLocal,
                                 const RiddickVRVector3& lVel, const RiddickVRVector3& rVel,
                                 float leftGrip, float rightGrip, float rightTrigger,
                                 bool crouched, bool deepProne,
                                 bool physicalBlockEngaged, bool isClimbingActive,
                                 bool twoHandedActive, bool meleeFired, bool southpaw) {
    ReloadIntent out;
    float reloadHandDist = sqrtf(
        powf(leftLocal.x - rightLocal.x, 2) +
        powf(leftLocal.y - rightLocal.y, 2) +
        powf(leftLocal.z - rightLocal.z, 2)
    );

    // 6. Tactical Weapon Reload Gestures (off-hand receiver/waist/cell-eject).
    // ADR 0394: suppressed while two-handed stabilization holds; off-hand
    // resolves via southpaw (Track C #14).
    const RiddickVRVector3& offHandLocal = southpaw ? rightLocal : leftLocal;
    float offHandVelY = southpaw ? rVel.y : lVel.y;
    float offGrip = southpaw ? rightGrip : leftGrip;
    ReloadGestureRequest reloadReq = DetectReloadGesture(reloadHandDist, offGrip, offHandVelY,
                                                         offHandLocal.y, offHandLocal.x);
    if (!physicalBlockEngaged && !isClimbingActive && !twoHandedActive && (now - st.lastReloadGestureTime > 1200)) {
        if (reloadReq.cellEject) {
            st.lastReloadGestureTime = now;
            out.cellEject = true;
            out.cellEjectVelY = offHandVelY;
            out.cellEjectDist = reloadHandDist;
        } else if (reloadReq.magInsert || reloadReq.waistPouch) {
            st.lastReloadGestureTime = now;
            out.manualReload = true;
            out.manualIsMagInsert = reloadReq.magInsert;
            out.manualDist = reloadHandDist;
            out.manualOffY = offHandLocal.y;
        }
    }

    // 6a. Physical Shiv Sharpening & Blade Grinding (ADR 0035).
    bool inHoneProximity = (reloadHandDist < 0.15f) && (leftGrip > 0.40f) && (rightGrip > 0.40f);
    if (inHoneProximity && !physicalBlockEngaged && !isClimbingActive) {
        float relVelY = fabsf(lVel.y - rVel.y);
        if (relVelY > 0.55f && (now - st.lastHoneStrokeTime > 250)) {
            st.lastHoneStrokeTime = now;
            out.honeStroke = true;
            out.honeRelVelY = relVelY;
            out.honeDist = reloadHandDist;
        }
    }

    // 6a-2. Inmate Toothbrush Shank Whittling & Rapid Friction Sharpening (ADR 0068).
    bool crouchedForWhittle = (crouched || deepProne);
    bool grippingShank = (rightGrip > 0.50f);
    bool dominantNearFloor = (rightLocal.y < -0.45f && rightLocal.z > -0.45f && rightLocal.z < -0.15f);
    bool fastFrictionStroke = (fabsf(rVel.x) > 0.65f);
    if (crouchedForWhittle && dominantNearFloor && grippingShank && fastFrictionStroke && !physicalBlockEngaged) {
        if (now - st.lastWhittleStrokeTime > 180) {
            st.lastWhittleStrokeTime = now;
            st.whittleStrokeCount++;
            out.whittleStroke = true;
            out.whittleCount = st.whittleStrokeCount;
            out.whittleSpeedX = rVel.x;
            if (st.whittleStrokeCount >= 5) {
                st.whittleStrokeCount = 0;
                out.whittleComplete = true;
            }
        }
    }

    // 6a-3. Inmate Mess Hall Contraband Spork Whittling & Tine Sharpening (ADR 0080).
    bool sporkTableHeight = (rightLocal.y > -0.55f && rightLocal.y < -0.25f) &&
                            (rightLocal.z > -0.42f && rightLocal.z < -0.16f);
    bool fastSporkStroke = (fabsf(rVel.x) > 0.60f);
    if (crouchedForWhittle && sporkTableHeight && grippingShank && fastSporkStroke && !physicalBlockEngaged) {
        if (now - st.lastSporkStrokeTime > 160) {
            st.lastSporkStrokeTime = now;
            st.sporkStrokeCount++;
            out.sporkStroke = true;
            out.sporkCount = st.sporkStrokeCount;
            out.sporkSpeedX = rVel.x;
            if (st.sporkStrokeCount >= 4) {
                st.sporkStrokeCount = 0;
                out.sporkComplete = true;
            }
        }
    }

    // 6a-4. Security Camera Thermal Decoy Flare Igniting Gesture (ADR 0089).
    bool flareInOffHand = (leftLocal.z > -0.35f && leftLocal.z < -0.15f) && (leftGrip > 0.55f);
    bool strikerAtCap = (reloadHandDist < 0.16f) && (rightGrip > 0.55f);
    float shearVelY = fabsf(lVel.y - rVel.y);
    float shearVelX = fabsf(lVel.x - rVel.x);
    if (flareInOffHand && strikerAtCap && (shearVelY > 0.80f || shearVelX > 0.80f) && !physicalBlockEngaged && !meleeFired && (now - st.lastFlareIgniteTime > 1800)) {
        st.lastFlareIgniteTime = now;
        out.flareIgnited = true;
        out.flareShearY = shearVelY;
        out.flareShearX = shearVelX;
    }

    // 6a-5. Medical Syringe Sedative Self-Injection & Bio-Adrenaline Rush (ADR 0087).
    bool dominantAtOffArm = (rightLocal.x > -0.24f && rightLocal.x < -0.06f) &&
                            (rightLocal.y > -0.18f && rightLocal.y < 0.12f) &&
                            (rightLocal.z > -0.25f && rightLocal.z < -0.05f);
    bool syringePlungerPressed = (rightGrip > 0.50f) && (rightTrigger > 0.65f);
    if (dominantAtOffArm && syringePlungerPressed && !physicalBlockEngaged && (now - st.lastSyringeInjectTime > 1500)) {
        st.lastSyringeInjectTime = now;
        out.syringeInjected = true;
        out.syringeX = rightLocal.x;
        out.syringeY = rightLocal.y;
        out.syringeZ = rightLocal.z;
    }
    return out;
}

// ---------------------------------------------------------------------------
// GH #17 step 9: section 5 melee combat gesture state + detection.
// All ~191 Update-local statics (punch/finisher cooldowns, combo/charge windows,
// directional keys, guard hysteresis) hoisted here. Detection lives in
// UpdateMeleeCounters / UpdateMeleeStrikes below; ALL side effects
// (SendGestureTap/SendKeyTap/SendMouseClick/HapticRequest/VR_LOG_INFO) stay at
// the call site in CameraHook::Update with identical strings. No behavior change:
// same velocities, volumes, cooldowns, and dispatch priority order.
// ---------------------------------------------------------------------------
struct MeleeState {
    DWORD lastPunchTime = 0;
    DWORD lastFinisherTime = 0;
    DWORD guardPoseHoldTimer = 0;
    DWORD guardPoseReleaseTimer = 0;
    bool currentBlockEngaged = false;
    DWORD lastPocketAshRiposteTime = 0;
    DWORD lastArcWaveTime = 0;
    DWORD arcWaveChargeStart = 0;
    DWORD lastRibUnderhookTime = 0;
    DWORD dartExtractedTime = 0;
    DWORD lastDartCounterJabTime = 0;
    DWORD lastDoubleEarClapTime = 0;
    DWORD lastBatonRibJabTime = 0;
    DWORD lastParrySnapTime = 0;
    DWORD eyeRubWipeTime = 0;
    DWORD lastEyeRubRiposteTime = 0;
    DWORD lastKneeStompTime = 0;
    DWORD lastInductionOverheatTime = 0;
    DWORD inductionOverheatStart = 0;
    DWORD liverHookTime = 0;
    DWORD lastLiverElbowComboTime = 0;
    DWORD lastSpearThrustTime = 0;
    DWORD lastHeadbuttTime = 0;
    DWORD lastBatonGroundPinTime = 0;
    DWORD lastLeadKneeTime = 0;
    DWORD lastArcWallPulse = 0;
    DWORD lastKidneyCounterTime = 0;
    DWORD lastBatonArcTrapTime = 0;
    DWORD batonArcTrapStart = 0;
    DWORD lastPalmChinComboTime = 0;
    DWORD palmChinStrikeTime = 0;
    DWORD lastBatonGroundShockTime = 0;
    DWORD lastSnapdownChokeTime = 0;
    DWORD snapdownStartTime = 0;
    DWORD lastSkullBreakerTime = 0;
    DWORD lastBatonDisarmTime = 0;
    DWORD lastShinTrapTime = 0;
    DWORD shinTrapTime = 0;
    DWORD lastSuplexSlamTime = 0;
    DWORD suplexHoistTime = 0;
    DWORD lastCrossArcBurstTime = 0;
    DWORD crossArcStartTime = 0;
    DWORD lastBatonJavelinTime = 0;
    DWORD lastOutsideTripTime = 0;
    DWORD lastKimuraTime = 0;
    DWORD lastArmTriangleTime = 0;
    DWORD armTriangleStartTime = 0;
    DWORD lastDiagonalParryTime = 0;
    DWORD lastRadialSweepTime = 0;
    DWORD lastSingleLegTakedownTime = 0;
    DWORD lastBatonJabTripTime = 0;
    DWORD batonJabTime = 0;
    DWORD lastDoubleLegSlamTime = 0;
    DWORD doubleLegLiftTime = 0;
    DWORD lastPistolDisarmTime = 0;
    DWORD pistolTrapTime = 0;
    DWORD lastFiremansCarryTime = 0;
    DWORD firemansLoadTime = 0;
    DWORD lastFrontHeadlockTime = 0;
    DWORD lastCrossParryTime = 0;
    DWORD crossParryDeflectTime = 0;
    char crossParryDirKey = 'A';
    DWORD lastSuloevTime = 0;
    DWORD lastVaultDropTime = 0;
    DWORD lastBackSuplexTime = 0;
    DWORD backSuplexClinchTime = 0;
    DWORD lastRadialDisarmTime = 0;
    DWORD radialDisarmTwistTime = 0;
    char radialDisarmDirKey = 'A';
    DWORD lastThaiPlumTime = 0;
    DWORD thaiPlumClinchTime = 0;
    DWORD lastJavelinVaultTime = 0;
    DWORD lastWhizzerTime = 0;
    char whizzerDirKey = 'A';
    DWORD lastHammerfistTime = 0;
    DWORD lastHipTossTime = 0;
    char hipTossDirKey = 'A';
    DWORD lastBatonBashTime = 0;
    DWORD lastThaiPlumElbowTime = 0;
    char thaiPlumElbowDirKey = 'A';
    DWORD lastGroundingThrustTime = 0;
    DWORD lastLateralSuplexTime = 0;
    char lateralSuplexDirKey = 'A';
    DWORD lastKineticCleaveTime = 0;
    DWORD lastWhizzerCounterTime = 0;
    char whizzerCounterDirKey = 'A';
    DWORD lastCleaveArcBlastTime = 0;
    DWORD lastCervicalSpikeTime = 0;
    char cervicalSpikeDirKey = 'A';
    DWORD lastVaultGroundArcTime = 0;
    DWORD lastThoracicSlamTime = 0;
    char thoracicSlamDirKey = 'A';
    DWORD lastVaultKineticCleaveTime = 0;
    DWORD lastHyperextensionSpikeTime = 0;
    char hyperextensionSpikeDirKey = 'A';
    DWORD lastDeckImpactTime = 0;
    DWORD lastWhizzerGroundFinisherTime = 0;
    char whizzerGroundFinisherDirKey = 'A';
    DWORD lastOmnidirectionalArcTime = 0;
    DWORD lastWhizzerGroundDislocationTime = 0;
    char whizzerGroundDislocationDirKey = 'A';
    DWORD lastOmnidirectionalPulseTime = 0;
    DWORD lastSternumKneeDropTime = 0;
    char sternumKneeDropDirKey = 'A';
    DWORD lastKineticPulseWaveTime = 0;
    DWORD lastSternumFractureSpikeTime = 0;
    char sternumFractureSpikeDirKey = 'A';
    DWORD lastPlasmaCleaveWaveTime = 0;
    DWORD lastSternumGratingSlamTime = 0;
    char sternumGratingSlamDirKey = 'A';
    DWORD lastPlasmaOverloadWaveTime = 0;
    DWORD lastCervicalGratingSlamTime = 0;
    char cervicalGratingSlamDirKey = 'A';
    DWORD lastPlasmaResonantWaveTime = 0;
    DWORD lastHyperextensionFinisherTime = 0;
    char hyperextensionFinisherDirKey = 'A';
    DWORD lastPlasmaHarmonicQuakeTime = 0;
    DWORD lastThoracicDislocationTime = 0;
    char thoracicDislocationDirKey = 'A';
    DWORD lastPlasmaResonantSeismTime = 0;
    DWORD lastThoracicMatSlamTime = 0;
    char thoracicMatSlamDirKey = 'A';
    DWORD lastPlasmaSeismOverdriveTime = 0;
    DWORD lastThoracicPinLockTime = 0;
    char thoracicPinLockDirKey = 'A';
    DWORD lastPlasmaCataclysmTime = 0;
    DWORD lastSternumElbowDropTime = 0;
    char sternumElbowDropDirKey = 'A';
    DWORD lastPlasmaCataclysmOverdriveTime = 0;
    DWORD lastCranialDeckDriverTime = 0;
    char cranialDeckDriverDirKey = 'A';
    DWORD lastPlasmaSingularityTime = 0;
    DWORD lastSternumKneeSpikeTime = 0;
    char sternumKneeSpikeDirKey = 'A';
    DWORD lastPlasmaSupernovaTime = 0;
    DWORD lastCervicalDeckPinTime = 0;
    char cervicalDeckPinDirKey = 'A';
    DWORD lastPlasmaHypernovaTime = 0;
    DWORD lastCranialGrindTime = 0;
    char cranialGrindDirKey = 'A';
    DWORD lastPlasmaSingularityOverdriveTime = 0;
    DWORD lastCranialPostImpactTime = 0;
    char cranialPostImpactDirKey = 'A';
    DWORD lastPlasmaGenesisTime = 0;
    DWORD lastCranialStompTime = 0;
    char cranialStompDirKey = 'A';
    DWORD lastPlasmaBigBangTime = 0;
    DWORD lastCranialDoubleStrikeTime = 0;
    char cranialDoubleStrikeDirKey = 'A';
    DWORD lastPlasmaCosmicDawnTime = 0;
    DWORD lastCranialPivotTwistTime = 0;
    char cranialPivotTwistDirKey = 'A';
    DWORD lastPlasmaSuperclusterTime = 0;
    DWORD lastCranialPivotSlamTime = 0;
    char cranialPivotSlamDirKey = 'A';
    DWORD lastPlasmaOmniverseTime = 0;
    DWORD lastCranialLeverPinTime = 0;
    char cranialLeverPinDirKey = 'A';
    DWORD lastPlasmaMetaverseTime = 0;
    DWORD lastCranialFulcrumChokeTime = 0;
    char cranialFulcrumChokeDirKey = 'A';
    DWORD lastPlasmaHyperdimensionalTime = 0;
    DWORD lastCranialFulcrumArmbarTime = 0;
    char cranialFulcrumArmbarDirKey = 'A';
    DWORD lastPlasmaTransdimensionalTime = 0;
    DWORD lastCranialFulcrumKneeTime = 0;
    char cranialFulcrumKneeDirKey = 'A';
    DWORD lastPlasmaInterdimensionalTime = 0;
    DWORD lastCranialFulcrumRibSlamTime = 0;
    char cranialFulcrumRibSlamDirKey = 'A';
    DWORD lastPlasmaContinuumSingularityTime = 0;
    DWORD lastCranialFulcrumClaviclePinTime = 0;
    char cranialFulcrumClaviclePinDirKey = 'A';
    DWORD lastPlasmaMultiverseSingularityTime = 0;
    DWORD lastCranialFulcrumScapulaPinTime = 0;
    char cranialFulcrumScapulaPinDirKey = 'A';
    DWORD lastPlasmaOmniverseSingularityTime = 0;
    DWORD lastCranialFulcrumSternumPinTime = 0;
    char cranialFulcrumSternumPinDirKey = 'A';
    DWORD lastPlasmaTransCosmicSingularityTime = 0;
    DWORD lastCranialFulcrumMandiblePinTime = 0;
    char cranialFulcrumMandiblePinDirKey = 'A';
    DWORD lastPlasmaMacrocosmSingularityTime = 0;
    DWORD lastCranialFulcrumZygomaticPinTime = 0;
    char cranialFulcrumZygomaticPinDirKey = 'A';
    DWORD lastPlasmaUniversalSingularityTime = 0;
    DWORD lastOutsideLegSweepTime = 0;
    DWORD outsideLegSweepHookTime = 0;
    char outsideLegSweepDirKey = 'A';
    DWORD lastCrossThrustTime = 0;
    DWORD crossBlockHoldStart = 0;
};
static MeleeState s_melee;

struct MeleeCounterIntent {
    bool dualCrossSlash = false;
    bool isForwardPunch = false;
    bool isHookPunch = false;
    bool isUppercutPunch = false;
    bool isMeleeStrike = false;
    bool dualRifleStrip = false;
    bool isForwardParry = false;
    bool isEyeGougeRiposte = false;
    bool isShieldPryDisarm = false;
    bool isPocketAshRiposte = false;
};

// GH #17 step 9: section 5 counter/disarm detection (rifle-strip, ash-riposte,
// shield-pry, eye-gouge, parry) plus the strike-gate classifiers. Pure detection
// over frame inputs + MeleeState; side effects stay at the call site.
static MeleeCounterIntent UpdateMeleeCounters(MeleeState& st, DWORD now,
                                               const RiddickVRVector3& lVel, const RiddickVRVector3& rVel,
                                               const RiddickVRVector3& leftLocal, const RiddickVRVector3& rightLocal,
                                               float leftGrip, float rightGrip,
                                               float leftTrigger, float rightTrigger) {
    MeleeCounterIntent out;
    (void)now;

            // ADR 0403 / GH #62 / GH #65: discriminate deliberate directional
            // punch strikes from *raising* the arms into Boxer Guard. Raising
            // arms upward (Vy > 0) into guard must NEVER trigger an attack or
            // cancel guard, and a fast swing thrown from a raised guard must
            // still fire. The guard-volume computation below is retained only so
            // the block pose and the strike gate share the ADR-0434 front-of-HMD
            // envelope; since GH #65 the strike decision itself is velocity
            // driven (see combat_strike.h) and is deliberately NOT suppressed
            // inside the volume -- a jab thrown while blocking is a legitimate
            // forward punch.
            const GuardEnvelope guardVolume = GetGuardEnvelope();
            bool inGuardVolume = InGuardEnvelope(guardVolume,
                                                 leftLocal.y, leftLocal.z,
                                                 rightLocal.y, rightLocal.z,
                                                 leftLocal.x - rightLocal.x);

            // GH #65: the punch/hook/uppercut classifiers are a shared,
            // INI-tunable, unit-tested threshold set (`combat_strike.h`,
            // `Strike*` keys). Do NOT suppress strikes inside the guard volume --
            // a jab thrown while blocking is a legitimate forward punch. A guard
            // *raise* (Vy > 0 with Vz ~ 0) still matches none of the strike
            // conditions, preserving the ADR-0403 intent.
            const StrikeThresholds strikeThresholds = GetStrikeThresholds();
            const StrikeIntent strike = ClassifyStrike(strikeThresholds,
                                                       StrikeVelocity{lVel.x, lVel.y, lVel.z},
                                                       StrikeVelocity{rVel.x, rVel.y, rVel.z},
                                                       leftLocal.y, leftLocal.z,
                                                       rightLocal.y, rightLocal.z);
            (void)inGuardVolume;
            bool dualCrossSlash = strike.dualCrossSlash;
            bool isForwardPunch = strike.forwardPunch;
            bool isHookPunch = strike.hookPunch;
            bool isUppercutPunch = strike.uppercutPunch;
            bool isMeleeStrike = strike.strike;

            // 5a. Physical Parry / Counter-Disarm Thrust (ADR 0018)
            // In Starbreeze engine: Left Click while Right Click (Block) is held executes Counter-Disarm!
            // When in active Boxer Guard (s_currentBlockEngaged), driving fists forward into an attack (Vz < -1.15 m/s)
            // executes the reactive counter-strike / disarm without canceling block mode.
            // Guard Rifle Firearm Stripping & Mag Ejection Disarm (ADR 0063)
            bool dualRifleStrip = st.currentBlockEngaged &&
                                  (rVel.z < -1.15f && lVel.z < -1.15f) &&
                                  (rightTrigger > 0.60f && leftTrigger > 0.60f);

            bool isForwardParry = st.currentBlockEngaged &&
                ((rVel.z < -1.15f && rightLocal.z < -0.15f) || (lVel.z < -1.15f && leftLocal.z < -0.15f));

            // Inmate Shiv Parrying Riposte & Eye-Gouge Counter (ADR 0092)
            bool isEyeGougeRiposte = st.currentBlockEngaged &&
                (rVel.z < -1.10f && rVel.y > 1.05f && rightLocal.y > 0.04f);

            // Security Guard Riot Shield Baton Wedge Pry & Disarm (ADR 0111)
            bool bothAtShieldZone = (rightLocal.z > -0.42f && rightLocal.z < -0.16f && leftLocal.z > -0.42f && leftLocal.z < -0.16f) &&
                                    (rightLocal.y > -0.20f && rightLocal.y < 0.20f && leftLocal.y > -0.20f && leftLocal.y < 0.20f);
            bool shieldGrippedAndWedged = (leftGrip > 0.60f) &&
                                          (rightTrigger > 0.50f || rightGrip > 0.60f);
            bool opposingShieldWrench = (lVel.x < -0.90f || lVel.y < -0.80f) && (rVel.x > 0.90f || rVel.y > 0.80f);
            bool isShieldPryDisarm = bothAtShieldZone && shieldGrippedAndWedged && opposingShieldWrench;

            // Inmate Pit Brawl Pocket Ash Throw Riposte (ADR 0115)
            bool rightAtPocket = (rightLocal.y > -0.45f && rightLocal.y < -0.15f) &&
                                 (fabsf(rightLocal.x) > 0.10f && fabsf(rightLocal.x) < 0.26f) &&
                                 (rightGrip > 0.50f);
            bool pocketAshFling = (rVel.y > 1.10f && rVel.z < -0.90f);
            bool isPocketAshRiposte = st.currentBlockEngaged && rightAtPocket && pocketAshFling;
    out.dualCrossSlash = dualCrossSlash;
    out.isForwardPunch = isForwardPunch;
    out.isHookPunch = isHookPunch;
    out.isUppercutPunch = isUppercutPunch;
    out.isMeleeStrike = isMeleeStrike;
    out.dualRifleStrip = dualRifleStrip;
    out.isForwardParry = isForwardParry;
    out.isEyeGougeRiposte = isEyeGougeRiposte;
    out.isShieldPryDisarm = isShieldPryDisarm;
    out.isPocketAshRiposte = isPocketAshRiposte;
    return out;
}

struct MeleeStrikeIntent {
    bool mechPowerPunch = false;
    bool droneEmpSnap = false;
    bool dualShieldBash = false;
    bool dualShieldPlant = false;
    bool dualStoolHeavySmash = false;
    bool dualPalmShove = false;
    bool isLookingDown = false;
    bool isDownwardPlunge = false;
    bool dualOverheadSlam = false;
    bool leftTrayBatter = false;
    bool twoHandedBatonSlam = false;
    bool dualSledgeOverhead = false;
    bool isDischargingArcWave = false;
    bool stealthCarotidSlash = false;
    bool isRibUnderhook = false;
    bool dartCounterJabStrike = false;
    bool isDoubleEarClap = false;
    bool isBatonRibJab = false;
    bool isParrySnap = false;
    bool blindRiposteStrike = false;
    bool isKneeStomp = false;
    bool isInductionOverheated = false;
    bool sternumElbowSmash = false;
    bool isBatonChargedSpear = false;
    bool isClinchHeadbutt = false;
    bool isBatonGroundPin = false;
    bool isLeadKneeThrust = false;
    bool isCrossChestArcWall = false;
    bool isDuckUnderKidneyCounter = false;
    bool isBatonArcTrapArmed = false;
    bool cervicalSnapWhip = false;
    bool isBatonGroundDischarge = false;
    bool guillotineWrapStrike = false;
    bool isBatonSkullBreaker = false;
    bool isBatonLateralDisarm = false;
    bool hammerfistStrike = false;
    bool suplexSpineSlam = false;
    bool isCrossArcBurstReady = false;
    bool isBatonJavelinThrust = false;
    bool isOutsideTripMount = false;
    bool isKimuraFracture = false;
    bool isArmTriangleReady = false;
    bool isBatonDiagonalParry = false;
    bool isBatonRadialSweep = false;
    bool isSingleLegTakedown = false;
    bool batonTripSweep = false;
    bool doubleLegSpikeSlam = false;
    bool batonThroatThrustStrike = false;
    bool firemansWhipThrow = false;
    bool frontHeadlockWrap = false;
    bool frontHeadlockArchTorque = false;
    bool crossParryRiposte = false;
    bool suloevLowCrouch = false;
    bool suloevHookHold = false;
    bool suloevArchPull = false;
    bool isLookingDownward = false;
    bool batonOverheadVault = false;
    bool batonVaultPlunge = false;
    bool backSuplexHoistSlam = false;
    bool radialDisarmThrust = false;
    bool thaiPlumKneeDrive = false;
    bool javelinVaultStrike = false;
    bool whizzerDownWhip = false;
    bool batonHammerfistPlunge = false;
    bool hipTossRotation = false;
    bool batonShieldBashThrust = false;
    bool thaiPlumElbowPlunge = false;
    bool groundingThrustLunge = false;
    bool lateralSuplexThrow = false;
    bool kineticCleaveSwing = false;
    bool whizzerCounterThrow = false;
    bool cleaveArcBlastStrike = false;
    bool cervicalSpikeThrow = false;
    bool vaultGroundArcPlunge = false;
    bool thoracicSlamThrow = false;
    bool vaultKineticCleavePlunge = false;
    bool hyperextensionSpikeThrow = false;
    bool deckImpactPlunge = false;
    bool whizzerGroundFinisherThrow = false;
    bool omnidirectionalArcPlunge = false;
    bool whizzerGroundDislocationThrow = false;
    bool omnidirectionalPulsePlunge = false;
    bool sternumKneeDropThrow = false;
    bool kineticPulseWavePlunge = false;
    bool sternumFractureSpikeThrow = false;
    bool plasmaCleaveWavePlunge = false;
    bool sternumGratingSlamThrow = false;
    bool plasmaOverloadWavePlunge = false;
    bool cervicalGratingSlamThrow = false;
    bool plasmaResonantWavePlunge = false;
    bool hyperextensionFinisherThrow = false;
    bool plasmaHarmonicQuakePlunge = false;
    bool thoracicDislocationThrow = false;
    bool plasmaResonantSeismPlunge = false;
    bool thoracicMatSlamThrow = false;
    bool plasmaSeismOverdrivePlunge = false;
    bool thoracicPinLockThrow = false;
    bool plasmaCataclysmPlunge = false;
    bool sternumElbowDropPlunge = false;
    bool plasmaCataclysmOverdrivePlunge = false;
    bool cranialDeckDriverPlunge = false;
    bool plasmaSingularityPlunge = false;
    bool sternumKneeSpikePlunge = false;
    bool plasmaSupernovaPlunge = false;
    bool cervicalDeckPinPlunge = false;
    bool plasmaHypernovaPlunge = false;
    bool cranialGrindPlunge = false;
    bool plasmaSingularityOverdrivePlunge = false;
    bool cranialPostImpactPlunge = false;
    bool plasmaGenesisPlunge = false;
    bool cranialStompPlunge = false;
    bool plasmaBigBangPlunge = false;
    bool cranialDoubleStrikePlunge = false;
    bool plasmaCosmicDawnPlunge = false;
    bool cranialPivotTwistPlunge = false;
    bool plasmaSuperclusterPlunge = false;
    bool cranialPivotSlamPlunge = false;
    bool plasmaOmniversePlunge = false;
    bool cranialLeverPinPlunge = false;
    bool plasmaMetaversePlunge = false;
    bool cranialFulcrumChokePlunge = false;
    bool plasmaHyperdimensionalPlunge = false;
    bool cranialFulcrumArmbarPlunge = false;
    bool plasmaTransdimensionalPlunge = false;
    bool cranialFulcrumKneePlunge = false;
    bool plasmaInterdimensionalPlunge = false;
    bool cranialFulcrumRibSlamPlunge = false;
    bool plasmaContinuumSingularityPlunge = false;
    bool cranialFulcrumClaviclePinPlunge = false;
    bool plasmaMultiverseSingularityPlunge = false;
    bool cranialFulcrumScapulaPinPlunge = false;
    bool plasmaOmniverseSingularityPlunge = false;
    bool cranialFulcrumSternumPinPlunge = false;
    bool plasmaTransCosmicSingularityPlunge = false;
    bool cranialFulcrumMandiblePinPlunge = false;
    bool plasmaMacrocosmSingularityPlunge = false;
    bool cranialFulcrumZygomaticPinPlunge = false;
    bool plasmaUniversalSingularityPlunge = false;
    bool outsideLegKneeDrop = false;
    bool isCrossThrustReady = false;
    bool isCrossThrustOvercharged = false;
    bool arcWaveCharging = false;
    bool dartRipped = false;
    bool eyeWipeCleared = false;
    bool inductionBuzz = false;
    bool liverHookConnected = false;
    bool arcWallPulse = false;
    bool arcTrapBuzz = false;
    bool palmChinConnected = false;
    bool snapdownInitiated = false;
    bool shinTrapDeflected = false;
    bool suplexHoistInitiated = false;
    bool crossArcCharging = false;
    bool batonDischargeJabConnected = false;
    bool doubleLegLiftInitiated = false;
    bool pistolTrapWedged = false;
    bool firemansLoaded = false;
    bool crossParryDeflected = false;
    bool rearWaistClinchLocked = false;
    bool radialDisarmTwisted = false;
    bool thaiPlumLocked = false;
    bool outsideLegHooked = false;
    bool crossThrustSupercharged = false;
    bool crossThrustBarrierHum = false;
    float arcWaveChargeRatio = 0.0f;
    float inductionRampRatio = 0.0f;
    float arcTrapDwellRatio = 0.0f;
};

// GH #17 step 9: section 5 strike/finisher detection (punch/jab/hook/uppercut
// classifiers, per-weapon finisher tables, charge/combo staging). Detection + staging
// state updates only; edge side effects surface as intent flags for the call site.
// Call inside the isMeleeStrike branch AFTER the guard disengage (st.currentBlockEngaged
// is already cleared there, matching the original evaluation order).
static MeleeStrikeIntent UpdateMeleeStrikes(MeleeState& st, DWORD now,
                                             const RiddickVRVector3& lVel, const RiddickVRVector3& rVel,
                                             const RiddickVRVector3& leftLocal, const RiddickVRVector3& rightLocal,
                                             float lSpeed, float rSpeed,
                                             float leftGrip, float rightGrip,
                                             float leftTrigger, float rightTrigger,
                                             uint32_t rightButtons,
                                             bool crouched, bool deepProne,
                                             float hmdPitch, bool twoHandedActive) {
    MeleeStrikeIntent out;
    const auto& activeVel = (rSpeed > lSpeed) ? rVel : lVel;
                // Heavy Exoskeleton Power Fist Kinetic Shockwave Punch (ADR 0066)
                bool mechPowerPunch = (rVel.z < -1.60f) && (rightGrip > 0.60f);

                // Security Drone EMP Overload & Chassis Snap (ADR 0058)
                bool dualGrappleDrone = (rightLocal.z < -0.20f && leftLocal.z < -0.20f) &&
                                        (rightLocal.z > -0.45f && leftLocal.z > -0.45f) &&
                                        (rightGrip > 0.65f && leftGrip > 0.65f);
                bool dualTorqueSnap = (fabsf(lVel.y - rVel.y) > 1.20f) || (fabsf(lVel.x - rVel.x) > 1.20f);
                bool droneEmpSnap = dualGrappleDrone && dualTorqueSnap;

                // Dual-Hand Riot Shield Concussive Bash (ADR 0052)
                bool dualShieldBash = twoHandedActive &&
                                      (rVel.z < -1.15f && lVel.z < -1.15f) &&
                                      (rightLocal.z < -0.18f && leftLocal.z < -0.18f);

                // Dual-Hand Riot Shield Ballistic Plant (ADR 0052)
                bool dualShieldPlant = twoHandedActive &&
                                       (lVel.y < -0.75f && rVel.y < -0.75f) &&
                                       (lVel.z < -0.35f && rVel.z < -0.35f);

                // Inmate Pit Brawl Stool / Chair Blunt Weapon Heavy Smash (ADR 0054)
                bool dualStoolHeavySmash = (leftGrip > 0.60f && rightGrip > 0.60f) &&
                                           (fabsf(leftLocal.x - rightLocal.x) < 0.38f) &&
                                           (lSpeed > 1.35f && rSpeed > 1.35f);

                // Pit Fighting Arena Dual-Palm Concussive Shove (ADR 0041)
                bool dualPalmShove = (rVel.z < -1.10f && lVel.z < -1.10f) &&
                                     (rightLocal.z < -0.15f && leftLocal.z < -0.15f) &&
                                     (rightGrip > 0.40f && leftGrip > 0.40f);

                // Vent Duct Pipe Hang & Death-From-Above Aerial Drop Ambush (ADR 0033)
                bool isLookingDown = (hmdPitch < -0.70f); // Looking steeply downward (< -40 deg) from ledge/duct
                bool isDownwardPlunge = (activeVel.y < -1.10f);

                // Two-Handed Overhead Pipe Slam / Heavy Club Crushing Strike (ADR 0026)
                bool dualOverheadSlam = (leftLocal.y > 0.10f && rightLocal.y > 0.10f) &&
                                        (leftGrip > 0.45f && rightGrip > 0.45f) &&
                                        (lVel.y < -1.30f && rVel.y < -1.30f);

                // Inmate Mess Hall Tray Facial Batter (ADR 0060)
                bool leftTrayBatter = (leftLocal.z < -0.18f) && (lVel.z < -1.0f) && (leftGrip > 0.50f);

                // Guard Stun Baton Heavy Two-Handed Defibrillator Slam (ADR 0077)
                bool twoHandedBatonSlam = twoHandedActive &&
                                          (lVel.y < -1.30f && rVel.y < -1.30f) &&
                                          (rightTrigger > 0.60f && leftTrigger > 0.60f);

                // Mining Core Heavy Ore Sledgehammer Two-Handed Overhead Crush (ADR 0102)
                float handsDistSledge = sqrtf(
                    powf(leftLocal.x - rightLocal.x, 2) +
                    powf(leftLocal.y - rightLocal.y, 2) +
                    powf(leftLocal.z - rightLocal.z, 2)
                );
                bool dualSledgeOverhead = (handsDistSledge < 0.40f) &&
                                          (leftLocal.y > 0.28f && rightLocal.y > 0.28f) &&
                                          (leftGrip > 0.55f && rightGrip > 0.55f) &&
                                          (lVel.y < -1.40f && rVel.y < -1.40f);

                // Tactical Stun Baton Overload Arc Wave Discharge (ADR 0118)
                bool crossedBatonCharge = (handsDistSledge < 0.18f) &&
                                          (leftLocal.y > -0.25f && leftLocal.y < 0.10f && rightLocal.y > -0.25f && rightLocal.y < 0.10f) &&
                                          (leftGrip > 0.70f && rightGrip > 0.70f) &&
                                          (leftTrigger > 0.65f && rightTrigger > 0.65f);
                bool isDischargingArcWave = false;
                if (crossedBatonCharge) {
                    if (st.arcWaveChargeStart == 0) st.arcWaveChargeStart = now;
                    DWORD chargeTime = now - st.arcWaveChargeStart;
                    float chargeRatio = (chargeTime < 400) ? (chargeTime / 400.0f) : 1.0f;
                    out.arcWaveCharging = true; out.arcWaveChargeRatio = chargeRatio;
                } else {
                    if (st.arcWaveChargeStart != 0 && (now - st.arcWaveChargeStart >= 250)) {
                        bool explosiveRadialThrust = (lVel.x < -1.05f && rVel.x > 1.05f) || (lVel.z < -1.10f && rVel.z < -1.10f);
                        if (explosiveRadialThrust && (now - st.lastArcWaveTime > 2500)) {
                            st.lastArcWaveTime = now;
                            isDischargingArcWave = true;
                        }
                    }
                    st.arcWaveChargeStart = 0;
                }

                bool crouchedForStealth = (crouched || deepProne);
                bool atThroatCorridor = (rightLocal.y > -0.12f && rightLocal.y < 0.16f) &&
                                        (rightLocal.z > -0.34f && rightLocal.z < -0.10f) &&
                                        (rightGrip > 0.50f);
                bool carotidSlice = (fabsf(rVel.x) > 1.25f);
                bool stealthCarotidSlash = crouchedForStealth && atThroatCorridor && carotidSlice;

                // Inmate Shiv Rib-Cage Underhook Puncture & Lift (ADR 0122)
                bool lowStanceForUnderhook = (crouched || deepProne) || (rightLocal.y < -0.15f);
                bool reverseBladeGrip = (rightGrip > 0.55f && rightTrigger < 0.40f);
                bool underhookRisingThrust = (rVel.y > 1.15f && rVel.z < -0.90f);
                bool isRibUnderhook = lowStanceForUnderhook && reverseBladeGrip && underhookRisingThrust;

                // Security Guard Stun Dart Extraction & Counter-Jab (ADR 0126)
                bool handAtChestDart = (fabsf(rightLocal.x) < 0.16f && rightLocal.y > -0.25f && rightLocal.y < 0.05f && rightLocal.z > -0.25f && rightLocal.z < -0.06f);
                bool dartRipYank = (rVel.z > 0.70f || fabsf(rVel.x) > 0.75f);
                if (handAtChestDart && rightGrip > 0.65f && dartRipYank && (now - st.lastDartCounterJabTime > 1500)) {
                    if (st.dartExtractedTime == 0) {
                        st.dartExtractedTime = now;
                        out.dartRipped = true;
                    }
                }
                bool dartCounterJabStrike = (st.dartExtractedTime > 0 && (now - st.dartExtractedTime < 800) &&
                                            rVel.z < -1.15f && rightLocal.y > -0.10f && rightLocal.y < 0.22f);
                if (st.dartExtractedTime != 0 && (now - st.dartExtractedTime >= 800)) st.dartExtractedTime = 0; // GH #25: timeout reset

                // Inmate Pit Brawl Double-Ear Concussive Clap Strike (ADR 0128)
                bool bothHandsOpenForClap = (leftTrigger < 0.25f && rightTrigger < 0.25f &&
                                             leftGrip < 0.35f && rightGrip < 0.35f);
                bool lateralInwardClapAcc = (lVel.x > 0.95f && rVel.x < -0.95f);
                bool clapCorridor = (handsDistSledge < 0.24f && rightLocal.y > -0.10f && rightLocal.y < 0.20f && rightLocal.z > -0.35f && rightLocal.z < -0.10f);
                bool isDoubleEarClap = bothHandsOpenForClap && lateralInwardClapAcc && clapCorridor;

                // Security Guard Stun Baton Rib-Jab & Ground Takedown (ADR 0133)
                bool batonRibJabArmed = (rightGrip > 0.60f && rightTrigger > 0.60f);
                bool batonRibJabThrust = (rVel.z < -1.10f && rVel.y < -0.85f);
                bool batonRibJabCorridor = (rightLocal.y > -0.30f && rightLocal.y < 0.05f && rightLocal.z > -0.35f && rightLocal.z < -0.10f);
                bool isBatonRibJab = batonRibJabArmed && batonRibJabThrust && batonRibJabCorridor;

                // Inmate Shiv Forearm Defensive Parry-and-Snap (ADR 0134)
                bool parrySnapOffHandDeflect = (fabsf(lVel.x) > 0.85f);
                bool parrySnapDominantStrike = (rVel.y < -1.15f);
                bool parrySnapProximity = (handsDistSledge < 0.28f);
                bool isParrySnap = parrySnapOffHandDeflect && parrySnapDominantStrike && parrySnapProximity;

                // Inmate Pit Brawl Dirt / Ash Eye-Rub Recovery Riposte (ADR 0136)
                bool offHandAtEyes = (fabsf(leftLocal.x) < 0.14f && leftLocal.y > -0.08f && leftLocal.y < 0.15f && leftLocal.z > -0.22f && leftLocal.z < -0.06f);
                bool eyeWipeStroke = (fabsf(lVel.x) > 0.50f);
                if (offHandAtEyes && eyeWipeStroke && (now - st.lastEyeRubRiposteTime > 1500)) {
                    if (st.eyeRubWipeTime == 0) {
                        st.eyeRubWipeTime = now;
                        out.eyeWipeCleared = true;
                    }
                }
                bool blindRiposteStrike = (st.eyeRubWipeTime > 0 && (now - st.eyeRubWipeTime < 600) && rVel.z < -1.15f);
                if (st.eyeRubWipeTime != 0 && (now - st.eyeRubWipeTime >= 600)) st.eyeRubWipeTime = 0; // GH #25: timeout reset

                // Inmate Pit Brawl Shin Kick & Knee Stomp Hyper-Extension (ADR 0141)
                bool kneeStompPosture = (st.currentBlockEngaged || crouched || deepProne);
                bool kneeStompStrike = (rVel.y < -1.10f && lVel.z < -0.70f);
                bool isKneeStomp = kneeStompPosture && kneeStompStrike;

                // Security Guard Stun Baton Weapon Shock Induction Arc Overheat (ADR 0142)
                bool inductionGroundedStance = (handsDistSledge < 0.16f) &&
                                               (leftGrip > 0.60f && rightGrip > 0.60f) &&
                                               (leftTrigger > 0.65f && rightTrigger > 0.65f);
                bool isInductionOverheated = false;
                if (inductionGroundedStance) {
                    if (st.inductionOverheatStart == 0) st.inductionOverheatStart = now;
                    DWORD overchargeTime = now - st.inductionOverheatStart;
                    float rampRatio = (overchargeTime < 350) ? (overchargeTime / 350.0f) : 1.0f;
                    out.inductionBuzz = true; out.inductionRampRatio = rampRatio;
                    if (overchargeTime >= 350 && (now - st.lastInductionOverheatTime > 2500)) {
                        st.lastInductionOverheatTime = now;
                        st.inductionOverheatStart = 0;
                        isInductionOverheated = true;
                    }
                } else {
                    st.inductionOverheatStart = 0;
                }

                // Inmate Pit Brawl Liver Hook & Sternum Elbow Smash Combination (ADR 0145)
                bool liverHookStrike = (st.currentBlockEngaged && rVel.y > 1.10f && fabsf(rVel.x) > 0.90f);
                if (liverHookStrike && (now - st.lastLiverElbowComboTime > 1500)) {
                    if (st.liverHookTime == 0) {
                        st.liverHookTime = now;
                        out.liverHookConnected = true;
                    }
                }
                bool sternumElbowSmash = (st.liverHookTime > 0 && (now - st.liverHookTime < 500) && rVel.y < -1.20f);
                if (st.liverHookTime != 0 && (now - st.liverHookTime >= 500)) st.liverHookTime = 0; // GH #25: timeout reset

                // Security Guard Stun Baton Charged Underarm Spear Thrust (ADR 0146)
                bool batonChargedSpearArmed = (rightGrip > 0.60f && rightTrigger > 0.65f);
                bool batonSpearBracing = (handsDistSledge < 0.22f);
                bool batonLinearSpearThrust = (rVel.z < -1.35f);
                bool isBatonChargedSpear = batonChargedSpearArmed && batonSpearBracing && batonLinearSpearThrust;

                // Inmate Pit Brawl Collar Tie Clinch & Headbutt Stun (ADR 0149)
                bool collarTieClinchGrip = (leftLocal.z > -0.35f && leftLocal.z < -0.12f && leftLocal.y > -0.15f && leftLocal.y < 0.15f && leftGrip > 0.65f);
                bool headbuttKineticSurge = (lVel.z > 0.55f && rVel.z < -1.05f) || (rVel.z < -1.25f);
                bool isClinchHeadbutt = collarTieClinchGrip && headbuttKineticSurge;

                // Security Guard Stun Baton Ground Pin & Taser Kneel (ADR 0150)
                bool lowGroundPosture = (crouched || deepProne || rightLocal.y < -0.35f);
                bool batonGroundEnergized = (rightGrip > 0.60f && rightTrigger > 0.65f);
                bool verticalGroundPlunge = (rVel.y < -1.15f);
                bool isBatonGroundPin = lowGroundPosture && batonGroundEnergized && verticalGroundPlunge;

                // Inmate Pit Brawl Muay Thai Lead Knee Thrust to Abdomen (ADR 0153)
                bool leadKneePosture = st.currentBlockEngaged;
                bool leadKneeStrikes = (rVel.y > 1.20f && lVel.y < -0.85f);
                bool isLeadKneeThrust = leadKneePosture && leadKneeStrikes;

                // Security Guard Stun Baton Two-Handed Cross-Chest Defensive Arc Wall (ADR 0154)
                bool batonArcWallGeometry = (handsDistSledge > 0.20f && handsDistSledge < 0.38f && fabsf(rightLocal.x - leftLocal.x) > 0.22f &&
                                             rightLocal.y > -0.20f && rightLocal.y < 0.10f && leftLocal.y > -0.20f && leftLocal.y < 0.10f &&
                                             rightLocal.z > -0.30f && rightLocal.z < -0.10f && leftLocal.z > -0.30f && leftLocal.z < -0.10f);
                bool batonArcWallGrips = (leftGrip > 0.65f && rightGrip > 0.65f &&
                                          leftTrigger > 0.60f && rightTrigger > 0.60f);
                bool isCrossChestArcWall = batonArcWallGeometry && batonArcWallGrips;
                if (isCrossChestArcWall) {
                    if (now - st.lastArcWallPulse > 180) {
                        st.lastArcWallPulse = now;
                        out.arcWallPulse = true;
                    }
                }

                // Inmate Pit Brawl Duck-Under Kidney Liver Counter-Jab (ADR 0157)
                bool duckUnderCrouch = (crouched || deepProne);
                bool kidneyStrikeCorridor = (rightLocal.y > -0.35f && rightLocal.y < -0.05f);
                bool kidneyCounterPunch = (fabsf(rVel.x) > 0.85f && rVel.z < -1.05f);
                bool isDuckUnderKidneyCounter = duckUnderCrouch && kidneyStrikeCorridor && kidneyCounterPunch;

                // Security Guard Stun Baton High-Voltage Capacitance Bleed Arc Trap (ADR 0158)
                bool batonAtConduitSurface = (handsDistSledge < 0.22f &&
                                              rightLocal.z > -0.42f && rightLocal.z < -0.15f && rightLocal.y > -0.30f && rightLocal.y < 0.10f &&
                                              rSpeed < 0.12f &&
                                              leftTrigger > 0.70f && rightTrigger > 0.70f);
                bool isBatonArcTrapArmed = false;
                if (batonAtConduitSurface) {
                    if (st.batonArcTrapStart == 0) st.batonArcTrapStart = now;
                    DWORD chargeDwell = now - st.batonArcTrapStart;
                    float dwellRatio = (chargeDwell < 400) ? (chargeDwell / 400.0f) : 1.0f;
                    out.arcTrapBuzz = true; out.arcTrapDwellRatio = dwellRatio;
                    if (chargeDwell >= 400 && (now - st.lastBatonArcTrapTime > 2000)) {
                        st.lastBatonArcTrapTime = now;
                        st.batonArcTrapStart = 0;
                        isBatonArcTrapArmed = true;
                    }
                } else {
                    st.batonArcTrapStart = 0;
                }

                // Inmate Pit Brawl Upward Palm Chin-Strike & Cervical Snap (ADR 0161)
                bool upwardPalmStrike = (rVel.y > 1.25f && rVel.z < -0.80f && rightLocal.y > -0.10f && rightLocal.y < 0.25f);
                if (upwardPalmStrike && (now - st.lastPalmChinComboTime > 1500)) {
                    if (st.palmChinStrikeTime == 0) {
                        st.palmChinStrikeTime = now;
                        out.palmChinConnected = true;
                    }
                }
                bool cervicalSnapWhip = (st.palmChinStrikeTime > 0 && (now - st.palmChinStrikeTime < 500) && fabsf(lVel.x) > 1.10f);
                if (st.palmChinStrikeTime != 0 && (now - st.palmChinStrikeTime >= 500)) st.palmChinStrikeTime = 0; // GH #25: timeout reset

                // Security Guard Stun Baton Dual-Hand Ground Discharge Kinetic Shockwave (ADR 0162)
                bool batonDualGroundEnergized = (handsDistSledge < 0.22f &&
                                                 rightGrip > 0.65f && rightTrigger > 0.65f &&
                                                 leftGrip > 0.60f && leftTrigger > 0.60f);
                bool dualGroundPlunge = (rVel.y < -1.25f && lVel.y < -1.25f && rightLocal.y < -0.40f);
                bool isBatonGroundDischarge = batonDualGroundEnergized && dualGroundPlunge;

                // Inmate Pit Brawl Collar Tie Snap-Down & Guillotine Choke (ADR 0165)
                bool collarSnapdownGrip = (leftLocal.z > -0.35f && leftLocal.z < -0.12f && leftGrip > 0.65f);
                bool collarSnapdownPull = (lVel.y < -0.90f);
                if (collarSnapdownGrip && collarSnapdownPull && (now - st.lastSnapdownChokeTime > 1500)) {
                    if (st.snapdownStartTime == 0) {
                        st.snapdownStartTime = now;
                        out.snapdownInitiated = true;
                    }
                }
                bool guillotineWrapStrike = (st.snapdownStartTime > 0 && (now - st.snapdownStartTime < 500) && rVel.y > 1.05f && rVel.z < -0.75f);
                if (st.snapdownStartTime != 0 && (now - st.snapdownStartTime >= 500)) st.snapdownStartTime = 0; // GH #25: timeout reset

                // Security Guard Stun Baton Charged High-Arc Downward Skull Breaker (ADR 0166)
                bool batonOverheadEnergized = (rightLocal.y > 0.25f && rightGrip > 0.60f && rightTrigger > 0.65f);
                bool batonExtremeCleave = (rVel.y < -1.45f);
                bool isBatonSkullBreaker = batonOverheadEnergized && batonExtremeCleave;

                // Security Guard Stun Baton Lateral Cross-Sweep Deflection & Disarm (ADR 0170)
                bool batonLateralSweepArmed = (rightGrip > 0.60f && rightTrigger > 0.65f &&
                                               rightLocal.y > -0.20f && rightLocal.y < 0.20f);
                bool batonCrossSweepFast = (fabsf(rVel.x) > 1.25f);
                bool isBatonLateralDisarm = batonLateralSweepArmed && batonCrossSweepFast;

                // Inmate Pit Brawl Shin Trap & Overhand Hammerfist Ground Finisher (ADR 0169)
                bool shinTrapDeflect = (leftLocal.y < -0.20f && leftGrip > 0.55f && lVel.y < -0.85f);
                if (shinTrapDeflect && (now - st.lastShinTrapTime > 1500)) {
                    if (st.shinTrapTime == 0) {
                        st.shinTrapTime = now;
                        out.shinTrapDeflected = true;
                    }
                }
                bool hammerfistStrike = (st.shinTrapTime > 0 && (now - st.shinTrapTime < 500) && rVel.y < -1.30f);
                if (st.shinTrapTime != 0 && (now - st.shinTrapTime >= 500)) st.shinTrapTime = 0; // GH #25: timeout reset

                // Inmate Pit Brawl Underhook Suplex & Spine Slam (ADR 0173)
                bool underhookGripHold = (handsDistSledge < 0.25f && leftGrip > 0.65f && rightGrip > 0.65f);
                bool underhookVerticalHoist = (lVel.y > 1.10f && rVel.y > 1.10f);
                if (underhookGripHold && underhookVerticalHoist && (now - st.lastSuplexSlamTime > 1800)) {
                    if (st.suplexHoistTime == 0) {
                        st.suplexHoistTime = now;
                        out.suplexHoistInitiated = true;
                    }
                }
                bool suplexSpineSlam = (st.suplexHoistTime > 0 && (now - st.suplexHoistTime < 500) && lVel.y < -1.20f && rVel.y < -1.20f);
                if (st.suplexHoistTime != 0 && (now - st.suplexHoistTime >= 500)) st.suplexHoistTime = 0; // GH #25: timeout reset

                // Security Guard Stun Baton Dual-Hand Cross-Block Reactive Arc Burst (ADR 0174)
                bool crossWristFrame = (handsDistSledge < 0.16f &&
                                        rightLocal.y > -0.15f && rightLocal.y < 0.15f && rightLocal.z > -0.30f && rightLocal.z < -0.10f &&
                                        leftGrip > 0.65f && rightGrip > 0.65f &&
                                        leftTrigger > 0.65f && rightTrigger > 0.65f);
                bool isCrossArcBurstReady = false;
                if (crossWristFrame) {
                    if (st.crossArcStartTime == 0) st.crossArcStartTime = now;
                    if (now - st.crossArcStartTime >= 300) {
                        out.crossArcCharging = true;
                        if (rVel.z < -1.15f || lVel.z < -1.15f) {
                            isCrossArcBurstReady = true;
                        }
                    }
                } else {
                    st.crossArcStartTime = 0;
                }

                // Security Guard Stun Baton Forward Two-Handed Javelin Thrust (ADR 0178)
                bool batonJavelinGrip = (handsDistSledge > 0.18f && handsDistSledge < 0.40f &&
                                         rightGrip > 0.60f && leftGrip > 0.50f &&
                                         rightTrigger > 0.65f);
                bool batonJavelinThrust = (rVel.z < -1.35f && lVel.z < -1.05f);
                bool isBatonJavelinThrust = batonJavelinGrip && batonJavelinThrust;

                // Inmate Pit Brawl Outside Trip & Mount Ground-and-Pound (ADR 0177)
                bool outsideTripHook = (leftLocal.y < -0.30f && leftGrip > 0.50f && lVel.x < -0.80f);
                bool outsideTripDrive = (rVel.z < -1.10f && rVel.y < -0.75f);
                bool isOutsideTripMount = outsideTripHook && outsideTripDrive;

                // Inmate Pit Brawl Kimura Shoulder Lock & Arm Fracture (ADR 0181)
                bool kimuraGrip = (handsDistSledge < 0.25f &&
                                   leftGrip > 0.65f && rightGrip > 0.65f &&
                                   leftLocal.y > -0.20f && leftLocal.y < 0.20f && rightLocal.y > -0.20f && rightLocal.y < 0.20f);
                bool kimuraCrank = (rVel.y > 1.05f && fabsf(rVel.x) > 0.85f);
                bool isKimuraFracture = kimuraGrip && kimuraCrank;

                // Inmate Pit Brawl Standing Arm-Triangle Choke & Tracheal Clamp (ADR 0185)
                bool armTriangleGrapple = (leftLocal.z > -0.35f && leftLocal.z < -0.10f && leftGrip > 0.65f &&
                                           handsDistSledge < 0.20f && rightGrip > 0.65f &&
                                           leftTrigger > 0.60f && rightTrigger > 0.60f);
                bool isArmTriangleReady = false;
                if (armTriangleGrapple) {
                    if (st.armTriangleStartTime == 0) st.armTriangleStartTime = now;
                    if (now - st.armTriangleStartTime >= 300 && (now - st.lastArmTriangleTime > 2000)) {
                        isArmTriangleReady = true;
                    }
                } else {
                    st.armTriangleStartTime = 0;
                }

                // Security Guard Stun Baton Upward Diagonal Cross-Parry & Counter-Discharge (ADR 0186)
                bool batonDiagonalParryGrip = (handsDistSledge < 0.24f &&
                                               rightGrip > 0.60f && rightTrigger > 0.65f &&
                                               leftGrip > 0.50f &&
                                               rightLocal.y > 0.05f && rightLocal.y < 0.35f);
                bool batonDiagonalParryThrust = (rVel.y > 1.15f && fabsf(rVel.x) > 0.85f);
                bool isBatonDiagonalParry = batonDiagonalParryGrip && batonDiagonalParryThrust;

                // Security Guard Stun Baton Radial Kinetic Sweep & Electrical Stun Ring (ADR 0182)
                bool batonRadialGrip = (handsDistSledge < 0.25f &&
                                        rightGrip > 0.60f && rightTrigger > 0.65f &&
                                        leftGrip > 0.50f);
                bool batonRadialSweep = (fabsf(rVel.x) > 1.35f && fabsf(lVel.x) > 1.10f &&
                                         ((rVel.x > 0.0f && lVel.x > 0.0f) || (rVel.x < 0.0f && lVel.x < 0.0f)));
                bool isBatonRadialSweep = batonRadialGrip && batonRadialSweep;

                // Inmate Pit Brawl Single-Leg Takedown & Ankle Pick Slam (ADR 0189)
                bool singleLegLowStance = (crouched || deepProne || leftLocal.y < -0.30f);
                bool anklePickGrip = (leftLocal.y < -0.40f && leftGrip > 0.60f && lVel.y > 0.85f);
                bool shoulderDriveThrough = (rVel.z < -1.10f);
                bool isSingleLegTakedown = singleLegLowStance && anklePickGrip && shoulderDriveThrough;

                // Security Guard Stun Baton Forward High-Voltage Discharge Jab & Taser Trip (ADR 0190)
                bool batonJabArmed = (rightGrip > 0.60f && rightTrigger > 0.65f);
                bool batonLinearJab = (rVel.z < -1.25f);
                if (batonJabArmed && batonLinearJab && (now - st.lastBatonJabTripTime > 1800)) {
                    if (st.batonJabTime == 0) {
                        st.batonJabTime = now;
                        out.batonDischargeJabConnected = true;
                    }
                }
                bool batonTripSweep = (st.batonJabTime > 0 && (now - st.batonJabTime < 500) &&
                                       rVel.y < -1.10f && fabsf(rVel.x) > 0.80f);
                if (st.batonJabTime != 0 && (now - st.batonJabTime >= 500)) st.batonJabTime = 0; // GH #25: timeout reset

                // Inmate Pit Brawl Double-Leg Takedown & High-Amplitude Mat Slam (ADR 0193)
                bool doubleLegLowStance = (crouched || deepProne || leftLocal.y < -0.30f);
                bool doubleLegThighWrap = (handsDistSledge < 0.25f &&
                                           leftGrip > 0.65f && rightGrip > 0.65f &&
                                           lVel.y > 1.15f && rVel.y > 1.15f);
                if (doubleLegLowStance && doubleLegThighWrap && (now - st.lastDoubleLegSlamTime > 2000)) {
                    if (st.doubleLegLiftTime == 0) {
                        st.doubleLegLiftTime = now;
                        out.doubleLegLiftInitiated = true;
                    }
                }
                bool doubleLegSpikeSlam = (st.doubleLegLiftTime > 0 && (now - st.doubleLegLiftTime < 500) &&
                                           lVel.y < -1.30f && rVel.y < -1.30f);
                if (st.doubleLegLiftTime != 0 && (now - st.doubleLegLiftTime >= 500)) st.doubleLegLiftTime = 0; // GH #25: timeout reset

                // Security Guard Stun Baton Forward Two-Handed Cross-Pistol Disarm & Neck Strike (ADR 0194)
                bool batonPistolDisarmGrip = (handsDistSledge < 0.24f &&
                                              rightGrip > 0.60f && rightTrigger > 0.65f &&
                                              leftGrip > 0.50f &&
                                              rightLocal.y > -0.20f && rightLocal.y < 0.10f);
                bool batonDownwardPistolTrap = (rVel.y < -0.90f && lVel.y < -0.90f && rVel.z < -0.80f);
                if (batonPistolDisarmGrip && batonDownwardPistolTrap && (now - st.lastPistolDisarmTime > 1800)) {
                    if (st.pistolTrapTime == 0) {
                        st.pistolTrapTime = now;
                        out.pistolTrapWedged = true;
                    }
                }
                bool batonThroatThrustStrike = (st.pistolTrapTime > 0 && (now - st.pistolTrapTime < 500) && rVel.z < -1.20f);
                if (st.pistolTrapTime != 0 && (now - st.pistolTrapTime >= 500)) st.pistolTrapTime = 0; // GH #25: timeout reset

                // Inmate Pit Brawl Fireman's Carry & Deck Body-Spike Slam (ADR 0197)
                bool firemansLowDuck = (crouched || deepProne || leftLocal.y < -0.25f);
                bool firemansUnderhookLoad = (handsDistSledge < 0.25f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f &&
                                              rVel.y > 1.10f && lVel.z > 0.70f);
                if (firemansLowDuck && firemansUnderhookLoad && (now - st.lastFiremansCarryTime > 2000)) {
                    if (st.firemansLoadTime == 0) {
                        st.firemansLoadTime = now;
                        out.firemansLoaded = true;
                    }
                }
                bool firemansWhipThrow = (st.firemansLoadTime > 0 && (now - st.firemansLoadTime < 500) &&
                                          (fabsf(lVel.x) > 1.25f || fabsf(rVel.x) > 1.25f));
                if (st.firemansLoadTime != 0 && (now - st.firemansLoadTime >= 500)) st.firemansLoadTime = 0; // GH #25: timeout reset

                // Inmate Pit Brawl Standing Front Headlock & Guillotine Neck Crack (ADR 0205)
                bool frontHeadlockWrap = (handsDistSledge < 0.25f && leftLocal.y > -0.15f &&
                                          leftGrip > 0.65f && rightGrip > 0.65f);
                bool frontHeadlockArchTorque = (lVel.y > 1.15f && rVel.y > 1.15f);

                // Security Guard Stun Baton Lateral Two-Handed Cross-Chest Kinetic Parrying Riposte (ADR 0206)
                bool crossParryStance = (handsDistSledge > 0.18f && handsDistSledge < 0.35f &&
                                         fabsf(rightLocal.x - leftLocal.x) > 0.20f &&
                                         leftGrip > 0.65f && rightGrip > 0.65f &&
                                         rightTrigger > 0.65f);
                bool crossParryDeflect = (crossParryStance && (fabsf(rVel.x) > 0.85f || fabsf(lVel.x) > 0.85f));
                if (crossParryDeflect && (now - st.lastCrossParryTime > 2000)) {
                    if (st.crossParryDeflectTime == 0) {
                        st.crossParryDeflectTime = now;
                        st.crossParryDirKey = (rVel.x > 0.0f || lVel.x > 0.0f) ? 'D' : 'A';
                        out.crossParryDeflected = true;
                    }
                }
                bool crossParryRiposte = (st.crossParryDeflectTime > 0 && (now - st.crossParryDeflectTime < 500) && rVel.z < -1.25f);
                if (st.crossParryDeflectTime != 0 && (now - st.crossParryDeflectTime >= 500)) st.crossParryDeflectTime = 0; // GH #25: timeout reset

                // Inmate Pit Brawl Suloev Stretch Hamstring Submission Lock (ADR 0201)
                bool suloevLowCrouch = (crouched || deepProne || leftLocal.y < -0.30f);
                bool suloevHookHold = (handsDistSledge < 0.25f && leftLocal.y < -0.35f &&
                                       leftGrip > 0.65f && rightGrip > 0.65f);
                bool suloevArchPull = (lVel.y > 1.05f && rVel.y > 1.05f);

                // Security Guard Stun Baton Forward High-Arc Overhead Vault Drop Strike (ADR 0202)
                bool isLookingDownward = (hmdPitch < -0.436f); // < -25 deg downward look
                bool batonOverheadVault = (rightLocal.y > 0.20f &&
                                           rightGrip > 0.60f &&
                                           rightTrigger > 0.65f);
                bool batonVaultPlunge = (rVel.y < -1.35f);

                // Inmate Pit Brawl Duck-Under Back-Clinch Belly-to-Back Suplex Slam (ADR 0213)
                bool duckUnderLow = (crouched || deepProne || leftLocal.y < -0.25f);
                bool duckUnderRearSlip = (duckUnderLow && handsDistSledge < 0.24f &&
                                          fabsf(rVel.x) > 0.85f &&
                                          leftGrip > 0.65f && rightGrip > 0.65f);
                if (duckUnderRearSlip && (now - st.lastBackSuplexTime > 2000)) {
                    if (st.backSuplexClinchTime == 0) {
                        st.backSuplexClinchTime = now;
                        out.rearWaistClinchLocked = true;
                    }
                }
                bool backSuplexHoistSlam = (st.backSuplexClinchTime > 0 && (now - st.backSuplexClinchTime < 500) &&
                                            lVel.y > 1.20f && rVel.y > 1.20f);

                // Security Guard Stun Baton Charged High-Velocity Forward Radial Thrust Disarm (ADR 0214)
                bool radialDisarmStance = (handsDistSledge < 0.24f &&
                                           leftGrip > 0.65f && rightGrip > 0.65f &&
                                           rightTrigger > 0.65f);
                bool radialDisarmTwist = (radialDisarmStance && fabsf(rVel.x - lVel.x) > 1.15f);
                if (radialDisarmTwist && (now - st.lastRadialDisarmTime > 2000)) {
                    if (st.radialDisarmTwistTime == 0) {
                        st.radialDisarmTwistTime = now;
                        st.radialDisarmDirKey = (rVel.x > 0.0f) ? 'D' : 'A';
                        out.radialDisarmTwisted = true;
                    }
                }
                bool radialDisarmThrust = (st.radialDisarmTwistTime > 0 && (now - st.radialDisarmTwistTime < 500) && rVel.z < -1.35f);

                // Inmate Pit Brawl Clinch Thai Plum Collar Tie & Liver Knee Strike (ADR 0217)
                bool thaiPlumGrip = (handsDistSledge < 0.25f &&
                                     leftLocal.y > -0.10f && rightLocal.y > -0.10f &&
                                     leftGrip > 0.65f && rightGrip > 0.65f);
                if (thaiPlumGrip && (now - st.lastThaiPlumTime > 2000)) {
                    if (st.thaiPlumClinchTime == 0) {
                        st.thaiPlumClinchTime = now;
                        out.thaiPlumLocked = true;
                    }
                }
                bool thaiPlumKneeDrive = (st.thaiPlumClinchTime > 0 && (now - st.thaiPlumClinchTime < 600) &&
                                          lVel.y < -1.10f && rVel.y < -1.10f);

                // Security Guard Stun Baton Forward Two-Handed Javelin Vault Leap Thrust (ADR 0218)
                bool javelinVaultStance = (handsDistSledge < 0.22f &&
                                           leftGrip > 0.65f && rightGrip > 0.65f &&
                                           rightTrigger > 0.65f);
                bool isJumpingMotion = ((rightButtons & RIDDICK_VR_BTN_B) != 0 || rVel.y > 0.85f || lVel.y > 0.85f);
                bool javelinVaultStrike = (javelinVaultStance && isJumpingMotion && rVel.z < -1.40f && lVel.z < -1.20f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Takedown & Arm Trap (ADR 0221)
                bool whizzerClinchGrip = (handsDistSledge < 0.25f &&
                                          rightLocal.y > -0.15f && rightLocal.y < 0.15f &&
                                          rightGrip > 0.65f && leftGrip > 0.65f);
                bool whizzerDownWhip = (whizzerClinchGrip && rVel.y < -1.15f && fabsf(lVel.x) > 0.90f);
                if (whizzerDownWhip && (now - st.lastWhizzerTime > 2000)) {
                    st.whizzerDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Arc Kinetic Hammerfist Deck Strike (ADR 0222)
                bool batonHammerfistElevated = (rightLocal.y > 0.15f &&
                                                rightGrip > 0.65f &&
                                                rightTrigger > 0.65f);
                bool batonHammerfistPlunge = (batonHammerfistElevated && rVel.y < -1.35f && rVel.z < -0.80f);

                // Inmate Pit Brawl Double Underhook Body Lock Foot Sweep & Hip Toss (ADR 0225)
                bool underhookBodyLockGrip = (handsDistSledge < 0.22f &&
                                              leftLocal.y > -0.20f && leftLocal.y < 0.15f &&
                                              rightLocal.y > -0.20f && rightLocal.y < 0.15f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f);
                bool hipTossRotation = (underhookBodyLockGrip && fabsf(rVel.x - lVel.x) > 1.20f &&
                                        (rVel.y > 1.05f || lVel.y > 1.05f));
                if (hipTossRotation && (now - st.lastHipTossTime > 2000)) {
                    st.hipTossDirKey = (rVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Lateral Two-Handed Kinetic Shield Bash & Arc Burst (ADR 0226)
                bool batonShieldBashStance = (handsDistSledge > 0.18f && handsDistSledge < 0.35f &&
                                              fabsf(rightLocal.x - leftLocal.x) > 0.20f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f &&
                                              rightTrigger > 0.65f);
                bool batonShieldBashThrust = (batonShieldBashStance && rVel.z < -1.35f && lVel.z < -1.15f);

                // Inmate Pit Brawl Clinch Thai Plum Elbow Spike & Cranial Fracture (ADR 0229)
                bool thaiPlumElbowStance = (handsDistSledge < 0.25f &&
                                            leftLocal.y > -0.10f && rightLocal.y > 0.15f &&
                                            leftGrip > 0.65f && rightGrip > 0.65f);
                bool thaiPlumElbowPlunge = (thaiPlumElbowStance && rVel.y < -1.30f && lVel.z > 0.60f);
                if (thaiPlumElbowPlunge && (now - st.lastThaiPlumElbowTime > 2000)) {
                    st.thaiPlumElbowDirKey = (rVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Grounding Thrust & Arc Discharge Ring (ADR 0230)
                bool groundingThrustStance = ((leftLocal.y < -0.25f || crouched) &&
                                              handsDistSledge < 0.24f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f &&
                                              rightTrigger > 0.65f);
                bool groundingThrustLunge = (groundingThrustStance && rVel.z < -1.35f && lVel.z < -1.10f);

                // Inmate Pit Brawl Double Underhook Lateral Suplex Arch & Mat Spike (ADR 0233)
                bool lateralSuplexStance = (handsDistSledge < 0.28f &&
                                            leftLocal.y > -0.20f && rightLocal.y > -0.20f &&
                                            leftGrip > 0.65f && rightGrip > 0.65f);
                bool lateralSuplexThrow = (lateralSuplexStance && rVel.y > 0.70f && lVel.y > 0.70f &&
                                           (fabsf(rVel.x) > 0.85f || fabsf(lVel.x) > 0.85f));
                if (lateralSuplexThrow && (now - st.lastLateralSuplexTime > 2000)) {
                    st.lateralSuplexDirKey = (rVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Arc Kinetic Cleave & Shockwave Stun (ADR 0234)
                bool kineticCleaveStance = (handsDistSledge < 0.28f &&
                                            rightLocal.y > 0.18f && leftLocal.y > 0.10f &&
                                            leftGrip > 0.65f && rightGrip > 0.65f &&
                                            rightTrigger > 0.65f);
                bool kineticCleaveSwing = (kineticCleaveStance && rVel.y < -1.40f && lVel.y < -1.10f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Shoulder Dislocation (ADR 0237)
                bool whizzerCounterStance = (handsDistSledge < 0.26f &&
                                             leftLocal.y > -0.15f && leftLocal.y < 0.15f &&
                                             leftGrip > 0.65f && rightGrip > 0.65f);
                bool whizzerCounterThrow = (whizzerCounterStance && rVel.y < -1.25f && fabsf(lVel.x) > 0.95f);
                if (whizzerCounterThrow && (now - st.lastWhizzerCounterTime > 2000)) {
                    st.whizzerCounterDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward Two-Handed Kinetic Cleave & Arc Discharge Blast (ADR 0238)
                bool cleaveArcBlastStance = (handsDistSledge < 0.28f &&
                                             rightLocal.y > 0.16f && leftLocal.y > 0.08f &&
                                             leftGrip > 0.65f && rightGrip > 0.65f &&
                                             rightTrigger > 0.65f);
                bool cleaveArcBlastStrike = (cleaveArcBlastStance && rVel.y < -1.30f && lVel.y < -1.05f && rVel.z < -1.00f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Cervical Compression Mat Spike (ADR 0241)
                bool cervicalSpikeStance = (handsDistSledge < 0.26f &&
                                            leftLocal.y > -0.10f && leftLocal.y < 0.20f &&
                                            leftGrip > 0.65f && rightGrip > 0.65f);
                bool cervicalSpikeThrow = (cervicalSpikeStance && rVel.y < -1.35f &&
                                           lVel.y < -1.05f && fabsf(lVel.x) > 0.85f);
                if (cervicalSpikeThrow && (now - st.lastCervicalSpikeTime > 2000)) {
                    st.cervicalSpikeDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Ground Arc Disruption (ADR 0242)
                bool isJumpingStance = ((rightButtons & RIDDICK_VR_BTN_B) != 0 || rVel.y > 0.85f || lVel.y > 0.85f);
                bool vaultGroundArcStance = (handsDistSledge < 0.24f &&
                                             leftGrip > 0.65f && rightGrip > 0.65f &&
                                             rightTrigger > 0.65f);
                bool vaultGroundArcPlunge = (vaultGroundArcStance && isJumpingStance && rVel.y < -1.40f && lVel.y < -1.15f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Grating Slam (ADR 0245)
                bool thoracicSlamStance = (handsDistSledge < 0.26f &&
                                           leftLocal.y > -0.20f && leftLocal.y < 0.15f &&
                                           leftGrip > 0.65f && rightGrip > 0.65f);
                bool thoracicSlamThrow = (thoracicSlamStance && rVel.y < -1.30f &&
                                          fabsf(lVel.x) > 0.90f && lVel.y > 0.60f);
                if (thoracicSlamThrow && (now - st.lastThoracicSlamTime > 2000)) {
                    st.thoracicSlamDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Kinetic Cleave Shockwave (ADR 0246)
                bool vaultKineticCleaveStance = (handsDistSledge < 0.28f &&
                                                 rightLocal.y > 0.15f &&
                                                 leftGrip > 0.65f && rightGrip > 0.65f &&
                                                 rightTrigger > 0.65f);
                bool vaultKineticCleavePlunge = (vaultKineticCleaveStance && isJumpingStance &&
                                                 rVel.y < -1.45f && lVel.y < -1.15f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Cervical Hyperextension Mat Spike (ADR 0249)
                bool hyperextensionSpikeStance = (handsDistSledge < 0.26f &&
                                                  leftLocal.y > -0.05f &&
                                                  leftGrip > 0.65f && rightGrip > 0.65f);
                bool hyperextensionSpikeThrow = (hyperextensionSpikeStance && rVel.y < -1.40f &&
                                                 lVel.y > 0.85f && fabsf(lVel.x) > 0.90f);
                if (hyperextensionSpikeThrow && (now - st.lastHyperextensionSpikeTime > 2000)) {
                    st.hyperextensionSpikeDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Concussive Deck Impact Shockwave (ADR 0250)
                bool deckImpactStance = (handsDistSledge < 0.28f &&
                                         leftGrip > 0.65f && rightGrip > 0.65f &&
                                         rightTrigger > 0.65f);
                bool deckImpactPlunge = (deckImpactStance && isJumpingStance &&
                                         rVel.y < -1.50f && lVel.y < -1.25f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Cervical Compression Ground Finisher (ADR 0253)
                bool whizzerGroundFinisherStance = (handsDistSledge < 0.26f &&
                                                    leftLocal.y > -0.25f && leftLocal.y < 0.10f &&
                                                    leftGrip > 0.65f && rightGrip > 0.65f);
                bool whizzerGroundFinisherThrow = (whizzerGroundFinisherStance && rVel.y < -1.45f &&
                                                   lVel.y < -1.15f && fabsf(lVel.x) > 0.80f);
                if (whizzerGroundFinisherThrow && (now - st.lastWhizzerGroundFinisherTime > 2000)) {
                    st.whizzerGroundFinisherDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Deck Arc Dispersion (ADR 0254)
                bool omnidirectionalArcStance = (handsDistSledge < 0.28f &&
                                                 leftGrip > 0.65f && rightGrip > 0.65f &&
                                                 rightTrigger > 0.65f);
                bool omnidirectionalArcPlunge = (omnidirectionalArcStance && isJumpingStance &&
                                                 rVel.y < -1.45f && lVel.y < -1.20f &&
                                                 fabsf(rVel.x - lVel.x) > 0.80f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Cervical Compression Ground Dislocation (ADR 0257)
                bool whizzerGroundDislocationStance = (handsDistSledge < 0.26f &&
                                                       leftLocal.y > -0.30f && leftLocal.y < 0.05f &&
                                                       leftGrip > 0.65f && rightGrip > 0.65f);
                bool whizzerGroundDislocationThrow = (whizzerGroundDislocationStance && rVel.y < -1.50f &&
                                                      lVel.y < -1.20f && fabsf(lVel.x) > 0.85f);
                if (whizzerGroundDislocationThrow && (now - st.lastWhizzerGroundDislocationTime > 2000)) {
                    st.whizzerGroundDislocationDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Deck Arc Pulse (ADR 0258)
                bool omnidirectionalPulseStance = (handsDistSledge < 0.28f &&
                                                   leftGrip > 0.65f && rightGrip > 0.65f &&
                                                   rightTrigger > 0.65f);
                bool omnidirectionalPulsePlunge = (omnidirectionalPulseStance && isJumpingStance &&
                                                   rVel.y < -1.55f && lVel.y < -1.30f &&
                                                   fabsf(rVel.x - lVel.x) > 0.90f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Cervical Compression Ground Finish & Sternum Knee Drop (ADR 0261)
                bool sternumKneeDropStance = (handsDistSledge < 0.26f &&
                                              leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f);
                bool sternumKneeDropThrow = (sternumKneeDropStance && rVel.y < -1.55f &&
                                             lVel.y < -1.30f && fabsf(lVel.x) > 0.85f);
                if (sternumKneeDropThrow && (now - st.lastSternumKneeDropTime > 2000)) {
                    st.sternumKneeDropDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Pulse Discharge Wave (ADR 0262)
                bool kineticPulseWaveStance = (handsDistSledge < 0.28f &&
                                               leftGrip > 0.65f && rightGrip > 0.65f &&
                                               rightTrigger > 0.65f);
                bool kineticPulseWavePlunge = (kineticPulseWaveStance && isJumpingStance &&
                                               rVel.y < -1.60f && lVel.y < -1.35f &&
                                               fabsf(rVel.x - lVel.x) > 0.95f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Sternum Bone Fracture Mat Spike (ADR 0265)
                bool sternumFractureSpikeStance = (handsDistSledge < 0.26f &&
                                                   leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                   leftGrip > 0.65f && rightGrip > 0.65f);
                bool sternumFractureSpikeThrow = (sternumFractureSpikeStance && rVel.y < -1.60f &&
                                                  lVel.y < -1.35f && fabsf(lVel.x) > 0.85f);
                if (sternumFractureSpikeThrow && (now - st.lastSternumFractureSpikeTime > 2000)) {
                    st.sternumFractureSpikeDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Cleave Wave (ADR 0266)
                bool plasmaCleaveWaveStance = (handsDistSledge < 0.28f &&
                                               leftGrip > 0.65f && rightGrip > 0.65f &&
                                               rightTrigger > 0.65f);
                bool plasmaCleaveWavePlunge = (plasmaCleaveWaveStance && isJumpingStance &&
                                               rVel.y < -1.65f && lVel.y < -1.40f &&
                                               fabsf(rVel.x - lVel.x) > 1.00f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Sternum Bone Fracture Thoracic Grating Slam (ADR 0269)
                bool sternumGratingSlamStance = (handsDistSledge < 0.26f &&
                                                 leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                 leftGrip > 0.65f && rightGrip > 0.65f);
                bool sternumGratingSlamThrow = (sternumGratingSlamStance && rVel.y < -1.65f &&
                                                lVel.y < -1.40f && fabsf(lVel.x) > 0.85f);
                if (sternumGratingSlamThrow && (now - st.lastSternumGratingSlamTime > 2000)) {
                    st.sternumGratingSlamDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Shockwave Overload (ADR 0270)
                bool plasmaOverloadWaveStance = (handsDistSledge < 0.28f &&
                                                 leftGrip > 0.65f && rightGrip > 0.65f &&
                                                 rightTrigger > 0.65f);
                bool plasmaOverloadWavePlunge = (plasmaOverloadWaveStance && isJumpingStance &&
                                                 rVel.y < -1.70f && lVel.y < -1.45f &&
                                                 fabsf(rVel.x - lVel.x) > 1.05f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Cervical Compression Grating Fracture Slam (ADR 0273)
                bool cervicalGratingSlamStance = (handsDistSledge < 0.26f &&
                                                  leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                  leftGrip > 0.65f && rightGrip > 0.65f);
                bool cervicalGratingSlamThrow = (cervicalGratingSlamStance && rVel.y < -1.70f &&
                                                 lVel.y < -1.45f && fabsf(lVel.x) > 0.85f);
                if (cervicalGratingSlamThrow && (now - st.lastCervicalGratingSlamTime > 2000)) {
                    st.cervicalGratingSlamDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Detonation (ADR 0274)
                bool plasmaResonantWaveStance = (handsDistSledge < 0.28f &&
                                                 leftGrip > 0.65f && rightGrip > 0.65f &&
                                                 rightTrigger > 0.65f);
                bool plasmaResonantWavePlunge = (plasmaResonantWaveStance && isJumpingStance &&
                                                 rVel.y < -1.75f && lVel.y < -1.50f &&
                                                 fabsf(rVel.x - lVel.x) > 1.10f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Cervical Compression Grating Hyperextension Finisher (ADR 0277)
                bool hyperextensionFinisherStance = (handsDistSledge < 0.26f &&
                                                     leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                     leftGrip > 0.65f && rightGrip > 0.65f);
                bool hyperextensionFinisherThrow = (hyperextensionFinisherStance && rVel.y < -1.75f &&
                                                    lVel.y < -1.50f && fabsf(lVel.x) > 0.85f);
                if (hyperextensionFinisherThrow && (now - st.lastHyperextensionFinisherTime > 2000)) {
                    st.hyperextensionFinisherDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Harmonic Deck Quake (ADR 0278)
                bool plasmaHarmonicQuakeStance = (handsDistSledge < 0.28f &&
                                                  leftGrip > 0.65f && rightGrip > 0.65f &&
                                                  rightTrigger > 0.65f);
                bool plasmaHarmonicQuakePlunge = (plasmaHarmonicQuakeStance && isJumpingStance &&
                                                  rVel.y < -1.80f && lVel.y < -1.55f &&
                                                  fabsf(rVel.x - lVel.x) > 1.15f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Mat Dislocation (ADR 0281)
                bool thoracicDislocationStance = (handsDistSledge < 0.26f &&
                                                  leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                  leftGrip > 0.65f && rightGrip > 0.65f);
                bool thoracicDislocationThrow = (thoracicDislocationStance && rVel.y < -1.80f &&
                                                 lVel.y < -1.55f && fabsf(lVel.x) > 0.85f);
                if (thoracicDislocationThrow && (now - st.lastThoracicDislocationTime > 2000)) {
                    st.thoracicDislocationDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Seism Shockwave (ADR 0282)
                bool plasmaResonantSeismStance = (handsDistSledge < 0.28f &&
                                                  leftGrip > 0.65f && rightGrip > 0.65f &&
                                                  rightTrigger > 0.65f);
                bool plasmaResonantSeismPlunge = (plasmaResonantSeismStance && isJumpingStance &&
                                                  rVel.y < -1.85f && lVel.y < -1.60f &&
                                                  fabsf(rVel.x - lVel.x) > 1.20f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Mat Slam Finisher (ADR 0285)
                bool thoracicMatSlamStance = (handsDistSledge < 0.26f &&
                                              leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f);
                bool thoracicMatSlamThrow = (thoracicMatSlamStance && rVel.y < -1.85f &&
                                             lVel.y < -1.60f && fabsf(lVel.x) > 0.85f);
                if (thoracicMatSlamThrow && (now - st.lastThoracicMatSlamTime > 2000)) {
                    st.thoracicMatSlamDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Seism Overdrive (ADR 0286)
                bool plasmaSeismOverdriveStance = (handsDistSledge < 0.28f &&
                                                   leftGrip > 0.65f && rightGrip > 0.65f &&
                                                   rightTrigger > 0.65f);
                bool plasmaSeismOverdrivePlunge = (plasmaSeismOverdriveStance && isJumpingStance &&
                                                   rVel.y < -1.90f && lVel.y < -1.65f &&
                                                   fabsf(rVel.x - lVel.x) > 1.25f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Ground Pin Lock (ADR 0289)
                bool thoracicPinLockStance = (handsDistSledge < 0.26f &&
                                              leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f);
                bool thoracicPinLockThrow = (thoracicPinLockStance && rVel.y < -1.90f &&
                                             lVel.y < -1.65f && fabsf(lVel.x) > 0.85f);
                if (thoracicPinLockThrow && (now - st.lastThoracicPinLockTime > 2000)) {
                    st.thoracicPinLockDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Shockwave (ADR 0290)
                bool plasmaCataclysmStance = (handsDistSledge < 0.28f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f &&
                                              rightTrigger > 0.65f);
                bool plasmaCataclysmPlunge = (plasmaCataclysmStance && isJumpingStance &&
                                              rVel.y < -1.95f && lVel.y < -1.70f &&
                                              fabsf(rVel.x - lVel.x) > 1.30f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Sternum Elbow Drop (ADR 0293)
                bool sternumElbowDropStance = (handsDistSledge < 0.26f &&
                                               leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                               leftGrip > 0.65f && rightGrip > 0.65f);
                bool sternumElbowDropPlunge = (sternumElbowDropStance && rVel.y < -1.95f &&
                                               lVel.y < -1.70f && fabsf(lVel.x) > 0.90f);
                if (sternumElbowDropPlunge && (now - st.lastSternumElbowDropTime > 2000)) {
                    st.sternumElbowDropDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Overdrive (ADR 0294)
                bool plasmaCataclysmOverdriveStance = (handsDistSledge < 0.28f &&
                                                      leftGrip > 0.65f && rightGrip > 0.65f &&
                                                      rightTrigger > 0.65f);
                bool plasmaCataclysmOverdrivePlunge = (plasmaCataclysmOverdriveStance && isJumpingStance &&
                                                      rVel.y < -2.00f && lVel.y < -1.75f &&
                                                      fabsf(rVel.x - lVel.x) > 1.35f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Cranial Deck Driver (ADR 0297)
                bool cranialDeckDriverStance = (handsDistSledge < 0.26f &&
                                                leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialDeckDriverPlunge = (cranialDeckDriverStance && rVel.y < -2.00f &&
                                                lVel.y < -1.75f && fabsf(lVel.x) > 0.90f);
                if (cranialDeckDriverPlunge && (now - st.lastCranialDeckDriverTime > 2000)) {
                    st.cranialDeckDriverDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Singularity (ADR 0298)
                bool plasmaSingularityStance = (handsDistSledge < 0.28f &&
                                                leftGrip > 0.65f && rightGrip > 0.65f &&
                                                rightTrigger > 0.65f);
                bool plasmaSingularityPlunge = (plasmaSingularityStance && isJumpingStance &&
                                                rVel.y < -2.05f && lVel.y < -1.80f &&
                                                fabsf(rVel.x - lVel.x) > 1.40f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Sternum Knee Spike Finisher (ADR 0301)
                bool sternumKneeSpikeStance = (handsDistSledge < 0.26f &&
                                               leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                               leftGrip > 0.65f && rightGrip > 0.65f);
                bool sternumKneeSpikePlunge = (sternumKneeSpikeStance && rVel.y < -2.05f &&
                                               lVel.y < -1.80f && fabsf(lVel.x) > 0.90f);
                if (sternumKneeSpikePlunge && (now - st.lastSternumKneeSpikeTime > 2000)) {
                    st.sternumKneeSpikeDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Supernova (ADR 0302)
                bool plasmaSupernovaStance = (handsDistSledge < 0.28f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f &&
                                              rightTrigger > 0.65f);
                bool plasmaSupernovaPlunge = (plasmaSupernovaStance && isJumpingStance &&
                                              rVel.y < -2.10f && lVel.y < -1.85f &&
                                              fabsf(rVel.x - lVel.x) > 1.45f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Cervical Deck Pin Spike (ADR 0305)
                bool cervicalDeckPinStance = (handsDistSledge < 0.26f &&
                                              leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f);
                bool cervicalDeckPinPlunge = (cervicalDeckPinStance && rVel.y < -2.10f &&
                                              lVel.y < -1.85f && fabsf(lVel.x) > 0.90f);
                if (cervicalDeckPinPlunge && (now - st.lastCervicalDeckPinTime > 2000)) {
                    st.cervicalDeckPinDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Hypernova (ADR 0306)
                bool plasmaHypernovaStance = (handsDistSledge < 0.28f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f &&
                                              rightTrigger > 0.65f);
                bool plasmaHypernovaPlunge = (plasmaHypernovaStance && isJumpingStance &&
                                              rVel.y < -2.15f && lVel.y < -1.90f &&
                                              fabsf(rVel.x - lVel.x) > 1.50f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Cranial Grating Grind (ADR 0309)
                bool cranialGrindStance = (handsDistSledge < 0.26f &&
                                           leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                           leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialGrindPlunge = (cranialGrindStance && rVel.y < -2.15f &&
                                           lVel.y < -1.90f && fabsf(lVel.x) > 0.90f);
                if (cranialGrindPlunge && (now - st.lastCranialGrindTime > 2000)) {
                    st.cranialGrindDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Singularity Overdrive (ADR 0310)
                bool plasmaSingularityOverdriveStance = (handsDistSledge < 0.28f &&
                                                        leftGrip > 0.65f && rightGrip > 0.65f &&
                                                        rightTrigger > 0.65f);
                bool plasmaSingularityOverdrivePlunge = (plasmaSingularityOverdriveStance && isJumpingStance &&
                                                        rVel.y < -2.20f && lVel.y < -1.95f &&
                                                        fabsf(rVel.x - lVel.x) > 1.55f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Cranial Post-Impactor Slam (ADR 0313)
                bool cranialPostImpactStance = (handsDistSledge < 0.26f &&
                                                leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialPostImpactPlunge = (cranialPostImpactStance && rVel.y < -2.20f &&
                                                lVel.y < -1.95f && fabsf(lVel.x) > 0.95f);
                if (cranialPostImpactPlunge && (now - st.lastCranialPostImpactTime > 2000)) {
                    st.cranialPostImpactDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Genesis Overdrive (ADR 0314)
                bool plasmaGenesisStance = (handsDistSledge < 0.28f &&
                                            leftGrip > 0.65f && rightGrip > 0.65f &&
                                            rightTrigger > 0.65f);
                bool plasmaGenesisPlunge = (plasmaGenesisStance && isJumpingStance &&
                                            rVel.y < -2.25f && lVel.y < -2.00f &&
                                            fabsf(rVel.x - lVel.x) > 1.60f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Cranial Deck Grating Stomp Finisher (ADR 0317)
                bool cranialStompStance = (handsDistSledge < 0.26f &&
                                           leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                           leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialStompPlunge = (cranialStompStance && rVel.y < -2.25f &&
                                           lVel.y < -2.00f && fabsf(lVel.x) > 0.95f);
                if (cranialStompPlunge && (now - st.lastCranialStompTime > 2000)) {
                    st.cranialStompDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Big Bang Overdrive (ADR 0318)
                bool plasmaBigBangStance = (handsDistSledge < 0.28f &&
                                            leftGrip > 0.65f && rightGrip > 0.65f &&
                                            rightTrigger > 0.65f);
                bool plasmaBigBangPlunge = (plasmaBigBangStance && isJumpingStance &&
                                            rVel.y < -2.30f && lVel.y < -2.05f &&
                                            fabsf(rVel.x - lVel.x) > 1.65f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Cranial Post Double-Strike Execution (ADR 0321)
                bool cranialDoubleStrikeStance = (handsDistSledge < 0.26f &&
                                                  leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                  leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialDoubleStrikePlunge = (cranialDoubleStrikeStance && rVel.y < -2.30f &&
                                                  lVel.y < -2.05f && fabsf(lVel.x) > 0.95f);
                if (cranialDoubleStrikePlunge && (now - st.lastCranialDoubleStrikeTime > 2000)) {
                    st.cranialDoubleStrikeDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Cosmic Dawn Overdrive (ADR 0322)
                bool plasmaCosmicDawnStance = (handsDistSledge < 0.28f &&
                                               leftGrip > 0.65f && rightGrip > 0.65f &&
                                               rightTrigger > 0.65f);
                bool plasmaCosmicDawnPlunge = (plasmaCosmicDawnStance && isJumpingStance &&
                                               rVel.y < -2.35f && lVel.y < -2.10f &&
                                               fabsf(rVel.x - lVel.x) > 1.70f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Cranial Post Pivot-Twist Fracture (ADR 0325)
                bool cranialPivotTwistStance = (handsDistSledge < 0.26f &&
                                                leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialPivotTwistPlunge = (cranialPivotTwistStance && rVel.y < -2.35f &&
                                                lVel.y < -2.10f && fabsf(lVel.x) > 1.00f);
                if (cranialPivotTwistPlunge && (now - st.lastCranialPivotTwistTime > 2000)) {
                    st.cranialPivotTwistDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Supercluster Overdrive (ADR 0326)
                bool plasmaSuperclusterStance = (handsDistSledge < 0.28f &&
                                                 leftGrip > 0.65f && rightGrip > 0.65f &&
                                                 rightTrigger > 0.65f);
                bool plasmaSuperclusterPlunge = (plasmaSuperclusterStance && isJumpingStance &&
                                                 rVel.y < -2.40f && lVel.y < -2.15f &&
                                                 fabsf(rVel.x - lVel.x) > 1.75f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Hyperextension Cranial Post Pivot-Twist Deck Slam (ADR 0329)
                bool cranialPivotSlamStance = (handsDistSledge < 0.26f &&
                                               leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                               leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialPivotSlamPlunge = (cranialPivotSlamStance && rVel.y < -2.40f &&
                                               lVel.y < -2.15f && fabsf(lVel.x) > 1.00f);
                if (cranialPivotSlamPlunge && (now - st.lastCranialPivotSlamTime > 2000)) {
                    st.cranialPivotSlamDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Omniverse Overdrive (ADR 0330)
                bool plasmaOmniverseStance = (handsDistSledge < 0.28f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f &&
                                              rightTrigger > 0.65f);
                bool plasmaOmniversePlunge = (plasmaOmniverseStance && isJumpingStance &&
                                              rVel.y < -2.45f && lVel.y < -2.20f &&
                                              fabsf(rVel.x - lVel.x) > 1.80f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Cranial Post Lever-Pin Lock Takedown (ADR 0333)
                bool cranialLeverPinStance = (handsDistSledge < 0.26f &&
                                              leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialLeverPinPlunge = (cranialLeverPinStance && rVel.y < -2.45f &&
                                              lVel.y < -2.20f && fabsf(lVel.x) > 1.05f);
                if (cranialLeverPinPlunge && (now - st.lastCranialLeverPinTime > 2000)) {
                    st.cranialLeverPinDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Metaverse Overdrive (ADR 0334)
                bool plasmaMetaverseStance = (handsDistSledge < 0.28f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f &&
                                              rightTrigger > 0.65f);
                bool plasmaMetaversePlunge = (plasmaMetaverseStance && isJumpingStance &&
                                              rVel.y < -2.50f && lVel.y < -2.25f &&
                                              fabsf(rVel.x - lVel.x) > 1.85f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Cranial Post Fulcrum Leverage Chokehold Finisher (ADR 0337)
                bool cranialFulcrumChokeStance = (handsDistSledge < 0.26f &&
                                                  leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                  leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialFulcrumChokePlunge = (cranialFulcrumChokeStance && rVel.y < -2.50f &&
                                                  lVel.y < -2.25f && fabsf(lVel.x) > 1.10f);
                if (cranialFulcrumChokePlunge && (now - st.lastCranialFulcrumChokeTime > 2000)) {
                    st.cranialFulcrumChokeDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Hyperdimensional Overdrive (ADR 0338)
                bool plasmaHyperdimensionalStance = (handsDistSledge < 0.28f &&
                                                     leftGrip > 0.65f && rightGrip > 0.65f &&
                                                     rightTrigger > 0.65f);
                bool plasmaHyperdimensionalPlunge = (plasmaHyperdimensionalStance && isJumpingStance &&
                                                     rVel.y < -2.55f && lVel.y < -2.30f &&
                                                     fabsf(rVel.x - lVel.x) > 1.90f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Cranial Post Fulcrum Leverage Armbar Fracture (ADR 0341)
                bool cranialFulcrumArmbarStance = (handsDistSledge < 0.26f &&
                                                   leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                   leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialFulcrumArmbarPlunge = (cranialFulcrumArmbarStance && rVel.y < -2.55f &&
                                                   lVel.y < -2.30f && fabsf(lVel.x) > 1.15f);
                if (cranialFulcrumArmbarPlunge && (now - st.lastCranialFulcrumArmbarTime > 2000)) {
                    st.cranialFulcrumArmbarDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Transdimensional Overdrive (ADR 0342)
                bool plasmaTransdimensionalStance = (handsDistSledge < 0.28f &&
                                                     leftGrip > 0.65f && rightGrip > 0.65f &&
                                                     rightTrigger > 0.65f);
                bool plasmaTransdimensionalPlunge = (plasmaTransdimensionalStance && isJumpingStance &&
                                                     rVel.y < -2.60f && lVel.y < -2.35f &&
                                                     fabsf(rVel.x - lVel.x) > 1.95f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Cranial Post Fulcrum Leverage Sternum Knee Drive (ADR 0345)
                bool cranialFulcrumKneeStance = (handsDistSledge < 0.26f &&
                                                 leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                 leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialFulcrumKneePlunge = (cranialFulcrumKneeStance && rVel.y < -2.60f &&
                                                 lVel.y < -2.35f && fabsf(lVel.x) > 1.20f);
                if (cranialFulcrumKneePlunge && (now - st.lastCranialFulcrumKneeTime > 2000)) {
                    st.cranialFulcrumKneeDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Interdimensional Overdrive (ADR 0346)
                bool plasmaInterdimensionalStance = (handsDistSledge < 0.28f &&
                                                     leftGrip > 0.65f && rightGrip > 0.65f &&
                                                     rightTrigger > 0.65f);
                bool plasmaInterdimensionalPlunge = (plasmaInterdimensionalStance && isJumpingStance &&
                                                     rVel.y < -2.65f && lVel.y < -2.40f &&
                                                     fabsf(rVel.x - lVel.x) > 2.00f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Cranial Post Fulcrum Leverage Ribcage Fracture Deck Slam (ADR 0349)
                bool cranialFulcrumRibSlamStance = (handsDistSledge < 0.26f &&
                                                    leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                    leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialFulcrumRibSlamPlunge = (cranialFulcrumRibSlamStance && rVel.y < -2.65f &&
                                                    lVel.y < -2.40f && fabsf(lVel.x) > 1.25f);
                if (cranialFulcrumRibSlamPlunge && (now - st.lastCranialFulcrumRibSlamTime > 2000)) {
                    st.cranialFulcrumRibSlamDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Continuum Singularity Overdrive (ADR 0350)
                bool plasmaContinuumSingularityStance = (handsDistSledge < 0.28f &&
                                                         leftGrip > 0.65f && rightGrip > 0.65f &&
                                                         rightTrigger > 0.65f);
                bool plasmaContinuumSingularityPlunge = (plasmaContinuumSingularityStance && isJumpingStance &&
                                                         rVel.y < -2.70f && lVel.y < -2.45f &&
                                                         fabsf(rVel.x - lVel.x) > 2.05f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Cranial Post Fulcrum Leverage Clavicle Fracture Ground Pin (ADR 0353)
                bool cranialFulcrumClaviclePinStance = (handsDistSledge < 0.26f &&
                                                        leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                        leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialFulcrumClaviclePinPlunge = (cranialFulcrumClaviclePinStance && rVel.y < -2.70f &&
                                                        lVel.y < -2.45f && fabsf(lVel.x) > 1.30f);
                if (cranialFulcrumClaviclePinPlunge && (now - st.lastCranialFulcrumClaviclePinTime > 2000)) {
                    st.cranialFulcrumClaviclePinDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Multiverse Singularity Overdrive (ADR 0354)
                bool plasmaMultiverseSingularityStance = (handsDistSledge < 0.28f &&
                                                          leftGrip > 0.65f && rightGrip > 0.65f &&
                                                          rightTrigger > 0.65f);
                bool plasmaMultiverseSingularityPlunge = (plasmaMultiverseSingularityStance && isJumpingStance &&
                                                          rVel.y < -2.75f && lVel.y < -2.50f &&
                                                          fabsf(rVel.x - lVel.x) > 2.10f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Cranial Post Fulcrum Leverage Scapula Fracture Deck Pin (ADR 0357)
                bool cranialFulcrumScapulaPinStance = (handsDistSledge < 0.26f &&
                                                        leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                        leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialFulcrumScapulaPinPlunge = (cranialFulcrumScapulaPinStance && rVel.y < -2.75f &&
                                                        lVel.y < -2.50f && fabsf(lVel.x) > 1.35f);
                if (cranialFulcrumScapulaPinPlunge && (now - st.lastCranialFulcrumScapulaPinTime > 2000)) {
                    st.cranialFulcrumScapulaPinDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Omniverse Singularity Overdrive (ADR 0358)
                bool plasmaOmniverseSingularityStance = (handsDistSledge < 0.28f &&
                                                         leftGrip > 0.65f && rightGrip > 0.65f &&
                                                         rightTrigger > 0.65f);
                bool plasmaOmniverseSingularityPlunge = (plasmaOmniverseSingularityStance && isJumpingStance &&
                                                         rVel.y < -2.80f && lVel.y < -2.55f &&
                                                         fabsf(rVel.x - lVel.x) > 2.15f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Cranial Post Fulcrum Leverage Sternum Crushing Ground Pin (ADR 0361)
                bool cranialFulcrumSternumPinStance = (handsDistSledge < 0.26f &&
                                                        leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                        leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialFulcrumSternumPinPlunge = (cranialFulcrumSternumPinStance && rVel.y < -2.80f &&
                                                        lVel.y < -2.55f && fabsf(lVel.x) > 1.40f);
                if (cranialFulcrumSternumPinPlunge && (now - st.lastCranialFulcrumSternumPinTime > 2000)) {
                    st.cranialFulcrumSternumPinDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Trans-Cosmic Singularity Overdrive (ADR 0362)
                bool plasmaTransCosmicSingularityStance = (handsDistSledge < 0.28f &&
                                                           leftGrip > 0.65f && rightGrip > 0.65f &&
                                                           rightTrigger > 0.65f);
                bool plasmaTransCosmicSingularityPlunge = (plasmaTransCosmicSingularityStance && isJumpingStance &&
                                                           rVel.y < -2.85f && lVel.y < -2.60f &&
                                                           fabsf(rVel.x - lVel.x) > 2.20f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Cranial Post Fulcrum Leverage Mandible Dislocation Ground Pin (ADR 0365)
                bool cranialFulcrumMandiblePinStance = (handsDistSledge < 0.26f &&
                                                         leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                         leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialFulcrumMandiblePinPlunge = (cranialFulcrumMandiblePinStance && rVel.y < -2.85f &&
                                                         lVel.y < -2.60f && fabsf(lVel.x) > 1.45f);
                if (cranialFulcrumMandiblePinPlunge && (now - st.lastCranialFulcrumMandiblePinTime > 2000)) {
                    st.cranialFulcrumMandiblePinDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Macrocosm Singularity Overdrive (ADR 0366)
                bool plasmaMacrocosmSingularityStance = (handsDistSledge < 0.28f &&
                                                           leftGrip > 0.65f && rightGrip > 0.65f &&
                                                           rightTrigger > 0.65f);
                bool plasmaMacrocosmSingularityPlunge = (plasmaMacrocosmSingularityStance && isJumpingStance &&
                                                           rVel.y < -2.90f && lVel.y < -2.65f &&
                                                           fabsf(rVel.x - lVel.x) > 2.25f);

                // Inmate Pit Brawl Double Collar Tie Whizzer Counter-Throw & Thoracic Spine Cranial Post Fulcrum Leverage Zygomatic Arch Fracture Ground Pin (ADR 0369)
                bool cranialFulcrumZygomaticPinStance = (handsDistSledge < 0.26f &&
                                                         leftLocal.y > -0.35f && leftLocal.y < 0.05f &&
                                                         leftGrip > 0.65f && rightGrip > 0.65f);
                bool cranialFulcrumZygomaticPinPlunge = (cranialFulcrumZygomaticPinStance && rVel.y < -2.90f &&
                                                         lVel.y < -2.65f && fabsf(lVel.x) > 1.50f);
                if (cranialFulcrumZygomaticPinPlunge && (now - st.lastCranialFulcrumZygomaticPinTime > 2000)) {
                    st.cranialFulcrumZygomaticPinDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                }

                // Security Guard Stun Baton Forward High-Voltage Vault Jump Thrust & Omnidirectional Kinetic Plasma Resonant Cataclysm Universal Singularity Overdrive (ADR 0370)
                bool plasmaUniversalSingularityStance = (handsDistSledge < 0.28f &&
                                                           leftGrip > 0.65f && rightGrip > 0.65f &&
                                                           rightTrigger > 0.65f);
                bool plasmaUniversalSingularityPlunge = (plasmaUniversalSingularityStance && isJumpingStance &&
                                                           rVel.y < -2.95f && lVel.y < -2.70f &&
                                                           fabsf(rVel.x - lVel.x) > 2.30f);

                // Inmate Pit Brawl Outside Leg Hook Sweep & Ground Knee Drop (ADR 0209)
                bool outsideLegSweepHook = (leftLocal.y < -0.30f && fabsf(lVel.x) > 0.90f);
                if (outsideLegSweepHook && (now - st.lastOutsideLegSweepTime > 2000)) {
                    if (st.outsideLegSweepHookTime == 0) {
                        st.outsideLegSweepHookTime = now;
                        st.outsideLegSweepDirKey = (lVel.x > 0.0f) ? 'D' : 'A';
                        out.outsideLegHooked = true;
                    }
                }
                bool outsideLegKneeDrop = (st.outsideLegSweepHookTime > 0 && (now - st.outsideLegSweepHookTime < 500) && rVel.y < -1.25f);
                if (st.outsideLegSweepHookTime != 0 && (now - st.outsideLegSweepHookTime >= 500)) st.outsideLegSweepHookTime = 0; // GH #25: timeout reset

                // Security Guard Stun Baton Forward Two-Handed Cross-Chest Block & Arc Thrust Disruption (ADR 0198 & 0210)
                bool crossChestGuardStance = (handsDistSledge > 0.20f && handsDistSledge < 0.38f &&
                                              fabsf(rightLocal.x - leftLocal.x) > 0.22f &&
                                              rightLocal.y > -0.15f && rightLocal.y < 0.15f &&
                                              rightLocal.z > -0.30f && rightLocal.z < -0.10f &&
                                              leftGrip > 0.65f && rightGrip > 0.65f &&
                                              leftTrigger > 0.65f && rightTrigger > 0.65f);
                bool isCrossThrustReady = false;
                bool isCrossThrustOvercharged = false;
                if (crossChestGuardStance) {
                    if (st.crossBlockHoldStart == 0) st.crossBlockHoldStart = now;
                    DWORD crossHoldDuration = now - st.crossBlockHoldStart;
                    if (crossHoldDuration >= 500) {
                        out.crossThrustSupercharged = true;
                        if (rVel.z < -1.30f && lVel.z < -1.10f) {
                            isCrossThrustOvercharged = true;
                        }
                    } else if (crossHoldDuration >= 300) {
                        out.crossThrustBarrierHum = true;
                        if (rVel.z < -1.25f && lVel.z < -1.05f) {
                            isCrossThrustReady = true;
                        }
                    }
                } else {
                    st.crossBlockHoldStart = 0;
                }
    out.mechPowerPunch = mechPowerPunch;
    out.droneEmpSnap = droneEmpSnap;
    out.dualShieldBash = dualShieldBash;
    out.dualShieldPlant = dualShieldPlant;
    out.dualStoolHeavySmash = dualStoolHeavySmash;
    out.dualPalmShove = dualPalmShove;
    out.isLookingDown = isLookingDown;
    out.isDownwardPlunge = isDownwardPlunge;
    out.dualOverheadSlam = dualOverheadSlam;
    out.leftTrayBatter = leftTrayBatter;
    out.twoHandedBatonSlam = twoHandedBatonSlam;
    out.dualSledgeOverhead = dualSledgeOverhead;
    out.isDischargingArcWave = isDischargingArcWave;
    out.stealthCarotidSlash = stealthCarotidSlash;
    out.isRibUnderhook = isRibUnderhook;
    out.dartCounterJabStrike = dartCounterJabStrike;
    out.isDoubleEarClap = isDoubleEarClap;
    out.isBatonRibJab = isBatonRibJab;
    out.isParrySnap = isParrySnap;
    out.blindRiposteStrike = blindRiposteStrike;
    out.isKneeStomp = isKneeStomp;
    out.isInductionOverheated = isInductionOverheated;
    out.sternumElbowSmash = sternumElbowSmash;
    out.isBatonChargedSpear = isBatonChargedSpear;
    out.isClinchHeadbutt = isClinchHeadbutt;
    out.isBatonGroundPin = isBatonGroundPin;
    out.isLeadKneeThrust = isLeadKneeThrust;
    out.isCrossChestArcWall = isCrossChestArcWall;
    out.isDuckUnderKidneyCounter = isDuckUnderKidneyCounter;
    out.isBatonArcTrapArmed = isBatonArcTrapArmed;
    out.cervicalSnapWhip = cervicalSnapWhip;
    out.isBatonGroundDischarge = isBatonGroundDischarge;
    out.guillotineWrapStrike = guillotineWrapStrike;
    out.isBatonSkullBreaker = isBatonSkullBreaker;
    out.isBatonLateralDisarm = isBatonLateralDisarm;
    out.hammerfistStrike = hammerfistStrike;
    out.suplexSpineSlam = suplexSpineSlam;
    out.isCrossArcBurstReady = isCrossArcBurstReady;
    out.isBatonJavelinThrust = isBatonJavelinThrust;
    out.isOutsideTripMount = isOutsideTripMount;
    out.isKimuraFracture = isKimuraFracture;
    out.isArmTriangleReady = isArmTriangleReady;
    out.isBatonDiagonalParry = isBatonDiagonalParry;
    out.isBatonRadialSweep = isBatonRadialSweep;
    out.isSingleLegTakedown = isSingleLegTakedown;
    out.batonTripSweep = batonTripSweep;
    out.doubleLegSpikeSlam = doubleLegSpikeSlam;
    out.batonThroatThrustStrike = batonThroatThrustStrike;
    out.firemansWhipThrow = firemansWhipThrow;
    out.frontHeadlockWrap = frontHeadlockWrap;
    out.frontHeadlockArchTorque = frontHeadlockArchTorque;
    out.crossParryRiposte = crossParryRiposte;
    out.suloevLowCrouch = suloevLowCrouch;
    out.suloevHookHold = suloevHookHold;
    out.suloevArchPull = suloevArchPull;
    out.isLookingDownward = isLookingDownward;
    out.batonOverheadVault = batonOverheadVault;
    out.batonVaultPlunge = batonVaultPlunge;
    out.backSuplexHoistSlam = backSuplexHoistSlam;
    out.radialDisarmThrust = radialDisarmThrust;
    out.thaiPlumKneeDrive = thaiPlumKneeDrive;
    out.javelinVaultStrike = javelinVaultStrike;
    out.whizzerDownWhip = whizzerDownWhip;
    out.batonHammerfistPlunge = batonHammerfistPlunge;
    out.hipTossRotation = hipTossRotation;
    out.batonShieldBashThrust = batonShieldBashThrust;
    out.thaiPlumElbowPlunge = thaiPlumElbowPlunge;
    out.groundingThrustLunge = groundingThrustLunge;
    out.lateralSuplexThrow = lateralSuplexThrow;
    out.kineticCleaveSwing = kineticCleaveSwing;
    out.whizzerCounterThrow = whizzerCounterThrow;
    out.cleaveArcBlastStrike = cleaveArcBlastStrike;
    out.cervicalSpikeThrow = cervicalSpikeThrow;
    out.vaultGroundArcPlunge = vaultGroundArcPlunge;
    out.thoracicSlamThrow = thoracicSlamThrow;
    out.vaultKineticCleavePlunge = vaultKineticCleavePlunge;
    out.hyperextensionSpikeThrow = hyperextensionSpikeThrow;
    out.deckImpactPlunge = deckImpactPlunge;
    out.whizzerGroundFinisherThrow = whizzerGroundFinisherThrow;
    out.omnidirectionalArcPlunge = omnidirectionalArcPlunge;
    out.whizzerGroundDislocationThrow = whizzerGroundDislocationThrow;
    out.omnidirectionalPulsePlunge = omnidirectionalPulsePlunge;
    out.sternumKneeDropThrow = sternumKneeDropThrow;
    out.kineticPulseWavePlunge = kineticPulseWavePlunge;
    out.sternumFractureSpikeThrow = sternumFractureSpikeThrow;
    out.plasmaCleaveWavePlunge = plasmaCleaveWavePlunge;
    out.sternumGratingSlamThrow = sternumGratingSlamThrow;
    out.plasmaOverloadWavePlunge = plasmaOverloadWavePlunge;
    out.cervicalGratingSlamThrow = cervicalGratingSlamThrow;
    out.plasmaResonantWavePlunge = plasmaResonantWavePlunge;
    out.hyperextensionFinisherThrow = hyperextensionFinisherThrow;
    out.plasmaHarmonicQuakePlunge = plasmaHarmonicQuakePlunge;
    out.thoracicDislocationThrow = thoracicDislocationThrow;
    out.plasmaResonantSeismPlunge = plasmaResonantSeismPlunge;
    out.thoracicMatSlamThrow = thoracicMatSlamThrow;
    out.plasmaSeismOverdrivePlunge = plasmaSeismOverdrivePlunge;
    out.thoracicPinLockThrow = thoracicPinLockThrow;
    out.plasmaCataclysmPlunge = plasmaCataclysmPlunge;
    out.sternumElbowDropPlunge = sternumElbowDropPlunge;
    out.plasmaCataclysmOverdrivePlunge = plasmaCataclysmOverdrivePlunge;
    out.cranialDeckDriverPlunge = cranialDeckDriverPlunge;
    out.plasmaSingularityPlunge = plasmaSingularityPlunge;
    out.sternumKneeSpikePlunge = sternumKneeSpikePlunge;
    out.plasmaSupernovaPlunge = plasmaSupernovaPlunge;
    out.cervicalDeckPinPlunge = cervicalDeckPinPlunge;
    out.plasmaHypernovaPlunge = plasmaHypernovaPlunge;
    out.cranialGrindPlunge = cranialGrindPlunge;
    out.plasmaSingularityOverdrivePlunge = plasmaSingularityOverdrivePlunge;
    out.cranialPostImpactPlunge = cranialPostImpactPlunge;
    out.plasmaGenesisPlunge = plasmaGenesisPlunge;
    out.cranialStompPlunge = cranialStompPlunge;
    out.plasmaBigBangPlunge = plasmaBigBangPlunge;
    out.cranialDoubleStrikePlunge = cranialDoubleStrikePlunge;
    out.plasmaCosmicDawnPlunge = plasmaCosmicDawnPlunge;
    out.cranialPivotTwistPlunge = cranialPivotTwistPlunge;
    out.plasmaSuperclusterPlunge = plasmaSuperclusterPlunge;
    out.cranialPivotSlamPlunge = cranialPivotSlamPlunge;
    out.plasmaOmniversePlunge = plasmaOmniversePlunge;
    out.cranialLeverPinPlunge = cranialLeverPinPlunge;
    out.plasmaMetaversePlunge = plasmaMetaversePlunge;
    out.cranialFulcrumChokePlunge = cranialFulcrumChokePlunge;
    out.plasmaHyperdimensionalPlunge = plasmaHyperdimensionalPlunge;
    out.cranialFulcrumArmbarPlunge = cranialFulcrumArmbarPlunge;
    out.plasmaTransdimensionalPlunge = plasmaTransdimensionalPlunge;
    out.cranialFulcrumKneePlunge = cranialFulcrumKneePlunge;
    out.plasmaInterdimensionalPlunge = plasmaInterdimensionalPlunge;
    out.cranialFulcrumRibSlamPlunge = cranialFulcrumRibSlamPlunge;
    out.plasmaContinuumSingularityPlunge = plasmaContinuumSingularityPlunge;
    out.cranialFulcrumClaviclePinPlunge = cranialFulcrumClaviclePinPlunge;
    out.plasmaMultiverseSingularityPlunge = plasmaMultiverseSingularityPlunge;
    out.cranialFulcrumScapulaPinPlunge = cranialFulcrumScapulaPinPlunge;
    out.plasmaOmniverseSingularityPlunge = plasmaOmniverseSingularityPlunge;
    out.cranialFulcrumSternumPinPlunge = cranialFulcrumSternumPinPlunge;
    out.plasmaTransCosmicSingularityPlunge = plasmaTransCosmicSingularityPlunge;
    out.cranialFulcrumMandiblePinPlunge = cranialFulcrumMandiblePinPlunge;
    out.plasmaMacrocosmSingularityPlunge = plasmaMacrocosmSingularityPlunge;
    out.cranialFulcrumZygomaticPinPlunge = cranialFulcrumZygomaticPinPlunge;
    out.plasmaUniversalSingularityPlunge = plasmaUniversalSingularityPlunge;
    out.outsideLegKneeDrop = outsideLegKneeDrop;
    out.isCrossThrustReady = isCrossThrustReady;
    out.isCrossThrustOvercharged = isCrossThrustOvercharged;
    return out;
}

// ---------------------------------------------------------------------------
// GH #17 step 10: section 2 ladder climbing & mantling state + detection.
// The 5 Update-local statics (rung holds, climb direction, mantle cooldown)
// hoisted here. Detection lives in UpdateLadder below; ALL side effects
// (SendKeyTap/HapticRequest/VR_LOG_INFO) stay at the call site in
// CameraHook::Update with identical strings. No behavior change: same
// volumes (Y > 0.32, Z cone [-0.55, -0.16]), grip thresholds (> 0.5 grab /
// < 0.40 release), capacitive touch check, Vy thresholds (+/-0.25), and
// mantle cooldown (> 1000ms).
// ---------------------------------------------------------------------------
struct LadderState {
    bool leftHoldingRung = false;
    bool rightHoldingRung = false;
    bool climbUp = false;
    bool climbDown = false;
    DWORD lastMantleTime = 0;
    // GH #47: mantle grace window - first pull opens a 180ms window so the
    // second hand may land slightly late instead of requiring simultaneity.
    DWORD firstPullTime = 0;
    bool firstPullLeft = false;
    // GH #47: idle-while-hanging hint - holding a rung with no pull motion.
    DWORD lastPullActivityTime = 0;
    bool idleHintFired = false;
};
static LadderState s_ladder;

struct LadderIntent {
    bool leftGrabbed = false;  // edge: log + haptic + 'E' tap
    float leftGrabY = 0.0f;
    float leftGrabZ = 0.0f;
    bool rightGrabbed = false; // edge: log + haptic + 'E' tap
    float rightGrabY = 0.0f;
    float rightGrabZ = 0.0f;
    bool mantle = false;       // edge: log + SPACE tap + 0.6 haptics
    bool mantleWindow = false;   // level: first pull down, waiting for second (ready cue)
    bool mantleWindowOpened = false; // edge: window just opened (single ready double-pulse)
    bool hangingIdleHint = false;  // edge: holding rung 1.5s with no pull (teach PULL DOWN)
    bool climbUp = false;      // level: mirrors st (drives 'W')
    bool climbDown = false;    // level: mirrors st (drives 'S')
    bool lPullDown = false;
    bool rPullDown = false;
    bool lPushUp = false;
    bool rPushUp = false;
    bool climbingActive = false;
};

static LadderIntent UpdateLadder(LadderState& st, DWORD now,
                                 const RiddickVRVector3& leftLocal,
                                 const RiddickVRVector3& rightLocal,
                                 float leftGrip, float rightGrip,
                                 uint32_t leftButtons, uint32_t rightButtons,
                                 float leftTrigger, float rightTrigger,
                                 float lVelY, float rVelY,
                                 bool physicalBlockEngaged) {
    LadderIntent out;
    // Rung grab initiate: hand raised overhead (Y > 0.32m, Z in [-0.55, -0.16])
    // and grip squeezed (> 0.5f). ADR 0381: spatial forward-cone depth
    // filtering decouples ladder grabs from over-shoulder weapon drawing.
    // ADR 0386: capacitive sensor disambiguation (fingers curled, not resting).
    if (!st.leftHoldingRung) {
        bool leftTouchEngaged = (leftButtons & RIDDICK_VR_TOUCH_TRIGGER) != 0 || leftTrigger > 0.20f;
        if (leftLocal.y > 0.32f && (leftLocal.z >= -0.55f && leftLocal.z <= -0.16f) &&
            leftGrip > 0.5f && leftTouchEngaged && !physicalBlockEngaged) {
            st.leftHoldingRung = true;
            st.lastPullActivityTime = now;
            st.idleHintFired = false;
            out.leftGrabbed = true;
            out.leftGrabY = leftLocal.y;
            out.leftGrabZ = leftLocal.z;
        }
    } else {
        if (leftGrip < 0.40f) {
            st.leftHoldingRung = false;
        }
    }

    if (!st.rightHoldingRung) {
        bool rightTouchEngaged = (rightButtons & RIDDICK_VR_TOUCH_TRIGGER) != 0 || rightTrigger > 0.20f;
        if (rightLocal.y > 0.32f && (rightLocal.z >= -0.55f && rightLocal.z <= -0.16f) &&
            rightGrip > 0.5f && rightTouchEngaged && !physicalBlockEngaged) {
            st.rightHoldingRung = true;
            st.lastPullActivityTime = now;
            st.idleHintFired = false;
            out.rightGrabbed = true;
            out.rightGrabY = rightLocal.y;
            out.rightGrabZ = rightLocal.z;
        }
    } else {
        if (rightGrip < 0.40f) {
            st.rightHoldingRung = false;
        }
    }

    // Climbing pull / push velocities.
    out.lPullDown = st.leftHoldingRung && (lVelY < -0.25f);
    out.rPullDown = st.rightHoldingRung && (rVelY < -0.25f);
    out.lPushUp = st.leftHoldingRung && (lVelY > 0.25f);
    out.rPushUp = st.rightHoldingRung && (rVelY > 0.25f);

    // Two-handed mantle / ledge pull (GH #47: 180ms grace window for the
    // second hand - pull down with one hand, then the other, to mantle).
    constexpr DWORD MANTLE_GRACE_MS = 180;
    if ((now - st.lastMantleTime > 1000)) {
        if (out.lPullDown && out.rPullDown) {
            st.lastMantleTime = now;
            st.firstPullTime = 0;
            out.mantle = true;
        } else if (out.lPullDown || out.rPullDown) {
            bool thisLeft = out.lPullDown;
            if (st.firstPullTime == 0 || (now - st.firstPullTime > MANTLE_GRACE_MS) ||
                st.firstPullLeft == thisLeft) {
                // (re)open window on first pull or side change
                if (st.firstPullTime == 0) out.mantleWindowOpened = true;
                st.firstPullTime = now;
                st.firstPullLeft = thisLeft;
            } else if (st.firstPullLeft != thisLeft) {
                // Second hand landed inside the grace window.
                st.lastMantleTime = now;
                st.firstPullTime = 0;
                out.mantle = true;
            }
        } else if (st.firstPullTime != 0 && (now - st.firstPullTime > MANTLE_GRACE_MS)) {
            st.firstPullTime = 0; // window expired with no second hand
        }
    } else {
        st.firstPullTime = 0;
    }
    out.mantleWindow = (st.firstPullTime != 0);

    // Pull-to-climb motion translation (alternating hands = hand-over-hand).
    st.climbUp = (out.lPullDown || out.rPullDown);
    st.climbDown = !st.climbUp && (out.lPushUp || out.rPushUp);
    out.climbUp = st.climbUp;
    out.climbDown = st.climbDown;

    // GH #47: teach the hang. Holding a rung with zero pull motion for
    // 1.5s fires a one-shot hint (call site: pulse + PULL DOWN log) so a
    // player stuck on a ledge learns the next move. Resets on any pull/push.
    bool anyPullMotion = out.lPullDown || out.rPullDown || out.lPushUp || out.rPushUp;
    if (anyPullMotion) {
        st.lastPullActivityTime = now;
        st.idleHintFired = false;
    } else if ((st.leftHoldingRung || st.rightHoldingRung) && !st.idleHintFired &&
               (now - st.lastPullActivityTime > 1500)) {
        st.idleHintFired = true;
        out.hangingIdleHint = true;
    }

    out.climbingActive = st.leftHoldingRung || st.rightHoldingRung || st.climbUp || st.climbDown;
    return out;
}

// ---------------------------------------------------------------------------
// GH #17 step 11: section 6d environmental interaction gesture state +
// detection. The Update-local static s_lastEnvInteractTime (shared 800 ms
// gate) hoisted here. Detection lives in UpdateEnvGestures below; ALL side
// effects (SendKeyTap/HapticRequest/VR_LOG_INFO) stay at the call site in
// CameraHook::Update with identical strings. No behavior change: same
// volumes (Z < -0.20 vent pull / Z < -0.34 poke / Z < -0.30 palm), velocity
// thresholds, grip/trigger gates, cooldown (800 ms), and evaluation order
// (vent pry first, then right/left poke, then right/left palm).
// NOTE: only the live (compiled) prefix is extracted here — the ~3000-line
// #if 0-disabled legacy chain (ADR 0398: cryo-pod/keypad, vent unthread,
// breaker, handwheel, decon lever, turret, drone shunt, crane, container,
// airlock, rock-bolter-class, ...) stays inline for step 12.
// ---------------------------------------------------------------------------
struct EnvState {
    DWORD lastEnvInteractTime = 0;
};
static EnvState s_env;

struct EnvIntent {
    bool ventPry = false;      // edge: log + 'E'/SPACE taps + 0.75 reload haptics
    float ventLz = 0.0f;
    float ventRz = 0.0f;
    bool bioRight = false;     // edge: log + 'E' tap + 0.40 UI haptic (right)
    float bioRightZ = 0.0f;
    bool bioLeft = false;      // edge: log + 'E' tap + 0.40 UI haptic (left)
    float bioLeftZ = 0.0f;
    bool nanomedRight = false; // edge: log + 'E' tap + 0.50 UI haptic (right palm)
    float nanomedRightZ = 0.0f;
    bool nanomedLeft = false;  // edge: log + 'E' tap + 0.50 UI haptic (left palm)
    float nanomedLeftZ = 0.0f;
};

static EnvIntent UpdateEnvGestures(EnvState& st, DWORD now,
                                   const RiddickVRVector3& leftLocal,
                                   const RiddickVRVector3& rightLocal,
                                   const RiddickVRVector3& lVel,
                                   const RiddickVRVector3& rVel,
                                   float leftGrip, float rightGrip,
                                   float leftTrigger, float rightTrigger,
                                   bool isClimbingActive,
                                   bool physicalBlockEngaged,
                                   bool twoHandedActive) {
    EnvIntent out;
    if (!isClimbingActive && !physicalBlockEngaged && !twoHandedActive && (now - st.lastEnvInteractTime > 800)) {
        // Vent Grate Prying (Dual-Hand Pull)
        bool bothHandsForward = (leftLocal.z < -0.20f && rightLocal.z < -0.20f);
        bool bothGripping = (leftGrip > 0.50f && rightGrip > 0.50f);
        bool bothPullingBack = (lVel.z > 0.28f && rVel.z > 0.28f);

        if (bothHandsForward && bothGripping && bothPullingBack) {
            st.lastEnvInteractTime = now;
            out.ventPry = true;
            out.ventLz = lVel.z;
            out.ventRz = rVel.z;
        } else {
            // Biometric Terminal / Retinal Scanner Poke (Single-Hand Pointed Poke)
            bool rightPointing = (rightTrigger < 0.20f && rightGrip > 0.40f);
            bool rightPoke = (rightLocal.z < -0.34f && rVel.z < -0.28f);

            bool leftPointing = (leftTrigger < 0.20f && leftGrip > 0.40f);
            bool leftPoke = (leftLocal.z < -0.34f && lVel.z < -0.28f);

            // NanoMED Medical Regeneration Station Palm Touch (Flat Palm Contact - ADR 0019)
            bool rightPalmOpen = (rightTrigger < 0.20f && rightGrip < 0.25f);
            bool rightPalmTouch = (rightLocal.z < -0.30f && rVel.z < -0.25f);

            bool leftPalmOpen = (leftTrigger < 0.20f && leftGrip < 0.25f);
            bool leftPalmTouch = (leftLocal.z < -0.30f && lVel.z < -0.25f);

            if (rightPointing && rightPoke) {
                st.lastEnvInteractTime = now;
                out.bioRight = true;
                out.bioRightZ = rightLocal.z;
            } else if (leftPointing && leftPoke) {
                st.lastEnvInteractTime = now;
                out.bioLeft = true;
                out.bioLeftZ = leftLocal.z;
            } else if (rightPalmOpen && rightPalmTouch) {
                st.lastEnvInteractTime = now;
                out.nanomedRight = true;
                out.nanomedRightZ = rightLocal.z;
            } else if (leftPalmOpen && leftPalmTouch) {
                st.lastEnvInteractTime = now;
                out.nanomedLeft = true;
                out.nanomedLeftZ = leftLocal.z;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// GH #17 step 8: section 8 aim/block/fire state + detection.
// All six Update-local statics (right-trigger hysteresis level, auto-fire
// hold/cyclic timers, chaingun recoil-comp timer, tranq charge timer/flag)
// hoisted here. Detection lives in UpdateAim below; ALL side effects
// (SendMouseClick/HapticRequest/VR_LOG_INFO plus s_headlook.recoilKickPitch,
// s_melee.currentBlockEngaged, s_isAimingOrBlocking writes) stay at the call
// site in CameraHook::Update with identical strings. No behavior change:
// same hysteresis (press > 0.65 / release >= 0.45), suppression gates
// (reload/block/climb), recoil values, slot profiles (ADR 0376), cadences
// (150 ms auto-fire delay, 90 ms cyclic/comp ticks, 300 ms comp arm,
// 800 ms tranq ramp, 450 ms tranq discharge floor), and punch-recovery gate
// (GH #31: 200 ms since last punch/finisher).
// ---------------------------------------------------------------------------
struct AimState {
    bool rightTriggerDown = false;
    DWORD triggerHoldStartTime = 0;
    DWORD lastCyclicFireHaptic = 0;
    DWORD lastRecoilCompTime = 0;
    DWORD tranqChargeStartTime = 0;
    bool isChargingTranq = false;
};
static AimState s_aim;

struct AimIntent {
    bool fireChanged = false;   // edge: SendMouseClick(true, fireDown)
    bool fireDown = false;      // new level
    int fireSlot = 1;           // weapon slot at rising edge (ADR 0376)
    bool fireTwoHanded = false; // stabilization at rising edge
    float recoilKickPitch = 0.0f; // slot profile pitch (rising edge)
    float fireHapticRightAmp = 0.0f;
    int fireHapticRightPrio = kHapticUi;
    float fireHapticLeftAmp = 0.0f; // 0 = none (one-handed)
    int fireHapticLeftPrio = kHapticUi;
    bool cyclicPulse = false;   // 90 ms auto-fire tick: right 0.68 ladder haptic
    bool foregripCyclic = false; // + left 0.42 ambient foregrip transmission
    bool recoilComp = false;    // chaingun comp tick: haptics + pitch *= 0.35
    bool tranqCharging = false; // level: escalating capacitor hum haptic
    float tranqChargeAmp = 0.0f;
    bool tranqDischarged = false; // edge (charge > 450 ms): log + snap haptic
    DWORD tranqChargeTotalMs = 0;
    bool wantBlock = false;     // level: mirrors to s_isAimingOrBlocking
    bool blockEngage = false;   // edge (punch-gated): RIGHTDOWN + logs/haptics
    bool blockEngagePhysical = false; // true: boxer-guard log variant
    bool blockDisengage = false; // edge: RIGHTUP + log
};

static AimIntent UpdateAim(AimState& st, DWORD now,
                           float domTrigger, float offTrigger, float offGrip,
                           bool reloadActive, bool physicalBlockEngaged,
                           bool isClimbingActive, bool twoHandedActive,
                           int weaponSlot,
                           DWORD lastPunchTime, DWORD lastFinisherTime,
                           bool blockEngaged) {
    AimIntent out;
    // Right Trigger -> Fire hysteresis (GH #24) + suppression gates.
    bool wantRightTrigger = st.rightTriggerDown ? (domTrigger >= 0.45f) : (domTrigger > 0.65f);
    if (wantRightTrigger && (reloadActive || physicalBlockEngaged || isClimbingActive)) {
        wantRightTrigger = false;
    }
    if (wantRightTrigger != st.rightTriggerDown) {
        st.rightTriggerDown = wantRightTrigger;
        out.fireChanged = true;
        out.fireDown = wantRightTrigger;
        if (wantRightTrigger) {
            out.fireSlot = weaponSlot;
            out.fireTwoHanded = twoHandedActive;
            // ADR 0376: Weapon Slot Recoil & Haptic Profile Calibration.
            switch (weaponSlot) {
                case 3: // Melee / Shivs / Ulaks
                    out.recoilKickPitch = 0.005f;
                    out.fireHapticRightAmp = 0.45f;
                    out.fireHapticRightPrio = kHapticUi;
                    break;
                case 2: // Sidearm / Pistol / Tranq Gun
                    out.recoilKickPitch = twoHandedActive ? 0.012f : 0.024f;
                    out.fireHapticRightAmp = twoHandedActive ? 0.50f : 0.62f;
                    out.fireHapticRightPrio = kHapticLadder;
                    if (twoHandedActive) {
                        out.fireHapticLeftAmp = 0.35f;
                        out.fireHapticLeftPrio = kHapticUi;
                    }
                    break;
                case 1: // Primary Shotgun / Combat Long-Arm
                    out.recoilKickPitch = twoHandedActive ? 0.028f : 0.055f;
                    out.fireHapticRightAmp = twoHandedActive ? 0.80f : 0.95f;
                    out.fireHapticRightPrio = kHapticFinisher;
                    if (twoHandedActive) {
                        out.fireHapticLeftAmp = 0.65f;
                        out.fireHapticLeftPrio = kHapticLadder;
                    }
                    break;
                case 4: // Tactical Assault Rifle / Ordnance
                    out.recoilKickPitch = twoHandedActive ? 0.015f : 0.028f;
                    out.fireHapticRightAmp = twoHandedActive ? 0.60f : 0.72f;
                    out.fireHapticRightPrio = kHapticReload;
                    if (twoHandedActive) {
                        out.fireHapticLeftAmp = 0.45f;
                        out.fireHapticLeftPrio = kHapticUi;
                    }
                    break;
                case 5: // Heavy Weapon / SCAR Gun / Rotary Chaingun
                default:
                    out.recoilKickPitch = twoHandedActive ? 0.025f : 0.065f;
                    out.fireHapticRightAmp = twoHandedActive ? 0.85f : 0.98f;
                    out.fireHapticRightPrio = kHapticFinisher;
                    if (twoHandedActive) {
                        out.fireHapticLeftAmp = 0.75f;
                        out.fireHapticLeftPrio = kHapticReload;
                    }
                    break;
            }
        }
    }

    // Sustained Automatic Fire & Mech Rotary Cannon cadence (ADR 0024).
    if (wantRightTrigger) {
        if (st.triggerHoldStartTime == 0) {
            st.triggerHoldStartTime = now;
        } else if (now - st.triggerHoldStartTime > 150) {
            if (now - st.lastCyclicFireHaptic > 90) {
                st.lastCyclicFireHaptic = now;
                out.cyclicPulse = true;
                out.foregripCyclic = twoHandedActive;
            }
        }
    } else {
        st.triggerHoldStartTime = 0;
    }

    // Heavy Mech Rotary Chaingun Dual-Hand Recoil Compensation (ADR 0107).
    bool isDualHandBracedChaingun = twoHandedActive && (domTrigger > 0.80f) && (offGrip > 0.60f);
    if (isDualHandBracedChaingun && (st.triggerHoldStartTime != 0) && (now - st.triggerHoldStartTime > 300)) {
        if (now - st.lastRecoilCompTime > 90) {
            st.lastRecoilCompTime = now;
            out.recoilComp = true;
        }
    }

    // Security Tranq Gun Tactile Capacitor Charging (ADR 0039).
    bool isAimingStance = (offTrigger > 0.40f) || twoHandedActive;
    if (isAimingStance && domTrigger > 0.35f && domTrigger < 0.90f) {
        if (!st.isChargingTranq) {
            st.isChargingTranq = true;
            st.tranqChargeStartTime = now;
        }
        DWORD chargeElapsed = now - st.tranqChargeStartTime;
        float chargeRatio = (chargeElapsed < 800) ? (chargeElapsed / 800.0f) : 1.0f;
        out.tranqCharging = true;
        out.tranqChargeAmp = 0.20f + 0.50f * chargeRatio;
    } else {
        if (st.isChargingTranq) {
            st.isChargingTranq = false;
            DWORD chargeTotal = now - st.tranqChargeStartTime;
            if (chargeTotal > 450) {
                out.tranqDischarged = true;
                out.tranqChargeTotalMs = chargeTotal;
            }
        }
    }

    // Block / Aim level + punch-gated edges (GH #31).
    out.wantBlock = physicalBlockEngaged || (offTrigger > 0.5f);
    if (out.wantBlock != blockEngaged) {
        if (out.wantBlock) {
            if ((now - lastPunchTime > 200) && (now - lastFinisherTime > 200)) {
                out.blockEngage = true;
                out.blockEngagePhysical = physicalBlockEngaged;
            }
        } else {
            out.blockDisengage = true;
        }
    }
    return out;
}

void CameraHook::Shutdown() {
    FlushHeldInputs(); // GH #65: never leave a held click/key down on unload
}

void CameraHook::Update(RiddickVRSharedState& vrState) {
    FlushDueHeldInputs(GetTickCount()); // GH #65: release held clicks even when VR is inactive
    if (SimModeNoInput() && !s_loggedSimNoInput) {
        s_loggedSimNoInput = true;
        VR_LOG_INFO("XRSIM: RIDDICKVR_SIM=1 - desktop input injection disabled");
    }
    if (!m_initialized || !vrState.vrActive) return;

    DWORD now = GetTickCount();

    // Haptic decay (GH #38): fade stale pulses each frame so a one-shot reload /
    // NanoMED buzz ends even if the host clear races. Gestures file requests
    // into the central HapticArbiter (GH #32); single publish point at end of Update.
    vrState.leftHapticPulse *= 0.70f;
    vrState.rightHapticPulse *= 0.70f;
    if (vrState.leftHapticPulse < 0.05f) vrState.leftHapticPulse = 0.0f;
    if (vrState.rightHapticPulse < 0.05f) vrState.rightHapticPulse = 0.0f;
    s_hapticArbiter.Reset(); // GH #32: start a fresh dispatch frame (decay already applied to the carry above)

    // Detect if Windows mouse cursor is visible (Menus, Loading, Dialogue UI)
    CURSORINFO ci{ sizeof(CURSORINFO) };
    bool cursorShowing = false;
    if (GetCursorInfo(&ci)) {
        cursorShowing = (ci.flags & CURSOR_SHOWING) != 0;
    }
    // GH #43: feed the renderer mode detector (menus show cursor over 3D bg).
    OpenGLProxy::NoteMenuCursor(cursorShowing);

    uint32_t btns = vrState.rightController.buttons | vrState.leftController.buttons;
    // Track C #14 (southpaw): dominant/off-hand aliases resolved once in the
    // input mapping section. Default (southpaw=0): dom=right, off=left.
    const RiddickVRControllerInput& domCtrl = g_vrConfig.southpaw ? vrState.leftController : vrState.rightController;
    const RiddickVRControllerInput& offCtrl = g_vrConfig.southpaw ? vrState.rightController : vrState.leftController;
    CombatStanceThresholds stanceThresholds;
    stanceThresholds.enter = g_vrConfig.stanceEnterTrigger;
    stanceThresholds.exit = g_vrConfig.stanceExitTrigger;
    stanceThresholds.holdMs = static_cast<uint32_t>(g_vrConfig.stanceHoldMs);
    CombatStanceIntent combatStance = UpdateCombatStance(s_combatStance,
        vrState.leftController, vrState.rightController, static_cast<uint32_t>(now), stanceThresholds);
    if (combatStance.entered) {
        VR_LOG_INFO("Combat Stance: COMBAT MODE entered (both triggers held)");
        HapticRequest(0, 0.35f, kHapticUi);
        HapticRequest(1, 0.35f, kHapticUi);
    }
    if (combatStance.exited) {
        VR_LOG_INFO("Combat Stance: EXPLORATION MODE restored");
        if (s_melee.currentBlockEngaged) {
            SendMouseClick(false, false);
            s_melee.currentBlockEngaged = false;
        }
        s_physicalBlockEngaged = false;
        s_melee.guardPoseHoldTimer = 0;
        s_melee.guardPoseReleaseTimer = 0;
    }
    static uint32_t s_prevButtons = 0;
    static bool s_prevLeftYButton = false;
    bool leftYDown = (vrState.leftController.buttons & RIDDICK_VR_BTN_Y) != 0;
    bool leftYPressed = leftYDown && !s_prevLeftYButton;
    s_prevLeftYButton = leftYDown;
    static bool s_wasIn3D = false;

    // GH #37: edge-triggered recenter consume. The host emits a single-frame
    // pulse (edge-detected); calibration blocks below react to the rising edge
    // only so a held/level flag cannot recalibrate every frame.
    static bool s_prevRecenterIn = false;
    bool recenterEdge = vrState.shouldRecenter && !s_prevRecenterIn;
    s_prevRecenterIn = vrState.shouldRecenter;

    bool has3D = OpenGLProxy::Has3DGameplayProjection();

    // Auto-transition into Full 3D VR Immersion the moment the game loads its 3D gameplay projection
    if (has3D && !s_wasIn3D) {
        s_wasIn3D = true;
        VR_LOG_INFO("3D Gameplay projection intercepted: transitioned from Menu Letterbox to Full 3D VR Immersion");
    }

    // 1. Left Controller Button Y -> Riddick Eyeshine Night Vision ('Q') (ADR 0022 / ADR 0405)
    // Completely removed legacy cinema toggle: Button Y NEVER switches POV or introduces black bars.
    if (g_vrConfig.interactButton == 0 && leftYPressed) {
        SendKeyTap('Q');
        s_eyeshineActive = !s_eyeshineActive; // GH #35: toggle single source (published once below)
        HapticRequest(0, 0.35f, kHapticUi); // GH #32
        VR_LOG_INFO("Button Y: RIDDICK EYESHINE NIGHT VISION TOGGLED ('Q')");
    }

    // GH #43/#44: cinema mode is now authoritative from stereo_renderer's
    // Has3DThisFrame()+HasMenuCursor() detection in OnSwapBuffers. The hook
    // no longer overrides it — menus/cinematics get the cinema screen path,
    // gameplay gets full-bleed immersion with bar-crop.

    // Transform controller positions into HMD-relative space for spatial gestures
    // (computed before head-look so the guard-pose test can use them, GH #50/#51)
    RiddickVRVector3 leftLocal = WorldToHmdLocal(vrState.leftController.gripPose.position, vrState.hmdPose);
    RiddickVRVector3 rightLocal = WorldToHmdLocal(vrState.rightController.gripPose.position, vrState.hmdPose);

    // 0. Singularity-Free Relative Quaternion Head Tracking Look Input (ADR 0409)
    // GH #17 step 7: math delegated to UpdateHeadLook (explicit HeadLookState,
    // no behavior change); side effects (mouse move, ADS detent haptic) stay here.
    {
        const RiddickVRTrackedPose& ctrlAimPose =
            (s_twoHandedStabilizationActive && s_hasStabilizedPose) ?
            s_stabilizedAimPose : (domCtrl.aimPose.isValid ? domCtrl.aimPose : domCtrl.gripPose); // Track C #14
        bool lookReloading = (now - s_reload.lastReloadGestureTime <= 1100);
        bool lookAimHeld = s_isAimingOrBlocking || (offCtrl.trigger > 0.5f); // Track C #14
        // GH #50/#51: both fists in the front-of-face guard envelope (or just
        // after a swing) is combat, not aiming: the arms must not steer the view.
        const bool guardPoseLike = (!s_twoHandedStabilizationActive &&
            InGuardEnvelope(GetGuardEnvelope(), leftLocal.y, leftLocal.z, rightLocal.y, rightLocal.z,
                            leftLocal.x - rightLocal.x)) ||
            (now - s_melee.lastPunchTime < 300);
        HeadLookIntent lookIntent = UpdateHeadLook(s_headlook, now,
            vrState.hmdPose, ctrlAimPose,
            recenterEdge, lookReloading, lookAimHeld,
            s_physicalBlockEngaged, s_twoHandedStabilizationActive,
            g_vrConfig.southpaw, (s_activeWeaponSlot == 3), // GH #51: melee/guard never steers
            guardPoseLike);
        // GH #45: use the latching Has3DGameplayProjection() instead of the
        // per-frame Has3DThisFrame() — the latter caused judder by silently
        // dropping look deltas during brief ortho-HUD passes within gameplay.
        if (OpenGLProxy::Has3DGameplayProjection()) {
            if (lookIntent.moveX != 0 || lookIntent.moveY != 0)
                SendMouseMove(lookIntent.moveX, lookIntent.moveY); // GH #33: SendInput helper
        } else {
            // Keep the HMD baseline current in menus, but never bank menu
            // motion for a large mouse jump when the first 3D frame arrives.
            s_headlook.accumulatedLookX = 0.0f;
            s_headlook.accumulatedLookY = 0.0f;
        }
        if (lookIntent.adsAcquired) {
            HapticRequest(1, (0.18f), kHapticUi); // Tactile detent on sight picture acquisition
        }
    }

    // 1. Adaptive Physical Crouch Detection & Dynamic Height Calibration (ADR 0004 & 0005)
    // Track C #17 step 2: detection delegated to UpdatePosture (explicit
    // PostureState); side effects (keys/haptics/logs) stay at call site.
    static bool s_flashlightActive = false;
    PostureIntent postureIntent = UpdatePosture(s_posture, vrState.hmdPose.isValid,
        vrState.hmdPose.position.y, vrState.hmdPose.linearVelocity.y,
        vrState.hmdPose.position.x, vrState.hmdPose.position.z,
        recenterEdge, g_vrConfig.seated, g_vrConfig.seatedEyeOffset, s_headlook.lastHmdYaw);
    if (postureIntent.recenterCalibrated) {
        VR_LOG_INFO("Standing eye height calibrated on recenter: %.2fm", s_posture.standingEyeHeight);
    }
    if (postureIntent.baselineSnappedUp) {
        VR_LOG_INFO("Standing eye height snapped UP to %.2fm (was latched low)", s_posture.standingEyeHeight);
    }
    if (postureIntent.initialCalibrated) {
        VR_LOG_INFO("Initial standing eye height calibrated: %.2fm", s_posture.standingEyeHeight);
    }
    if (postureIntent.crouchChanged) {
        VR_LOG_INFO("Adaptive physical crouch %s (Current Y: %.2fm, Standing Y: %.2fm, Drop: %.2fm)",
            postureIntent.crouchEngaged ? "ENGAGED" : "DISENGAGED",
            postureIntent.crouchY, postureIntent.crouchStandingY, postureIntent.crouchDrop);
        SendKey('C', postureIntent.crouchEngaged);
        if (postureIntent.crouchEngaged) {
            HapticRequest(0, (0.2f), kHapticAmbient);
        }
    }

    if (postureIntent.proneChanged) {
        if (postureIntent.proneEngaged) {
            VR_LOG_INFO("Stealth Stance: DEEP PRONE / LASER TRIPWIRE LIMBO engaged (Current Y: %.2fm, Drop: %.2fm)", postureIntent.proneY, postureIntent.proneDrop);
            HapticRequest(0, (0.25f), kHapticUi);
        } else {
            VR_LOG_INFO("Stealth Stance: DEEP PRONE disengaged");
        }
    }

    // 1b. Adaptive Stealth Wall-Peeking & Dynamic Torso Leaning (ADR 0004 & 0010)
    // Tracks horizontal head displacement relative to calibrated torso base origin:
    // - Deadband [0, 0.38m]: Riddick's floor collision capsule remains safely behind wall cover while head peeks around corners
    // - Lateral lean detection (|Lean_right| > 0.15m): Signals stealth corner peek stance with subtle tactile haptic cue (0.15)
    // - Displacement > 0.38m: Torso origin smoothly tracks player translation to prevent roomscale detachment
    // (state + detection in UpdatePosture; edge side effects below at call site)
    if (postureIntent.peekChanged) {
        if (postureIntent.peekEngaged) {
            VR_LOG_INFO("Stealth Stance: CORNER WALL-PEEK engaged (LeanRight: %.2fm, LeanFwd: %.2fm, Crouched: %s)",
                postureIntent.leanRight, postureIntent.leanForward, s_posture.isPhysicallyCrouched ? "YES" : "NO");
            HapticRequest(0, (0.15f), kHapticAmbient); // Subtle stealth confirmation pulse
        } else {
            VR_LOG_INFO("Stealth Stance: CORNER WALL-PEEK disengaged");
        }
    }

    // GH #65: controller velocity arrives in world/stage space but every gesture
    // threshold is head-local (-Z = away from the body). Rotate into the HMD yaw
    // frame so a swing registers whichever way the player faces. Speeds are
    // rotation-invariant. HeadLocalVelocity=0 restores the legacy world frame.
    RiddickVRVector3 rVel = vrState.rightController.gripPose.linearVelocity;
    RiddickVRVector3 lVel = vrState.leftController.gripPose.linearVelocity;
    if (g_vrConfig.headLocalVelocity && vrState.hmdPose.isValid) {
        const RiddickVRQuaternion& hq = vrState.hmdPose.orientation;
        const StrikeVelocity r = ToHeadYawLocal(rVel.x, rVel.y, rVel.z, hq.x, hq.y, hq.z, hq.w);
        const StrikeVelocity l = ToHeadYawLocal(lVel.x, lVel.y, lVel.z, hq.x, hq.y, hq.z, hq.w);
        rVel = RiddickVRVector3{r.x, r.y, r.z};
        lVel = RiddickVRVector3{l.x, l.y, l.z};
    }
    float rSpeed = sqrtf(rVel.x * rVel.x + rVel.y * rVel.y + rVel.z * rVel.z);
    float lSpeed = sqrtf(lVel.x * lVel.x + lVel.y * lVel.y + lVel.z * lVel.z);

    // Dark Athena EVA Solar Flare Shadow Occlusion Stance (ADR 0119)
    static DWORD s_lastSolarShadowTuckTime = 0;
    static bool s_isSolarShadowTuck = false;
    bool handsTuckedIn = (fabsf(leftLocal.x) < 0.14f && fabsf(rightLocal.x) < 0.14f) &&
                         (leftLocal.z > -0.20f && rightLocal.z > -0.20f) &&
                         (vrState.leftController.grip > 0.50f && vrState.rightController.grip > 0.50f);
    bool wantSolarShadowTuck = (s_posture.isPhysicallyCrouched || s_posture.isDeepProne) && handsTuckedIn;
    if (wantSolarShadowTuck != s_isSolarShadowTuck) {
        s_isSolarShadowTuck = wantSolarShadowTuck;
        if (s_isSolarShadowTuck && (now - s_lastSolarShadowTuckTime > 1500)) {
            s_lastSolarShadowTuckTime = now;
            VR_LOG_INFO("Environmental Stealth: EVA SOLAR FLARE SHADOW OCCLUSION TUCK engaged (HandsX: [%.2f, %.2f])", leftLocal.x, rightLocal.x);
            SendKeyTap('C');
            SendKeyTap(VK_SPACE);
            SendKeyTap('Q');
            HapticRequest(0, (0.35f), kHapticUi);  // Cardiovascular shadow stabilization pulse (60 Hz)
            HapticRequest(1, (0.35f), kHapticUi);
        }
    }

    // 2. Physical Ladder Climbing & Mantling Gestures (ADR 0009)
    // - Rung Grabbing: Controller raised above chin/shoulder level (Y > 0.15m relative to HMD) and grip squeezed (> 0.5f)
    // - Mount / Initiate: Taps 'E' and triggers 0.35 amplitude haptic pulse upon grabbing a rung
    // - Pull-to-Climb: Gripping and pulling down (Vy < -0.25 m/s) dispatches 'W' (forward/upward climb)
    // - Push-to-Descend: Gripping and pushing upward (Vy > +0.25 m/s) dispatches 'S' (backward/downward climb)
    // - Two-Handed Mantle: Gripping and pulling both hands down simultaneously triggers Space (mantle confirm) with 0.6 haptics
    // GH #17 step 10: detection delegated to UpdateLadder (explicit
    // LadderState); side effects (keys/haptics/logs) stay at call site.
    LadderIntent ladderIntent = UpdateLadder(s_ladder, now,
        leftLocal, rightLocal,
        vrState.leftController.grip, vrState.rightController.grip,
        vrState.leftController.buttons, vrState.rightController.buttons,
        vrState.leftController.trigger, vrState.rightController.trigger,
        lVel.y, rVel.y,
        s_physicalBlockEngaged);
    // Keep visual rung/mantle feedback synchronized with the gesture detector.
    // During the grace window, highlight the other hand as the next pull target.
    float leftLadderGlow = s_ladder.leftHoldingRung ? 0.45f : 0.0f;
    float rightLadderGlow = s_ladder.rightHoldingRung ? 0.45f : 0.0f;
    if (ladderIntent.mantleWindow) {
        if (s_ladder.firstPullLeft) rightLadderGlow = 1.0f;
        else leftLadderGlow = 1.0f;
    }
    VRHandRenderer::Instance().SetLadderCue(leftLadderGlow, rightLadderGlow);
    if (ladderIntent.leftGrabbed) {
        VR_LOG_INFO("Physical Gesture: LADDER RUNG GRABBED (Left hand, Y: %.2fm, Z: %.2fm)", ladderIntent.leftGrabY, ladderIntent.leftGrabZ);
        HapticRequest(0, 0.35f, kHapticUi); // GH #32
        SendActionTap(VRAction::Interact);
    }
    if (ladderIntent.rightGrabbed) {
        VR_LOG_INFO("Physical Gesture: LADDER RUNG GRABBED (Right hand, Y: %.2fm, Z: %.2fm)", ladderIntent.rightGrabY, ladderIntent.rightGrabZ);
        HapticRequest(1, 0.35f, kHapticUi); // GH #32
        SendActionTap(VRAction::Interact);
    }

    // Two-Handed Mantle / Ledge Pull (GH #47): pull down with one hand, then
    // the other within the grace window (or both at once). The ready cue
    // below teaches the sequence: first pull -> double-pulse + log hint.
    if (ladderIntent.mantleWindowOpened) {
        VR_LOG_INFO("Climb Hint: MANTLE READY - pull DOWN with your other hand now!");
        HapticRequest(0, 0.30f, kHapticUi);
        HapticRequest(1, 0.30f, kHapticUi);
    }
    // GH #47: hanging with no pull motion - teach PULL DOWN TO CLIMB.
    if (ladderIntent.hangingIdleHint) {
        VR_LOG_INFO("Climb Hint: HOLDING RUNG - PULL DOWN hand-over-hand to climb, both hands down to mantle!");
        HapticRequest(0, 0.30f, kHapticUi);
        HapticRequest(1, 0.30f, kHapticUi);
    }
    if (ladderIntent.mantle) {
        VR_LOG_INFO("Physical Gesture: TWO-HANDED MANTLE / LEDGE PULL executed");
        SendKeyTap(VK_SPACE);
        HapticRequest(0, 0.60f, kHapticLadder); // GH #32
        HapticRequest(1, 0.60f, kHapticLadder); // GH #32
    }

    // Inmate Bunk Overhead Pipe Chin-Up & Ledge Pull Stance (ADR 0090)
    static DWORD s_lastPipeChinUpTime = 0;
    bool overheadHands = (leftLocal.y > 0.22f && rightLocal.y > 0.22f);
    bool bothGripsClenched = (vrState.leftController.grip > 0.60f && vrState.rightController.grip > 0.60f);
    bool chinUpPullDown = (lVel.y < -0.50f && rVel.y < -0.50f);
    if (overheadHands && bothGripsClenched && chinUpPullDown && (now - s_lastPipeChinUpTime > 1800)) {
        s_lastPipeChinUpTime = now;
        VR_LOG_INFO("Physical Gesture: INMATE BUNK OVERHEAD PIPE CHIN-UP & LEDGE PULL (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
        SendKeyTap(VK_SPACE);
        SendKeyTap('C'); // Hoist body into suspended ceiling tuck
        HapticRequest(0, (0.70f), kHapticReload); // Iron pipe flex strain (75 Hz)
        HapticRequest(1, (0.70f), kHapticReload);
    }

    // Pull-to-climb motion translation (alternating hands gives hand-over-hand climbing)
    // (climb direction state lives in s_ladder; stroke haptics below at call site)
    // Subtle tactile haptic pulse during active climbing strokes
    if (ladderIntent.climbUp) {
        if (ladderIntent.lPullDown) HapticRequest(0, (0.20f), kHapticAmbient);
        if (ladderIntent.rPullDown) HapticRequest(1, (0.20f), kHapticAmbient);
    } else if (ladderIntent.climbDown) {
        if (ladderIntent.lPushUp) HapticRequest(0, (0.20f), kHapticAmbient);
        if (ladderIntent.rPushUp) HapticRequest(1, (0.20f), kHapticAmbient);
    }

    bool isClimbingActive = ladderIntent.climbingActive;

    // 2b. Diegetic NanoMED Health Station & Physical Cartridge Interaction (ADR 0407)
    // - Biometric Scanner Palm Contact: Open flat palm placed on the health station's scanner console
    //   (Z in [-0.65, -0.20]m, Y in [-0.25, 0.25]m, |X| < 0.28m, open fingers: trigger < 0.20 && grip < 0.25).
    //   Dispatches 'E' and pulses rhythmic 120 Hz nanite restorative haptics while contact is maintained.
    // - Cartridge Draw & Insertion:
    //   Reaching to hip holster/waist (Y < -0.38m, grip > 0.60f) draws a NanoMED cartridge with tactile click haptics.
    //   Pushing down into the overhead station slot (Z in [-0.60, -0.22]m, Y in [0.05, 0.40]m, Vy < -0.22 m/s)
    //   slams the cartridge into the receiver, triggering 'E' / MSG_TRIGGERREFILL with heavy latch haptics (0.85f).
    // Track C #17 step 4 (GH #17): detection delegated to UpdateNanoMED
    // (explicit NanoMedState); side effects (keys/haptics/logs) stay at call site.
    // Track C #14: dominant-hand holster path resolves via southpaw.
    const RiddickVRVector3& domHandLocal = g_vrConfig.southpaw ? leftLocal : rightLocal;
    float domHandVelY = g_vrConfig.southpaw ? lVel.y : rVel.y;
    // Track C #17: palm-flat state hoisted for DetectNanoMEDContact (pure move).
    bool leftPalmFlat = (vrState.leftController.trigger < 0.20f && vrState.leftController.grip < 0.25f);
    bool rightPalmFlat = (vrState.rightController.trigger < 0.20f && vrState.rightController.grip < 0.25f);
    NanoMedIntent nanoIntent = UpdateNanoMED(s_nanomed, now, domHandLocal, domHandVelY, domCtrl.grip,
                                             leftLocal, rightLocal, leftPalmFlat, rightPalmFlat,
                                             s_physicalBlockEngaged);

    // A. Hip Holster Cartridge Draw
    if (nanoIntent.cartridgeDrawn) {
        HapticRequest(1, (0.50f), kHapticReload); // Firm cartridge draw snap
        VR_LOG_INFO("Diegetic Interaction: NANOMED CARTRIDGE DRAWN FROM WAIST HOLSTER");
    } else if (nanoIntent.cartridgeReturned) {
        // Return cartridge to holster if grip released away from station
        HapticRequest(1, (0.20f), kHapticAmbient);
        VR_LOG_INFO("Diegetic Interaction: NANOMED CARTRIDGE RETURNED TO HOLSTER");
    }

    // B. Downward Cartridge Insertion into Overhead Station Receiver Slot
    if (nanoIntent.cartridgeInserted) {
        SendGestureTap('E', "nanomed/cartridge-insert"); // Track C #15
        HapticRequest(1, (0.85f), kHapticReload); // Heavy mechanical locking latch impact
        VR_LOG_INFO("Diegetic Interaction: NANOMED CARTRIDGE INSERTED INTO STATION RECEIVER (MSG_TRIGGERREFILL)");
    }

    // C. Biometric Scanner Palm Contact (Open Palm on Health Station Scanner)
    bool leftScannerContact = nanoIntent.leftContact;
    bool rightScannerContact = nanoIntent.rightContact;

    if (nanoIntent.palmEngaged) {
        SendGestureTap('E', "nanomed/palm-scan"); // Track C #15
        VR_LOG_INFO("Diegetic Interaction: BIOMETRIC PALM SCANNER CONTACT ENGAGED (Left: %d, Right: %d)",
            leftScannerContact ? 1 : 0, rightScannerContact ? 1 : 0);
    }
    if (nanoIntent.palmActive) {
        // Continuous restorative nanite injection haptic cadence (120 Hz pulse oscillation)
        float pulseAmp = nanoIntent.pulseAmp;
        if (leftScannerContact) HapticRequest(0, pulseAmp, kHapticReload); // GH #32: NanoMED
        if (rightScannerContact) HapticRequest(1, pulseAmp, kHapticReload); // GH #32: NanoMED
    }

    s_vrFirstActionActive = isClimbingActive || s_nanomed.palmActive || s_nanomed.holdingCartridge;

    // 3. Radial Locomotion Deadzone & Diagonal Movement Normalization (ADR 0372)
    static bool s_wPressed = false, s_sPressed = false, s_aPressed = false, s_dPressed = false;
    // (Track C #10: RADIAL_DEADZONE superseded by g_vrConfig.moveDeadzone above.)
    float stickLX = vrState.leftController.thumbstickX;
    float stickLY = vrState.leftController.thumbstickY;
    float moveMag = sqrtf(stickLX * stickLX + stickLY * stickLY);

    // Track C #17: intent detection delegated to ComputeLocomotionIntent
    // (explicit LocomotionIntent state, same thresholds, no behavior change).
    // Track C #10: radial deadzone from riddick_vr.ini (default 0.22).
    LocomotionIntent locoIntent = ComputeLocomotionIntent(stickLX, stickLY, g_vrConfig.moveDeadzone);
    bool wantW = locoIntent.fwd;
    bool wantS = locoIntent.back;
    bool wantA = locoIntent.left;
    bool wantD = locoIntent.right;

    if (s_ladder.climbUp) wantW = true;
    if (s_ladder.climbDown) wantS = true;

    if (wantW != s_wPressed) { SendAction(VRAction::MoveForward, wantW); s_wPressed = wantW; }
    if (wantS != s_sPressed) { SendAction(VRAction::MoveBackward, wantS); s_sPressed = wantS; }
    if (wantA != s_aPressed) { SendAction(VRAction::MoveLeft, wantA); s_aPressed = wantA; }
    if (wantD != s_dPressed) { SendAction(VRAction::MoveRight, wantD); s_dPressed = wantD; }

    // Left Thumbstick Click Sprint & Outer Deflection Run (ADR 0391)
    static bool s_sprintActive = false;
    static bool s_prevStickLeftClick = false;
    static bool s_shiftDown = false;
    bool stickLeftClick = (btns & RIDDICK_VR_BTN_STICK_LEFT) != 0;

    if (stickLeftClick && !s_prevStickLeftClick && wantW) {
        s_sprintActive = !s_sprintActive;
        if (s_sprintActive) {
            HapticRequest(0, (0.25f), kHapticUi); // Crisp tactile click on sprint engage
        }
    }
    s_prevStickLeftClick = stickLeftClick;

    // Disengage sprint if player stops moving forward or enters crouch/prone
    if (!wantW || s_posture.isPhysicallyCrouched || s_posture.isDeepProne || isClimbingActive) {
        s_sprintActive = false;
    }

    bool isSprinting = wantW && (s_sprintActive || stickLY > 0.92f);
    if (isSprinting != s_shiftDown) {
        SendKey(VK_SHIFT, isSprinting);
        s_shiftDown = isSprinting;
    }

    // Prone crawl pacing (ADR 0030): rhythmic tactile crawl heartbeat in deep prone
    static DWORD s_lastPronePulseTime = 0;
    if (s_posture.isDeepProne && (wantW || wantS || wantA || wantD) && (now - s_lastPronePulseTime > 600)) {
        s_lastPronePulseTime = now;
        HapticRequest(0, (0.12f), kHapticAmbient); // Subtle stealth crawl pulse (85 Hz)
    }

    // Refined Locomotion Footstep Cadence (ADR 0392):
    // Subtle, low-amplitude tactile gait ticks during upright locomotion (0.06f walk, 0.12f sprint)
    static DWORD s_lastStepPulseTime = 0;
    static bool s_stepLeftFoot = false;
    DWORD stepInterval = isSprinting ? 360 : 540;
    if (!s_posture.isPhysicallyCrouched && !s_posture.isDeepProne && !isClimbingActive && (wantW || wantS || wantA || wantD) && (now - s_lastStepPulseTime > stepInterval)) {
        s_lastStepPulseTime = now;
        s_stepLeftFoot = !s_stepLeftFoot;
        float stepPulse = isSprinting ? 0.12f : 0.06f;
        if (s_stepLeftFoot) {
            HapticRequest(0, (stepPulse), kHapticAmbient);
        } else {
            HapticRequest(1, (stepPulse), kHapticAmbient);
        }
    }

    // 4. Thumbstick Snap vs Smooth Turning Ergonomics & Input Hysteresis (ADR 0371)
    // GH #17 step 3: delegated to UpdateTurn (explicit TurnState, no behavior change).
    UpdateTurn(s_turn, btns, vrState);

    // 4b. Dialogue & Menu Selection Navigation via Thumbstick Y (ADR 0410, GH #39)
    // In VR, vertical right-stick deflection is unused for camera pitch (head pitch is 1:1 physically tracked).
    // Mapping stick Y deflection to VK_UP / VK_DOWN provides intuitive dialogue choice selection
    // and inventory navigation without interfering with locomotion or camera look.
    // GH #39: gated behind menu/dialogue state (no 3D gameplay projection OR
    // OS cursor visible) so right-stick Y never leaks arrows during gameplay;
    // left-stick Y restored as an OR source for menu navigation.
    {
        static DialogueNavigationState s_dialogueNav;
        bool menuMode = !has3D || cursorShowing;
        float leftX = vrState.leftController.thumbstickX;
        float leftY = vrState.leftController.thumbstickY;
        float rightX = vrState.rightController.thumbstickX;
        float rightY = vrState.rightController.thumbstickY;
        const float deadzone = g_vrConfig.dialogueDeadzone;
        float navMagnitude = (std::max)(sqrtf(leftX * leftX + leftY * leftY),
                                        sqrtf(rightX * rightX + rightY * rightY));

        // GH #53 (3.3): direction picking (ChooseDialogueDirection) and the
        // held/diagonal hysteresis now live in dialogue_navigation.h. This
        // block only gates menu context and recentring: outside menu/dialogue
        // or inside the release deadzone we re-arm, so the same direction taps
        // again on the next deliberate push while a held push stays
        // edge-triggered and a stick resting on a diagonal cannot flicker.
        if (!menuMode || navMagnitude < deadzone * 0.55f) {
            ResetDialogueNavigation(s_dialogueNav);
        } else {
            DialogueDirection fired = UpdateDialogueNavigation(
                s_dialogueNav, leftX, leftY, rightX, rightY, deadzone);
            if (fired != DialogueDirection::Neutral) {
                const VRAction actions[] = { VRAction::DialogueUp, VRAction::DialogueDown,
                                             VRAction::DialogueLeft, VRAction::DialogueRight };
                SendActionTap(actions[static_cast<int>(fired) - 1]);
                HapticRequest(1, 0.20f, kHapticAmbient);
            }
        }
    }

    // Track C #9: comfort vignette proxy. Thumbstick move/turn activity drives
    // vrState.comfortVignette (0..1, INI toggle + gain); StereoRenderer applies
    // it to the HUD convergence cue as a subtle comfort proxy. FOLLOW-UP: true
    // post-process peripheral vignette via shader injection (not implemented yet).
    {
        float turnMag = fabsf(vrState.rightController.thumbstickX);
        float moveActive = (moveMag >= g_vrConfig.moveDeadzone) ? 0.5f : 0.0f;
        float turnActive = (turnMag >= g_vrConfig.turnDeadzone) ? 0.5f : 0.0f;
        float comfortTarget = g_vrConfig.comfortEnabled ? (moveActive + turnActive) * g_vrConfig.comfortStrength : 0.0f;
        if (comfortTarget > 1.0f) comfortTarget = 1.0f;
        vrState.comfortVignette += (comfortTarget - vrState.comfortVignette) * 0.2f;
    }

    // GH #31: namespaced finisher cooldown. Choreographed finisher dispatches
    // stamp this instead of s_melee.lastPunchTime so they no longer extend the
    // reactive melee gate / guard recovery window (cross-talk).
    // GH #31: per-frame arbitration snapshots (set before melee, read after).
    DWORD punchAtEntry = s_melee.lastPunchTime;
    DWORD finisherAtEntry = s_melee.lastFinisherTime;

    // 5. Physical Melee Combat Gestures (Jabs, Hooks, Uppercuts, Ulak Cross-Slashes, Shiv Executions, Counter-Parries)
    // GH #65: melee strikes are driven by controller swing velocity, NOT the
    // both-trigger combat chord, so a punch fires even in exploration mode.
    if (!isClimbingActive && (now - s_melee.lastPunchTime > 260)) {
        // GH #17 step 9: counter/disarm + strike-gate detection delegated to
        // UpdateMeleeCounters (explicit MeleeState, no behavior change).
        MeleeCounterIntent meleeCounters = UpdateMeleeCounters(s_melee, now,
            lVel, rVel, leftLocal, rightLocal,
            vrState.leftController.grip, vrState.rightController.grip,
            vrState.leftController.trigger, vrState.rightController.trigger);

        // GH #52: optional gate. With MeleeRequiresStance=1 a classified swing
        // outside the two-trigger stance is dropped (exploration mode).
        if (g_vrConfig.meleeRequiresStance && !combatStance.active && meleeCounters.isMeleeStrike) {
            static DWORD s_lastStanceRejectLog = 0;
            if (now - s_lastStanceRejectLog > 1000) {
                s_lastStanceRejectLog = now;
                VR_LOG_INFO("MELEE STRIKE REJECTED (exploration; MeleeRequiresStance=1)");
            }
            meleeCounters.isMeleeStrike = false;
            meleeCounters.dualCrossSlash = false;
            meleeCounters.isForwardPunch = false;
            meleeCounters.isHookPunch = false;
            meleeCounters.isUppercutPunch = false;
        }

        if (meleeCounters.dualRifleStrip) {
            s_melee.lastPunchTime = now;
            VR_LOG_INFO("Physical Counter: GUARD RIFLE STRIP & MAG EJECTION DISARM (Rz: %.2f, Lz: %.2f)", rVel.z, lVel.z);
            SendMouseTap(true); // GH #65: held click
            SendGestureTap('R', "melee/rifle-strip-eject"); // Track C #15
            SendGestureTap('1', "melee/rifle-strip-equip");
            HapticRequest(1, (0.95f), kHapticFinisher); // Receiver wrenching shockwave (280 Hz)
            HapticRequest(0, (0.95f), kHapticFinisher);
        } else if (meleeCounters.isPocketAshRiposte && (now - s_melee.lastPocketAshRiposteTime > 1600)) {
            s_melee.lastPocketAshRiposteTime = now;
            s_melee.lastPunchTime = now;
            VR_LOG_INFO("Physical Counter: POCKET ASH THROW BLIND & RIPOSTE (Ry: %.2f, Rz: %.2f)", rVel.y, rVel.z);
            SendGestureTap('E', "melee/ash-riposte"); // Track C #15
            SendGestureTap(VK_TAB, "melee/ash-riposte-tab");
            SendKey('W', true);
            SendMouseTap(true); // GH #65: held click
            SendKey('W', false);
            HapticRequest(1, (0.85f), kHapticReload); // Particulate scatter & strike snap (220 Hz)
        } else if (meleeCounters.isShieldPryDisarm) {
            s_melee.lastPunchTime = now;
            VR_LOG_INFO("Physical Counter: RIOT SHIELD WEDGE PRY & DISARM (Lx: %.2f, Rx: %.2f)", lVel.x, rVel.x);
            SendGestureTap('E', "melee/shield-pry"); // Track C #15
            SendMouseTap(true); // GH #65: held click
            SendGestureTap('1', "melee/shield-seize");
            HapticRequest(0, (0.95f), kHapticFinisher); // Metallic shear crack (280 Hz)
            HapticRequest(1, (0.95f), kHapticFinisher);
        } else if (meleeCounters.isEyeGougeRiposte) {
            s_melee.lastPunchTime = now;
            VR_LOG_INFO("Physical Counter: INMATE SHIV EYE-GOUGE RIPOSTE (Rz: %.2f, Ry: %.2f, Ey: %.2f)", rVel.z, rVel.y, rightLocal.y);
            SendKey('W', true);
            SendMouseTap(true); // GH #65: held click
            SendKey('W', false);
            HapticRequest(1, (0.95f), kHapticFinisher); // Perforation & orbital bone crunch (280 Hz)
        } else if (meleeCounters.isForwardParry) {
            s_melee.lastPunchTime = now;
            VR_LOG_INFO("Physical Gesture: PARRY / COUNTER-STRIKE THRUST (Rz: %.2f, Lz: %.2f while Blocking)", rVel.z, lVel.z);
            SendMouseTap(true); // GH #65: held click
            HapticRequest(1, (0.95f), kHapticFinisher); // High-frequency bone-cracking parry shockwave (280 Hz)
        } else if (meleeCounters.isMeleeStrike) {
            // Immediately disengage block if currently blocking so attacks are not canceled or stuck
            if (s_melee.currentBlockEngaged) {
                SendMouseClick(false, false); // GH #33: foreground-guarded (was mouse_event RIGHTUP)
                s_melee.currentBlockEngaged = false;
                s_physicalBlockEngaged = false;
                s_melee.guardPoseHoldTimer = 0;
                s_melee.guardPoseReleaseTimer = 0;
            }

            s_melee.lastPunchTime = now;
            const auto& activeVel = (rSpeed > lSpeed) ? rVel : lVel;
            const auto& activeLocal = (rSpeed > lSpeed) ? rightLocal : leftLocal;

            // GH #17 step 9: strike/finisher detection delegated to UpdateMeleeStrikes
            // (explicit MeleeState, no behavior change).
            MeleeStrikeIntent meleeStrikes = UpdateMeleeStrikes(s_melee, now,
                lVel, rVel, leftLocal, rightLocal, lSpeed, rSpeed,
                vrState.leftController.grip, vrState.rightController.grip,
                vrState.leftController.trigger, vrState.rightController.trigger,
                vrState.rightController.buttons,
                s_posture.isPhysicallyCrouched, s_posture.isDeepProne,
                s_headlook.lastHmdPitch, s_twoHandedStabilizationActive);
            // Detection-edge side effects from UpdateMeleeStrikes (identical strings,
            // original order); prioritized dispatch chain follows below.
            if (meleeStrikes.arcWaveCharging) {
                HapticRequest(0, (0.30f + 0.45f * meleeStrikes.arcWaveChargeRatio), kHapticUi);
                HapticRequest(1, (0.30f + 0.45f * meleeStrikes.arcWaveChargeRatio), kHapticUi);
            }
            if (meleeStrikes.dartRipped) {
                VR_LOG_INFO("Physical Combat: STUN DART RIPPED FROM CHEST (Rz: %.2f)", rVel.z);
                HapticRequest(1, (0.75f), kHapticReload); // Barbed extraction snap (280 Hz)
            }
            if (meleeStrikes.eyeWipeCleared) {
                VR_LOG_INFO("Physical Combat: EYES CLEARED OF DIRT/ASH WITH OFF-ARM (Lx: %.2f)", lVel.x);
                HapticRequest(0, (0.40f), kHapticUi); // Corneal wipe friction rasp (190 Hz)
            }
            if (meleeStrikes.inductionBuzz) {
                HapticRequest(0, (0.35f + 0.55f * meleeStrikes.inductionRampRatio), kHapticLadder); // Ascending electrostatic buzz (90 -> 280 Hz)
                HapticRequest(1, (0.35f + 0.55f * meleeStrikes.inductionRampRatio), kHapticLadder);
            }
            if (meleeStrikes.liverHookConnected) {
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL LIVER HOOK CONNECTED (Ry: %.2f, Rx: %.2f)", rVel.y, rVel.x);
                HapticRequest(1, (0.95f), kHapticFinisher); // Soft-tissue concussive shockwave (220 Hz)
            }
            if (meleeStrikes.arcWallPulse) {
                HapticRequest(0, (0.65f), kHapticLadder);  // Electrostatic repulsion barrier hum (240 Hz)
                HapticRequest(1, (0.65f), kHapticLadder);
            }
            if (meleeStrikes.arcTrapBuzz) {
                HapticRequest(0, (0.40f + 0.50f * meleeStrikes.arcTrapDwellRatio), kHapticUi); // Capacitive transfer buzz (280 Hz)
                HapticRequest(1, (0.40f + 0.50f * meleeStrikes.arcTrapDwellRatio), kHapticUi);
            }
            if (meleeStrikes.palmChinConnected) {
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL UPWARD PALM CHIN-STRIKE CONNECTED (Ry: %.2f, Rz: %.2f)", rVel.y, rVel.z);
                HapticRequest(1, (0.98f), kHapticFinisher); // Mandibular percussion shockwave (240 Hz)
            }
            if (meleeStrikes.snapdownInitiated) {
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL COLLAR TIE SNAP-DOWN INITIATED (Ly: %.2f)", lVel.y);
                HapticRequest(0, (0.90f), kHapticFinisher); // Cervical snap-down jerk (210 Hz)
            }
            if (meleeStrikes.shinTrapDeflected) {
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL SHIN TRAP DEFLECTED (Ly: %.2f)", lVel.y);
                HapticRequest(0, (0.75f), kHapticReload); // Shin trap deflection impact (180 Hz)
            }
            if (meleeStrikes.suplexHoistInitiated) {
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL DOUBLE UNDERHOOK HOIST INITIATED (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
                HapticRequest(0, (0.90f), kHapticFinisher);  // Torso lift resistance (220 Hz)
                HapticRequest(1, (0.90f), kHapticFinisher);
            }
            if (meleeStrikes.crossArcCharging) {
                HapticRequest(0, (0.60f), kHapticLadder);  // Pre-charge hum (240 Hz)
                HapticRequest(1, (0.60f), kHapticLadder);
            }
            if (meleeStrikes.batonDischargeJabConnected) {
                VR_LOG_INFO("Tactical Melee: STUN BATON HIGH-VOLTAGE DISCHARGE JAB CONNECTED (Rz: %.2f)", rVel.z);
                HapticRequest(1, (0.98f), kHapticFinisher); // Direct contact discharge (310 Hz)
            }
            if (meleeStrikes.doubleLegLiftInitiated) {
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL DOUBLE-LEG LIFT INITIATED (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
                HapticRequest(0, (0.90f), kHapticFinisher);  // Heavy body lift strain (210 Hz)
                HapticRequest(1, (0.90f), kHapticFinisher);
            }
            if (meleeStrikes.pistolTrapWedged) {
                VR_LOG_INFO("Tactical Melee: ENEMY FIREARM TRAPPED & WEDGED WITH STUN BATON (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                HapticRequest(1, (0.95f), kHapticFinisher); // Electrostatic disarm arc snap (280 Hz)
                HapticRequest(0, (0.70f), kHapticReload);
            }
            if (meleeStrikes.firemansLoaded) {
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL FIREMAN'S CARRY LOADED (Ry: %.2f, Lz: %.2f)", rVel.y, lVel.z);
                HapticRequest(0, (0.90f), kHapticFinisher);  // Heavy shoulder load strain (220 Hz)
                HapticRequest(1, (0.90f), kHapticFinisher);
            }
            if (meleeStrikes.crossParryDeflected) {
                VR_LOG_INFO("Tactical Melee: STUN BATON CROSS-CHEST PARRY DEFLECTED (Dir: %c, Vx: %.2f)", s_melee.crossParryDirKey, rVel.x);
                HapticRequest(0, (0.85f), kHapticFinisher);  // Parry deflection shockwave (300 Hz)
                HapticRequest(1, (0.85f), kHapticReload);
            }
            if (meleeStrikes.rearWaistClinchLocked) {
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL REAR WAIST CLINCH LOCKED (Rx: %.2f)", rVel.x);
                HapticRequest(0, (0.90f), kHapticFinisher);  // Abdominal lift strain (220 Hz)
                HapticRequest(1, (0.90f), kHapticFinisher);
            }
            if (meleeStrikes.radialDisarmTwisted) {
                VR_LOG_INFO("Tactical Melee: STUN BATON RADIAL DISARM TWIST EXECUTED (Dir: %c, DeltaVx: %.2f)", s_melee.radialDisarmDirKey, rVel.x - lVel.x);
                HapticRequest(0, (0.95f), kHapticFinisher);  // Torque disarm snap (310 Hz)
                HapticRequest(1, (0.95f), kHapticFinisher);
            }
            if (meleeStrikes.thaiPlumLocked) {
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL THAI PLUM COLLAR TIE LOCKED (Ly: %.2f, Ry: %.2f)", leftLocal.y, rightLocal.y);
                HapticRequest(0, (0.90f), kHapticFinisher);  // Cervical collar drag tension (220 Hz)
                HapticRequest(1, (0.90f), kHapticFinisher);
            }
            if (meleeStrikes.outsideLegHooked) {
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL OUTSIDE LEG HOOKED (Dir: %c, Lx: %.2f)", s_melee.outsideLegSweepDirKey, lVel.x);
                HapticRequest(0, (0.85f), kHapticReload); // Sweep trip torque (200 Hz)
            }
            if (meleeStrikes.crossThrustSupercharged) {
                HapticRequest(0, (0.90f), kHapticFinisher);  // Escalating supercharge hum (340 Hz)
                HapticRequest(1, (0.90f), kHapticFinisher);
            }
            if (meleeStrikes.crossThrustBarrierHum) {
                HapticRequest(0, (0.65f), kHapticLadder);  // Electrostatic barrier hum (240 Hz)
                HapticRequest(1, (0.65f), kHapticLadder);
            }

            if (meleeStrikes.plasmaUniversalSingularityPlunge && (now - s_melee.lastPlasmaUniversalSingularityTime > 2000)) {
                s_melee.lastPlasmaUniversalSingularityTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM UNIVERSAL SINGULARITY OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendKey('W', true);
                SendGestureTap('Q', "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap('C', "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap('Q', "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-universal-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-universal-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-universal-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-universal-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                SendKey('W', false);
                // Track C #15: release folded into debounced tap above.
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm universal singularity overdrive shockwave (550 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck universal recoil (480 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.cranialFulcrumZygomaticPinPlunge && (now - s_melee.lastCranialFulcrumZygomaticPinTime > 2000)) {
                s_melee.lastCranialFulcrumZygomaticPinTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST FULCRUM ZYGOMATIC PIN EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialFulcrumZygomaticPinDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialFulcrumZygomaticPinDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialFulcrumZygomaticPinDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive zygomatic fracture shockwave (540 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer overhook drag torque (470 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.plasmaMacrocosmSingularityPlunge && (now - s_melee.lastPlasmaMacrocosmSingularityTime > 2000)) {
                s_melee.lastPlasmaMacrocosmSingularityTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM MACROCOSM SINGULARITY OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendKey('W', true);
                SendGestureTap('Q', "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap('C', "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap('Q', "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-macrocosm-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                SendKey('W', false);
                // Track C #15: release folded into debounced tap above.
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm macrocosm singularity overdrive shockwave (540 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck macrocosm recoil (470 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.cranialFulcrumMandiblePinPlunge && (now - s_melee.lastCranialFulcrumMandiblePinTime > 2000)) {
                s_melee.lastCranialFulcrumMandiblePinTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST FULCRUM MANDIBLE PIN EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialFulcrumMandiblePinDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialFulcrumMandiblePinDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_RETURN, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_RETURN, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialFulcrumMandiblePinDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive mandible fracture shockwave (530 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer overhook drag torque (460 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.plasmaTransCosmicSingularityPlunge && (now - s_melee.lastPlasmaTransCosmicSingularityTime > 2000)) {
                s_melee.lastPlasmaTransCosmicSingularityTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM TRANS-COSMIC SINGULARITY OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendKey('W', true);
                SendGestureTap('Q', "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap('C', "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap('Q', "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-trans-cosmic-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                SendKey('W', false);
                // Track C #15: release folded into debounced tap above.
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm trans-cosmic singularity overdrive shockwave (530 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck trans-cosmic recoil (460 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.cranialFulcrumSternumPinPlunge && (now - s_melee.lastCranialFulcrumSternumPinTime > 2000)) {
                s_melee.lastCranialFulcrumSternumPinTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST FULCRUM STERNUM PIN EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialFulcrumSternumPinDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialFulcrumSternumPinDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialFulcrumSternumPinDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive sternum fracture shockwave (520 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer overhook drag torque (450 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.plasmaOmniverseSingularityPlunge && (now - s_melee.lastPlasmaOmniverseSingularityTime > 2000)) {
                s_melee.lastPlasmaOmniverseSingularityTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM OMNIVERSE SINGULARITY OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendKey('W', true);
                SendGestureTap('Q', "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap('C', "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap('Q', "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-omniverse-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-omniverse-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                SendKey('W', false);
                // Track C #15: release folded into debounced tap above.
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm omniverse singularity overdrive shockwave (520 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck omniverse recoil (450 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialFulcrumScapulaPinPlunge && (now - s_melee.lastCranialFulcrumScapulaPinTime > 2000)) {
                s_melee.lastCranialFulcrumScapulaPinTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST FULCRUM SCAPULA PIN EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialFulcrumScapulaPinDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialFulcrumScapulaPinDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialFulcrumScapulaPinDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive scapula fracture shockwave (510 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer overhook drag torque (440 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.plasmaMultiverseSingularityPlunge && (now - s_melee.lastPlasmaMultiverseSingularityTime > 2000)) {
                s_melee.lastPlasmaMultiverseSingularityTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM MULTIVERSE SINGULARITY OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendKey('W', true);
                SendGestureTap('Q', "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap('C', "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap('Q', "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-multiverse-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-multiverse-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-multiverse-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                SendKey('W', false);
                // Track C #15: release folded into debounced tap above.
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm multiverse singularity overdrive shockwave (510 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck multiverse recoil (440 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialFulcrumClaviclePinPlunge && (now - s_melee.lastCranialFulcrumClaviclePinTime > 2000)) {
                s_melee.lastCranialFulcrumClaviclePinTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST FULCRUM CLAVICLE PIN EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialFulcrumClaviclePinDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialFulcrumClaviclePinDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialFulcrumClaviclePinDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive clavicle fracture shockwave (510 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer overhook drag torque (440 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.plasmaContinuumSingularityPlunge && (now - s_melee.lastPlasmaContinuumSingularityTime > 2000)) {
                s_melee.lastPlasmaContinuumSingularityTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM CONTINUUM SINGULARITY OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendKey('W', true);
                SendGestureTap('Q', "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap('C', "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap('Q', "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-continuum-singularity-overdrive");
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-continuum-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-continuum-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-continuum-singularity-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-continuum-singularity-overdrive");
                SendMouseTap(true); // GH #65: held click
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                SendKey('W', false);
                // Track C #15: release folded into debounced tap above.
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm continuum singularity overdrive shockwave (500 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck continuum recoil (430 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialFulcrumRibSlamPlunge && (now - s_melee.lastCranialFulcrumRibSlamTime > 2000)) {
                s_melee.lastCranialFulcrumRibSlamTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST FULCRUM RIBCAGE SLAM EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialFulcrumRibSlamDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialFulcrumRibSlamDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialFulcrumRibSlamDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive ribcage fracture shockwave (500 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer overhook drag torque (430 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.plasmaInterdimensionalPlunge && (now - s_melee.lastPlasmaInterdimensionalTime > 2000)) {
                s_melee.lastPlasmaInterdimensionalTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM INTERDIMENSIONAL OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendKey('W', true);
                SendGestureTap('Q', "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap('C', "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap('Q', "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-interdimensional-overdrive");
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-interdimensional-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-interdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-interdimensional-overdrive");
                SendMouseTap(true); // GH #65: held click
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                SendKey('W', false);
                // Track C #15: release folded into debounced tap above.
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm Interdimensional overdrive shockwave (500 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck Interdimensional recoil (420 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialFulcrumKneePlunge && (now - s_melee.lastCranialFulcrumKneeTime > 2000)) {
                s_melee.lastCranialFulcrumKneeTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST FULCRUM KNEE EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialFulcrumKneeDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialFulcrumKneeDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialFulcrumKneeDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive fulcrum knee fracture shockwave (500 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer overhook drag torque (420 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.plasmaTransdimensionalPlunge && (now - s_melee.lastPlasmaTransdimensionalTime > 2000)) {
                s_melee.lastPlasmaTransdimensionalTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM TRANSDIMENSIONAL OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendKey('W', true);
                SendGestureTap('Q', "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap('C', "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap('Q', "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-transdimensional-overdrive");
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-transdimensional-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-transdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-transdimensional-overdrive");
                SendMouseTap(true); // GH #65: held click
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                SendKey('W', false);
                // Track C #15: release folded into debounced tap above.
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm Transdimensional overdrive shockwave (490 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck Transdimensional recoil (410 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialFulcrumArmbarPlunge && (now - s_melee.lastCranialFulcrumArmbarTime > 2000)) {
                s_melee.lastCranialFulcrumArmbarTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST FULCRUM ARMBAR EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialFulcrumArmbarDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialFulcrumArmbarDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialFulcrumArmbarDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive fulcrum armbar fracture shockwave (490 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer overhook drag torque (410 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.plasmaHyperdimensionalPlunge && (now - s_melee.lastPlasmaHyperdimensionalTime > 2000)) {
                s_melee.lastPlasmaHyperdimensionalTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM HYPERDIMENSIONAL OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendKey('W', true);
                SendGestureTap('Q', "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap('C', "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap('Q', "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-hyperdimensional-overdrive");
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-hyperdimensional-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-hyperdimensional-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-hyperdimensional-overdrive");
                SendMouseTap(true); // GH #65: held click
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                SendKey('W', false);
                // Track C #15: release folded into debounced tap above.
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm Hyperdimensional overdrive shockwave (480 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck Hyperdimensional recoil (400 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialFulcrumChokePlunge && (now - s_melee.lastCranialFulcrumChokeTime > 2000)) {
                s_melee.lastCranialFulcrumChokeTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST FULCRUM CHOKE EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialFulcrumChokeDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialFulcrumChokeDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialFulcrumChokeDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive fulcrum leverage shockwave (480 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag torque (400 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.plasmaMetaversePlunge && (now - s_melee.lastPlasmaMetaverseTime > 2000)) {
                s_melee.lastPlasmaMetaverseTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM METAVERSE OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm Metaverse overdrive shockwave (470 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck Metaverse recoil (390 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialLeverPinPlunge && (now - s_melee.lastCranialLeverPinTime > 2000)) {
                s_melee.lastCranialLeverPinTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST LEVER PIN EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialLeverPinDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialLeverPinDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialLeverPinDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive fulcrum lever lock shockwave (470 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag torque (390 Hz)
                goto combat_gesture_done;
            }

            if (meleeStrikes.plasmaOmniversePlunge && (now - s_melee.lastPlasmaOmniverseTime > 2000)) {
                s_melee.lastPlasmaOmniverseTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM OMNIVERSE OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-overdrive");
                SendKey('W', true);
                SendGestureTap('Q', "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap('C', "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap('Q', "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-overdrive");
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-overdrive");
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_TAB, "melee/stun-baton-omniverse-overdrive");
                SendMouseTap(true); // GH #65: held click
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_RETURN, "melee/stun-baton-omniverse-overdrive");
                SendGestureTap(VK_SPACE, "melee/stun-baton-omniverse-overdrive");
                SendMouseTap(true); // GH #65: held click
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                // Track C #15: release folded into debounced tap above.
                SendKey('W', false);
                // Track C #15: release folded into debounced tap above.
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm Omniverse overdrive shockwave (460 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck Omniverse recoil (380 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialPivotSlamPlunge && (now - s_melee.lastCranialPivotSlamTime > 2000)) {
                s_melee.lastCranialPivotSlamTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST PIVOT SLAM EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialPivotSlamDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialPivotSlamDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialPivotSlamDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive deck impact and spinal fracture shockwave (460 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag (380 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaSuperclusterPlunge && (now - s_melee.lastPlasmaSuperclusterTime > 2000)) {
                s_melee.lastPlasmaSuperclusterTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM SUPERCLUSTER OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm Supercluster overdrive shockwave (450 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck Supercluster recoil (360 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialPivotTwistPlunge && (now - s_melee.lastCranialPivotTwistTime > 2000)) {
                s_melee.lastCranialPivotTwistTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST PIVOT TWIST EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialPivotTwistDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialPivotTwistDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialPivotTwistDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Cervical vertebra fracture shockwave (450 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag (370 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaCosmicDawnPlunge && (now - s_melee.lastPlasmaCosmicDawnTime > 2000)) {
                s_melee.lastPlasmaCosmicDawnTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM COSMIC DAWN OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm Cosmic Dawn overdrive shockwave (440 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck Cosmic Dawn recoil (340 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialDoubleStrikePlunge && (now - s_melee.lastCranialDoubleStrikeTime > 2000)) {
                s_melee.lastCranialDoubleStrikeTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST DOUBLE STRIKE EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialDoubleStrikeDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialDoubleStrikeDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialDoubleStrikeDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Dual cranial structural impact shockwave (440 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag (360 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaBigBangPlunge && (now - s_melee.lastPlasmaBigBangTime > 2000)) {
                s_melee.lastPlasmaBigBangTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM BIG BANG OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm Big Bang shockwave (430 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck Big Bang recoil (320 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialStompPlunge && (now - s_melee.lastCranialStompTime > 2000)) {
                s_melee.lastCranialStompTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL DECK STOMP EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialStompDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialStompDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, true);
                SendKey(VK_SPACE, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialStompDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Cranial deck grating fracture shockwave (430 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag (350 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaGenesisPlunge && (now - s_melee.lastPlasmaGenesisTime > 2000)) {
                s_melee.lastPlasmaGenesisTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM GENESIS OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm genesis overdrive shockwave (420 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck genesis recoil (300 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialPostImpactPlunge && (now - s_melee.lastCranialPostImpactTime > 2000)) {
                s_melee.lastCranialPostImpactTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL POST IMPACT SLAM EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialPostImpactDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialPostImpactDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialPostImpactDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Cranial steel post collision shockwave (420 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag (340 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaSingularityOverdrivePlunge && (now - s_melee.lastPlasmaSingularityOverdriveTime > 2000)) {
                s_melee.lastPlasmaSingularityOverdriveTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM SINGULARITY OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm singularity overdrive shockwave (410 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck singularity recoil (280 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialGrindPlunge && (now - s_melee.lastCranialGrindTime > 2000)) {
                s_melee.lastCranialGrindTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL GRATING GRIND EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialGrindDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialGrindDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialGrindDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Cranial diamond grating abrasion shockwave (410 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag (330 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaHypernovaPlunge && (now - s_melee.lastPlasmaHypernovaTime > 2000)) {
                s_melee.lastPlasmaHypernovaTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM HYPERNOVA EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN)
                SendKey(VK_SPACE, true);
                SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm hypernova shockwave (400 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck hypernova recoil (260 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cervicalDeckPinPlunge && (now - s_melee.lastCervicalDeckPinTime > 2000)) {
                s_melee.lastCervicalDeckPinTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CERVICAL DECK PIN SPIKE EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cervicalDeckPinDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cervicalDeckPinDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cervicalDeckPinDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Cervical vertebra fracture shockwave (400 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag (320 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaSupernovaPlunge && (now - s_melee.lastPlasmaSupernovaTime > 2000)) {
                s_melee.lastPlasmaSupernovaTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM SUPERNOVA EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm supernova shockwave (400 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck supernova recoil (240 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.sternumKneeSpikePlunge && (now - s_melee.lastSternumKneeSpikeTime > 2000)) {
                s_melee.lastSternumKneeSpikeTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER STERNUM KNEE SPIKE EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.sternumKneeSpikeDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.sternumKneeSpikeDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.sternumKneeSpikeDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Sternum fracture shockwave (400 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag (310 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaSingularityPlunge && (now - s_melee.lastPlasmaSingularityTime > 2000)) {
                s_melee.lastPlasmaSingularityTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM SINGULARITY EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm singularity shockwave (390 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck singularity recoil (220 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cranialDeckDriverPlunge && (now - s_melee.lastCranialDeckDriverTime > 2000)) {
                s_melee.lastCranialDeckDriverTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CRANIAL DECK DRIVER EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cranialDeckDriverDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cranialDeckDriverDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cranialDeckDriverDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Cranial deck grating impact shockwave (390 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag (300 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaCataclysmOverdrivePlunge && (now - s_melee.lastPlasmaCataclysmOverdriveTime > 2000)) {
                s_melee.lastPlasmaCataclysmOverdriveTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm overdrive shockwave (380 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural girder recoil (200 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.sternumElbowDropPlunge && (now - s_melee.lastSternumElbowDropTime > 2000)) {
                s_melee.lastSternumElbowDropTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER STERNUM ELBOW DROP EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.sternumElbowDropDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.sternumElbowDropDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.sternumElbowDropDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Sternum fracture shockwave (380 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Whizzer collar tie drag (290 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaCataclysmPlunge && (now - s_melee.lastPlasmaCataclysmTime > 2000)) {
                s_melee.lastPlasmaCataclysmTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT CATACLYSM EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant cataclysm shockwave (360 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural girder recoil (160 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.thoracicPinLockThrow && (now - s_melee.lastThoracicPinLockTime > 2000)) {
                s_melee.lastThoracicPinLockTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER THORACIC PIN LOCK EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.thoracicPinLockDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.thoracicPinLockDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.thoracicPinLockDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Shoulder joint keylock fracture shockwave (360 Hz)
                HapticRequest(0, (0.92f), kHapticFinisher);  // Whizzer collar tie drag (270 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaSeismOverdrivePlunge && (now - s_melee.lastPlasmaSeismOverdriveTime > 2000)) {
                s_melee.lastPlasmaSeismOverdriveTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT SEISM OVERDRIVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant seism overdrive shockwave (360 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural girder recoil (150 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.thoracicMatSlamThrow && (now - s_melee.lastThoracicMatSlamTime > 2000)) {
                s_melee.lastThoracicMatSlamTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER THORACIC MAT SLAM EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.thoracicMatSlamDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.thoracicMatSlamDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.thoracicMatSlamDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Thoracic cage fracture shockwave (360 Hz)
                HapticRequest(0, (0.92f), kHapticFinisher);  // Whizzer collar tie drag (270 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaResonantSeismPlunge && (now - s_melee.lastPlasmaResonantSeismTime > 2000)) {
                s_melee.lastPlasmaResonantSeismTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT SEISM EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey(VK_TAB, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_TAB, false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Resonant seism shockwave (360 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural girder recoil (140 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.thoracicDislocationThrow && (now - s_melee.lastThoracicDislocationTime > 2000)) {
                s_melee.lastThoracicDislocationTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER THORACIC DISLOCATION EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.thoracicDislocationDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.thoracicDislocationDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.thoracicDislocationDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Thoracic vertebra dislocation shockwave (360 Hz)
                HapticRequest(0, (0.92f), kHapticFinisher);  // Whizzer collar tie drag (260 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaHarmonicQuakePlunge && (now - s_melee.lastPlasmaHarmonicQuakeTime > 2000)) {
                s_melee.lastPlasmaHarmonicQuakeTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA HARMONIC DECK QUAKE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Harmonic plasma quake wave (360 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck recoil (130 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.hyperextensionFinisherThrow && (now - s_melee.lastHyperextensionFinisherTime > 2000)) {
                s_melee.lastHyperextensionFinisherTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER HYPEREXTENSION FINISHER EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.hyperextensionFinisherDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.hyperextensionFinisherDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.hyperextensionFinisherDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Cervical hyperextension fracture shockwave (360 Hz)
                HapticRequest(0, (0.92f), kHapticFinisher);  // Whizzer collar tie drag (260 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaResonantWavePlunge && (now - s_melee.lastPlasmaResonantWaveTime > 2000)) {
                s_melee.lastPlasmaResonantWaveTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA RESONANT DETONATION EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Harmonic plasma detonation wave (360 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck recoil (120 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cervicalGratingSlamThrow && (now - s_melee.lastCervicalGratingSlamTime > 2000)) {
                s_melee.lastCervicalGratingSlamTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER CERVICAL GRATING SLAM EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cervicalGratingSlamDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cervicalGratingSlamDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cervicalGratingSlamDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Cervical vertebra fracture shockwave (360 Hz)
                HapticRequest(0, (0.92f), kHapticFinisher);  // Whizzer collar tie drag (250 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaOverloadWavePlunge && (now - s_melee.lastPlasmaOverloadWaveTime > 2000)) {
                s_melee.lastPlasmaOverloadWaveTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA OVERLOAD WAVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('C', false);
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Plasma overload detonation wave (360 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck recoil (110 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.sternumGratingSlamThrow && (now - s_melee.lastSternumGratingSlamTime > 2000)) {
                s_melee.lastSternumGratingSlamTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER STERNUM GRATING SLAM EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.sternumGratingSlamDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.sternumGratingSlamDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.sternumGratingSlamDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Thoracic cage fracture shockwave (360 Hz)
                HapticRequest(0, (0.92f), kHapticFinisher);  // Whizzer collar tie drag (250 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.plasmaCleaveWavePlunge && (now - s_melee.lastPlasmaCleaveWaveTime > 2000)) {
                s_melee.lastPlasmaCleaveWaveTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL PLASMA CLEAVE WAVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendKey(VK_RETURN, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_RETURN, false);
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Kinetic plasma cleave wave (360 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck recoil (100 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.sternumFractureSpikeThrow && (now - s_melee.lastSternumFractureSpikeTime > 2000)) {
                s_melee.lastSternumFractureSpikeTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER STERNUM FRACTURE SPIKE EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.sternumFractureSpikeDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.sternumFractureSpikeDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.sternumFractureSpikeDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Sternum bone fracture shockwave (360 Hz)
                HapticRequest(0, (0.92f), kHapticFinisher);  // Collar tie tension (250 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.kineticPulseWavePlunge && (now - s_melee.lastKineticPulseWaveTime > 2000)) {
                s_melee.lastKineticPulseWaveTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL KINETIC PULSE WAVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_RETURN, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_RETURN, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Kinetic plasma detonation wave (360 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck recoil (90 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.sternumKneeDropThrow && (now - s_melee.lastSternumKneeDropTime > 2000)) {
                s_melee.lastSternumKneeDropTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER STERNUM KNEE DROP EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.sternumKneeDropDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.sternumKneeDropDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.sternumKneeDropDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Sternum bone fracture shockwave (360 Hz)
                HapticRequest(0, (0.92f), kHapticFinisher);  // Collar tie tension (240 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.omnidirectionalPulsePlunge && (now - s_melee.lastOmnidirectionalPulseTime > 2000)) {
                s_melee.lastOmnidirectionalPulseTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL DECK ARC PULSE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Double-frequency electrostatic pulse blast (360 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Seismic deck concussion (80 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.whizzerGroundDislocationThrow && (now - s_melee.lastWhizzerGroundDislocationTime > 2000)) {
                s_melee.lastWhizzerGroundDislocationTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER GROUND DISLOCATION EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.whizzerGroundDislocationDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.whizzerGroundDislocationDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.whizzerGroundDislocationDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Shoulder socket fracture and vertebral crack (360 Hz)
                HapticRequest(0, (0.92f), kHapticFinisher);  // Overhook torque strain (230 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.omnidirectionalArcPlunge && (now - s_melee.lastOmnidirectionalArcTime > 2000)) {
                s_melee.lastOmnidirectionalArcTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON OMNIDIRECTIONAL DECK ARC EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_TAB, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_TAB, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Radial dielectric breakdown wave (360 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Landing seismic dispersion (75 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.whizzerGroundFinisherThrow && (now - s_melee.lastWhizzerGroundFinisherTime > 2000)) {
                s_melee.lastWhizzerGroundFinisherTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER GROUND FINISHER EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.whizzerGroundFinisherDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.whizzerGroundFinisherDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.whizzerGroundFinisherDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Bone fracture impact shockwave (360 Hz)
                HapticRequest(0, (0.90f), kHapticFinisher);  // Downward collar tie drag (220 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.deckImpactPlunge && (now - s_melee.lastDeckImpactTime > 2000)) {
                s_melee.lastDeckImpactTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON VAULT CONCUSSIVE DECK IMPACT EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive electro-kinetic deck blast (360 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Structural deck resonance (60 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.hyperextensionSpikeThrow && (now - s_melee.lastHyperextensionSpikeTime > 2000)) {
                s_melee.lastHyperextensionSpikeTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL HYPEREXTENSION MAT SPIKE EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.hyperextensionSpikeDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.hyperextensionSpikeDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.hyperextensionSpikeDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Cervical facet dislocation and mat impact shockwave (360 Hz)
                HapticRequest(0, (0.92f), kHapticFinisher);  // Chin thrust hyperextension torque (240 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.vaultKineticCleavePlunge && (now - s_melee.lastVaultKineticCleaveTime > 2000)) {
                s_melee.lastVaultKineticCleaveTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON VAULT KINETIC CLEAVE EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey('S', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('S', false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Kinetic plasma detonation blast (360 Hz)
                HapticRequest(0, (0.90f), kHapticFinisher);  // Two-handed landing frame recoil (80 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.thoracicSlamThrow && (now - s_melee.lastThoracicSlamTime > 2000)) {
                s_melee.lastThoracicSlamTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL THORACIC GRATING SLAM EXECUTED (Dir: %c, Ry: %.2f, Lx: %.2f)", s_melee.thoracicSlamDirKey, rVel.y, lVel.x);
                SendKey('W', true);
                SendKey(s_melee.thoracicSlamDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.thoracicSlamDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Ribcage crush and deck grating impact shockwave (360 Hz)
                HapticRequest(0, (0.90f), kHapticFinisher);  // Thoracic squeeze lift strain (220 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.vaultGroundArcPlunge && (now - s_melee.lastVaultGroundArcTime > 2000)) {
                s_melee.lastVaultGroundArcTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON VAULT JUMP GROUND ARC EXECUTED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('C', false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Dielectric deck breakdown (360 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Landing seismic dispersion (70 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cervicalSpikeThrow && (now - s_melee.lastCervicalSpikeTime > 2000)) {
                s_melee.lastCervicalSpikeTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL CERVICAL MAT SPIKE EXECUTED (Dir: %c, Ry: %.2f, Ly: %.2f)", s_melee.cervicalSpikeDirKey, rVel.y, lVel.y);
                SendKey('W', true);
                SendKey(s_melee.cervicalSpikeDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.cervicalSpikeDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Cervical vertebra fracture shockwave (360 Hz)
                HapticRequest(0, (0.90f), kHapticFinisher);  // Collar tie neck drag tension (220 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.cleaveArcBlastStrike && (now - s_melee.lastCleaveArcBlastTime > 2000)) {
                s_melee.lastCleaveArcBlastTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON TWO-HANDED CLEAVE & ARC BLAST EXECUTED (Ry: %.2f, Rz: %.2f)", rVel.y, rVel.z);
                SendKey('S', true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey('S', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Frontal arc discharge detonation (360 Hz)
                HapticRequest(0, (0.90f), kHapticFinisher);  // Two-handed shaft recoil (220 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.whizzerCounterThrow && (now - s_melee.lastWhizzerCounterTime > 2000)) {
                s_melee.lastWhizzerCounterTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER COUNTER-THROW EXECUTED (Dir: %c, Ry: %.2f, Lx: %.2f)", s_melee.whizzerCounterDirKey, rVel.y, lVel.x);
                SendKey('W', true);
                SendKey(s_melee.whizzerCounterDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.whizzerCounterDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Shoulder capsule dislocation and bone crack (350 Hz)
                HapticRequest(0, (0.85f), kHapticReload);  // Overhook fulcrum strain (210 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.kineticCleaveSwing && (now - s_melee.lastKineticCleaveTime > 2000)) {
                s_melee.lastKineticCleaveTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON KINETIC CLEAVE & SHOCKWAVE STUN STRUCK (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey('S', true);
                SendKey('W', true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey('W', false);
                SendKey('S', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // High-arc kinetic impact and plasma discharge (360 Hz)
                HapticRequest(0, (0.88f), kHapticReload);  // Secondary grip dielectric dissipation (240 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.lateralSuplexThrow && (now - s_melee.lastLateralSuplexTime > 2000)) {
                s_melee.lastLateralSuplexTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL LATERAL SUPLEX EXECUTED (Dir: %c, Ry: %.2f, Rx: %.2f)", s_melee.lateralSuplexDirKey, rVel.y, rVel.x);
                SendKey('W', true);
                SendKey(s_melee.lateralSuplexDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.lateralSuplexDirKey, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Spine compression and mat impact shockwave (360 Hz)
                HapticRequest(0, (0.92f), kHapticFinisher);  // Double underhook torso lift torque (190 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.groundingThrustLunge && (now - s_melee.lastGroundingThrustTime > 2000)) {
                s_melee.lastGroundingThrustTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON GROUNDING THRUST & ARC RING EXECUTED (Rz: %.2f, Lz: %.2f)", rVel.z, lVel.z);
                SendKey('C', true);
                SendKey('W', true);
                SendKey('Q', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('Q', false);
                SendKey('W', false);
                SendKey('C', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Closed-circuit dielectric breakdown (350 Hz)
                HapticRequest(0, (0.90f), kHapticFinisher);  // Floor conduit surge (60 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.thaiPlumElbowPlunge && (now - s_melee.lastThaiPlumElbowTime > 2000)) {
                s_melee.lastThaiPlumElbowTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL THAI PLUM ELBOW SPIKE STRUCK (Dir: %c, Ry: %.2f)", s_melee.thaiPlumElbowDirKey, rVel.y);
                SendKey('W', true);
                SendKey(s_melee.thaiPlumElbowDirKey, true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey(s_melee.thaiPlumElbowDirKey, false);
                SendKey('W', false);
                HapticRequest(0, (0.85f), kHapticFinisher);  // Cervical collar tension (220 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher); // Cranial bone fracture shockwave (350 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.batonShieldBashThrust && (now - s_melee.lastBatonBashTime > 2000)) {
                s_melee.lastBatonBashTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON KINETIC SHIELD BASH & ARC BURST EXECUTED (Rz: %.2f, Lz: %.2f)", rVel.z, lVel.z);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Planar electrostatic blast (340 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);  // Blunt kinetic recoil (80 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.hipTossRotation && (now - s_melee.lastHipTossTime > 2000)) {
                s_melee.lastHipTossTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL DOUBLE UNDERHOOK HIP TOSS EXECUTED (Dir: %c, DeltaVx: %.2f)", s_melee.hipTossDirKey, rVel.x - lVel.x);
                SendKey('W', true);
                SendKey(s_melee.hipTossDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.hipTossDirKey, false);
                SendKey('W', false);
                HapticRequest(0, (0.90f), kHapticFinisher);  // Thoracic squeeze lift tension (230 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher); // Hip toss deck impact blast (40 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.batonHammerfistPlunge && (now - s_melee.lastHammerfistTime > 2000)) {
                s_melee.lastHammerfistTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON KINETIC HAMMERFIST DECK STRIKE EXECUTED (Ry: %.2f, Rz: %.2f)", rVel.y, rVel.z);
                SendKey('S', true);
                SendKey('W', true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey('W', false);
                SendKey('S', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // High-voltage arc detonation blast (330 Hz)
                HapticRequest(0, (0.70f), kHapticReload);  // Forearm bone recoil (70 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.whizzerDownWhip && (now - s_melee.lastWhizzerTime > 2000)) {
                s_melee.lastWhizzerTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL WHIZZER TAKEDOWN EXECUTED (Dir: %c, Ry: %.2f)", s_melee.whizzerDirKey, rVel.y);
                SendKey('W', true);
                SendKey(s_melee.whizzerDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.whizzerDirKey, false);
                SendKey('W', false);
                HapticRequest(0, (0.85f), kHapticReload);  // Overhook shoulder lever strain (210 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive mat impact (45 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.javelinVaultStrike && (now - s_melee.lastJavelinVaultTime > 2000)) {
                s_melee.lastJavelinVaultTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON JAVELIN VAULT LEAP THRUST EXECUTED (Rz: %.2f, Lz: %.2f)", rVel.z, lVel.z);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Airborne arc detonation blast (360 Hz)
                HapticRequest(0, (0.90f), kHapticFinisher);  // Landing deck recoil (60 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.thaiPlumKneeDrive && (now - s_melee.lastThaiPlumTime > 2000)) {
                s_melee.lastThaiPlumTime = now;
                s_melee.thaiPlumClinchTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL THAI PLUM LIVER KNEE STRUCK (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
                SendKey('W', true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey('W', false);
                HapticRequest(0, (0.95f), kHapticFinisher);  // Ribcage fracture & posture break (280 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive liver impact shockwave (320 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.radialDisarmThrust && (now - s_melee.lastRadialDisarmTime > 2000)) {
                s_melee.lastRadialDisarmTime = now;
                s_melee.radialDisarmTwistTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON RADIAL DISARM PENETRATIVE THRUST DISCHARGED (Rz: %.2f)", rVel.z);
                SendMouseClick(false, true); // GH #33: foreground-guarded (was mouse_event RIGHTDOWN)
                SendKey(s_melee.radialDisarmDirKey, true);
                SendKey('W', true);
                SendKey('E', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('E', false);
                SendKey('W', false);
                SendKey(s_melee.radialDisarmDirKey, false);
                SendMouseClick(false, false); // GH #33: foreground-guarded (was mouse_event RIGHTUP)
                HapticRequest(1, (0.98f), kHapticFinisher); // Penetrative arc detonation blast (350 Hz)
                HapticRequest(0, (0.70f), kHapticReload);
                goto combat_gesture_done;
            }
            if (meleeStrikes.backSuplexHoistSlam && (now - s_melee.lastBackSuplexTime > 2000)) {
                s_melee.lastBackSuplexTime = now;
                s_melee.backSuplexClinchTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL BELLY-TO-BACK SUPLEX SLAM EXECUTED (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
                SendKey('S', true);
                SendKey('W', true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey('W', false);
                SendKey('S', false);
                HapticRequest(0, (0.98f), kHapticFinisher);  // Vertebral floor impact shockwave (40 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isCrossThrustOvercharged && (now - s_melee.lastCrossThrustTime > 2000)) {
                s_melee.lastCrossThrustTime = now;
                s_melee.crossBlockHoldStart = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON CROSS-CHEST OVERCHARGE DETONATION DISCHARGED");
                SendMouseClick(false, true); // GH #33: foreground-guarded (was mouse_event RIGHTDOWN)
                SendKey('W', true);
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendKey(VK_RETURN, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_RETURN, false);
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('W', false);
                SendMouseClick(false, false); // GH #33: foreground-guarded (was mouse_event RIGHTUP)
                HapticRequest(1, (0.98f), kHapticFinisher); // Catastrophic dielectric breakdown blast (360 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.outsideLegKneeDrop && (now - s_melee.lastOutsideLegSweepTime > 2000)) {
                s_melee.lastOutsideLegSweepTime = now;
                s_melee.outsideLegSweepHookTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL KNEE DROP FINISHER EXECUTED (Ry: %.2f)", rVel.y);
                SendKey(s_melee.outsideLegSweepDirKey, true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendKey('S', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('S', false);
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey(s_melee.outsideLegSweepDirKey, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive knee-drop mat impact (320 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.crossParryRiposte && (now - s_melee.lastCrossParryTime > 2000)) {
                s_melee.lastCrossParryTime = now;
                s_melee.crossParryDeflectTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON CROSS-PARRY RIPOSTE DISCHARGED (Dir: %c, Rz: %.2f)", s_melee.crossParryDirKey, rVel.z);
                SendMouseClick(false, true); // GH #33: foreground-guarded (was mouse_event RIGHTDOWN)
                SendKey(s_melee.crossParryDirKey, true);
                SendKey('W', true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey('W', false);
                SendKey(s_melee.crossParryDirKey, false);
                SendMouseClick(false, false); // GH #33: foreground-guarded (was mouse_event RIGHTUP)
                HapticRequest(1, (0.98f), kHapticFinisher); // Electro-kinetic counter-detonation blast (350 Hz)
                HapticRequest(0, (0.70f), kHapticReload);
                goto combat_gesture_done;
            }
            if (meleeStrikes.frontHeadlockWrap && meleeStrikes.frontHeadlockArchTorque && (now - s_melee.lastFrontHeadlockTime > 2000)) {
                s_melee.lastFrontHeadlockTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL FRONT HEADLOCK GUILLOTINE CRACK (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
                SendKey('W', true);
                SendKey('S', true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey('S', false);
                SendKey('W', false);
                HapticRequest(0, (0.98f), kHapticFinisher);  // Cervical compression strain & bone snap (340 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isLookingDownward && meleeStrikes.batonOverheadVault && meleeStrikes.batonVaultPlunge && (now - s_melee.lastVaultDropTime > 2000)) {
                s_melee.lastVaultDropTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON HIGH-ARC OVERHEAD VAULT DROP STRIKE EXECUTED (Ry: %.2f)", rVel.y);
                SendKey(VK_SPACE, true);
                SendKey('S', true);
                SendKey('W', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('W', false);
                SendKey('S', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // High-voltage arc detonation blast (350 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);  // Heavy landing deck dispersion (55 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.suloevLowCrouch && meleeStrikes.suloevHookHold && meleeStrikes.suloevArchPull && (now - s_melee.lastSuloevTime > 2000)) {
                s_melee.lastSuloevTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL SULOEV STRETCH SUBMISSION EXECUTED (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
                SendKey('C', true);
                SendKey('S', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('S', false);
                SendKey('C', false);
                HapticRequest(0, (0.98f), kHapticFinisher);  // Tendon tear & hip dislocation shockwave (330 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isCrossThrustReady && (now - s_melee.lastCrossThrustTime > 2000)) {
                s_melee.lastCrossThrustTime = now;
                s_melee.crossBlockHoldStart = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON CROSS-CHEST BLOCK & ARC THRUST DISRUPTION DISCHARGED");
                SendMouseClick(false, true); // GH #33: foreground-guarded (was mouse_event RIGHTDOWN)
                SendKey('W', true);
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('W', false);
                SendMouseClick(false, false); // GH #33: foreground-guarded (was mouse_event RIGHTUP)
                HapticRequest(1, (0.98f), kHapticFinisher); // High-voltage arc detonation blast (340 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.firemansWhipThrow && (now - s_melee.lastFiremansCarryTime > 2000)) {
                s_melee.lastFiremansCarryTime = now;
                s_melee.firemansLoadTime = 0;
                s_melee.lastFinisherTime = now;
                char throwDirKey = (rVel.x > 0.0f || lVel.x > 0.0f) ? 'D' : 'A';
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL FIREMAN'S CARRY SLAM EXECUTED (Dir: %c, ThrowX: %.2f)", throwDirKey, rVel.x);
                SendKey('W', true);
                SendKey(throwDirKey, true);
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey(throwDirKey, false);
                SendKey('W', false);
                HapticRequest(0, (0.98f), kHapticFinisher);  // Body-spike mat shockwave (45 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.batonThroatThrustStrike && (now - s_melee.lastPistolDisarmTime > 1800)) {
                s_melee.lastPistolDisarmTime = now;
                s_melee.pistolTrapTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON CROSS-PISTOL DISARM & THROAT THRUST EXECUTED (Rz: %.2f)", rVel.z);
                SendKey('W', true);
                SendMouseClick(false, true); // GH #33: foreground-guarded (was mouse_event RIGHTDOWN)
                SendKey('E', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('E', false);
                SendMouseClick(false, false); // GH #33: foreground-guarded (was mouse_event RIGHTUP)
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Tracheal kinetic impact (320 Hz)
                HapticRequest(0, (0.65f), kHapticLadder);
                goto combat_gesture_done;
            }
            if (meleeStrikes.doubleLegSpikeSlam && (now - s_melee.lastDoubleLegSlamTime > 2000)) {
                s_melee.lastDoubleLegSlamTime = now;
                s_melee.doubleLegLiftTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL DOUBLE-LEG MAT SLAM EXECUTED (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
                SendKey('C', true);
                SendKey('W', true);
                SendKey(VK_SPACE, true);
                SendKey('S', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('S', false);
                SendKey(VK_SPACE, false);
                SendKey('W', false);
                SendKey('C', false);
                HapticRequest(0, (0.98f), kHapticFinisher);  // Concussive mat impact blast (40 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.batonTripSweep && (now - s_melee.lastBatonJabTripTime > 1800)) {
                s_melee.lastBatonJabTripTime = now;
                s_melee.batonJabTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON JAB-TO-TASER-TRIP TAKEDOWN EXECUTED (Ry: %.2f, Rx: %.2f)", rVel.y, rVel.x);
                SendKey('W', true);
                SendKey('S', true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('C', false);
                SendKey('S', false);
                SendKey('W', false);
                HapticRequest(1, (0.90f), kHapticFinisher); // Low sweep trip impact (180 Hz)
                HapticRequest(0, (0.60f), kHapticLadder);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isSingleLegTakedown && (now - s_melee.lastSingleLegTakedownTime > 1800)) {
                s_melee.lastSingleLegTakedownTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL SINGLE-LEG ANKLE PICK TAKEDOWN EXECUTED (Ly: %.2f, Rz: %.2f)", lVel.y, rVel.z);
                SendKey('C', true);
                SendKey('W', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('W', false);
                SendKey('C', false);
                HapticRequest(0, (0.80f), kHapticReload);  // Ankle pick scoop (160 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher); // Deck takedown slam (260 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isBatonDiagonalParry && (now - s_melee.lastDiagonalParryTime > 1800)) {
                s_melee.lastDiagonalParryTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON UPWARD DIAGONAL CROSS-PARRY & DISCHARGE EXECUTED (Ry: %.2f, Rx: %.2f)", rVel.y, rVel.x);
                SendMouseClick(false, true); // GH #33: foreground-guarded (was mouse_event RIGHTDOWN)
                SendKey('W', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendMouseClick(false, false); // GH #33: foreground-guarded (was mouse_event RIGHTUP)
                SendKey(VK_SPACE, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Kinetic deflection shockwave (330 Hz)
                HapticRequest(0, (0.75f), kHapticReload);  // Off-hand conductive arc feedback (160 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isArmTriangleReady) {
                s_melee.lastArmTriangleTime = now;
                s_melee.armTriangleStartTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL STANDING ARM-TRIANGLE CHOKE LOCKED & EXECUTED");
                SendKey('W', true);
                SendKey('C', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('C', false);
                SendKey('W', false);
                HapticRequest(0, (0.85f), kHapticReload);  // Vascular occlusion compression (140 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher); // Cervical submission snap (280 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isBatonRadialSweep && (now - s_melee.lastRadialSweepTime > 2000)) {
                s_melee.lastRadialSweepTime = now;
                s_melee.lastFinisherTime = now;
                char dirKey = (rVel.x > 0.0f) ? 'D' : 'A';
                VR_LOG_INFO("Tactical Melee: STUN BATON RADIAL KINETIC SWEEP & STUN RING EXECUTED (Dir: %c, Rx: %.2f)", dirKey, rVel.x);
                SendKey(dirKey, true);
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey(dirKey, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Radial discharge blast (310 Hz)
                HapticRequest(0, (0.85f), kHapticReload);  // Off-hand follow-through ionization (280 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isKimuraFracture && (now - s_melee.lastKimuraTime > 2000)) {
                s_melee.lastKimuraTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL KIMURA SHOULDER LOCK & FRACTURE EXECUTED (Ry: %.2f, Rx: %.2f)", rVel.y, rVel.x);
                SendKey('S', true);
                SendKey('A', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('A', false);
                SendKey('S', false);
                HapticRequest(0, (0.75f), kHapticReload);  // Wrist bind resistance (190 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher); // Humerus fracture crack (320 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isBatonJavelinThrust && (now - s_melee.lastBatonJavelinTime > 1800)) {
                s_melee.lastBatonJavelinTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON FORWARD TWO-HANDED JAVELIN THRUST EXECUTED (Rz: %.2f, Lz: %.2f)", rVel.z, lVel.z);
                SendKey('W', true);
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Electro-kinetic impact pulse (320 Hz)
                HapticRequest(0, (0.65f), kHapticLadder);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isOutsideTripMount && (now - s_melee.lastOutsideTripTime > 1800)) {
                s_melee.lastOutsideTripTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL OUTSIDE TRIP & MOUNT POUND EXECUTED (Lx: %.2f, Rz: %.2f)", lVel.x, rVel.z);
                SendKey('W', true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('C', false);
                SendKey('W', false);
                HapticRequest(0, (0.80f), kHapticReload);  // Ankle hook resistance impact (170 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher); // Cranial deck pound shockwave (260 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isCrossArcBurstReady && (now - s_melee.lastCrossArcBurstTime > 1800)) {
                s_melee.lastCrossArcBurstTime = now;
                s_melee.crossArcStartTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON DUAL-HAND CROSS-BLOCK REACTIVE ARC BURST DISCHARGED");
                SendMouseClick(false, true); // GH #33: foreground-guarded (was mouse_event RIGHTDOWN)
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendMouseClick(false, false); // GH #33: foreground-guarded (was mouse_event RIGHTUP)
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                HapticRequest(0, (0.98f), kHapticFinisher);  // Concussive electrostatic repulsion blast (320 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.suplexSpineSlam && (now - s_melee.lastSuplexSlamTime > 1800)) {
                s_melee.lastSuplexSlamTime = now;
                s_melee.suplexHoistTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL UNDERHOOK SUPLEX SPINE SLAM EXECUTED");
                SendKey('S', true);
                SendKey(VK_SPACE, true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('C', false);
                SendKey(VK_SPACE, false);
                SendKey('S', false);
                HapticRequest(0, (0.98f), kHapticFinisher);  // Deep sub-bass floor impact shockwave (45 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isBatonLateralDisarm && (now - s_melee.lastBatonDisarmTime > 1500)) {
                s_melee.lastBatonDisarmTime = now;
                s_melee.lastFinisherTime = now;
                char dirKey = (rVel.x < 0) ? 'A' : 'D';
                VR_LOG_INFO("Tactical Melee: STUN BATON LATERAL CROSS-SWEEP DEFLECTION & DISARM (Rx: %.2f)", rVel.x);
                SendKey(dirKey, true);
                SendKey('E', true);
                SendMouseTap(false); // GH #65: held click
                SendKey('E', false);
                SendKey(dirKey, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Electromagnetic repulsion arc blast (310 Hz)
                HapticRequest(0, (0.65f), kHapticLadder);
                goto combat_gesture_done;
            }
            if (meleeStrikes.hammerfistStrike && (now - s_melee.lastShinTrapTime > 1500)) {
                s_melee.lastShinTrapTime = now;
                s_melee.shinTrapTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: OVERHAND HAMMERFIST COMPLETED COMBO (Ry: %.2f)", rVel.y);
                SendKey('S', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('S', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Skeletal hammerfist shockwave (280 Hz)
                HapticRequest(0, (0.60f), kHapticLadder);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isBatonSkullBreaker && (now - s_melee.lastSkullBreakerTime > 1500)) {
                s_melee.lastSkullBreakerTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON CHARGED HIGH-ARC DOWNWARD SKULL BREAKER CLEAVED (Ry: %.2f)", rVel.y);
                SendKey('W', true);
                SendKey('S', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('S', false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Electro-kinetic cranial detonation blast (340 Hz)
                HapticRequest(0, (0.60f), kHapticLadder);
                goto combat_gesture_done;
            }
            if (meleeStrikes.guillotineWrapStrike && (now - s_melee.lastSnapdownChokeTime > 1500)) {
                s_melee.lastSnapdownChokeTime = now;
                s_melee.snapdownStartTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: GUILLOTINE CHOKE LOCKED (Ry: %.2f, Rz: %.2f)", rVel.y, rVel.z);
                SendKey('S', true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('C', false);
                SendKey('S', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Tracheal compression chokehold pulse (280 Hz)
                HapticRequest(0, (0.70f), kHapticReload);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isBatonGroundDischarge && (now - s_melee.lastBatonGroundShockTime > 2000)) {
                s_melee.lastBatonGroundShockTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON DUAL-HAND GROUND DISCHARGE KINETIC SHOCKWAVE DETONATED (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey('S', true);
                SendKey(VK_SPACE, true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey(VK_SPACE, false);
                SendKey('S', false);
                HapticRequest(0, (0.98f), kHapticFinisher);  // Seismic ground shock & dielectric breakdown (50 Hz & 320 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.cervicalSnapWhip && (now - s_melee.lastPalmChinComboTime > 1500)) {
                s_melee.lastPalmChinComboTime = now;
                s_melee.palmChinStrikeTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: CERVICAL SNAP COMPLETED COMBO (Lx: %.2f)", lVel.x);
                SendKey('W', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('W', false);
                HapticRequest(0, (0.98f), kHapticFinisher); // Cervical vertebrae fracture snap (300 Hz)
                HapticRequest(1, (0.70f), kHapticReload);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isBatonArcTrapArmed) {
                VR_LOG_INFO("Tactical Melee: STUN BATON HIGH-VOLTAGE CAPACITANCE BLEED ARC TRAP ARMED");
                SendKeyTap('E');
                SendKeyTap('Q');
                SendKeyTap(VK_RETURN);
                HapticRequest(1, (0.95f), kHapticFinisher); // Static surface detonation snap (140 Hz)
                HapticRequest(0, (0.70f), kHapticReload);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isCrossChestArcWall) {
                SendKey('S', true);
                SendKey('Q', true);
                SendMouseTap(false); // GH #65: held click
                SendKey('Q', false);
                SendKey('S', false);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isInductionOverheated) {
                VR_LOG_INFO("Tactical Melee: STUN BATON INDUCTION COIL OVERHEAT ARC DETONATED");
                SendKey('S', true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey('S', false);
                HapticRequest(0, (0.98f), kHapticFinisher);  // Blinding EMP arc detonation shockwave (300 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isDischargingArcWave) {
                VR_LOG_INFO("Tactical Melee: STUN BATON OVERLOAD RADIAL ARC WAVE DISCHARGED (Lx: %.2f, Rx: %.2f)", lVel.x, rVel.x);
                SendKey('S', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('S', false);
                SendKeyTap(VK_SPACE);
                SendKeyTap('Q');
                HapticRequest(0, (0.98f), kHapticFinisher);  // High-voltage electrostatic arc ring shockwave (300 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isBatonGroundPin && (now - s_melee.lastBatonGroundPinTime > 1500)) {
                s_melee.lastBatonGroundPinTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON GROUND PIN & TASER KNEEL (Ry: %.2f)", rVel.y);
                SendKey('C', true);
                SendKey('S', true);
                SendKey('Q', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('Q', false);
                SendKey('S', false);
                SendKey('C', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Heavy deck collision & ground circuit arc (260 Hz)
                HapticRequest(0, (0.65f), kHapticLadder);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isClinchHeadbutt && (now - s_melee.lastHeadbuttTime > 1500)) {
                s_melee.lastHeadbuttTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL COLLAR TIE CLINCH & HEADBUTT STUN (Lz: %.2f, Rz: %.2f)", lVel.z, rVel.z);
                SendKey('W', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('W', false);
                HapticRequest(0, (0.98f), kHapticFinisher);  // Concussive cranial skull collision shockwave (180 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.sternumElbowSmash && (now - s_melee.lastLiverElbowComboTime > 1500)) {
                s_melee.lastLiverElbowComboTime = now;
                s_melee.liverHookTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: STERNUM ELBOW SMASH COMPLETED COMBO (Ry: %.2f)", rVel.y);
                SendKey('W', true);
                SendKey('A', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('A', false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Bone-fracturing sternum blunt crack (280 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isBatonChargedSpear && (now - s_melee.lastSpearThrustTime > 1500)) {
                s_melee.lastSpearThrustTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON CHARGED UNDERARM SPEAR THRUST (Rz: %.2f)", rVel.z);
                SendKey('W', true);
                SendKey('S', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('S', false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // High-voltage linear shockwave (310 Hz)
                HapticRequest(0, (0.65f), kHapticLadder);  // Forearm bracing recoil (140 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isKneeStomp && (now - s_melee.lastKneeStompTime > 1500)) {
                s_melee.lastKneeStompTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL SHIN KICK & KNEE HYPER-EXTENSION (Ry: %.2f, Lz: %.2f)", rVel.y, lVel.z);
                SendKey('C', true);
                SendKey('S', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('S', false);
                SendKey('C', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Joint-crushing skeletal fracture shockwave (240 Hz)
                HapticRequest(0, (0.60f), kHapticLadder);  // Body-weight plant & brace feedback (100 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isLeadKneeThrust && (now - s_melee.lastLeadKneeTime > 1500)) {
                s_melee.lastLeadKneeTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL LEAD KNEE THRUST (Ry: %.2f, Ly: %.2f)", rVel.y, lVel.y);
                SendKey('W', true);
                SendKey('C', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('C', false);
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Concussive abdominal organ shockwave (200 Hz)
                HapticRequest(0, (0.65f), kHapticLadder);  // Frame clamp vibration (100 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isDuckUnderKidneyCounter && (now - s_melee.lastKidneyCounterTime > 1500)) {
                s_melee.lastKidneyCounterTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL DUCK-UNDER KIDNEY COUNTER-JAB (Rx: %.2f, Rz: %.2f)", rVel.x, rVel.z);
                SendKey('C', true);
                SendKey('D', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('D', false);
                SendKey('C', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Visceral renal concussive shockwave (220 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.blindRiposteStrike && (now - s_melee.lastEyeRubRiposteTime > 1500)) {
                s_melee.lastEyeRubRiposteTime = now;
                s_melee.eyeRubWipeTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: DIRT/ASH BLIND RECOVERY RIPOSTE DISPATCHED (Rz: %.2f)", rVel.z);
                SendKey(VK_SPACE, true);
                SendKey('W', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('W', false);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Retaliatory counter-strike blast (240 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isParrySnap && (now - s_melee.lastParrySnapTime > 1500)) {
                s_melee.lastParrySnapTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE SHIV FOREARM DEFENSIVE PARRY-AND-SNAP (Lx: %.2f, Ry: %.2f)", lVel.x, rVel.y);
                SendKey('W', true);
                SendKey('E', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('E', false);
                SendKey('W', false);
                HapticRequest(0, (0.80f), kHapticReload);  // Forearm deflection snap (220 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher); // Joint-shattering bone crack (280 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isBatonRibJab && (now - s_melee.lastBatonRibJabTime > 1500)) {
                s_melee.lastBatonRibJabTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Tactical Melee: STUN BATON RIB-JAB & GROUND TAKEDOWN (Ry: %.2f, Rz: %.2f)", rVel.y, rVel.z);
                SendKey('S', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('S', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Neuromuscular shock & ground deck impact (290 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isDoubleEarClap && (now - s_melee.lastDoubleEarClapTime > 1500)) {
                s_melee.lastDoubleEarClapTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE PIT BRAWL DOUBLE-EAR CONCUSSIVE CLAP (Lx: %.2f, Rx: %.2f)", lVel.x, rVel.x);
                SendKey('W', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('W', false);
                HapticRequest(0, (0.98f), kHapticFinisher);  // Concussive acoustic blast (160 Hz)
                HapticRequest(1, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.dartCounterJabStrike && (now - s_melee.lastDartCounterJabTime > 1500)) {
                s_melee.lastDartCounterJabTime = now;
                s_melee.dartExtractedTime = 0;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: STUN DART COUNTER-JAB DISPATCHED (Rz: %.2f)", rVel.z);
                SendKey('E', true);
                SendKey('3', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('3', false);
                SendKey('E', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Bio-electric discharge shockwave (320 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.isRibUnderhook && (now - s_melee.lastRibUnderhookTime > 1500)) {
                s_melee.lastRibUnderhookTime = now;
                s_melee.lastFinisherTime = now;
                VR_LOG_INFO("Physical Combat: INMATE SHIV RIB-CAGE UNDERHOOK PUNCTURE & LIFT (Ry: %.2f, Rz: %.2f)", rVel.y, rVel.z);
                SendKey('W', true);
                SendKey(VK_SPACE, true);
                SendMouseTap(true); // GH #65: held click
                SendKey(VK_SPACE, false);
                SendKey('W', false);
                HapticRequest(1, (0.95f), kHapticFinisher); // Cartilage perforation & body lift rumble (280 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.dualSledgeOverhead) {
                VR_LOG_INFO("Physical Gesture: HEAVY ORE SLEDGEHAMMER OVERHEAD CRUSH (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
                SendKey('S', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('S', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Catastrophic blunt fracture shockwave (80 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.stealthCarotidSlash) {
                VR_LOG_INFO("Physical Stealth: CAROTID ARTERIAL JUGULAR SLASH (SpeedX: %.2f m/s, Ry: %.2f)", rVel.x, rightLocal.y);
                SendKey('W', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('W', false);
                HapticRequest(1, (0.95f), kHapticFinisher); // Arterial release incision & spray (260 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.twoHandedBatonSlam) {
                VR_LOG_INFO("Physical Gesture: TWO-HANDED STUN BATON DEFIBRILLATOR SLAM (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
                SendKey('S', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('S', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Electro-kinetic shockwave blast (280 Hz)
                HapticRequest(0, (0.98f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.mechPowerPunch) {
                VR_LOG_INFO("Physical Gesture: EXOSKELETON POWER FIST KINETIC SHOCKWAVE PUNCH (Rz: %.2f m/s)", rVel.z);
                SendKey('W', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('W', false);
                HapticRequest(1, (0.98f), kHapticFinisher); // Piston shockwave blast (100 Hz)
                HapticRequest(0, (0.85f), kHapticReload);
                goto combat_gesture_done;
            }
            if (meleeStrikes.droneEmpSnap) {
                VR_LOG_INFO("Physical Combat: SECURITY DRONE EMP OVERLOAD & CHASSIS SNAP (TorqueRelY: %.2f)", fabsf(lVel.y - rVel.y));
                SendKeyTap('E');
                SendMouseTap(true); // GH #65: held click
                HapticRequest(1, (0.95f), kHapticFinisher); // Mechanical spine crunch & EMP arc wave (280 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.leftTrayBatter) {
                VR_LOG_INFO("Physical Gesture: INMATE MESS TRAY CONCUSSIVE BATTER (Lz: %.2f)", lVel.z);
                SendKey('W', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('W', false);
                HapticRequest(0, (0.90f), kHapticFinisher); // Sheet metal concussive clatter (160 Hz)
                goto combat_gesture_done;
            }
            if (meleeStrikes.dualShieldBash) {
                VR_LOG_INFO("Physical Gesture: RIOT SHIELD CONCUSSIVE BASH (Rz: %.2f, Lz: %.2f)", rVel.z, lVel.z);
                SendKey('W', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('W', false);
                HapticRequest(1, (0.95f), kHapticFinisher); // Shield rim blunt shockwave (140 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.dualShieldPlant) {
                VR_LOG_INFO("Physical Gesture: RIOT SHIELD BALLISTIC DECK PLANT (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
                SendKey('C', true); // Crouch lock cover stance
                SendKey(VK_SPACE, true);
                SendKey(VK_SPACE, false);
                HapticRequest(1, (0.75f), kHapticReload); // Deck clamp thud (80 Hz)
                HapticRequest(0, (0.75f), kHapticReload);
                goto combat_gesture_done;
            }
            if (meleeStrikes.dualStoolHeavySmash) {
                VR_LOG_INFO("Physical Gesture: INMATE STOOL / BLUNT WEAPON HEAVY SMASH (LSpd: %.2f, RSpd: %.2f)", lSpeed, rSpeed);
                SendKey('S', true);
                SendMouseTap(true); // GH #65: held click
                SendKey('S', false);
                HapticRequest(1, (0.95f), kHapticFinisher); // Bone-shattering blunt fracture (120 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.dualPalmShove) {
                VR_LOG_INFO("Physical Gesture: PIT FIGHTING ARENA DUAL-PALM CONCUSSIVE SHOVE (Rz: %.2f, Lz: %.2f)", rVel.z, lVel.z);
                SendKeyHeld('W');
                SendMouseTap(true); // GH #65: held click
                HapticRequest(1, (0.85f), kHapticReload); // Elastic ring boundary / concussive shockwave (140 Hz)
                HapticRequest(0, (0.85f), kHapticReload);
                goto combat_gesture_done;
            }
            if (meleeStrikes.isLookingDown && meleeStrikes.isDownwardPlunge) {
                VR_LOG_INFO("Physical Gesture: DEATH-FROM-ABOVE AERIAL TAKEDOWN / DROP AMBUSH (Pitch: %.2f rad, Vy: %.2f m/s)", s_headlook.lastHmdPitch, activeVel.y);
                SendMouseTap(true); // GH #65: held click
                HapticRequest(1, (0.95f), kHapticFinisher); // Bone-shattering ground impact shockwave (280 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeStrikes.dualOverheadSlam) {
                VR_LOG_INFO("Physical Gesture: TWO-HANDED OVERHEAD CRUSHING SLAM / PIPE STRIKE (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
                SendKeyHeld('S');
                SendMouseTap(true); // GH #65: held click

                HapticRequest(1, (0.95f), kHapticFinisher); // Heavy blunt concussive shockwave (120 Hz)
                HapticRequest(0, (0.95f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (meleeCounters.dualCrossSlash) {
                VR_LOG_INFO("Physical Gesture: DUAL ULAK SCISSOR CROSS-SLASH (Rx: %.2f, Lx: %.2f)", rVel.x, lVel.x);
                // Held inputs cannot be sequenced within one frame: A+D together cancel,
                // so one held slash goes the way the dominant hand swept.
                SendKeyHeld((rVel.x < 0.0f) ? 'A' : 'D');
                SendMouseTap(true); // GH #65: held click

                HapticRequest(1, (0.95f), kHapticFinisher);
                HapticRequest(0, (0.95f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if (activeVel.y < -1.35f && (activeLocal.y > -0.10f || s_posture.isPhysicallyCrouched)) {
                VR_LOG_INFO("Physical Gesture: OVERHEAD SHIV PLUNGE / FINISHER (Vy: %.2f m/s, Y: %.2fm)", activeVel.y, activeLocal.y);
                SendKeyHeld('S');
                SendMouseTap(true); // GH #65: held click

                if (rSpeed > lSpeed) HapticRequest(1, (0.95f), kHapticFinisher);
                else HapticRequest(0, (0.95f), kHapticFinisher);
                goto combat_gesture_done;
            }
            if ((s_posture.isPhysicallyCrouched || s_posture.isStealthPeeking) && activeLocal.z < -0.25f && (activeVel.z < -1.1f || fabsf(activeVel.x) > 1.1f)) {
                VR_LOG_INFO("Physical Gesture: STEALTH TAKEDOWN / THROAT-SLIT initiated (Vz: %.2f, Vx: %.2f)", activeVel.z, activeVel.x);
                SendKeyTap('E');
                SendMouseTap(true); // GH #65: held click

                if (rSpeed > lSpeed) HapticRequest(1, (0.90f), kHapticFinisher);
                else HapticRequest(0, (0.90f), kHapticFinisher);
                goto combat_gesture_done;
            }
            {
                // ADR 0378: Melee Trajectory Filtering & Dominant Axis Strike Direction Alignment
                float fwdZ = -activeVel.z; // Forward punch velocity is along -Z
                float absX = fabsf(activeVel.x);
                float absY = fabsf(activeVel.y);

                // Forward Straight Punch / Blade Thrust: forward speed dominates lateral & vertical
                if (fwdZ > 1.10f && fwdZ >= absX && fwdZ >= absY) {
                    bool isBatonArcJab = (vrState.rightController.trigger > 0.60f);
                    VR_LOG_INFO("Physical Gesture: %s detected (Vz: %.2f m/s, Speed: %.2f m/s)",
                        isBatonArcJab ? "RIOT BATON ARC JAB" : "STRAIGHT JAB / BLADE THRUST", fwdZ, (rSpeed > lSpeed) ? rSpeed : lSpeed);
                    SendKeyHeld('W');
                    SendMouseTap(true); // GH #65: held click

                    float pulse = isBatonArcJab ? 0.95f : 0.90f;
                    if (rSpeed > lSpeed) HapticRequest(1, pulse, kHapticFinisher); // GH #32: finisher
                    else HapticRequest(0, pulse, kHapticFinisher); // GH #32: finisher
                    goto combat_gesture_done;
                }
                // Uppercut: upward vertical speed dominates forward & lateral
                else if (activeVel.y > 1.10f && activeVel.y > absX && activeVel.y > fwdZ) {
                    VR_LOG_INFO("Physical Gesture: UPPERCUT detected (Vy: %.2f m/s)", activeVel.y);
                    SendKeyHeld('S');
                    SendMouseTap(true); // GH #65: held click

                    if (rSpeed > lSpeed) HapticRequest(1, (0.9f), kHapticFinisher);
                    else HapticRequest(0, (0.9f), kHapticFinisher);
                    goto combat_gesture_done;
                }
                // Lateral Hook / Blade Slash: lateral speed dominates
                else if (absX > 1.10f) {
                    char hookKey = (activeVel.x > 0.0f) ? 'D' : 'A';
                    VR_LOG_INFO("Physical Gesture: %s HOOK / BLADE SLASH detected (Vx: %.2f m/s)",
                        (hookKey == 'D') ? "RIGHT" : "LEFT", activeVel.x);
                    SendKeyHeld(hookKey);
                    SendMouseTap(true); // GH #65: held click

                    if (rSpeed > lSpeed) HapticRequest(1, (0.9f), kHapticFinisher);
                    else HapticRequest(0, (0.9f), kHapticFinisher);
                    goto combat_gesture_done;
                }
            }
            // GH #65: a classified strike no branch above dispatched (e.g. a diagonal
            // swing with no dominant axis) still lands as a plain click.
            VR_LOG_INFO("Physical Gesture: GENERIC STRIKE (Vx: %.2f, Vy: %.2f, Vz: %.2f)", activeVel.x, activeVel.y, activeVel.z);
            SendMouseTap(true);
        combat_gesture_done:
            s_melee.lastPunchTime = now;
        }
    }

    // GH #31: single dispatch per frame (priority: melee first). Any melee
    // dispatch above (reactive chain, finisher, generic classifier) suppresses
    // lower-priority env tap/click dispatches below so one gesture fires once.
    bool meleeFiredThisFrame =
        (s_melee.lastPunchTime != punchAtEntry) || (s_melee.lastFinisherTime != finisherAtEntry);
    if (meleeFiredThisFrame) {
        VR_LOG_INFO("MELEE STRIKE DISPATCHED (hold=%ums)", static_cast<unsigned>(g_vrConfig.meleeClickHoldMs));
    }

    // 5b. Physical Boxer Guard Pose Detection ("holding both fists up in front of the VR")
    // Criteria (ADR 0400):
    // - Both controllers raised in front of chest/chin/face level relative to HMD (y in -0.36 to +0.25)
    // - Both controllers in front of face (z in -0.58 to -0.10)
    // - Both hands natural shield distance (|x_left - x_right| < 0.48m)
    // - Hands steady (not throwing a punch, rSpeed < 1.3 m/s and lSpeed < 1.3 m/s)
    // - Outside punch execution recovery window (> 200ms since last punch)
    // - Not actively holding ladder rungs or climbing
    // GH #62: block is a *front-of-head* pose. Both hands must be raised in
    // front of the HMD at/just below face height and held steady. Previously the
    // pose also required the both-trigger combat chord and allowed y down to
    // -0.40 m (well below the view), so "block" could be held low. Now it
    // triggers on the pose alone and requires the hands in front of the face.
    // GH #62: the block window is the shared, INI-tunable front-of-HMD
    // envelope (ADR-0434). Deriving the y/z/span tests from the same numbers the
    // strike gate uses keeps "hold the fists in front of the face" and "a swing
    // always fires" consistent.
    const GuardEnvelope guardEnv = GetGuardEnvelope();
    bool chinLevel = (leftLocal.y >= guardEnv.yMin && leftLocal.y <= guardEnv.yMax) &&
                     (rightLocal.y >= guardEnv.yMin && rightLocal.y <= guardEnv.yMax);
    bool inFrontOfFace = (leftLocal.z >= guardEnv.zMin && leftLocal.z <= guardEnv.zMax) &&
                         (rightLocal.z >= guardEnv.zMin && rightLocal.z <= guardEnv.zMax);
    bool handsClose = fabsf(leftLocal.x - rightLocal.x) < guardEnv.maxHandSpan;
    bool handsSteady = (rSpeed < g_vrConfig.guardSteadySpeed && lSpeed < g_vrConfig.guardSteadySpeed);
    bool notPunching = (now - s_melee.lastPunchTime > 120) && (now - s_melee.lastFinisherTime > 120); // GH #31

    bool guardPoseDetected = !isClimbingActive && !s_twoHandedStabilizationActive &&
                             chinLevel && inFrontOfFace && handsClose && handsSteady && notPunching;

    // Temporal hysteresis & debouncing (GH #62, combat_guard.h): engage after
    // GuardEngageMs, release after GuardReleaseMs, or instantly on a real punch.
    // The timers stay in s_melee so the existing resets keep working.
    {
        const bool isThrowingPunch = (rVel.z < -1.15f || lVel.z < -1.15f) || (fabsf(rVel.x) > 1.30f || fabsf(lVel.x) > 1.30f) || (rVel.y < -1.25f || lVel.y < -1.25f);
        GuardPoseState guardState;
        guardState.holdStart = s_melee.guardPoseHoldTimer;
        guardState.releaseStart = s_melee.guardPoseReleaseTimer;
        guardState.engaged = s_physicalBlockEngaged;
        GuardPoseTiming guardTiming;
        guardTiming.engageMs = static_cast<unsigned>(g_vrConfig.guardEngageMs);
        guardTiming.releaseMs = static_cast<unsigned>(g_vrConfig.guardReleaseMs);
        guardTiming.steadySpeed = g_vrConfig.guardSteadySpeed;
        s_physicalBlockEngaged = UpdateGuardPose(guardState, guardPoseDetected, isThrowingPunch, now, guardTiming);
        s_melee.guardPoseHoldTimer = guardState.holdStart;
        s_melee.guardPoseReleaseTimer = guardState.releaseStart;
    }

    // 6. Tactical Weapon Reload Gestures (F.E.A.R. / Riddick VR Style)
    // GH #17 step 8: detection delegated to UpdateReload (explicit
    // ReloadState); side effects (gesture taps/keys/haptics/logs) stay here.
    // Same volumes, velocities, cooldowns, and stroke counts; no behavior change.
    // Supports physical gestures:
    // A) Off-hand brought to weapon receiver / magazine well (< 20cm)
    // B) Off-hand reaching down to waist ammo pouch (y < -0.38m relative to HMD)
    ReloadIntent reloadIntent = UpdateReload(s_reload, now,
        leftLocal, rightLocal, lVel, rVel,
        vrState.leftController.grip, vrState.rightController.grip,
        vrState.rightController.trigger,
        s_posture.isPhysicallyCrouched, s_posture.isDeepProne,
        s_physicalBlockEngaged, isClimbingActive,
        s_twoHandedStabilizationActive, meleeFiredThisFrame,
        g_vrConfig.southpaw);
    if (reloadIntent.cellEject) {
        VR_LOG_INFO("Tactical Weapon: POWER CELL HOT-SWAP EJECTED (Vy: %.2f m/s, Dist: %.2fm)", reloadIntent.cellEjectVelY, reloadIntent.cellEjectDist);
        SendGestureTap('R', "reload/powercell-eject");
        HapticRequest(0, 0.75f, kHapticReload); // GH #32: magnetic cell disconnect snap (140 Hz)
        HapticRequest(1, 0.50f, kHapticUi);
    } else if (reloadIntent.manualReload) {
        VR_LOG_INFO("Tactical Gesture: MANUAL WEAPON RELOAD triggered (%s, HandDist: %.2fm, LeftY: %.2f)",
            reloadIntent.manualIsMagInsert ? "Weapon Receiver / Mag-Well" : "Waist Ammo Pouch", reloadIntent.manualDist, reloadIntent.manualOffY);
        SendGestureTap('R', "reload/manual");
        HapticRequest(0, 0.55f, kHapticLadder); // GH #32
        HapticRequest(1, 0.75f, kHapticReload);
    }

    // 6a. Physical Shiv Sharpening & Blade Grinding (ADR 0035)
    if (reloadIntent.honeStroke) {
        VR_LOG_INFO("Physical Gesture: SHIV SHARPENING / BLADE GRINDING STROKE (RelVelY: %.2f m/s, Dist: %.2fm)", reloadIntent.honeRelVelY, reloadIntent.honeDist);
        HapticRequest(0, (0.55f), kHapticLadder);  // Crisp metallic scrape (220 Hz)
        HapticRequest(1, (0.55f), kHapticLadder);
    }

    // 6a-2. Inmate Toothbrush Shank Whittling & Rapid Friction Sharpening (ADR 0068)
    if (reloadIntent.whittleStroke) {
        VR_LOG_INFO("Physical Gesture: TOOTHBRUSH SHANK WHITTLING STROKE #%d (SpeedX: %.2f m/s)", reloadIntent.whittleCount, reloadIntent.whittleSpeedX);
        HapticRequest(1, (0.50f), kHapticUi); // Abrasive concrete grinding pulse (200 Hz)
        if (reloadIntent.whittleComplete) {
            VR_LOG_INFO("Physical Gesture: IMPROVISED SHIV RAZOR TIP HONED / EQUIPPED");
            SendKeyTap('E');
            SendKeyTap('3'); // Equip shank slot
            HapticRequest(1, (0.70f), kHapticReload); // Hone completion resonance (260 Hz)
        }
    }

    // 6a-3. Inmate Mess Hall Contraband Spork Whittling & Tine Sharpening (ADR 0080)
    if (reloadIntent.sporkStroke) {
        VR_LOG_INFO("Physical Gesture: MESS SPORK TINE NOTCHING STROKE #%d (SpeedX: %.2f m/s)", reloadIntent.sporkCount, reloadIntent.sporkSpeedX);
        HapticRequest(1, (0.45f), kHapticUi); // Serrated rasp pulse (190 Hz)
        if (reloadIntent.sporkComplete) {
            VR_LOG_INFO("Physical Gesture: SERRATED CONTRABAND PUNCTURE SPORK CRAFTED / EQUIPPED");
            SendKeyTap('E');
            SendKeyTap('3'); // Equip shank/shiv slot
            HapticRequest(1, (0.70f), kHapticReload); // Hone ping resonance (240 Hz)
        }
    }

    // 6a-4. Security Camera Thermal Decoy Flare Igniting Gesture (ADR 0089)
    if (reloadIntent.flareIgnited) { // GH #31: melee priority evaluated in UpdateReload
        VR_LOG_INFO("Tactical Decoy: THERMAL MAGNESIUM FLARE IGNITED (ShearVelY: %.2f, ShearVelX: %.2f)", reloadIntent.flareShearY, reloadIntent.flareShearX);
        SendKeyTap('4'); // Equip flare / throwable slot
        SendMouseTap(true); // GH #65: held click
        HapticRequest(0, (0.85f), kHapticReload);  // Sulfur percussion strike snap (260 Hz)
        HapticRequest(1, (0.85f), kHapticReload);
    }

    // 6a-5. Medical Syringe Sedative Self-Injection & Bio-Adrenaline Rush (ADR 0087)
    if (reloadIntent.syringeInjected) {
        VR_LOG_INFO("Medical Restoration: PNEUMATIC BIO-ADRENALINE SYRINGE INJECTED (Rx: %.2f, Ry: %.2f, Rz: %.2f)", reloadIntent.syringeX, reloadIntent.syringeY, reloadIntent.syringeZ);
        SendKeyTap('E');
        SendKeyTap('Q'); // Bio-adrenaline / stimulant surge
        HapticRequest(1, (0.85f), kHapticReload); // Needle plunge & adrenaline rush pulse (100 Hz)
        HapticRequest(0, (0.65f), kHapticLadder);  // Arm tissue injection feedback
    }

    // 6a-6. Inmate Pit Brawl Dirt / Ash Pocket Concealment (ADR 0096)
    static DWORD s_lastGritScoopTime = 0;
    static bool s_hasScoopedGrit = false;
    bool crouchedNearGround = (s_posture.isPhysicallyCrouched || s_posture.isDeepProne) && (rightLocal.y < -0.45f);
    bool gritFloorScoop = crouchedNearGround && (vrState.rightController.grip > 0.50f) && (rVel.z > 0.35f);
    if (gritFloorScoop && !s_hasScoopedGrit && (now - s_lastGritScoopTime > 1500)) {
        s_hasScoopedGrit = true;
        s_lastGritScoopTime = now;
        VR_LOG_INFO("Physical Stealth: PIT BRAWL DIRT / ASH SCOOPED FROM FLOOR (Ry: %.2f, Rz: %.2f)", rightLocal.y, rVel.z);
        HapticRequest(1, (0.35f), kHapticUi); // Abrasive grit sifting pulse (140 Hz)
    }
    if (s_hasScoopedGrit && (rightLocal.y > -0.38f && rightLocal.y < -0.15f && fabsf(rightLocal.x) > 0.12f)) {
        s_hasScoopedGrit = false;
        VR_LOG_INFO("Physical Stealth: PIT BRAWL GRIT CONCEALED IN WAIST POCKET");
        SendKeyTap('E');
        SendKeyTap(VK_TAB);
        HapticRequest(1, (0.45f), kHapticUi); // Cloth pocket rustle confirmation (180 Hz)
    }

    // 6a-7. Security Guard Taser Wire Snip & Circuit Disarm (ADR 0101)
    static DWORD s_lastTaserSnipTime = 0;
    bool handAtChestCables = (fabsf(rightLocal.x) < 0.15f) &&
                             (rightLocal.y > -0.18f && rightLocal.y < 0.10f) &&
                             (rightLocal.z > -0.24f && rightLocal.z < -0.05f) &&
                             (vrState.rightController.grip > 0.50f);
    bool wireShearSlice = (fabsf(rVel.x) > 0.90f);
    if (handAtChestCables && wireShearSlice && !s_physicalBlockEngaged && (now - s_lastTaserSnipTime > 1200)) {
        s_lastTaserSnipTime = now;
        VR_LOG_INFO("Tactical Defense: GUARD TASER WIRE SNIPPED / CIRCUIT GROUNDED (Vx: %.2f m/s)", rVel.x);
        SendKeyTap('E');
        SendKeyTap('3'); // Ground discharge into shiv blade
        HapticRequest(1, (0.75f), kHapticReload); // Dielectric wire shear snap (220 Hz)
    }

    // 6b. Two-Handed Firearm Stabilization (ADR 0005)
    // Detect when player brings off-hand to steady the two-handed weapon (ADR 0379):
    // - Distance hysteresis: Attach within 28cm, detach when separated beyond 38cm
    // - Grip hysteresis: Attach when leftController.grip > 0.50, detach when < 0.30
    // - Not in boxer guard, reloading, or climbing
    float twoHandDist = sqrtf(
        powf(vrState.leftController.gripPose.position.x - vrState.rightController.gripPose.position.x, 2) +
        powf(vrState.leftController.gripPose.position.y - vrState.rightController.gripPose.position.y, 2) +
        powf(vrState.leftController.gripPose.position.z - vrState.rightController.gripPose.position.z, 2)
    );

    bool isReloading = (now - s_reload.lastReloadGestureTime <= 1100);
    bool isHarriesStance = s_flashlightActive && (twoHandDist < 0.18f) && (vrState.leftController.grip > 0.35f);

    constexpr float DIST_ATTACH = 0.28f;
    constexpr float DIST_DETACH = 0.38f;
    constexpr float GRIP_ATTACH = 0.50f;
    constexpr float GRIP_DETACH = 0.30f;

    bool wantTwoHanded = false;
    if (s_twoHandedStabilizationActive) {
        wantTwoHanded = (twoHandDist < DIST_DETACH && vrState.leftController.grip > GRIP_DETACH) || isHarriesStance;
    } else {
        wantTwoHanded = (twoHandDist < DIST_ATTACH && vrState.leftController.grip > GRIP_ATTACH) || isHarriesStance;
    }
    wantTwoHanded = wantTwoHanded && !s_physicalBlockEngaged && !isReloading && !isClimbingActive;

    if (wantTwoHanded != s_twoHandedStabilizationActive) {
        s_twoHandedStabilizationActive = wantTwoHanded;
        if (s_twoHandedStabilizationActive) {
            VR_LOG_INFO("Two-Handed Firearm Stabilization ENGAGED (%s, HandDist: %.2fm)",
                isHarriesStance ? "Tactical Harries Flashlight Stance" : "Foregrip Hold", twoHandDist);
            HapticRequest(0, (isHarriesStance ? 0.35f : 0.25f), kHapticUi);  // Subtle haptic confirmation pulse
            HapticRequest(1, (isHarriesStance ? 0.35f : 0.25f), kHapticUi);
        } else {
            VR_LOG_INFO("Two-Handed Firearm Stabilization DISENGAGED (HandDist: %.2fm)", twoHandDist);
            s_hasStabilizedPose = false;
        }
    }

    // Apply exponential smoothing / spherical interpolation on weapon aim pose when two-handed
    if (vrState.rightController.aimPose.isValid) {
        if (s_twoHandedStabilizationActive) {
            if (!s_hasStabilizedPose) {
                s_stabilizedAimPose = vrState.rightController.aimPose;
                s_hasStabilizedPose = true;
            } else {
                float smoothAlpha = isHarriesStance ? 0.22f : 0.35f;

                // Cybernetic Drone HUD Decoupled Sight & Reticle Drift Dampening (ADR 0062)
                bool isDroneAiming = (vrState.leftController.grip > 0.60f && vrState.rightController.grip > 0.60f);
                if (isDroneAiming) {
                    smoothAlpha = 0.28f; // Heavy cybernetic servo lag & drift dampening
                }

                // Sniper Rifle Diegetic Eye Relief & Bipod Bracing (ADR 0042)
                float scopeEyeDist = sqrtf(
                    powf(rightLocal.x, 2) +
                    powf(rightLocal.y, 2) +
                    powf(rightLocal.z, 2)
                );
                if (scopeEyeDist < 0.16f) {
                    smoothAlpha = 0.12f; // High-precision eye relief stabilization
                    if (lSpeed < 0.08f) {
                        smoothAlpha = 0.05f; // Bipod railing bench-rest lock
                    }
                }

                s_stabilizedAimPose.position.x += smoothAlpha * (vrState.rightController.aimPose.position.x - s_stabilizedAimPose.position.x);
                s_stabilizedAimPose.position.y += smoothAlpha * (vrState.rightController.aimPose.position.y - s_stabilizedAimPose.position.y);
                s_stabilizedAimPose.position.z += smoothAlpha * (vrState.rightController.aimPose.position.z - s_stabilizedAimPose.position.z);

                // SLERP / Normalized Lerp for aim orientation quaternion
                float dot = s_stabilizedAimPose.orientation.x * vrState.rightController.aimPose.orientation.x +
                            s_stabilizedAimPose.orientation.y * vrState.rightController.aimPose.orientation.y +
                            s_stabilizedAimPose.orientation.z * vrState.rightController.aimPose.orientation.z +
                            s_stabilizedAimPose.orientation.w * vrState.rightController.aimPose.orientation.w;

                float qSign = (dot < 0.0f) ? -1.0f : 1.0f;
                s_stabilizedAimPose.orientation.x += smoothAlpha * (qSign * vrState.rightController.aimPose.orientation.x - s_stabilizedAimPose.orientation.x);
                s_stabilizedAimPose.orientation.y += smoothAlpha * (qSign * vrState.rightController.aimPose.orientation.y - s_stabilizedAimPose.orientation.y);
                s_stabilizedAimPose.orientation.z += smoothAlpha * (qSign * vrState.rightController.aimPose.orientation.z - s_stabilizedAimPose.orientation.z);
                s_stabilizedAimPose.orientation.w += smoothAlpha * (qSign * vrState.rightController.aimPose.orientation.w - s_stabilizedAimPose.orientation.w);

                float qNorm = sqrtf(s_stabilizedAimPose.orientation.x * s_stabilizedAimPose.orientation.x +
                                    s_stabilizedAimPose.orientation.y * s_stabilizedAimPose.orientation.y +
                                    s_stabilizedAimPose.orientation.z * s_stabilizedAimPose.orientation.z +
                                    s_stabilizedAimPose.orientation.w * s_stabilizedAimPose.orientation.w);
                if (qNorm > 0.0001f) {
                    s_stabilizedAimPose.orientation.x /= qNorm;
                    s_stabilizedAimPose.orientation.y /= qNorm;
                    s_stabilizedAimPose.orientation.z /= qNorm;
                    s_stabilizedAimPose.orientation.w /= qNorm;
                }

                vrState.rightController.aimPose = s_stabilizedAimPose;
            }
        } else {
            // ADR 0373 & ADR 0402: Speed-Adaptive One-Euro Filter for One-Handed Weapon Aim Stabilization
            // When firing or aiming one-handed, natural physiological hand tremor (3-8 Hz) causes sight jitter.
            // When holding steady in physical ADS corridor, apply high damping (alpha â‰ˆ 0.15) to lock sight rock-steady.
            // As movement speed increases, ramp alpha up to 0.90 for zero-latency target acquisition.
            static RiddickVRTrackedPose s_oneHandedPose{};
            static bool s_hasOneHandedPose = false;

            if (!s_hasOneHandedPose) {
                s_oneHandedPose = vrState.rightController.aimPose;
                s_hasOneHandedPose = true;
            } else {
                float rSpeed = sqrtf(rVel.x * rVel.x + rVel.y * rVel.y + rVel.z * rVel.z);
                float baseAlpha = s_headlook.isPhysicalADS ? 0.15f : 0.22f;
                float alpha = baseAlpha;
                if (rSpeed > 0.08f) {
                    float factor = (rSpeed - 0.08f) / 0.35f;
                    if (factor > 1.0f) factor = 1.0f;
                    alpha = baseAlpha + factor * (0.90f - baseAlpha);
                }

                s_oneHandedPose.position.x += alpha * (vrState.rightController.aimPose.position.x - s_oneHandedPose.position.x);
                s_oneHandedPose.position.y += alpha * (vrState.rightController.aimPose.position.y - s_oneHandedPose.position.y);
                s_oneHandedPose.position.z += alpha * (vrState.rightController.aimPose.position.z - s_oneHandedPose.position.z);

                float dot = s_oneHandedPose.orientation.x * vrState.rightController.aimPose.orientation.x +
                            s_oneHandedPose.orientation.y * vrState.rightController.aimPose.orientation.y +
                            s_oneHandedPose.orientation.z * vrState.rightController.aimPose.orientation.z +
                            s_oneHandedPose.orientation.w * vrState.rightController.aimPose.orientation.w;
                float qSign = (dot < 0.0f) ? -1.0f : 1.0f;

                s_oneHandedPose.orientation.x += alpha * (qSign * vrState.rightController.aimPose.orientation.x - s_oneHandedPose.orientation.x);
                s_oneHandedPose.orientation.y += alpha * (qSign * vrState.rightController.aimPose.orientation.y - s_oneHandedPose.orientation.y);
                s_oneHandedPose.orientation.z += alpha * (qSign * vrState.rightController.aimPose.orientation.z - s_oneHandedPose.orientation.z);
                s_oneHandedPose.orientation.w += alpha * (qSign * vrState.rightController.aimPose.orientation.w - s_oneHandedPose.orientation.w);

                float qNorm = sqrtf(s_oneHandedPose.orientation.x * s_oneHandedPose.orientation.x +
                                    s_oneHandedPose.orientation.y * s_oneHandedPose.orientation.y +
                                    s_oneHandedPose.orientation.z * s_oneHandedPose.orientation.z +
                                    s_oneHandedPose.orientation.w * s_oneHandedPose.orientation.w);
                if (qNorm > 0.0001f) {
                    s_oneHandedPose.orientation.x /= qNorm;
                    s_oneHandedPose.orientation.y /= qNorm;
                    s_oneHandedPose.orientation.z /= qNorm;
                    s_oneHandedPose.orientation.w /= qNorm;
                }

                vrState.rightController.aimPose = s_oneHandedPose;
            }
            s_hasStabilizedPose = false;
        }
    }

    // Laser Tripwire Spatial Shield Deflection (ADR 0059)
    static DWORD s_lastLaserDeflectTime = 0;
    if (s_twoHandedStabilizationActive) {
        bool shieldHeldForward = (rightLocal.z > -0.35f && rightLocal.z < -0.15f) && (fabsf(rightLocal.y) < 0.20f);
        if (shieldHeldForward && (now - s_lastLaserDeflectTime > 150)) {
            s_lastLaserDeflectTime = now;
            HapticRequest(1, (0.30f), kHapticUi); // Laser scatter refraction micro-vibration (220 Hz)
            HapticRequest(0, (0.30f), kHapticUi);
        }
    }

    // 6g. Two-Handed Riot Shotgun Pump Action & Chambering (ADR 0034)
    static bool s_shotgunPumpRackedBack = false;
    static DWORD s_lastPumpActionTime = 0;
    if (s_twoHandedStabilizationActive && !isHarriesStance) {
        float relZ = leftLocal.z - rightLocal.z;
        if (!s_shotgunPumpRackedBack && relZ > 0.06f && (now - s_lastPumpActionTime > 400)) {
            s_shotgunPumpRackedBack = true;
            s_lastPumpActionTime = now;
            VR_LOG_INFO("Physical Gunplay: SHOTGUN PUMP RACKED BACK / CASING EJECTED (relZ: %.2fm)", relZ);
            HapticRequest(0, (0.55f), kHapticLadder); // Mechanical slide rack click (180 Hz)
            HapticRequest(1, (0.35f), kHapticUi);
        } else if (s_shotgunPumpRackedBack && relZ < 0.01f) {
            s_shotgunPumpRackedBack = false;
            s_lastPumpActionTime = now;
            VR_LOG_INFO("Physical Gunplay: SHOTGUN SLIDE PUSHED FORWARD / SHELL CHAMBERED (relZ: %.2fm)", relZ);
            HapticRequest(0, (0.75f), kHapticReload); // Heavy chamber lock snap (240 Hz)
            HapticRequest(1, (0.55f), kHapticLadder);
        }
    } else {
        s_shotgunPumpRackedBack = false;
    }

    // Heavy Riot Shotgun Breaching Hinge Blast & Kick (ADR 0095)
    static DWORD s_lastShotgunBreachTime = 0;
    bool pointBlankLock = (rightLocal.z > -0.38f && rightLocal.z < -0.16f) && (fabsf(rightLocal.y) < 0.15f);
    bool breachForwardThrust = (rVel.z < -1.20f && lVel.z < -1.20f);
    bool triggerBreachPull = (vrState.rightController.trigger > 0.80f);
    if (s_twoHandedStabilizationActive && pointBlankLock && breachForwardThrust && triggerBreachPull && !meleeFiredThisFrame && (now - s_lastShotgunBreachTime > 1800)) { // GH #31
        s_lastShotgunBreachTime = now;
        VR_LOG_INFO("Physical Gunplay: SHOTGUN BREACHING BLAST & DOOR KICK (Rz: %.2f, Lz: %.2f)", rVel.z, lVel.z);
        SendMouseTap(true); // GH #65: held click
        SendKeyTap('W');
        SendKeyTap(VK_SPACE);
        HapticRequest(1, (0.98f), kHapticFinisher); // Point-blank concussive muzzle blast (90 Hz)
        HapticRequest(0, (0.98f), kHapticFinisher);
    }

    // 6h. Mining Core Pneumatic Rock Drill & Industrial Jackhammer Rumble (ADR 0037)
    static DWORD s_lastMiningDrillTime = 0;
    if (s_twoHandedStabilizationActive && (vrState.rightController.trigger > 0.80f) && (vrState.leftController.trigger > 0.80f)) {
        if (now - s_lastMiningDrillTime > 70) {
            s_lastMiningDrillTime = now;
            HapticRequest(1, (0.95f), kHapticFinisher); // Seismic subterranean percussion (65 Hz)
            HapticRequest(0, (0.95f), kHapticFinisher);
        }
    }

    // Heavy Mining Pneumatic Jackhammer Dual-Handle Ground Chisel (ADR 0113)
    static DWORD s_lastGroundChiselTime = 0;
    float dxJackhammer = leftLocal.x - rightLocal.x;
    float dyJackhammer = leftLocal.y - rightLocal.y;
    float dzJackhammer = leftLocal.z - rightLocal.z;
    float distJackhammer = sqrtf(dxJackhammer * dxJackhammer + dyJackhammer * dyJackhammer + dzJackhammer * dzJackhammer);

    bool dualHandleGrip = (distJackhammer >= 0.22f && distJackhammer <= 0.45f) &&
                          (leftLocal.y > -0.45f && leftLocal.y < -0.05f && rightLocal.y > -0.45f && rightLocal.y < -0.05f) &&
                          (vrState.leftController.grip > 0.60f && vrState.rightController.grip > 0.60f);
    bool violentDownwardChiselPlunge = (lVel.y < -1.15f && rVel.y < -1.15f);
    if (dualHandleGrip && violentDownwardChiselPlunge && !meleeFiredThisFrame && (now - s_lastGroundChiselTime > 2000)) { // GH #31
        s_lastGroundChiselTime = now;
        VR_LOG_INFO("Environmental Gesture: MINING PNEUMATIC JACKHAMMER GROUND CHISEL PLUNGE (Ly: %.2f, Ry: %.2f)", lVel.y, rVel.y);
        SendKeyTap('S');
        SendKeyTap(VK_SPACE);
        SendMouseTap(true); // GH #65: held click
        HapticRequest(0, (0.98f), kHapticFinisher);  // Seismic percussive hammer strike (50 Hz)
        HapticRequest(1, (0.98f), kHapticFinisher);
    }

    // 6c. Spatial Weapon Holstering & Physical Inventory Gestures (ADR 0015, 0021, 0023, 0038)
    // - Right Hand Over-Shoulder (Y > 0.15m, Z > -0.05m, grip > 0.50): Equips / Holsters Primary Weapon ('1')
    // - Right Hand Chest Bandolier (Y in [-0.22, 0.05], |X| < 0.14, Z > -0.25, grip > 0.50): Equips Grenades ('4')
    // - Right Hand Low Spine / Back Waist (Y < -0.30m, Z > -0.05m, |X| < 0.15m, grip > 0.50): Equips Heavy / SCAR Gun ('5')
    // - Right Hand Hip Draw (Y < -0.32m, X > 0.14m, grip > 0.50): Equips / Holsters Sidearm / Pistol ('2')
    // - Left Hand Hip Draw (Y < -0.32m, X < -0.14m, grip > 0.50): Equips / Draws Melee Blades / Ulaks ('3')
    // - Hand at Ankle Boot (Y < -0.65m, grip > 0.50) while Crouched: Stashes / Retrieves Contraband (ADR 0038)
    static DWORD s_lastHolsterTime = 0;
    // ADR 0376: 1: Primary, 2: Sidearm, 3: Melee/Blades, 4: Ordnance, 5: Heavy
    // (file scope: section 0 head-look melee gate reads it before section 6c).
    // Track C #14: southpaw mirrors dom/off-hand holster zones (X-mirrored copies
    // so physical-side predicates keep working; haptics stay on physical hands).
    RiddickVRVector3 domHolster = g_vrConfig.southpaw ? leftLocal : rightLocal;
    RiddickVRVector3 offHolster = g_vrConfig.southpaw ? rightLocal : leftLocal;
    float domHolsterGrip = g_vrConfig.southpaw ? vrState.leftController.grip : vrState.rightController.grip;
    float offHolsterGrip = g_vrConfig.southpaw ? vrState.rightController.grip : vrState.leftController.grip;
    if (g_vrConfig.southpaw) { domHolster.x = -domHolster.x; offHolster.x = -offHolster.x; }
    bool inGuardPose = (leftLocal.z < -0.10f && rightLocal.z < -0.10f && leftLocal.y > -0.38f && rightLocal.y > -0.38f && fabsf(leftLocal.x - rightLocal.x) < 0.50f);
    if (!isClimbingActive && !s_physicalBlockEngaged && !inGuardPose && !s_twoHandedStabilizationActive && (now - s_lastHolsterTime > 900)) {
        bool rightOverShoulder = (domHolster.y > 0.15f && domHolster.z > -0.05f && domHolsterGrip > 0.50f); // Track C #14
        bool rightAtChest = (domHolster.y >= -0.45f && domHolster.y <= -0.22f && fabsf(domHolster.x) < 0.14f && domHolster.z > -0.16f && domHolsterGrip > 0.50f); // Track C #14
        bool rightLowSpine = (domHolster.y < -0.30f && domHolster.z > -0.05f && fabsf(domHolster.x) < 0.15f && domHolsterGrip > 0.50f); // Track C #14
        bool rightAtHip = (domHolster.y < -0.32f && domHolster.x > 0.14f && domHolsterGrip > 0.50f); // Track C #14
        bool leftAtHip = (offHolster.y < -0.32f && offHolster.x < -0.14f && offHolsterGrip > 0.50f); // Track C #14
        bool leftAtBootSheath = (offHolster.y < -0.60f && offHolster.x >= -0.25f && offHolster.x <= 0.0f && offHolsterGrip > 0.50f); // Track C #14
        bool rightAtBootSheath = (domHolster.y < -0.60f && domHolster.x >= -0.25f && domHolster.x <= 0.0f && domHolsterGrip > 0.50f); // Track C #14
        bool rightAtAnkle = (domHolster.y < -0.65f && domHolsterGrip > 0.50f); // Track C #14
        bool leftAtAnkle = (offHolster.y < -0.65f && offHolsterGrip > 0.50f); // Track C #14

        if (rightOverShoulder) {
            s_lastHolsterTime = now;
            s_activeWeaponSlot = 1;
            VR_LOG_INFO("Holster Gesture: PRIMARY WEAPON slot toggled (Over-Shoulder)");
            SendKeyTap('1');
            HapticRequest(1, (0.75f), kHapticReload); // Solid mechanical latch pulse
        } else if (rightAtChest) {
            s_lastHolsterTime = now;
            s_activeWeaponSlot = 4;
            VR_LOG_INFO("Holster Gesture: TACTICAL GRENADE slot toggled (Chest Bandolier)");
            SendKeyTap('4');
            HapticRequest(1, (0.65f), kHapticLadder); // Grenade bandolier snap pulse
        } else if (rightLowSpine) {
            s_lastHolsterTime = now;
            s_activeWeaponSlot = 5;
            VR_LOG_INFO("Holster Gesture: HEAVY WEAPON / SCAR GUN slot toggled (Low Spine)");
            SendKeyTap('5');
            HapticRequest(1, (0.85f), kHapticReload); // Heavy industrial magnetic clamp pulse
        } else if ((s_posture.isPhysicallyCrouched || s_posture.isDeepProne) && (leftAtBootSheath || rightAtBootSheath)) {
            s_lastHolsterTime = now;
            s_activeWeaponSlot = 3;
            VR_LOG_INFO("Holster Gesture: CONCEALED SHIV DRAWN FROM BOOT SHEATH (Y: %.2fm)", leftAtBootSheath ? offHolster.y : domHolster.y);
            SendKeyTap('3'); // Equip shiv/knife slot
            if (leftAtBootSheath) HapticRequest(0, (0.45f), kHapticUi); // Leather boot sheath friction click (160 Hz)
            if (rightAtBootSheath) HapticRequest(1, (0.45f), kHapticUi);
        } else if ((s_posture.isPhysicallyCrouched || s_posture.isDeepProne) && (rightAtAnkle || leftAtAnkle)) {
            s_lastHolsterTime = now;
            VR_LOG_INFO("Holster Gesture: CONTRABAND ANKLE BOOT POUCH accessed (HandY: %.2fm)", rightAtAnkle ? domHolster.y : offHolster.y);
            SendKeyTap(VK_TAB); // Cube PDA / contraband manifest
            if (rightAtAnkle) HapticRequest(1, (0.35f), kHapticUi); // Subtle fabric rustle (140 Hz)
            if (leftAtAnkle) HapticRequest(0, (0.35f), kHapticUi);
        } else if (rightAtHip) {
            s_lastHolsterTime = now;
            s_activeWeaponSlot = 2;
            VR_LOG_INFO("Holster Gesture: SIDEARM / PISTOL slot toggled (Right Hip)");
            SendKeyTap('2');
            HapticRequest(1, (0.65f), kHapticLadder); // Leather holster draw pulse
        } else if (leftAtHip) {
            s_lastHolsterTime = now;
            s_activeWeaponSlot = 3;
            VR_LOG_INFO("Holster Gesture: MELEE / ULAK slot toggled (Left Hip)");
            SendKeyTap('3');
            HapticRequest(0, (0.70f), kHapticReload); // Metallic blade unsheathe pulse
        }
    }

    // 6e. Tactical Grenade Priming & Pin-Pulling Gesture (ADR 0021)
    // Bringing off-hand to right hand (< 16cm) with Left Grip pulls the grenade pin (mouse LMB Down cook)
    static bool s_grenadePrimed = false;
    static DWORD s_lastPinPullTime = 0;
    if (!isClimbingActive && !s_physicalBlockEngaged && !s_twoHandedStabilizationActive) {
        float handProximity = sqrtf(
            powf(leftLocal.x - rightLocal.x, 2) +
            powf(leftLocal.y - rightLocal.y, 2) +
            powf(leftLocal.z - rightLocal.z, 2)
        );

        if (!s_grenadePrimed && handProximity < 0.16f && vrState.leftController.grip > 0.50f && !meleeFiredThisFrame && (now - s_lastPinPullTime > 1200)) { // GH #31
            s_lastPinPullTime = now;
            s_grenadePrimed = true;
            VR_LOG_INFO("Tactical Gesture: GRENADE PIN PULLED / PRIMED (Hand Proximity: %.2fm)", handProximity);
            SendMouseClick(true, true); // GH #33: foreground-guarded (was mouse_event LEFTDOWN) // Begin throw cook
            HapticRequest(0, (0.75f), kHapticReload); // Sharp metallic cotter pin friction (180 Hz)
            HapticRequest(1, (0.50f), kHapticUi);
        } else if (s_grenadePrimed && (rSpeed > 1.2f || vrState.rightController.trigger < 0.20f)) {
            // Throw release: dynamic forward arm swing or releasing right hand
            s_grenadePrimed = false;
            VR_LOG_INFO("Tactical Gesture: GRENADE LAUNCHED / THROWN (Speed: %.2f m/s)", rSpeed);
            SendMouseClick(true, false); // GH #33: foreground-guarded (was mouse_event LEFTUP)
            HapticRequest(1, (0.60f), kHapticLadder); // Throw release impulse
        }
    }

    // 6f. SCAR Rifle Air Mine Detonation (ADR 0023)
    // Squeezing off-hand trigger + grip clench while two-handed detonates armed SCAR compressed air mines!
    static bool s_scarDetonatorDown = false;
    static DWORD s_lastScarDetonateTime = 0;
    if (s_twoHandedStabilizationActive) {
        bool wantDetonate = (vrState.leftController.trigger > 0.85f && vrState.leftController.grip > 0.60f);
        if (wantDetonate && !s_scarDetonatorDown && (now - s_lastScarDetonateTime > 600)) {
            s_lastScarDetonateTime = now;
            s_scarDetonatorDown = true;
            VR_LOG_INFO("Tactical Gesture: SCAR AIR MINE DETONATION TRIGGERED (Off-Hand Clench)");
            SendMouseTap(false); // GH #65: held click
            HapticRequest(0, (0.95f), kHapticFinisher); // Heavy explosive shockwave (280 Hz)
            HapticRequest(1, (0.95f), kHapticFinisher);
        } else if (!wantDetonate) {
            s_scarDetonatorDown = false;
        }
    }

    // 6d. Interactive Environmental Manipulation, Vent Prying, and Retinal Biometrics (ADR 0016)
    // - Physical Vent Grate Prying: Both hands extended forward (Z < -0.20m) and pulling backward (Vz > +0.28 m/s) with grips held
    // - Biometric Terminal / Retinal Poke: Index pointing forward into terminal plane (Z < -0.34m, Vz < -0.28 m/s)
    // GH #17 step 11: detection delegated to UpdateEnvGestures (explicit
    // EnvState); side effects (keys/haptics/logs) stay at call site.
    EnvIntent envIntent = UpdateEnvGestures(s_env, now,
        leftLocal, rightLocal, lVel, rVel,
        vrState.leftController.grip, vrState.rightController.grip,
        vrState.leftController.trigger, vrState.rightController.trigger,
        isClimbingActive, s_physicalBlockEngaged, s_twoHandedStabilizationActive);
    if (envIntent.ventPry) {
        VR_LOG_INFO("Environmental Gesture: VENTILATION GRATE PRY / DUCT OPEN executed (Lz: %.2f, Rz: %.2f)", envIntent.ventLz, envIntent.ventRz);
        SendKeyTap('E');
        SendKeyTap(VK_SPACE);
        HapticRequest(0, (0.75f), kHapticReload); // Heavy mechanical wrenching pulse
        HapticRequest(1, (0.75f), kHapticReload);
    } else {
        if (envIntent.bioRight) {
            VR_LOG_INFO("Environmental Gesture: BIOMETRIC TERMINAL TOUCH / RETINAL SCANNER (Right Hand, Z: %.2fm)", envIntent.bioRightZ);
            SendKeyTap('E');
            HapticRequest(1, (0.40f), kHapticUi); // Digital tactile beep
        } else if (envIntent.bioLeft) {
            VR_LOG_INFO("Environmental Gesture: BIOMETRIC TERMINAL TOUCH / RETINAL SCANNER (Left Hand, Z: %.2fm)", envIntent.bioLeftZ);
            SendKeyTap('E');
            HapticRequest(0, (0.40f), kHapticUi); // Digital tactile beep
        } else if (envIntent.nanomedRight) {
            VR_LOG_INFO("Environmental Gesture: NANOMED MEDICAL REGENERATION STATION TOUCH (Right Palm, Z: %.2fm)", envIntent.nanomedRightZ);
            SendKeyTap('E');
            HapticRequest(1, (0.50f), kHapticUi); // Bio-restorative soothing haptic wave
        } else if (envIntent.nanomedLeft) {
            VR_LOG_INFO("Environmental Gesture: NANOMED MEDICAL REGENERATION STATION TOUCH (Left Palm, Z: %.2fm)", envIntent.nanomedLeftZ);
            SendKeyTap('E');
            HapticRequest(0, (0.50f), kHapticUi); // Bio-restorative soothing haptic wave
        }

// GH #16/#17-step-12: 3019-line legacy procedural environmental gesture block
// (ADR 0398-disabled) removed; live prefix lives in UpdateEnvGestures. History in git.
        }

    // 7. Tactical Eyeshine & Flashlight Gestures (ADR 0012)
    // - Right Hand Temple Tap (Y > 0.08m, X > 0.12m): Toggles Eyeshine / Night Vision ('Q') with 0.85 haptic pulse
    // - Left Hand Shoulder/Temple Tap (Y > 0.06m, X < -0.12m): Toggles Tactical Shoulder Flashlight ('F') with 0.75 haptic pulse
    static DWORD s_lastEyeshineTime = 0;
    static DWORD s_lastFlashlightTime = 0;

    if (!isClimbingActive) {
        // Right Hand -> Eyeshine Night Vision (ADR 0380: 3D Anatomical Bounding Box)
        bool rightAtTemple = (rightLocal.y >= 0.06f && rightLocal.y <= 0.22f) &&
                             (rightLocal.x >= 0.12f && rightLocal.x <= 0.28f) &&
                             (rightLocal.z >= -0.15f && rightLocal.z <= 0.08f) &&
                             (vrState.rightController.grip > 0.5f);
        if (rightAtTemple && (now - s_lastEyeshineTime > 1200)) {
            s_lastEyeshineTime = now;
            s_eyeshineActive = !s_eyeshineActive;
            VR_LOG_INFO("Tactical Gesture: EYESHINE %s (Right Temple Tap)", s_eyeshineActive ? "ACTIVATED" : "DEACTIVATED");
            SendGestureTap('Q', "tactical/eyeshine"); // Track C #15
            HapticRequest(1, (0.85f), kHapticReload); // High-resonance optical buzz
        }

        // Left Hand -> Tactical Shoulder Flashlight (ADR 0380: 3D Anatomical Bounding Box)
        bool leftAtShoulder = (leftLocal.y >= -0.10f && leftLocal.y <= 0.12f) &&
                              (leftLocal.x >= -0.28f && leftLocal.x <= -0.12f) &&
                              (leftLocal.z >= -0.20f && leftLocal.z <= 0.06f) &&
                              (vrState.leftController.grip > 0.5f);
        if (leftAtShoulder && (now - s_lastFlashlightTime > 1200)) {
            s_lastFlashlightTime = now;
            s_flashlightActive = !s_flashlightActive;
            VR_LOG_INFO("Tactical Gesture: SHOULDER FLASHLIGHT %s (Left Shoulder Tap)", s_flashlightActive ? "ACTIVATED" : "DEACTIVATED");
            SendGestureTap('F', "tactical/flashlight"); // Track C #15
            HapticRequest(0, (0.75f), kHapticReload); // Mechanical click pulse
        }

        // Tactical Radio Transceiver Intercept / Eavesdropping (ADR 0071)
        static DWORD s_radioHoldStartTime = 0;
        static DWORD s_lastRadioInterceptTime = 0;
        bool rightAtEar = (rightLocal.x > 0.16f && rightLocal.y > 0.05f && rightLocal.y < 0.22f && rightLocal.z > -0.15f && rightLocal.z < 0.08f);
        bool leftAtEar = (leftLocal.x < -0.16f && leftLocal.y > 0.05f && leftLocal.y < 0.22f && leftLocal.z > -0.15f && leftLocal.z < 0.08f);
        bool earGripHeld = (rightAtEar && vrState.rightController.grip > 0.50f) || (leftAtEar && vrState.leftController.grip > 0.50f);

        if (earGripHeld) {
            if (s_radioHoldStartTime == 0) {
                s_radioHoldStartTime = now;
            } else if ((now - s_radioHoldStartTime >= 120) && (now - s_lastRadioInterceptTime > 1200)) {
                s_lastRadioInterceptTime = now;
                VR_LOG_INFO("Tactical Intercept: GUARD RADIO TRANSCEIVER EAVESDROPPING ENGAGED");
                SendGestureTap(VK_TAB, "tactical/radio-intercept"); // Track C #15
                if (rightAtEar) HapticRequest(1, (0.40f), kHapticUi); // Squelch burst (220 Hz)
                if (leftAtEar) HapticRequest(0, (0.40f), kHapticUi);
            }
        } else {
            s_radioHoldStartTime = 0;
        }

        // EVA Spacewalk Solar Radiation Shield Visor Downward Pull (ADR 0086)
        static DWORD s_lastSolarVisorTime = 0;
        bool rightAtBrow = (rightLocal.y > 0.18f && rightLocal.z > -0.18f && rightLocal.z < 0.05f && fabsf(rightLocal.x) < 0.20f && vrState.rightController.grip > 0.50f);
        bool leftAtBrow = (leftLocal.y > 0.18f && leftLocal.z > -0.18f && leftLocal.z < 0.05f && fabsf(leftLocal.x) < 0.20f && vrState.leftController.grip > 0.50f);
        bool visorPullDown = (rightAtBrow && rVel.y < -0.55f) || (leftAtBrow && lVel.y < -0.55f);

        if (visorPullDown && (now - s_lastSolarVisorTime > 1200)) {
            s_lastSolarVisorTime = now;
            s_eyeshineActive = !s_eyeshineActive; // GH #35: visor toggles the single source
            VR_LOG_INFO("Orbital EVA Gesture: POLARIZED SOLAR RADIATION VISOR PULLED DOWN");
            SendGestureTap('Q', "tactical/visor"); // Track C #15
            if (rightAtBrow) HapticRequest(1, (0.65f), kHapticLadder); // Solid mechanical visor latch click (150 Hz)
            if (leftAtBrow) HapticRequest(0, (0.65f), kHapticLadder);
        }
    }

    vrState.eyeshineActive = s_eyeshineActive;
    vrState.flashlightActive = s_flashlightActive;

    // 8. Aim & Block (Unified Physical Boxer Guard + Left Trigger Aim/Block)
    // In Riddick:
    // Right Mouse Button (ACTION_SECONDARYFIRE 241) = Guard / Block when unarmed/melee, Aim/Zoom when holding firearms.
    // GH #17 step 8: detection delegated to UpdateAim (explicit AimState,
    // no behavior change); side effects (clicks/haptics/logs) stay here.
    // Track C #14 southpaw: dominant controller owns the trigger.
    bool aimReloadActive = (now - s_reload.lastReloadGestureTime <= 1100);
    AimIntent aimIntent = UpdateAim(s_aim, now,
        (g_vrConfig.southpaw ? vrState.leftController.trigger : vrState.rightController.trigger),
        offCtrl.trigger, offCtrl.grip,
        aimReloadActive, s_physicalBlockEngaged, isClimbingActive,
        s_twoHandedStabilizationActive, s_activeWeaponSlot,
        s_melee.lastPunchTime, s_melee.lastFinisherTime, s_melee.currentBlockEngaged);
    if (aimIntent.fireChanged) {
        SendMouseClick(true, aimIntent.fireDown); // GH #33: SendInput helper
        if (aimIntent.fireDown) {
            // ADR 0376: Weapon Slot Recoil & Haptic Profile Calibration
            s_headlook.recoilKickPitch = aimIntent.recoilKickPitch;
            HapticRequest(1, aimIntent.fireHapticRightAmp, aimIntent.fireHapticRightPrio);
            if (aimIntent.fireHapticLeftAmp > 0.0f) {
                HapticRequest(0, aimIntent.fireHapticLeftAmp, aimIntent.fireHapticLeftPrio);
            }
        }
    }

    // Sustained Automatic Fire & Mech Rotary Cannon Haptic Feedback (ADR 0024):
    // Automatic fire / Rotary chaingun cycling every 90ms
    if (aimIntent.cyclicPulse) {
        HapticRequest(1, (0.68f), kHapticLadder);
        if (aimIntent.foregripCyclic) {
            HapticRequest(0, (0.42f), kHapticAmbient); // Foregrip cyclic transmission
        }
    }

    // Heavy Mech Rotary Chaingun Dual-Hand Recoil Compensation (ADR 0107)
    if (aimIntent.recoilComp) {
        HapticRequest(1, (0.65f), kHapticLadder); // Mechanical bolt cycling (160 Hz)
        HapticRequest(0, (0.70f), kHapticReload);  // Heavy hydraulic chassis counter-torque (110 Hz)
        s_headlook.recoilKickPitch *= 0.35f;       // Active kinematic muzzle climb clamp
    }

    // Security Tranq Gun Tactile Capacitor Charging (ADR 0039)
    if (aimIntent.tranqCharging) {
        HapticRequest(1, aimIntent.tranqChargeAmp, kHapticUi); // Escalating electrostatic capacitor hum (80 -> 260 Hz)
    }
    if (aimIntent.tranqDischarged) {
        VR_LOG_INFO("Tactical Weapon: TRANQ GUN PEAK CHARGE DART DISCHARGED (Charge: %ums)", aimIntent.tranqChargeTotalMs);
        HapticRequest(1, (0.85f), kHapticReload); // Electric discharge snap (280 Hz)
    }

    // Block / Aim: Engaged physically via Boxer Guard pose OR manually via Left Trigger
    s_isAimingOrBlocking = aimIntent.wantBlock;
    if (aimIntent.blockEngage) {
        // GH #31: punch recovery covers reactive strikes and finishers.
        SendMouseClick(false, true); // GH #33: foreground-guarded (was mouse_event RIGHTDOWN)
        s_melee.currentBlockEngaged = true;
        if (aimIntent.blockEngagePhysical) {
            VR_LOG_INFO("Combat State: BOXER GUARD / BLOCK MODE engaged (Physical Gesture)");
            HapticRequest(0, (0.35f), kHapticUi);
            HapticRequest(1, (0.35f), kHapticUi);
        } else {
            VR_LOG_INFO("Combat State: AIM / BLOCK engaged (Left Trigger)");
            HapticRequest(0, (0.3f), kHapticUi);
        }
    } else if (aimIntent.blockDisengage) {
        SendMouseClick(false, false); // GH #33: foreground-guarded (was mouse_event RIGHTUP)
        s_melee.currentBlockEngaged = false;
        VR_LOG_INFO("Combat State: AIM / BLOCK disengaged");
    }

    // 9. Left & Right Grip Mapping (Interactions, Physical Throwing, Body Dragging, Climbing, Reload - ADR 0017 & 0020)
    // GH #17 step 5: detection delegated to UpdateGrips (explicit GripsState, no behavior change).
    GripsIntent gripsIntent = UpdateGrips(s_grips, now,
        vrState.leftController.grip, vrState.rightController.grip,
        vrState.rightController.trigger,
        leftLocal, rightLocal, lVel, rVel, lSpeed,
        vrState.leftController.thumbstickX, vrState.leftController.thumbstickY,
        isClimbingActive, s_twoHandedStabilizationActive, s_physicalBlockEngaged,
        meleeFiredThisFrame,
        s_posture.isPhysicallyCrouched, s_posture.isDeepProne, s_posture.isStealthPeeking,
        s_headlook.lastHmdDeltaYaw);

    // Left Grip -> Grab / Secondary Interact ('E') / Physical Object Throwing / Body Dragging
    // (release evaluated at moment of release, ADR 0382: Forward Trajectory Vector Gating)
    if (gripsIntent.leftThrow) {
        VR_LOG_INFO("Physical Gesture: OBJECT THROW / PROPULSION executed (Speed: %.2f m/s, Vel: [%.2f, %.2f, %.2f])",
            gripsIntent.throwSpeed, gripsIntent.throwVel.x, gripsIntent.throwVel.y, gripsIntent.throwVel.z);
        SendMouseTap(true); // GH #65: held click
        HapticRequest(0, (0.60f), kHapticLadder); // Tactile release impulse
    } else if (gripsIntent.leftDrop) {
        SendKey('E', false); // Gentle drop at feet
    }
    if (gripsIntent.leftGrab) {
        SendKey('E', true);
        HapticRequest(0, (0.35f), kHapticUi); // Object grab latch
    }

    // Inmate Gambling Dice Shaking & Roll Throwing (ADR 0045)
    if (gripsIntent.diceRattle) {
        HapticRequest(0, (0.30f), kHapticUi); // Rapid bone-on-bone dice rattle micro-tap (160 Hz)
    }

    // Interactive Inmate Gambling Card Flipping & Dealing (ADR 0075)
    if (gripsIntent.cardDeal) {
        VR_LOG_INFO("Diegetic Interaction: INMATE GAMBLING CARD FLIPPED / DEALT (Vx: %.2f, Vz: %.2f)", gripsIntent.cardVx, gripsIntent.cardVz);
        SendKeyTap('E');
        SendKeyTap(VK_RETURN);
        HapticRequest(1, (0.35f), kHapticUi); // Crisp paper card friction micro-haptic (240 Hz)
    }

    // Inmate Pit Brawl Dirt / Ash Throw Blind (ADR 0065)
    if (gripsIntent.ashThrow) {
        VR_LOG_INFO("Combat Dirty Tactic: DIRT / ASH THROWN INTO ENEMY EYES (Vy: %.2f, Vz: %.2f)", gripsIntent.ashVy, gripsIntent.ashVz);
        SendKeyTap('E');
        SendMouseTap(true); // GH #65: held click
        HapticRequest(1, (0.50f), kHapticUi); // Fine particulate scatter burst (180 Hz)
    }

    // Inmate Pit Brawl Sand / Ash Throw Two-Handed Fan Scatter (ADR 0085)
    if (gripsIntent.fanScatter) {
        VR_LOG_INFO("Combat Dirty Tactic: DUAL-HANDED SAND / ASH FAN SCATTER (Lx: %.2f, Rx: %.2f)", gripsIntent.fanLx, gripsIntent.fanRx);
        SendKeyTap('E');
        SendMouseTap(true); // GH #65: held click
        HapticRequest(0, (0.65f), kHapticLadder); // Dual wide-arc particulate scatter pulses (180 Hz)
        HapticRequest(1, (0.65f), kHapticLadder);
    }

    // Inmate Pit Brawl Dirt / Ash Two-Handed Fan Clench & Burst (ADR 0104)
    if (gripsIntent.fanClenchEngaged) {
        HapticRequest(0, (0.45f), kHapticUi);  // Friction grind during clench (200 Hz)
        HapticRequest(1, (0.45f), kHapticUi);
    }
    if (gripsIntent.fanBurst) {
        VR_LOG_INFO("Combat Dirty Tactic: DIRT/ASH TWO-HANDED FAN CLENCH & BURST (Lx: %.2f, Rx: %.2f)", gripsIntent.burstLx, gripsIntent.burstRx);
        SendKeyTap('E');
        SendMouseTap(true); // GH #65: held click
        SendKeyTap(VK_TAB);
        HapticRequest(0, (0.70f), kHapticReload);  // Wide particulate cloud rush (175 Hz)
        HapticRequest(1, (0.70f), kHapticReload);
    }

    // Inmate Pit Brawl Dirt / Ash Eye Recovery Blink Gesture (ADR 0073)
    if (gripsIntent.eyeRecovery) {
        VR_LOG_INFO("Combat Defense: CORNEAL BLINK / DIRT WIPED FROM EYES (%s)",
            gripsIntent.eyeHeadShake ? "Rapid Head Shake" : "Forearm Eye Wipe");
        SendKeyTap(VK_SPACE); // Visual overlay recovery accelerator
        HapticRequest(1, (0.30f), kHapticUi); // Fine corneal blink micro-haptic (180 Hz)
        HapticRequest(0, (0.30f), kHapticUi);
    }

    // Kinematic Body / Heavy Object Drag Resistance (ADR 0020):
    // When crouched and hauling an object/corpse backwards or sideways, emit periodic drag friction pulses
    if (gripsIntent.dragPulse) {
        HapticRequest(0, (0.38f), kHapticAmbient); // Low-frequency drag friction rumble (85 Hz)
    }

    // Diegetic Collectible Inspection: Cigarette Packs & Bounty Cards (ADR 0029)
    // Holding item in off-hand up to eye level (Y > -0.05m, Z > -0.32m) engages inspection
    if (gripsIntent.inspectChanged && gripsIntent.inspectEngaged) {
        VR_LOG_INFO("Diegetic Interaction: COLLECTIBLE INSPECTION / CIGARETTE PACK (Eye Level: %.2fm)", gripsIntent.inspectY);
        HapticRequest(0, (0.25f), kHapticUi); // Tactile inspection buzz (180 Hz)
    }

    // Interactive Guard Interrogation: Physical Throat-Grip Shakedown (ADR 0032)
    // Off-hand held forward at collar/chest height (Z in [-0.60, -0.22]m, Y in [-0.25, +0.15]m) with Left Grip
    if (gripsIntent.throatEngaged) {
        VR_LOG_INFO("Diegetic Interaction: NPC THROAT / COLLAR GRIP engaged (Pos: [%.2f, %.2f, %.2f])",
            gripsIntent.throatX, gripsIntent.throatY, gripsIntent.throatZ);
        HapticRequest(0, (0.45f), kHapticUi); // Throaty struggle pulse (120 Hz)
    }
    // Physical Forward/Back Shake: |lVel.z| > 0.70 m/s
    if (gripsIntent.shakedown) {
        VR_LOG_INFO("Diegetic Interaction: PHYSICAL SHAKEDOWN GESTURE executed (VelZ: %.2f m/s)", gripsIntent.shakeVelZ);
        SendKeyTap('E'); // Advance extortion/interrogation dialogue
        HapticRequest(0, (0.75f), kHapticReload); // Compliance snap haptic (220 Hz)
    }
    if (gripsIntent.throatReleased) {
        VR_LOG_INFO("Diegetic Interaction: NPC THROAT GRIP released");
    }

    // Guard Subdue Chokehold & Silent Neck Snap (ADR 0047)
    // Sneaking in crouch/prone with left arm wrapped across chest level (Z in [-0.35, -0.15]m, Y in [-0.20, +0.10]m) with Left Grip
    if (gripsIntent.chokeEngaged) {
        VR_LOG_INFO("Stealth Takedown: REAR CHOKEHOLD GRAPPLE engaged");
        HapticRequest(0, (0.45f), kHapticUi); // Rhythmic struggle pulse (100 Hz)
    }
    if (gripsIntent.chokePulse) {
        HapticRequest(0, (0.40f), kHapticUi);
    }
    // Cervical Twist Finisher: rapid lateral jerk of right hand (|rVel.x| > 0.80 m/s)
    if (gripsIntent.chokeFinisher) {
        VR_LOG_INFO("Stealth Takedown: SILENT CERVICAL NECK SNAP FINISHER executed (Vx: %.2f m/s)", gripsIntent.chokeVx);
        SendMouseTap(true); // GH #65: held click
        HapticRequest(1, (0.95f), kHapticFinisher); // Bone-cracking crunch (280 Hz)
        HapticRequest(0, (0.95f), kHapticFinisher);
    }
    if (gripsIntent.chokeReleased) {
        VR_LOG_INFO("Stealth Takedown: REAR CHOKEHOLD released");
    }

    // Inmate Bunk Mattress Stash Search (ADR 0050)
    if (gripsIntent.bunkSearch) {
        VR_LOG_INFO("Environmental Gesture: INMATE BUNK MATTRESS STASH SEARCH (SweepVx: %.2f m/s)", gripsIntent.bunkSweepVx);
        SendKeyTap('E');
        HapticRequest(1, (0.60f), kHapticUi); // Metallic discovery chime haptic (200 Hz)
        HapticRequest(0, (0.30f), kHapticUi);
    }

    // Inmate Bunk Shiv Sheath Concealment Under Mattress Frame (ADR 0110)
    if (gripsIntent.bunkConceal) {
        VR_LOG_INFO("Environmental Gesture: INMATE BUNK SHIV SHEATH CONCEALED UNDER MATTRESS FRAME (Vz: %.2f m/s)", gripsIntent.concealVz);
        SendKeyTap('3'); // Holster / stow shiv weapon
        SendKeyTap('E');
        HapticRequest(0, (0.35f), kHapticUi);  // Mattress wire tension hum (120 Hz)
        HapticRequest(1, (0.60f), kHapticLadder); // Spring-steel sheath latch click (240 Hz)
    }

    // Right Grip -> Weapon Reload ('R') (suppressed during ladder climb)
    if (gripsIntent.rightChanged) {
        SendKey('R', gripsIntent.rightDown);
        if (gripsIntent.rightDown) HapticRequest(1, (0.5f), kHapticUi);
    }

    // 10. Face Buttons Mapping (ADR 0410: Unified Action & Dialogue Confirmation)
    // GH #17 step 6: detection delegated to UpdateButtons (explicit ButtonsState, no behavior change).
    ButtonsIntent buttonsIntent = UpdateButtons(s_buttons, btns, s_prevButtons,
        vrState.leftController.grip, vrState.rightController.grip);

    // BUTTON A -> Primary Interact / Use / Talk / Climb / Open ('E') + Dialogue Confirm (VK_RETURN)
    if (g_vrConfig.interactButton == 0 && buttonsIntent.aChanged) {
        SendAction(VRAction::Interact, buttonsIntent.aDown);
        SendAction(VRAction::Confirm, buttonsIntent.aDown);
        if (buttonsIntent.aDown) HapticRequest(1, 0.35f, kHapticUi);
    }
    static bool s_prevInteractYLevel = false;
    if (g_vrConfig.interactButton == 1 && leftYDown != s_prevInteractYLevel) {
        s_prevInteractYLevel = leftYDown;
        SendAction(VRAction::Interact, leftYDown);
        SendAction(VRAction::Confirm, leftYDown);
        if (leftYDown) HapticRequest(0, 0.35f, kHapticUi);
    }

    // BUTTON B -> Jump (Space) & Crouch-Jump Mantle Clearance (ADR 0025)
    if (buttonsIntent.bChanged) {
        if (buttonsIntent.bDown && s_posture.isPhysicallyCrouched) {
            VR_LOG_INFO("Movement Stance: PHYSICAL CROUCH-JUMP / MANTLE CLEARANCE executed");
            SendKey('C', true);
            SendKey(VK_SPACE, true);
            HapticRequest(1, (0.45f), kHapticUi);
        } else {
            SendKey(VK_SPACE, buttonsIntent.bDown);
        }
        if (buttonsIntent.bDown && !s_posture.isPhysicallyCrouched) HapticRequest(1, (0.30f), kHapticAmbient);
    }

    // BUTTON X -> Crouch Toggle ('C')
    if (buttonsIntent.xPressed) {
        SendKeyTap('C');
        HapticRequest(0, (0.30f), kHapticUi);
    }

    // Dual Thumbstick Click -> Emergency Ejection / Drone Neural Disconnect (ADR 0031)
    // GH #20: single edge-triggered Menu handler (computed once in UpdateButtons); a Menu
    // edge in the same frame suppresses the dual-stick ESC so VK_ESCAPE fires once.
    if (buttonsIntent.dualStickPressed) {
        if (buttonsIntent.dualGrip) {
            // Emergency Mech Cockpit Purge / Canopy Blow (ADR 0049)
            VR_LOG_INFO("Cockpit Systems: EMERGENCY MECH CANOPY BOLTS BLOWN / PILOT EJECTION");
            SendKeyTap(VK_ESCAPE);
            SendKeyTap('E');
            SendKeyTap(VK_SPACE);
            HapticRequest(0, (0.95f), kHapticFinisher); // Explosive canopy bolt decompression
            HapticRequest(1, (0.95f), kHapticFinisher);
        } else {
            VR_LOG_INFO("Cybernetic Uplink: EMERGENCY NEURAL EJECTION / DRONE DISCONNECT triggered (Dual-Stick Click)");
            SendKeyTap(VK_ESCAPE);
            SendKeyTap('E');
            HapticRequest(0, (0.85f), kHapticReload);  // Intense static burst (280 Hz)
            HapticRequest(1, (0.85f), kHapticReload);
        }
    } else {
        // Left Thumbstick Click -> Sprint Toggle is handled in locomotion section.
        // Crouch toggle is mapped to Button X and physical crouch stance.

        // Right Thumbstick Click -> Recenter HMD & Reset Standing Eye Height (GH #18: no cinema)
        // Gated by grip so toggling snap/smooth turn mode does not unintentionally reset view
        if (buttonsIntent.recenterPressed) {
            vrState.shouldRecenter = true;
            s_headlook.hasPrevHmdOrientation = false;
            if (vrState.hmdPose.isValid) {
                s_posture.standingEyeHeight = vrState.hmdPose.position.y + (g_vrConfig.seated ? g_vrConfig.seatedEyeOffset : 0.0f); // Track C #11 + GH #37 edge
                VR_LOG_INFO("Right Stick Click: HMD recentered and standing eye height recalibrated to %.2fm", s_posture.standingEyeHeight);
            }
            HapticRequest(1, (0.6f), kHapticLadder);
        }
    }

    // Left Controller Menu Button -> Clean Pause / Resume Menu Toggle (Escape)
    // GH #18/#20: the single Menu handler (edge-triggered, logs once).
    if (buttonsIntent.menuEdge) {
        SendKeyTap(VK_ESCAPE);
        HapticRequest(0, 0.40f, kHapticUi); // GH #32
        VR_LOG_INFO("Menu Button tapped (VK_ESCAPE)");
    }

    // GH #32: single haptic publish point. Per side the highest-priority
    // request wins (ties -> max amplitude); the winner max-merges over the
    // decayed carry so a fresh pulse never loses to stale state, while an
    // idle frame simply keeps the decayed value.
    if (s_hapticArbiter.HasRequest(0) && s_hapticArbiter.Winner(0) > vrState.leftHapticPulse)
        vrState.leftHapticPulse = s_hapticArbiter.Winner(0);
    if (s_hapticArbiter.HasRequest(1) && s_hapticArbiter.Winner(1) > vrState.rightHapticPulse)
        vrState.rightHapticPulse = s_hapticArbiter.Winner(1);

    s_prevButtons = btns;
}

} // namespace RiddickVR
