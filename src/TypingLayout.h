#pragma once

#include <stddef.h>
#include "KeyInput.h"

// Preset IDs are persisted: keep their numeric values stable.
// Presets change letter/punctuation positions; user overrides apply afterward.
// Fn and the upper layers remain physical-position recovery bindings.
class TypingLayout {
public:
    enum Preset { Qwerty = 0, Colemak = 1, Dvorak = 2, Count = 3 };
    static const int StorageSize = 1 + RowCount * ColCount * 2;

    explicit TypingLayout(KeyInput::Keymap &map) : map(map) {
        for (int row = 0; row < RowCount; ++row) {
            for (int col = 0; col < ColCount; ++col) defaults[row][col] = map[0][row][col];
        }
        clearOverrides();
    }

    int selected() const { return current; }

    bool select(int preset) {
        if (preset < 0 || preset >= Count) return false;
        static const char rows[Count][3][11] = {
            {"qwertyuiop", "asdfghjkl;", "zxcvbnm,./"},
            {"qwfpgjluy;", "arstdhneio", "zxcvbkm,./"},
            {"',.pyfgcrl", "aoeuidhtns", ";qjkxbmwvz"}
        };
        for (int row = 0; row < RowCount; ++row) {
            for (int col = 0; col < ColCount; ++col) map[0][row][col] = defaults[row][col];
        }
        for (int row = 0; row < 3; ++row) {
            for (int key = 0; key < 10; ++key) {
                // The up-arrow switch interrupts the bottom typing row.
                const int col = row == 2 && key == 9 ? 11 : key + 1;
                map[0][row + 1][col] = rows[preset][row][key];
            }
        }
        for (int row = 0; row < RowCount; ++row) {
            for (int col = 0; col < ColCount; ++col) {
                if (overrides[row][col] != Unmapped) map[0][row][col] = overrides[row][col];
            }
        }
        current = preset;
        return true;
    }

    int actionAt(int row, int col) const {
        return KeyInput::fitted(row, col) ? map[0][row][col] : -1;
    }

    int overrideAt(int row, int col) const {
        if (!KeyInput::fitted(row, col) || overrides[row][col] == Unmapped) return -1;
        return overrides[row][col];
    }

    bool remap(int row, int col, int action) {
        if (!editable(row, col) || !validAction(action)) return false;
        overrides[row][col] = action;
        select(current);
        return true;
    }

    bool resetKey(int row, int col) {
        if (!editable(row, col)) return false;
        overrides[row][col] = Unmapped;
        select(current);
        return true;
    }

    bool clearOverrides() {
        bool changed = false;
        for (auto &row : overrides) {
            for (auto &action : row) {
                changed |= action != Unmapped;
                action = Unmapped;
            }
        }
        select(current);
        return changed;
    }

    // Version 1: byte 1, then 60 row-major little-endian uint16 actions.
    // 0xFFFF inherits the preset; 0 disables a switch. NVS protects blob integrity.
    void encode(uint8_t (&bytes)[StorageSize]) const {
        bytes[0] = 1;
        int index = 1;
        for (const auto &row : overrides) {
            for (auto action : row) {
                bytes[index++] = action & 0xFF;
                bytes[index++] = action >> 8;
            }
        }
    }

    bool restore(const uint8_t *bytes, size_t size) {
        if (size != StorageSize || bytes[0] != 1) return false;
        // Validate the whole record before applying any part of it.
        uint16_t restored[RowCount][ColCount];
        int index = 1;
        for (int row = 0; row < RowCount; ++row) {
            for (int col = 0; col < ColCount; ++col) {
                const int action = bytes[index] | (uint16_t(bytes[index + 1]) << 8);
                index += 2;
                if (action != Unmapped && (!editable(row, col) || !validAction(action))) return false;
                restored[row][col] = action;
            }
        }
        for (int row = 0; row < RowCount; ++row) {
            for (int col = 0; col < ColCount; ++col) overrides[row][col] = restored[row][col];
        }
        select(current);
        return true;
    }

private:
    static const uint16_t Unmapped = 0xFFFF;
    KeyInput::Keymap &map;
    int defaults[RowCount][ColCount];
    uint16_t overrides[RowCount][ColCount] = {};
    int current = Qwerty;

    bool editable(int row, int col) const {
        return KeyInput::fitted(row, col) && defaults[row][col] != FUNCTION_SW;
    }

    static bool validAction(int action) {
        // Printable US ASCII, explicit BLE HID usages/modifiers, or disabled.
        return action == 0 || (action >= 32 && action <= 126) ||
               (action >= 128 && action <= 255);
    }
};
