#pragma once

#include "KeyInput.h"

// Preset IDs are persisted: keep their numeric values stable.
// Only the base layer's letter/punctuation positions change. Fn, number-pad,
// modifiers, navigation and command keys remain physical-position bindings.
class TypingLayout {
public:
    enum Preset { Qwerty = 0, Colemak = 1, Dvorak = 2, Count = 3 };

    explicit TypingLayout(KeyInput::Keymap &map) : map(map) { select(Qwerty); }

    int selected() const { return current; }

    bool select(int preset) {
        if (preset < 0 || preset >= Count) return false;
        static const char rows[Count][3][11] = {
            {"qwertyuiop", "asdfghjkl;", "zxcvbnm,./"},
            {"qwfpgjluy;", "arstdhneio", "zxcvbkm,./"},
            {"',.pyfgcrl", "aoeuidhtns", ";qjkxbmwvz"}
        };
        for (int row = 0; row < 3; ++row) {
            for (int key = 0; key < 10; ++key) {
                // The up-arrow switch interrupts the bottom typing row.
                const int col = row == 2 && key == 9 ? 11 : key + 1;
                map[0][row + 1][col] = rows[preset][row][key];
            }
        }
        current = preset;
        return true;
    }

private:
    KeyInput::Keymap &map;
    int current = Qwerty;
};
