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

    bool sample(int row, int col, bool down, uint32_t now,
                const Keymap &keymap, Event &event) {
        // The 51 fitted switches in the Puffin PCB's 5 x 12 matrix.
        static const uint16_t columns[RowCount] = {
            0x1F8, 0xFFF, 0xFFF, 0xFFF, 0xF57
        };
        if (!(columns[row] & (1U << col))) return false;

        State &key = keys[row][col];
        if (down != key.raw) {
            key.raw = down;
            key.changedAt = now;
        }
        if (down == key.pressed || uint32_t(now - key.changedAt) < 5) {
            return false;
        }

        // Resolve once, before changing held-layer state. Release this exact
        // action even if another key changes the layer or the map is edited.
        if (down) key.action = keymap[layer()][row][col];
        key.pressed = down;
        event = {key.action, down};
        return true;
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
        uint32_t changedAt = 0;
        int action = 0;
    };

    State keys[RowCount][ColCount] = {};
};
