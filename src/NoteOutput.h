#pragma once

#include <stdint.h>

// MIDI 1.0 cannot release one voice independently when owners share a pitch.
// Send the physical note-off only after the last local owner releases it.
class NoteOutput {
public:
    using SendNote = void (*)(uint8_t note, uint8_t velocity, bool on);

    explicit NoteOutput(SendNote send) : send(send) {}

    void press(int note, int velocity) {
        if (note < 0 || note > 127 || velocity < 1 || velocity > 127) return;
        if (owners[note]++ == 0) send(note, velocity, true);
    }

    void release(int note) {
        if (note < 0 || note > 127 || !owners[note]) return;
        if (--owners[note] == 0) send(note, 0, false);
    }

    void panic() {
        for (int note = 0; note < 128; ++note) {
            if (!owners[note]) continue;
            owners[note] = 0;
            send(note, 0, false);
        }
    }

private:
    SendNote send;
    uint8_t owners[128] = {};
};
