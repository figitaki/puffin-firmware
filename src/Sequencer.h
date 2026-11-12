#pragma once

#include "NoteOutput.h"

// Musical state only: no Arduino, display, MIDI interface, or key-function codes.
class Sequencer {
public:
    static const int StepsPerPage = 16;
    static const int MaxPages = 4;
    static const int Capacity = StepsPerPage * MaxPages;

    explicit Sequencer(NoteOutput &output, uint8_t channel = 1)
        : output(output), channel(channel) {}

    bool play = false;
    int s = 0; // Next step to play/edit, retained by Stop/Continue.
    int getLength() const { return length; }
    bool isMuted() const { return muted; }

    void setMuted(bool value) {
        if (value) releaseNote();
        muted = value;
    }

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

    bool getStep(int index, uint8_t &note, uint8_t &velocity) const {
        if (index < 0 || index >= length) return false;
        note = notes[index].note;
        velocity = notes[index].velocity;
        return true;
    }

    void clear() { notes[s] = {}; }

    void step() {
        if (!play) return;
        releaseNote();
        const Step &note = notes[s];
        if (note.velocity && !muted) {
            activeNote = note.note;
            output.press(note.note, note.velocity, channel);
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
    struct Step {
        uint8_t note;
        uint8_t velocity; // Zero is a rest; MIDI note zero is still playable.
    };

    NoteOutput &output;
    uint8_t channel;
    bool muted = false;
    Step notes[Capacity] = {};
    int length = StepsPerPage;
    int activeNote = -1;

    void releaseNote() {
        if (activeNote < 0) return;
        output.release(activeNote, channel);
        activeNote = -1;
    }
};
