#pragma once

#include <stdint.h>

// Own translated HID usages and modifier bits, not just mapped actions.
// Action encoding and ASCII translation match the pinned BLE Keyboard library.
class KeyboardOutput {
public:
    explicit KeyboardOutput(const uint8_t *asciiMap) : asciiMap(asciiMap) {}

    void press(uint8_t action) { update(action, true); }
    void release(uint8_t action) { update(action, false); }

    void report(uint8_t &modifiers, uint8_t (&keys)[6]) const {
        modifiers = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if (modifierOwners[bit]) modifiers |= 1U << bit;
        }
        for (auto &key : keys) key = 0;
        int count = 0;
        for (int usage = 1; usage < 128; ++usage) {
            if (!usageOwners[usage]) continue;
            if (count == 6) {
                // Standard boot-keyboard ErrorRollOver. Keep all owners so
                // the report recovers as soon as six or fewer usages remain.
                for (auto &key : keys) key = 1;
                return;
            }
            keys[count++] = usage;
        }
    }

    // Emit a chord without changing physical owners. A held tap usage must
    // first go up so the host sees a fresh key-down with the chord modifiers.
    // With all six slots occupied by other keys (or rollover), skip the tap
    // rather than releasing an unrelated key to make room.
    template<class Send>
    bool tapUsage(uint8_t usage, uint8_t modifiers, Send send) const {
        uint8_t heldModifiers;
        uint8_t held[6];
        report(heldModifiers, held);
        uint8_t chord[6];
        int slot = -1;
        for (int i = 0; i < 6; ++i) {
            chord[i] = held[i];
            if (held[i] == usage) slot = i;
        }
        if (slot >= 0) {
            chord[slot] = 0;
            send(heldModifiers, chord);
        } else {
            for (int i = 0; i < 6; ++i) {
                if (!held[i]) { slot = i; break; }
            }
        }
        if (slot < 0) return false;
        chord[slot] = usage;
        send(uint8_t(heldModifiers | modifiers), chord);
        send(heldModifiers, held);
        return true;
    }

private:
    const uint8_t *asciiMap;
    uint8_t usageOwners[128] = {};
    uint8_t modifierOwners[8] = {};

    void update(uint8_t action, bool pressed) {
        uint8_t usage = 0;
        uint8_t modifiers = 0;
        if (action >= 136) {
            usage = action - 136;
        } else if (action >= 128) {
            modifiers = 1U << (action - 128);
        } else {
            const uint8_t translated = asciiMap[action];
            usage = translated & 0x7F;
            if (translated & 0x80) modifiers = 0x02;
        }
        if (usage) {
            if (pressed) ++usageOwners[usage];
            else if (usageOwners[usage]) --usageOwners[usage];
        }
        for (int bit = 0; bit < 8; ++bit) {
            if (!(modifiers & (1U << bit))) continue;
            if (pressed) ++modifierOwners[bit];
            else if (modifierOwners[bit]) --modifierOwners[bit];
        }
    }
};
