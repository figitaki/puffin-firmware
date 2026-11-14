#pragma once

#include "NoteOutput.h"

// Physical performance keys keep their press-time pitch/channel until release.
// The octave is an entry tool, not a transpose of existing sequence steps.
class NoteKeyboard {
public:
    explicit NoteKeyboard(NoteOutput &output) : output(output) {}

    int octave() const { return offset; }
    void shiftOctave(int delta) {
        const int next = offset + delta;
        offset = next < -4 ? -4 : next > 4 ? 4 : next;
    }

    int press(int baseNote, int velocity, int channel) {
        if (baseNote < 53 || baseNote > 72 || velocity < 1 || velocity > 127 ||
            channel < 1 || channel > 16) return -1;
        Held &held = keys[baseNote - 53];
        if (held.channel) return -1;
        held.note = uint8_t(baseNote + offset * 12);
        held.channel = uint8_t(channel);
        output.press(held.note, velocity, held.channel);
        return held.note;
    }

    void release(int baseNote) {
        if (baseNote < 53 || baseNote > 72) return;
        Held &held = keys[baseNote - 53];
        if (held.channel) output.release(held.note, held.channel);
        held.channel = 0;
    }

    void releaseAll() {
        for (int note = 53; note <= 72; ++note) release(note);
    }

    bool active(int pitchClass) const {
        for (const auto &held : keys)
            if (held.channel && held.note % 12 == pitchClass) return true;
        return false;
    }

private:
    struct Held { uint8_t note = 0, channel = 0; };
    NoteOutput &output;
    Held keys[20];
    int offset = 0;
};
