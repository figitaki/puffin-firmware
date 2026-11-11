#pragma once

#include "NoteOutput.h"

// Musical state only: no Arduino, display, MIDI interface, or key-function codes.
class Sequencer {
public:
    explicit Sequencer(NoteOutput &output) : output(output) {}

    bool play = false;
    int s = 0; // Next step to play/edit, retained by Stop/Continue.

    bool setLength(int value) {
        if (value < 1 || value > Capacity) return false;
        stop();
        length = value;
        s = 0;
        return true;
    }

    bool setStep(int index, int note, int velocity) {
        if (index == -1) index = s;
        if (index < 0 || index >= length || note < 0 || note > 127 ||
            velocity < 0 || velocity > 127) return false;
        notes[index] = {uint8_t(note), uint8_t(velocity)};
        return true;
    }

    void clear() { notes[s] = {}; }

    void step() {
        if (!play) return;
        releaseNote();
        const Step &note = notes[s];
        if (note.velocity) {
            activeNote = note.note;
            output.press(note.note, note.velocity);
        }
        s = (s + 1) % length;
    }

    void stop() {
        releaseNote();
        play = false;
    }

    void reset() {
        stop();
        s = 0;
    }

    void start() {
        reset();
        play = true;
        step();
    }

    void resume() { play = true; }

private:
    static const int Capacity = 96;
    struct Step {
        uint8_t note;
        uint8_t velocity; // Zero is a rest; MIDI note zero is still playable.
    };

    NoteOutput &output;
    Step notes[Capacity] = {};
    int length = Capacity;
    int activeNote = -1;

    void releaseNote() {
        if (activeNote < 0) return;
        output.release(activeNote);
        activeNote = -1;
    }
};
