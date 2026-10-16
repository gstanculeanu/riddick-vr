#pragma once

// GH #65: held-input scheduler (pure, no Win32).
//
// Melee attacks used to send mouse DOWN and UP back to back inside one frame.
// The engine polls input once per tick, so a zero-length click is often never
// seen and the swing does nothing. This scheduler lets the caller send DOWN
// now and release the input a few milliseconds later from the top of a later
// frame, without sleeping on the game thread.
//
// `Press` returns true when the caller must emit the DOWN event now; false
// when the same input is already held (the release time is only extended, so
// no second DOWN is sent), and also when all slots are busy (nothing is
// pressed, so nothing can stick). `CollectDue` and `CollectAll` hand back the inputs
// whose UP must be emitted and clear their slots.

namespace RiddickVR {

enum class HeldInput : unsigned char { MouseLeft, MouseRight, Key };

struct HeldRelease {
    HeldInput kind = HeldInput::MouseLeft;
    unsigned short vk = 0;
    unsigned long releaseAt = 0;
};

class InputHoldScheduler {
public:
    static constexpr int kCapacity = 8;

    bool Press(HeldInput kind, unsigned short vk, unsigned long now, unsigned long holdMs) {
        const unsigned short key = (kind == HeldInput::Key) ? vk : 0;
        for (int i = 0; i < kCapacity; ++i) {
            if (m_used[i] && m_slot[i].kind == kind && m_slot[i].vk == key) {
                const unsigned long until = now + holdMs;
                if (Later(until, m_slot[i].releaseAt)) m_slot[i].releaseAt = until;
                return false;
            }
        }
        for (int i = 0; i < kCapacity; ++i) {
            if (!m_used[i]) {
                m_used[i] = true;
                m_slot[i].kind = kind;
                m_slot[i].vk = key;
                m_slot[i].releaseAt = now + holdMs;
                return true;
            }
        }
        // Table full: refuse to press. An untracked DOWN could never be released.
        return false;
    }

    int CollectDue(unsigned long now, HeldRelease* out, int max) {
        int n = 0;
        for (int i = 0; i < kCapacity && n < max; ++i) {
            if (m_used[i] && !Later(m_slot[i].releaseAt, now)) {
                out[n++] = m_slot[i];
                m_used[i] = false;
            }
        }
        return n;
    }

    int CollectAll(HeldRelease* out, int max) {
        int n = 0;
        for (int i = 0; i < kCapacity && n < max; ++i) {
            if (m_used[i]) {
                out[n++] = m_slot[i];
                m_used[i] = false;
            }
        }
        return n;
    }

private:
    // Wrap-safe "a is after b" for 32-bit millisecond tick counts.
    static bool Later(unsigned long a, unsigned long b) {
        return static_cast<int>(static_cast<unsigned int>(a - b)) > 0;
    }

    HeldRelease m_slot[kCapacity];
    bool m_used[kCapacity] = {};
};

} // namespace RiddickVR
