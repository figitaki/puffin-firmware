#include <assert.h>
#include <stdio.h>
#include "KeyInput.h"
#include "TypingLayout.h"

struct Keyboard {
    KeyInput input;
    KeyInput::Keymap map = {};
    KeyInput::Event event = {};

    Keyboard() {
        for (int layer = 0; layer < NumLayers; ++layer) {
            for (auto &row : map[layer]) {
                for (auto &action : row) action = 'a' + layer;
            }
            map[layer][4][8] = FUNCTION_SW;
        }
        map[0][4][2] = 'g';
        map[1][4][2] = FUNCTION_SW2;
        map[2][4][2] = FUNCTION_SW2;
    }

    bool sample(int row, int col, bool down, uint32_t time) {
        input.sample(row, col, down, time);
        return input.nextEvent(map, event);
    }

    void edge(int row, int col, bool down, uint32_t time, int action) {
        assert(!sample(row, col, down, time));
        assert(sample(row, col, down, time + 5));
        assert(event.action == action);
        assert(event.pressed == down);
    }
};

void debounce() {
    Keyboard keyboard;
    assert(!keyboard.sample(1, 0, true, 0));
    assert(!keyboard.sample(1, 0, false, 2));
    assert(!keyboard.sample(1, 0, true, 4));
    assert(!keyboard.sample(1, 0, true, 8));
    assert(keyboard.sample(1, 0, true, 9));
    assert(keyboard.event.action == 'a' && keyboard.event.pressed);
    assert(!keyboard.sample(1, 0, true, 20));

    assert(!keyboard.sample(1, 0, false, 21));
    assert(!keyboard.sample(1, 0, true, 23));
    assert(!keyboard.sample(1, 0, false, 24));
    assert(!keyboard.sample(1, 0, false, 28));
    assert(keyboard.sample(1, 0, false, 29));
    assert(keyboard.event.action == 'a' && !keyboard.event.pressed);
    assert(!keyboard.sample(1, 0, false, 40));
}

void independentSwitches() {
    Keyboard keyboard;
    keyboard.map[0][1][1] = 'z';
    assert(!keyboard.sample(1, 0, true, 0));
    assert(!keyboard.sample(1, 1, true, 3));
    assert(keyboard.sample(1, 0, true, 5));
    assert(keyboard.event.action == 'a');
    assert(!keyboard.sample(1, 1, true, 7));
    assert(keyboard.sample(1, 1, true, 8));
    assert(keyboard.event.action == 'z');
}

void heldActionsSurviveLayerChanges() {
    Keyboard keyboard;
    keyboard.edge(1, 0, true, 0, 'a');
    keyboard.edge(4, 8, true, 10, FUNCTION_SW);
    for (uint32_t time = 16; time < 100; ++time) {
        assert(!keyboard.sample(4, 8, true, time));
    }
    keyboard.edge(1, 1, true, 100, 'b');
    keyboard.edge(4, 8, false, 110, FUNCTION_SW);
    keyboard.edge(1, 0, false, 120, 'a');
    keyboard.edge(1, 1, false, 130, 'b');
    keyboard.edge(1, 2, true, 140, 'a');
}

void layersReleaseOutOfOrder() {
    Keyboard keyboard;
    keyboard.edge(4, 8, true, 0, FUNCTION_SW);
    keyboard.edge(4, 2, true, 10, FUNCTION_SW2);
    keyboard.edge(1, 0, true, 20, 'c');
    keyboard.edge(4, 8, false, 30, FUNCTION_SW);
    keyboard.edge(1, 1, true, 40, 'c');
    keyboard.edge(4, 2, false, 50, FUNCTION_SW2);
    keyboard.edge(1, 0, false, 60, 'c');
    keyboard.edge(1, 1, false, 70, 'c');
    // Re-pressing the former layer key resolves from base, not its old action.
    keyboard.edge(4, 2, true, 80, 'g');
    keyboard.edge(1, 2, true, 90, 'a');

    Keyboard reverse;
    reverse.edge(4, 8, true, 0, FUNCTION_SW);
    reverse.edge(4, 2, true, 10, FUNCTION_SW2);
    reverse.edge(4, 2, false, 20, FUNCTION_SW2);
    reverse.edge(1, 0, true, 30, 'b');
    reverse.edge(4, 8, false, 40, FUNCTION_SW);
    reverse.edge(1, 1, true, 50, 'a');
    reverse.edge(1, 0, false, 60, 'b');
}

void remapAndMidiActionsKeepTheirReleases() {
    Keyboard keyboard;
    keyboard.edge(1, 0, true, 0, 'a');
    keyboard.map[0][1][0] = PLAY_C;
    keyboard.edge(1, 0, false, 10, 'a');
    keyboard.edge(1, 0, true, 20, PLAY_C);
    keyboard.edge(4, 8, true, 30, FUNCTION_SW);
    keyboard.edge(1, 0, false, 40, PLAY_C);
    keyboard.map[1][1][0] = PLAY_G;
    keyboard.edge(1, 0, true, 50, PLAY_G);
    keyboard.edge(4, 8, false, 60, FUNCTION_SW);
    keyboard.edge(1, 0, false, 70, PLAY_G);
}

void clockWraparound() {
    Keyboard keyboard;
    assert(!keyboard.sample(1, 0, true, UINT32_MAX - 2));
    assert(!keyboard.sample(1, 0, true, 1));
    assert(keyboard.sample(1, 0, true, 2));
    assert(keyboard.event.action == 'a' && keyboard.event.pressed);
    keyboard.edge(1, 0, false, 10, 'a');
}

void onlyPopulatedSwitchesGenerateEvents() {
    Keyboard keyboard;
    int presses = 0;
    int releases = 0;
    for (int row = 0; row < RowCount; ++row) {
        for (int col = 0; col < ColCount; ++col) {
            // Derived from switch locations, independently of the bit masks.
            const bool fitted = (row == 0 ? col >= 3 && col <= 8 :
                                 row == 4 ? col != 3 && col != 5 && col != 7 :
                                 true);
            // Use ordinary actions so layer keys do not affect this scan.
            keyboard.map[0][row][col] = 'x';
            assert(!keyboard.sample(row, col, true, 0));
            const bool press = keyboard.sample(row, col, true, 5);
            assert(press == fitted);
            if (press) {
                assert(keyboard.event.action == 'x' && keyboard.event.pressed);
                ++presses;
            }
            assert(!keyboard.sample(row, col, false, 10));
            const bool release = keyboard.sample(row, col, false, 15);
            assert(release == fitted);
            if (release) {
                assert(keyboard.event.action == 'x' && !keyboard.event.pressed);
                ++releases;
            }
        }
    }
    assert(presses == 51 && releases == 51);
}

void emptyMappingDoesNotActivateALayer() {
    Keyboard keyboard;
    keyboard.map[0][1][0] = 0;
    keyboard.edge(1, 0, true, 0, 0);
    keyboard.edge(1, 1, true, 10, 'a');
    keyboard.edge(1, 0, false, 20, 0);
}

void simultaneousLayerPressesIgnoreScanOrder() {
    Keyboard keyboard;
    // GUI and a typing key are scanned before Fn, but must resolve after it.
    for (uint32_t time = 0; time <= 5; time += 5) {
        keyboard.input.sample(4, 2, true, time);
        keyboard.input.sample(1, 0, true, time);
        keyboard.input.sample(4, 8, true, time);
    }
    assert(keyboard.input.nextEvent(keyboard.map, keyboard.event));
    assert(keyboard.event.action == FUNCTION_SW && keyboard.event.pressed);
    assert(keyboard.input.nextEvent(keyboard.map, keyboard.event));
    assert(keyboard.event.action == FUNCTION_SW2 && keyboard.event.pressed);
    assert(keyboard.input.nextEvent(keyboard.map, keyboard.event));
    assert(keyboard.event.action == 'c' && keyboard.event.pressed);
    assert(!keyboard.input.nextEvent(keyboard.map, keyboard.event));

    // Simultaneous layer releases precede an earlier-column ordinary press.
    for (uint32_t time = 10; time <= 15; time += 5) {
        keyboard.input.sample(1, 1, true, time);
        keyboard.input.sample(4, 2, false, time);
        keyboard.input.sample(4, 8, false, time);
    }
    assert(keyboard.input.nextEvent(keyboard.map, keyboard.event));
    assert(keyboard.event.action == FUNCTION_SW2 && !keyboard.event.pressed);
    assert(keyboard.input.nextEvent(keyboard.map, keyboard.event));
    assert(keyboard.event.action == FUNCTION_SW && !keyboard.event.pressed);
    assert(keyboard.input.nextEvent(keyboard.map, keyboard.event));
    assert(keyboard.event.action == 'a' && keyboard.event.pressed);
    assert(!keyboard.input.nextEvent(keyboard.map, keyboard.event));
    keyboard.edge(1, 0, false, 20, 'c');
}

void sharedActionsRetainAllOwners() {
    Keyboard keyboard;
    keyboard.map[0][0][5] = '3';
    keyboard.map[1][1][3] = '3';
    keyboard.edge(0, 5, true, 0, '3');
    assert(keyboard.input.heldCount('3') == 1);
    keyboard.edge(4, 8, true, 10, FUNCTION_SW);
    keyboard.edge(1, 3, true, 20, '3');
    assert(keyboard.input.heldCount('3') == 2);
    keyboard.edge(0, 5, false, 30, '3');
    assert(keyboard.input.heldCount('3') == 1);
    keyboard.edge(4, 8, false, 40, FUNCTION_SW);
    keyboard.edge(1, 3, false, 50, '3');
    assert(keyboard.input.heldCount('3') == 0);
}

void simultaneousModifiersPrecedeTyping() {
    Keyboard keyboard;
    keyboard.map[0][1][1] = 'q';
    // BLE Keyboard encodes left Shift/Ctrl/GUI as 0x81/0x80/0x83.
    keyboard.map[0][3][0] = 0x81;
    keyboard.map[0][4][0] = 0x80;
    keyboard.map[0][4][2] = 0x83;
    for (uint32_t time = 0; time <= 5; time += 5) {
        keyboard.input.sample(1, 1, true, time);
        keyboard.input.sample(3, 0, true, time);
        keyboard.input.sample(4, 0, true, time);
        keyboard.input.sample(4, 2, true, time);
    }
    int modifiers = 0;
    for (int i = 0; i < 3; ++i) {
        assert(keyboard.input.nextEvent(keyboard.map, keyboard.event));
        assert(keyboard.event.pressed);
        assert(keyboard.event.action >= 0x80 && keyboard.event.action <= 0x87);
        modifiers |= 1 << (keyboard.event.action - 0x80);
    }
    assert(modifiers == 0x0B);
    assert(keyboard.input.nextEvent(keyboard.map, keyboard.event));
    assert(keyboard.event.action == 'q' && keyboard.event.pressed);
    assert(!keyboard.input.nextEvent(keyboard.map, keyboard.event));
}

void typingPresetsPreserveNonTypingBindings() {
    KeyInput::Keymap map = {};
    for (int layer = 0; layer < NumLayers; ++layer) {
        for (int row = 0; row < RowCount; ++row) {
            for (int col = 0; col < ColCount; ++col) {
                map[layer][row][col] = 1000 + layer * 100 + row * 12 + col;
            }
        }
    }
    TypingLayout layout(map);
    assert(layout.selected() == TypingLayout::Qwerty);
    const char *expected[][3] = {
        {"qwertyuiop", "asdfghjkl;", "zxcvbnm,./"},
        {"qwfpbjluy;", "arstgmneio", "zxcdvkh,./"},
        {"',.pyfgcrl", "aoeuidhtns", ";qjkxbmwvz"}
    };
    // Return to QWERTY after both alternatives, not just from initialization.
    const int choices[] = {1, 2, 0};
    for (int choice : choices) {
        assert(layout.select(choice) && layout.selected() == choice);
        for (int layer = 0; layer < NumLayers; ++layer) {
            for (int row = 0; row < RowCount; ++row) {
                for (int col = 0; col < ColCount; ++col) {
                    int value = 1000 + layer * 100 + row * 12 + col;
                    if (layer == 0 && row >= 1 && row <= 3) {
                        if (col >= 1 && col <= (row == 3 ? 9 : 10)) {
                            value = expected[choice][row - 1][col - 1];
                        } else if (row == 3 && col == 11) {
                            value = expected[choice][2][9];
                        }
                    }
                    assert(map[layer][row][col] == value);
                }
            }
        }
    }
    assert(!layout.select(-1) && !layout.select(3) && !layout.select(255));
    assert(layout.selected() == TypingLayout::Qwerty && map[0][1][3] == 'e');
}

void typingLayoutChangesKeepCapturedReleases() {
    Keyboard keyboard;
    TypingLayout layout(keyboard.map);
    keyboard.edge(1, 3, true, 0, 'e');
    layout.select(TypingLayout::Colemak);
    keyboard.edge(1, 3, false, 10, 'e');
    keyboard.edge(1, 3, true, 20, 'f');
    keyboard.edge(4, 8, true, 30, FUNCTION_SW);
    layout.select(TypingLayout::Dvorak);
    keyboard.edge(1, 3, false, 40, 'f');
    keyboard.edge(1, 3, true, 50, 'b'); // Fn layer is not rearranged.
    keyboard.edge(4, 8, false, 60, FUNCTION_SW);
    keyboard.edge(1, 3, false, 70, 'b');
    keyboard.edge(1, 3, true, 80, '.');
    keyboard.edge(1, 3, false, 90, '.');
    // Dvorak's bottom-right Z must not replace the up-arrow position.
    keyboard.edge(3, 10, true, 100, 'a');
    keyboard.edge(3, 11, true, 110, 'z');
}

int main() {
    debounce();
    independentSwitches();
    heldActionsSurviveLayerChanges();
    layersReleaseOutOfOrder();
    remapAndMidiActionsKeepTheirReleases();
    clockWraparound();
    onlyPopulatedSwitchesGenerateEvents();
    emptyMappingDoesNotActivateALayer();
    simultaneousLayerPressesIgnoreScanOrder();
    sharedActionsRetainAllOwners();
    simultaneousModifiersPrecedeTyping();
    typingPresetsPreserveNonTypingBindings();
    typingLayoutChangesKeepCapturedReleases();
    puts("Key input regression tests passed");
}
