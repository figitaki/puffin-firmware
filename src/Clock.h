#pragma once

#include <stdint.h>

// Internal sequencing uses 48 PPQN; MIDI clock stays straight at 24 PPQN.
// External sequencing follows actual 24-PPQN arrivals without interpolation.
// All timestamps are monotonic microseconds, including external arrivals.
class Clock {
public:
    enum Source { Internal, BLE, TRS };
    enum Tick { None, Pulse, Step, Overrun };

    Source source() const { return selected; }
    bool running() const { return playing; }
    int getBPM() const { return selected == Internal ? bpm : measuredBPM; }
    int internalBPM() const { return bpm; }
    int swing() const { return swingAmount; }
    bool midiPulse() const { return outputPulse; }
    int pulseScale() const { return selected == Internal ? 2 : 1; }
    int stepDuration() const { return duration; }

    bool setSwing(int value) {
        if (value < 0 || value > 2) return false;
        swingAmount = uint8_t(value); // Internal changes latch on the next pair.
        return true;
    }

    bool setBPM(int value, uint32_t now) {
        if (value < 20 || value > 300) return false;
        accrue(now); // Account for elapsed time at the previous tempo first.
        bpm = value;
        return true;
    }

    void setSource(Source value) {
        stop();
        selected = value;
        phase = 0;
        measuredBPM = 0;
        measuredPulses = 0;
        measuring = false;
    }

    void start(uint32_t now) {
        phase = 0;
        playing = false;
        resume(now);
    }

    bool resume(uint32_t now) {
        if (playing) return false;
        playing = true;
        lastUpdate = now;
        lastExternal = now;
        // Internal Start/Continue has an immediate sequencer tick; only every
        // other internal tick emits MIDI clock. External waits for TimingClock.
        credit = selected == Internal ? Units : 0;
        return true;
    }

    void stop() { playing = false; credit = 0; }

    Tick poll(uint32_t now) {
        if (!playing) return None;
        if (selected != Internal) {
            if (uint32_t(now - lastExternal) < 500000) return None;
            stop();
            return Overrun; // Lost master: don't leave a sequence note held.
        }
        accrue(now);
        // Never burst multiple overdue ticks/notes into the output. A stall
        // of two pending 48-PPQN ticks is a fault; the caller must release
        // the sequence and send Stop before any further playback.
        if (credit >= 2 * Units) { stop(); return Overrun; }
        if (credit < Units) return None;
        credit -= Units;
        return advance();
    }

    Tick externalPulse(uint32_t now) {
        if (selected == Internal) return None;
        if (playing && uint32_t(now - lastExternal) >= 500000) {
            stop();
            return Overrun; // A late arrival must not hide a lost-clock fault.
        }
        lastExternal = now;
        if (!measuring) {
            measuring = true;
            measuredAt = now;
        } else if (++measuredPulses == 24) {
            const uint32_t elapsed = now - measuredAt;
            const uint32_t value = elapsed ? uint32_t(60000000ULL / elapsed) : 0;
            if (value >= 20 && value <= 300) measuredBPM = int(value);
            measuredAt = now;
            measuredPulses = 0;
        }
        return playing ? advance() : None;
    }

private:
    static const uint64_t Units = 60000000ULL;
    Source selected = Internal;
    int bpm = 120;
    int measuredBPM = 0;
    bool playing = false;
    uint8_t phase = 0, swingAmount = 0, activeSwing = 0;
    uint8_t duration = 12;
    bool outputPulse = false;
    bool measuring = false;
    uint8_t measuredPulses = 0;
    uint32_t measuredAt = 0;
    uint32_t lastExternal = 0;
    uint32_t lastUpdate = 0;
    uint64_t credit = 0;

    void accrue(uint32_t now) {
        if (playing && selected == Internal)
            credit += uint64_t(uint32_t(now - lastUpdate)) * uint32_t(bpm * 48);
        lastUpdate = now;
    }

    Tick advance() {
        if (selected != Internal) {
            outputPulse = true;
            duration = 6;
            const Tick tick = phase == 0 ? Step : Pulse;
            phase = (phase + 1) % 6;
            return tick;
        }
        if (phase == 0) activeSwing = swingAmount;
        const int longStep = 12 + activeSwing * 2;
        const bool step = phase == 0 || phase == longStep;
        if (step) duration = uint8_t(phase == 0 ? longStep : 24 - longStep);
        outputPulse = phase % 2 == 0; // Straight 24-PPQN wire clock, never swung.
        phase = (phase + 1) % 24;
        return step ? Step : Pulse;
    }
};
