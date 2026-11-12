#pragma once

#include "NoteOutput.h"

// Musical state only: no Arduino, display, MIDI interface, or key-function codes.
class Sequencer {
public:
    static const int StepsPerPage = 16;
    static const int MaxPages = 4;
    static const int Capacity = StepsPerPage * MaxPages;
    static const int MaxNotes = 4;
    static const int MaxLocks = 4;
    using Random = uint32_t (*)();

    struct Lock { uint8_t control = 0, value = 0; };
    struct Step {
        uint8_t notes[MaxNotes] = {};
        uint8_t count = 0, velocity = 0;
        uint8_t chance = 100, ratchet = 1;
        Lock locks[MaxLocks];
        uint8_t lockCount = 0;
    };

    explicit Sequencer(NoteOutput &output, uint8_t channel = 1, Random random = nullptr)
        : output(output), channel(channel), random(random), randomState(channel) {}

    bool play = false;
    int s = 0; // Next step to play/edit, retained by Stop/Continue.
    int getLength() const { return length; }
    bool isMuted() const { return muted; }

    void setMuted(bool value) {
        if (value) { releaseNote(); triggered = false; }
        muted = value;
    }

    bool setLength(int value) {
        if (value < 1 || value > Capacity) return false;
        stop();
        length = value;
        s = 0;
        return true;
    }

    bool setStep(int index, const Step &value) {
        if (index == -1) index = s;
        if (index < 0 || index >= length || value.count > MaxNotes ||
            value.velocity > 127 || value.chance > 100 || value.lockCount > MaxLocks ||
            !(value.ratchet == 1 || value.ratchet == 2 || value.ratchet == 3 || value.ratchet == 6)) return false;
        for (int i = 0; i < value.count; ++i) {
            if (value.notes[i] > 127) return false;
            for (int j = 0; j < i; ++j) if (value.notes[i] == value.notes[j]) return false;
        }
        for (int i = 0; i < value.lockCount; ++i) {
            // Channel-mode CCs (120–127) can silence/reset other owners.
            if (value.locks[i].control > 119 || value.locks[i].value > 127) return false;
            for (int j = 0; j < i; ++j)
                if (value.locks[i].control == value.locks[j].control) return false;
        }
        notes[index] = value;
        return true;
    }

    bool setStep(int index, int note, int velocity, bool append = false) {
        if (index == -1) index = s;
        if (index < 0 || index >= length || note < 0 || note > 127 ||
            velocity < 0 || velocity > 127) return false;
        Step &value = notes[index];
        if (append && velocity) {
            for (int i = 0; i < value.count; ++i) if (value.notes[i] == note) return true;
            if (value.count == MaxNotes) return false;
            value.notes[value.count++] = uint8_t(note);
        } else {
            value.notes[0] = uint8_t(note);
            value.count = velocity ? 1 : 0;
        }
        value.velocity = uint8_t(velocity);
        return true;
    }

    const Step *getStep(int index) const {
        return index >= 0 && index < length ? &notes[index] : nullptr;
    }

    bool getStep(int index, uint8_t &note, uint8_t &velocity) const {
        if (index < 0 || index >= length) return false;
        note = notes[index].notes[0];
        velocity = notes[index].velocity;
        return true;
    }

    void clear(int index = -1) {
        if (index == -1) index = s;
        if (index >= 0 && index < length) notes[index] = Step{};
    }

    void step() {
        if (!play) return;
        releaseNote();
        triggered = false;
        pulsePhase = 0;
        const Step &value = notes[s];
        if (!muted && value.chance && (value.chance == 100 || roll() < value.chance)) {
            activeStep = value; // Running edits affect the next visit, not pending ratchets.
            for (int i = 0; i < value.lockCount; ++i)
                output.sendControl(value.locks[i].control, value.locks[i].value, channel);
            triggered = value.count && value.velocity;
            pressNote();
        }
        s = (s + 1) % length;
    }

    // Five non-step pulses follow each step at 24 PPQN. Ratchets divide these
    // six pulse slots exactly, independent of tempo and clock source.
    void pulse() {
        if (!play || pulsePhase >= 5) return;
        ++pulsePhase;
        if (triggered && pulsePhase % (6 / activeStep.ratchet) == 0) {
            releaseNote();
            pressNote();
        }
    }

    void stop() {
        releaseNote();
        triggered = false;
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
    NoteOutput &output;
    uint8_t channel;
    Random random;
    uint32_t randomState;
    bool muted = false;
    Step notes[Capacity] = {};
    int length = StepsPerPage;
    Step activeStep;
    bool sounding = false, triggered = false;
    uint8_t pulsePhase = 0;

    uint8_t roll() {
        uint32_t value;
        if (random) value = random();
        else {
            randomState ^= randomState << 13;
            randomState ^= randomState >> 17;
            randomState ^= randomState << 5;
            value = randomState;
        }
        return uint8_t((uint64_t(value) * 100) >> 32);
    }

    void pressNote() {
        if (!triggered) return;
        for (int i = 0; i < activeStep.count; ++i)
            output.press(activeStep.notes[i], activeStep.velocity, channel);
        sounding = true;
    }

    void releaseNote() {
        if (!sounding) return;
        for (int i = 0; i < activeStep.count; ++i) output.release(activeStep.notes[i], channel);
        sounding = false;
    }
};
