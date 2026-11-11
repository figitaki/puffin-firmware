#pragma once

#include <stdint.h>
#include "Connections.h"

// Physical switch state is independent of the currently selected keymap/layer.
class KeyInput {
public:
    using Keymap = int[NumLayers][RowCount][ColCount];

    struct Event {
        int action;
        bool pressed;
    };

    // Collect a complete matrix before resolving any actions with nextEvent.
    void sample(int row, int col, bool down, uint32_t now) {
        // The 51 fitted switches in the Puffin PCB's 5 x 12 matrix.
        static const uint16_t columns[RowCount] = {
            0x1F8, 0xFFF, 0xFFF, 0xFFF, 0xF57
        };
        if (!(columns[row] & (1U << col))) return;

        State &key = keys[row][col];
        if (down != key.raw) {
            key.raw = down;
            key.changedAt = now;
        }
        key.ready = down != key.pressed && uint32_t(now - key.changedAt) >= 5;
    }

    bool nextEvent(const Keymap &keymap, Event &event) {
        // Releases use their captured actions. Then resolve layer presses
        // before ordinary presses, regardless of their matrix positions.
        for (int phase = 0; phase < 3; ++phase) {
            for (int row = 0; row < RowCount; ++row) {
                for (int col = 0; col < ColCount; ++col) {
                    State &key = keys[row][col];
                    if (!key.ready) continue;
                    if (phase == 0 && key.raw) continue;
                    const int action = key.raw ? keymap[layer()][row][col]
                                               : key.action;
                    if (phase == 1 && action != FUNCTION_SW &&
                        action != FUNCTION_SW2) continue;
                    key.ready = false;
                    key.pressed = key.raw;
                    key.action = action;
                    event = {action, key.pressed};
                    return true;
                }
            }
        }
        return false;
    }

    int heldCount(int action) const {
        int count = 0;
        for (const auto &row : keys) {
            for (const auto &key : row) {
                if (key.pressed && key.action == action) ++count;
            }
        }
        return count;
    }

    int layer() const {
        int selected = 0;
        for (const auto &row : keys) {
            for (const auto &key : row) {
                if (!key.pressed) continue;
                if (key.action == FUNCTION_SW2) return 2;
                if (key.action == FUNCTION_SW) selected = 1;
            }
        }
        return selected;
    }

private:
    struct State {
        bool raw = false;
        bool pressed = false;
        bool ready = false;
        uint32_t changedAt = 0;
        int action = 0;
    };

    State keys[RowCount][ColCount] = {};
};
