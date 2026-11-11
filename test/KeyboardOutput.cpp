#include <assert.h>
#include <array>
#include <initializer_list>
#include <stdio.h>
#include <vector>
#include "KeyboardOutput.h"

struct Keyboard {
    uint8_t ascii[128] = {};
    KeyboardOutput output{ascii};

    Keyboard() {
        // Independent USB HID expectations, not the production translator.
        ascii['q'] = 0x14;
        ascii['Q'] = 0x94;
        ascii['9'] = 0x26;
        ascii['('] = 0xA6;
        ascii['5'] = 0x22;
        ascii['%'] = 0xA2;
    }

    void expect(uint8_t expectedModifiers, std::initializer_list<uint8_t> expectedKeys) {
        uint8_t modifiers;
        uint8_t keys[6];
        output.report(modifiers, keys);
        assert(modifiers == expectedModifiers);
        int i = 0;
        for (auto expected : expectedKeys) assert(keys[i++] == expected);
        while (i < 6) assert(keys[i++] == 0);
    }
};

void aliasesKeepSharedUsages() {
    Keyboard keyboard;
    keyboard.output.press('9');
    keyboard.output.press('(');
    keyboard.expect(0x02, {0x26});
    keyboard.output.release('9');
    keyboard.expect(0x02, {0x26});
    keyboard.output.release('(');
    keyboard.expect(0, {});

    keyboard.output.press('9');
    keyboard.output.press('(');
    keyboard.output.release('(');
    keyboard.expect(0, {0x26});
    // A non-printing action encoding the same HID usage is another owner.
    keyboard.output.press(136 + 0x26);
    keyboard.output.release('9');
    keyboard.expect(0, {0x26});
    keyboard.output.release(136 + 0x26);
    keyboard.expect(0, {});
}

void synthesizedAndPhysicalShiftShareOwnership() {
    Keyboard keyboard;
    keyboard.output.press(0x81);
    keyboard.output.press('(');
    keyboard.output.release('(');
    keyboard.expect(0x02, {});
    keyboard.output.release(0x81);
    keyboard.expect(0, {});

    keyboard.output.press(0x81); // physical left Shift
    keyboard.output.press('(');
    keyboard.output.press('Q');
    keyboard.output.release(0x81);
    keyboard.expect(0x02, {0x14, 0x26});
    keyboard.output.release('(');
    keyboard.expect(0x02, {0x14});
    keyboard.output.release('Q');
    keyboard.expect(0, {});
}

void screenshotEmitsFreshPressAndRestoresOwners() {
    using Report = std::array<uint8_t, 7>;
    for (uint8_t heldAction : {'5', '%'}) {
        Keyboard keyboard;
        keyboard.output.press('q');
        keyboard.output.press(heldAction);
        keyboard.output.press(0x83); // physical GUI
        keyboard.output.press(0x85); // independent right Shift
        const uint8_t modifiers = heldAction == '%' ? 0x2A : 0x28;
        std::vector<Report> sent;
        auto send = [&](uint8_t mod, const uint8_t (&keys)[6]) {
            sent.push_back({{mod, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5]}});
        };
        assert(keyboard.output.tapUsage(0x22, 0x0A, send));
        // Releasing only 5 creates a fresh down edge; unrelated q and held
        // modifiers survive every report. Final ownership is unchanged.
        assert((sent == std::vector<Report>{
            {{modifiers, 0x14, 0, 0, 0, 0, 0}},
            {{0x2A, 0x14, 0x22, 0, 0, 0, 0}},
            {{modifiers, 0x14, 0x22, 0, 0, 0, 0}}
        }));
        keyboard.expect(modifiers, {0x14, 0x22});
        keyboard.output.release(heldAction);
        keyboard.expect(0x28, {0x14});
        sent.clear();
        assert(keyboard.output.tapUsage(0x22, 0x0A, send));
        assert((sent == std::vector<Report>{
            {{0x2A, 0x14, 0x22, 0, 0, 0, 0}},
            {{0x28, 0x14, 0, 0, 0, 0, 0}}
        }));
        keyboard.output.release('q');
        keyboard.output.release(0x83);
        keyboard.output.release(0x85);
        keyboard.expect(0, {});
    }
}

void screenshotDoesNotEvictOtherKeys() {
    Keyboard keyboard;
    for (int usage = 4; usage <= 9; ++usage) keyboard.output.press(136 + usage);
    int reports = 0;
    auto send = [&](uint8_t, const uint8_t (&)[6]) { ++reports; };
    assert(!keyboard.output.tapUsage(0x22, 0x0A, send));
    assert(reports == 0);
    keyboard.expect(0, {4, 5, 6, 7, 8, 9});
    keyboard.output.press(136 + 10);
    assert(!keyboard.output.tapUsage(0x22, 0x0A, send));
    assert(reports == 0);
    keyboard.expect(0, {1, 1, 1, 1, 1, 1});
    keyboard.output.release(136 + 10);
    keyboard.output.release(136 + 9);
    assert(keyboard.output.tapUsage(0x22, 0x0A, send));
    assert(reports == 2);
    keyboard.expect(0, {4, 5, 6, 7, 8});
}

void rolloverRecoversWithoutLosingOwners() {
    Keyboard keyboard;
    for (int usage = 4; usage <= 9; ++usage) keyboard.output.press(136 + usage);
    keyboard.expect(0, {4, 5, 6, 7, 8, 9});
    keyboard.output.press(136 + 10);
    keyboard.output.press(0x80); // Ctrl is not a seventh ordinary key.
    keyboard.expect(0x01, {1, 1, 1, 1, 1, 1});
    keyboard.output.release(136 + 7);
    keyboard.expect(0x01, {4, 5, 6, 8, 9, 10});
    for (int usage = 4; usage <= 10; ++usage) {
        if (usage != 7) keyboard.output.release(136 + usage);
    }
    keyboard.output.release(0x80);
    keyboard.expect(0, {});
}

int main() {
    aliasesKeepSharedUsages();
    synthesizedAndPhysicalShiftShareOwnership();
    screenshotEmitsFreshPressAndRestoresOwners();
    screenshotDoesNotEvictOtherKeys();
    rolloverRecoversWithoutLosingOwners();
    puts("Keyboard output regression tests passed");
}
