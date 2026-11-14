#pragma once

#include <stdint.h>

// MIDI 1.0 cannot release one voice independently when owners share a channel/pitch.
// Send the physical note-off only after the last local owner releases it.
class NoteOutput {
public:
    using SendNote = void (*)(uint8_t note, uint8_t velocity, bool on, uint8_t channel);
    using SendControl = void (*)(uint8_t control, uint8_t value, uint8_t channel);

    explicit NoteOutput(SendNote send, SendControl control = nullptr) : send(send), control(control) {}

    void sendControl(uint8_t number, uint8_t value, uint8_t channel) {
        if (control) control(number, value, channel);
    }

    void press(int note, int velocity, int channel = 1) {
        if (note < 0 || note > 127 || velocity < 1 || velocity > 127 ||
            channel < 1 || channel > 16) return;
        if (owners[channel - 1][note]++ == 0) send(note, velocity, true, channel);
    }

    void release(int note, int channel = 1) {
        if (note < 0 || note > 127 || channel < 1 || channel > 16 ||
            !owners[channel - 1][note]) return;
        if (--owners[channel - 1][note] == 0) send(note, 0, false, channel);
    }

    void panic() {
        for (int channel = 1; channel <= 16; ++channel) {
            for (int note = 0; note < 128; ++note) {
                if (!owners[channel - 1][note]) continue;
                owners[channel - 1][note] = 0;
                send(note, 0, false, channel);
            }
        }
    }

private:
    SendNote send;
    SendControl control;
    uint8_t owners[16][128] = {};
};
