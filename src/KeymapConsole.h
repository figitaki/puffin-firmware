#pragma once

#include <stdio.h>
#include <string.h>
#include "TypingLayout.h"

// Bounded, allocation-free line protocol. Hardware polling and persistence
// belong to the caller, which must only feed this console in typing mode.
class KeymapConsole {
public:
    enum Result { None, Changed, PresetChanged, Okay, Value, Dump, Error };
    // "keymap dump PRESET" then 60 row-major tokens: "-" for an unfitted
    // switch, else the decimal action with "*" when it is a user override.
    static const size_t DumpSize = 16 + RowCount * ColCount * 5;
    explicit KeymapConsole(TypingLayout &layout) : layout(layout) {}

    int value() const { return lastValue; }

    void dump(char (&out)[DumpSize]) const {
        size_t used = snprintf(out, DumpSize, "keymap dump %d", layout.selected());
        for (int row = 0; row < RowCount; ++row) {
            for (int col = 0; col < ColCount; ++col) {
                const int action = layout.actionAt(row, col);
                if (action < 0) used += snprintf(out + used, DumpSize - used, " -");
                else used += snprintf(out + used, DumpSize - used, " %d%s", action,
                                      layout.overrideAt(row, col) >= 0 ? "*" : "");
            }
        }
    }

    Result feed(char byte) {
        if (byte == '\r' || byte == '\n') {
            line[length] = 0;
            const Result result = discarded ? Error : length ? execute() : None;
            length = 0;
            discarded = false;
            return result;
        }
        if (discarded) return None;
        if (byte < 32 || byte > 126 || length == sizeof(line) - 1) {
            discarded = true; // Never execute a truncated prefix or suffix.
        } else {
            line[length++] = byte;
        }
        return None;
    }

private:
    TypingLayout &layout;
    char line[64] = {};
    size_t length = 0;
    bool discarded = false;
    int lastValue = -1;

    static int number(const char *&cursor) {
        while (*cursor == ' ') ++cursor;
        if (*cursor < '0' || *cursor > '9') return -1;
        int value = 0;
        do {
            value = value * 10 + (*cursor++ - '0');
            if (value > 255) return -1; // Bound before any integer overflow.
        } while (*cursor >= '0' && *cursor <= '9');
        return value;
    }

    Result execute() {
        if (!strcmp(line, "keymap clear")) {
            return layout.clearOverrides() ? Changed : Okay;
        }
        if (!strcmp(line, "keymap dump")) return Dump;
        if (!strcmp(line, "keymap preset")) {
            lastValue = layout.selected();
            return Value;
        }
        if (!strncmp(line, "keymap preset ", 14)) {
            const char *cursor = line + 14;
            const int preset = number(cursor);
            while (*cursor == ' ') ++cursor;
            if (*cursor || preset < 0 || preset >= TypingLayout::Count) return Error;
            if (preset == layout.selected()) return Okay;
            layout.select(preset);
            return PresetChanged;
        }
        const bool set = !strncmp(line, "keymap set ", 11);
        const bool reset = !strncmp(line, "keymap reset ", 13);
        const bool get = !strncmp(line, "keymap get ", 11);
        if (!set && !reset && !get) return Error;
        const char *cursor = line + (reset ? 13 : 11);
        const int row = number(cursor);
        const int col = number(cursor);
        const int action = set ? number(cursor) : 0;
        while (*cursor == ' ') ++cursor;
        if (row < 0 || col < 0 || action < 0 || *cursor) return Error;
        if (get) {
            lastValue = layout.actionAt(row, col);
            return lastValue >= 0 ? Value : Error;
        }
        const int before = layout.overrideAt(row, col);
        if (set) {
            if (!layout.remap(row, col, action)) return Error;
            return before == action ? Okay : Changed;
        }
        if (!layout.resetKey(row, col)) return Error;
        return before == -1 ? Okay : Changed;
    }
};
