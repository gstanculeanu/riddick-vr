#include "gesture_tap.h"
#include "vr_config.h"
#include "logging.h"
#include <cstring>

namespace RiddickVR {

static HWND s_gestureHwnd = nullptr;

// GH #68: OpenXR-simulator safety. When the harness sets RIDDICKVR_SIM=1 the
// hook must NOT synthesize desktop input; the sim drives poses programmatically
// and any real SendInput would hijack the user's keyboard. Checked once, cached.
static bool SimModeNoInput() {
    static int cached = -1;
    if (cached < 0) {
        char buf[8] = {};
        cached = (GetEnvironmentVariableA("RIDDICKVR_SIM", buf, sizeof(buf)) > 0) ? 1 : 0;
    }
    return cached == 1;
}

void SetGestureGameHwnd(HWND hwnd) {
    s_gestureHwnd = hwnd;
}

static void SendKey(WORD vk, bool isDown) {
    // GH #68: in simulator mode never inject OS-GLOBAL input (SendInput /
    // keybd_event) -- but STILL post the targeted window message to the game so
    // the harness can advance menus without hijacking the user's desktop.
    const bool sim = SimModeNoInput();
    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);

    if (!sim) {
        // GH #33: foreground guard (mirrors camera_hook IsGameForeground).
        // Fail-open while the game HWND is unknown (tests); SetGestureGameHwnd
        // should be wired to the latched game window when available.
        if (s_gestureHwnd && IsWindow(s_gestureHwnd) && GetForegroundWindow() != s_gestureHwnd) return;

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
    }

    // 4. Direct window message posting to game window (targeted; harness-safe).
    if (s_gestureHwnd && IsWindow(s_gestureHwnd)) {
        LPARAM lParam = (scan << 16) | 1;
        if (!isDown) lParam |= (1 << 30) | (1 << 31);
        PostMessageA(s_gestureHwnd, isDown ? WM_KEYDOWN : WM_KEYUP, vk, lParam);
        if (isDown && (vk == VK_RETURN || vk == 'E' || vk == 'e' || vk == VK_ESCAPE || vk == VK_BACK || vk == VK_TAB || vk == VK_SPACE)) {
            PostMessageA(s_gestureHwnd, WM_CHAR, vk, lParam);
        }
    }
}

void SendKeyTap(WORD vk) {
    SendKey(vk, true);
    Sleep(15);
    SendKey(vk, false);
}

// Fixed-size dedup table (no heap churn on the input hot path).
struct GestureTapEntry {
    char actionId[48];
    WORD vk;
    DWORD lastFireTime;
    bool used;
};

static GestureTapEntry s_tapTable[64] = {};
static DWORD s_dedupDrops = 0;

void SendGestureTap(WORD vk, const char* actionId, DWORD debounceMsOverride) {
    DWORD window = debounceMsOverride;
    if (window < 120 || window > 250)
        window = static_cast<DWORD>(g_vrConfig.gestureDebounceMs);
    if (window < 120) window = 120;
    if (window > 250) window = 250;

    const char* id = (actionId && actionId[0]) ? actionId : "tap/unknown";
    DWORD now = GetTickCount();

    GestureTapEntry* slot = nullptr;
    for (auto& e : s_tapTable) {
        if (e.used && e.vk == vk && strncmp(e.actionId, id, sizeof(e.actionId)) == 0) {
            slot = &e;
            break;
        }
        if (!slot && !e.used)
            slot = &e; // first free slot
    }
    if (!slot)
        slot = &s_tapTable[0]; // table full: evict oldest-ish (slot 0), still debounce-safe

    if (slot->used && slot->vk == vk && strncmp(slot->actionId, id, sizeof(slot->actionId)) == 0) {
        if (now - slot->lastFireTime < window) {
            ++s_dedupDrops;
            return; // same chord inside debounce window: drop duplicate
        }
        slot->lastFireTime = now;
        SendKeyTap(vk);
        return;
    }

    slot->used = true;
    slot->vk = vk;
    strncpy_s(slot->actionId, sizeof(slot->actionId), id, _TRUNCATE);
    slot->lastFireTime = now;
    SendKeyTap(vk);
}

} // namespace RiddickVR
