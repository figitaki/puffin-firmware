#include <assert.h>
#include <stdio.h>
#include <sstream>
#include <string>
#include <vector>
#include "KeymapConsole.h"

struct Keyboard {
    KeyInput::Keymap map = {};
    TypingLayout layout;
    KeymapConsole console;

    Keyboard() : layout(prepare()), console(layout) {}
    KeyInput::Keymap &prepare() {
        map[0][4][8] = FUNCTION_SW;
        map[1][4][2] = FUNCTION_SW2;
        return map;
    }
    KeymapConsole::Result command(const std::string &text) {
        auto result = KeymapConsole::None;
        for (char byte : text) {
            const auto next = console.feed(byte);
            if (next != KeymapConsole::None) result = next;
        }
        return result;
    }
};

void editsFollowPositionsAcrossPresets() {
    Keyboard keyboard;
    assert(keyboard.command("keymap get 1 3\n") == KeymapConsole::Value);
    assert(keyboard.console.value() == 'e');
    assert(keyboard.command("keymap set 1 3 120\r\n") == KeymapConsole::Changed);
    assert(keyboard.command("keymap set 1 3 120\n") == KeymapConsole::Okay);
    keyboard.layout.select(TypingLayout::Colemak);
    assert(keyboard.map[0][1][3] == 'x');
    assert(keyboard.command("keymap reset 1 3\n") == KeymapConsole::Changed);
    assert(keyboard.map[0][1][3] == 'f');
    assert(keyboard.command("keymap reset 1 3\n") == KeymapConsole::Okay);
    assert(keyboard.command("keymap set 3 10 0\n") == KeymapConsole::Changed);
    assert(keyboard.map[0][3][10] == 0);
    assert(keyboard.command("keymap get 4 8\n") == KeymapConsole::Value);
    assert(keyboard.console.value() == FUNCTION_SW);
    assert(keyboard.command("keymap clear\n") == KeymapConsole::Changed);
    assert(keyboard.layout.selected() == TypingLayout::Colemak);
    assert(keyboard.command("keymap clear\n") == KeymapConsole::Okay);
    assert(keyboard.map[1][4][2] == FUNCTION_SW2);
}

void invalidCommandsAreAtomicAndStreamRecovers() {
    Keyboard keyboard;
    uint8_t before[TypingLayout::StorageSize], after[TypingLayout::StorageSize];
    keyboard.layout.encode(before);
    const char *invalid[] = {
        "keymap set -1 3 120", "keymap set 5 3 120", "keymap set 1 12 120",
        "keymap set 0 0 120", "keymap set 4 8 120", "keymap reset 4 8",
        "keymap set 1 3 256", "keymap set 1 3 127", "keymap set 1 3 31",
        "keymap set 1 3 -1", "keymap set 1 3 999999999999999999999999",
        "keymap set 1 3 120 extra", "keymap set 1 3", "keymap set 1 x 120",
        "keymap set 1 3 12x", "keymap get 1 3 120", "keymap get 0 0",
        "keymap clear extra", "set 1 3 120"
    };
    for (const auto text : invalid) {
        assert(keyboard.command(std::string(text) + "\n") == KeymapConsole::Error);
        keyboard.layout.encode(after);
        assert(!memcmp(before, after, sizeof(before)));
    }
    // A valid-looking truncated prefix or later suffix must not be executed.
    assert(keyboard.command("keymap set 1 3 120" + std::string(80, ' ') + "keymap clear\n") == KeymapConsole::Error);
    assert(keyboard.command(std::string("keymap set 1 3 ") + char(0) + "120\n") == KeymapConsole::Error);
    keyboard.layout.encode(after);
    assert(!memcmp(before, after, sizeof(before)));
    assert(keyboard.command("\r\n") == KeymapConsole::None);
    std::string boundary = "keymap set 1 3 121";
    boundary.resize(63, ' ');
    assert(keyboard.command(boundary + "\n") == KeymapConsole::Changed);
    assert(keyboard.map[0][1][3] == 'y');
    assert(keyboard.command(boundary + " \n") == KeymapConsole::Error);
    assert(keyboard.map[0][1][3] == 'y');
    assert(keyboard.command("keymap set 1 3 255\n") == KeymapConsole::Changed);
    assert(keyboard.map[0][1][3] == 255);
    assert(keyboard.command("keymap set 1 3 128\n") == KeymapConsole::Changed);
    assert(keyboard.map[0][1][3] == 128);
    assert(keyboard.command("keymap set 1 3 32\n") == KeymapConsole::Changed);
    assert(keyboard.map[0][1][3] == ' ');
}

void persistedRecordsAreValidatedBeforeApplication() {
    Keyboard keyboard;
    uint8_t record[TypingLayout::StorageSize];
    // Independently construct the documented version-1 wire format.
    memset(record, 0xFF, sizeof(record)); record[0] = 1;
    const int e = 1 + (1 * ColCount + 3) * 2;
    record[e] = 'X'; record[e + 1] = 0;
    const int right = 1 + (4 * ColCount + 11) * 2;
    record[right] = 0; record[right + 1] = 0; // Disabled right-arrow position.
    assert(keyboard.layout.restore(record, sizeof(record)));
    keyboard.layout.select(TypingLayout::Dvorak);
    assert(keyboard.map[0][1][3] == 'X' && keyboard.map[0][4][11] == 0);
    uint8_t encoded[TypingLayout::StorageSize];
    keyboard.layout.encode(encoded);
    assert(!memcmp(record, encoded, sizeof(record)));
    assert(!keyboard.layout.restore(record, sizeof(record) - 1));
    assert(!keyboard.layout.restore(record, sizeof(record) + 1));
    record[0] = 2;
    assert(!keyboard.layout.restore(record, sizeof(record)));
    record[0] = 1; record[e] = 'Y';
    record[right + 1] = 1; // Late invalid action must not apply the earlier Y.
    assert(!keyboard.layout.restore(record, sizeof(record)));
    assert(keyboard.map[0][1][3] == 'X');
    record[right + 1] = 0;
    const int fn = 1 + (4 * ColCount + 8) * 2;
    record[fn] = 'z'; record[fn + 1] = 0;
    assert(!keyboard.layout.restore(record, sizeof(record)));
    record[fn] = record[fn + 1] = 0xFF;
    record[1] = 32; record[2] = 0; // Unpopulated switch.
    assert(!keyboard.layout.restore(record, sizeof(record)));
    keyboard.layout.encode(record);
    Keyboard rebooted;
    rebooted.layout.select(TypingLayout::Colemak);
    assert(rebooted.layout.restore(record, sizeof(record)));
    assert(rebooted.map[0][1][3] == 'X' && rebooted.map[0][4][11] == 0);
    assert(rebooted.layout.resetKey(1, 3) && rebooted.map[0][1][3] == 'f');
}

void remappedHeldKeysReleaseOriginalActions() {
    Keyboard keyboard;
    KeyInput input;
    KeyInput::Event event;
    auto edge = [&](bool down, uint32_t now, int expected) {
        input.sample(1, 3, down, now);
        assert(!input.nextEvent(keyboard.map, event));
        input.sample(1, 3, down, now + 5);
        assert(input.nextEvent(keyboard.map, event));
        assert(event.pressed == down && event.action == expected);
    };
    edge(true, 0, 'e');
    assert(keyboard.command("keymap set 1 3 120\n") == KeymapConsole::Changed);
    edge(false, 10, 'e');
    edge(true, 20, 'x');
    keyboard.layout.select(TypingLayout::Colemak);
    assert(keyboard.command("keymap clear\n") == KeymapConsole::Changed);
    edge(false, 30, 'x');
    edge(true, 40, 'f');
}

std::vector<std::string> dumpTokens(Keyboard &keyboard) {
    assert(keyboard.command("keymap dump\n") == KeymapConsole::Dump);
    char line[KeymapConsole::DumpSize];
    keyboard.console.dump(line);
    std::vector<std::string> tokens;
    std::istringstream words(line);
    for (std::string word; words >> word;) tokens.push_back(word);
    assert(tokens.size() == 63 && tokens[0] == "keymap" && tokens[1] == "dump");
    return tokens;
}

void presetsAndDumpsDescribeTheWholeBaseLayer() {
    Keyboard keyboard;
    assert(keyboard.command("keymap preset\n") == KeymapConsole::Value);
    assert(keyboard.console.value() == TypingLayout::Qwerty);
    assert(keyboard.command("keymap set 1 3 120\n") == KeymapConsole::Changed);
    assert(keyboard.command("keymap preset 2 \n") == KeymapConsole::PresetChanged);
    assert(keyboard.command("keymap preset 2\n") == KeymapConsole::Okay);
    const char *invalid[] = {"keymap preset 3", "keymap preset -1", "keymap preset 1 1", "keymap preset x"};
    for (const auto text : invalid) {
        assert(keyboard.command(std::string(text) + "\n") == KeymapConsole::Error);
    }
    assert(keyboard.layout.selected() == TypingLayout::Dvorak);
    auto tokens = dumpTokens(keyboard);
    auto at = [&](int row, int col) { return tokens[3 + row * ColCount + col]; };
    assert(tokens[2] == "2");
    int unfitted = 0;
    for (size_t i = 3; i < tokens.size(); ++i) unfitted += tokens[i] == "-";
    assert(unfitted == RowCount * ColCount - 51 && at(0, 0) == "-" && at(4, 3) == "-");
    assert(at(1, 3) == "120*" && at(1, 2) == "44" && at(4, 8) == "256" && at(3, 11) == "122");
    assert(keyboard.command("keymap reset 1 3\n") == KeymapConsole::Changed);
    tokens = dumpTokens(keyboard);
    assert(at(1, 3) == "46");
    assert(keyboard.command("keymap dump extra\n") == KeymapConsole::Error);
}

int main() {
    editsFollowPositionsAcrossPresets();
    invalidCommandsAreAtomicAndStreamRecovers();
    persistedRecordsAreValidatedBeforeApplication();
    remappedHeldKeysReleaseOriginalActions();
    presetsAndDumpsDescribeTheWholeBaseLayer();
    puts("Keymap console and persistence regression tests passed");
}
