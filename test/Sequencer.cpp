#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <vector>
#include "Sequencer.h"
#include "Clock.h"

struct Event { int note, velocity; bool on; };
std::vector<Event> events;

void send(uint8_t note, uint8_t velocity, bool on, uint8_t channel) {
    assert(channel == 1);
    events.push_back({note, velocity, on});
}

void expect(std::initializer_list<Event> expected) {
    assert(events.size() == expected.size());
    unsigned int i = 0;
    for (const auto &event : expected) {
        assert(events[i].note == event.note);
        assert(events[i].velocity == event.velocity);
        assert(events[i++].on == event.on);
    }
    events.clear();
}

void restsVelocitiesAndWraparound() {
    NoteOutput output(send);
    Sequencer seq(output);
    seq.start();
    seq.step();
    seq.stop();
    expect({});
    assert(seq.setLength(3));
    assert(seq.setStep(0, 0, 7));
    assert(seq.setStep(2, 127, 113));
    seq.start();
    seq.step(); // Rest releases the first note.
    seq.step();
    seq.step(); // Wrap must release the last note, not read notes[-1].
    seq.stop();
    seq.stop();
    expect({{0, 7, true}, {0, 0, false}, {127, 113, true},
            {127, 0, false}, {0, 7, true}, {0, 0, false}});
}

void stopContinueStartAndEdits() {
    NoteOutput output(send);
    Sequencer seq(output);
    assert(seq.setLength(3));
    assert(seq.setStep(0, 60, 91));
    assert(seq.setStep(1, 67, 82));
    assert(seq.setStep(2, 73, 99));
    seq.start();
    seq.step();
    seq.stop();
    seq.step(); // Stopped updates must neither sound nor advance.
    assert(!seq.play && seq.s == 2);
    seq.resume();
    seq.step();
    assert(seq.s == 0);
    // Editing a sounding step must not change which pitch gets released.
    assert(seq.setStep(2, 79, 21));
    seq.start();
    seq.reset();
    expect({{60, 91, true}, {60, 0, false}, {67, 82, true}, {67, 0, false},
            {73, 99, true}, {73, 0, false}, {60, 91, true}, {60, 0, false}});
    assert(!seq.play && seq.s == 0);
    seq.resume();
    seq.clear(); // Clear the next step before it is played.
    seq.step();
    assert(seq.setStep(-1, 42, 101)); // Edit next step using musical values.
    seq.step();
    seq.stop();
    expect({{42, 101, true}, {42, 0, false}});
}

void liveAndSequencedNotesShareOwnership() {
    NoteOutput output(send);
    Sequencer seq(output);
    assert(seq.setStep(0, 60, 111));
    output.press(60, 87); // Held live note owns the first note-on's velocity.
    seq.start();
    seq.stop();
    expect({{60, 87, true}});
    output.release(60);
    expect({{60, 0, false}});
    seq.start();
    output.press(60, 87);
    output.release(60);
    expect({{60, 111, true}});
    seq.stop();
    expect({{60, 0, false}});
    output.release(60); // A stale release must not emit an extra note-off.
    expect({});
}

void boundedPatternsAndPanic() {
    NoteOutput output(send);
    Sequencer seq(output);
    assert(seq.getLength() == 16);
    assert(!seq.setLength(0) && !seq.setLength(65));
    assert(seq.setLength(64));
    assert(!seq.setStep(-2, 60, 90) && !seq.setStep(64, 60, 90));
    assert(!seq.setStep(0, -1, 90) && !seq.setStep(0, 128, 90));
    assert(!seq.setStep(0, 60, -1) && !seq.setStep(0, 60, 128));
    assert(seq.setStep(0, 60, 0)); // Velocity zero is a rest, not note-on zero.
    assert(seq.setStep(63, 127, 127));
    seq.start();
    for (int i = 1; i < 64; ++i) seq.step();
    assert(seq.s == 0);
    assert(seq.setLength(1)); // Resizing stops and releases the active note.
    expect({{127, 127, true}, {127, 0, false}});
    assert(!seq.setStep(1, 60, 90));
    output.press(0, 1);
    output.press(127, 126);
    output.press(127, 95);
    output.panic();
    output.panic();
    output.release(127);
    expect({{0, 1, true}, {127, 126, true}, {0, 0, false}, {127, 0, false}});
}

uint32_t randomValue = 0;
uint32_t controlledRandom() { return randomValue; }
void sendControl(uint8_t control, uint8_t value, uint8_t channel) {
    assert(channel == 1);
    events.push_back({128 + control, value, false}); // CCs share the ordered event log.
}

void chanceAndLocksApplyOnceToTheWholeStep() {
    NoteOutput output(send, sendControl);
    Sequencer seq(output, 1, controlledRandom);
    seq.setLength(1);
    seq.setStep(0, 60, 81);
    seq.setStep(0, 67, 81, true);
    Sequencer::Step value = *seq.getStep(0);
    value.chance = 25; value.ratchet = 2;
    value.lockCount = 2;
    value.locks[0].control = 74; value.locks[0].value = 0;
    value.locks[1].control = 1; value.locks[1].value = 127;
    assert(seq.setStep(0, value));
    randomValue = 0x40000000; // Exactly 25: must fail chance=25 (strictly less).
    seq.start();
    for (int i = 0; i < 5; ++i) seq.pulse();
    expect({});
    randomValue = 0x3fffffff; // Just below 25: must trigger the entire chord/locks.
    seq.step();
    expect({{202,0,false},{129,127,false},{60,81,true},{67,81,true}});
    randomValue = UINT32_MAX; // A reroll would now fail; pending ratchets must still sound.
    seq.pulse(); seq.pulse(); expect({});
    seq.pulse();
    expect({{60,0,false},{67,0,false},{60,81,true},{67,81,true}});
    randomValue = 0;
    value.chance = 0; seq.setStep(0, value); seq.step();
    expect({{60,0,false},{67,0,false}}); // Chance zero still releases the old chord.
    value.chance = 100; value.count = 0; value.velocity = 0;
    seq.setStep(0, value); randomValue = UINT32_MAX;
    seq.step();
    expect({{202,0,false},{129,127,false}}); // Automation-only step, including CC value zero.
    seq.setMuted(true); seq.step(); expect({});
    seq.setMuted(false); seq.clear(0); seq.step(); expect({});
    assert(seq.getStep(0)->chance == 100 && seq.getStep(0)->ratchet == 1);
    seq.stop();
}

void chordRatchetTimingAndCancellation() {
    const int counts[] = {1, 2, 3, 6};
    // Independently specified MIDI pulse schedules, including the first hit.
    const bool hits[][6] = {{true,false,false,false,false,false},
                            {true,false,false,true,false,false},
                            {true,false,true,false,true,false},
                            {true,true,true,true,true,true}};
    for (int pattern = 0; pattern < 4; ++pattern) {
        NoteOutput output(send);
        Sequencer seq(output);
        seq.setLength(1);
        seq.setStep(0, 0, 93); seq.setStep(0, 64, 93, true); seq.setStep(0, 127, 93, true);
        Sequencer::Step value = *seq.getStep(0); value.ratchet = counts[pattern];
        seq.setStep(0, value); seq.start();
        expect({{0,93,true},{64,93,true},{127,93,true}});
        seq.clear(0); // Current chord/ratchets are a snapshot, not the edited step.
        for (int pulse = 1; pulse < 6; ++pulse) {
            seq.pulse();
            if (hits[pattern][pulse])
                expect({{0,0,false},{64,0,false},{127,0,false},
                        {0,93,true},{64,93,true},{127,93,true}});
            else expect({});
        }
        seq.pulse(); expect({}); // No extra hit after the five non-step pulses.
        seq.step(); expect({{0,0,false},{64,0,false},{127,0,false}});
        seq.setStep(0, value); seq.step(); expect({{0,93,true},{64,93,true},{127,93,true}});
        seq.stop(); seq.pulse(); seq.resume(); seq.pulse();
        expect({{0,0,false},{64,0,false},{127,0,false}});
        seq.step(); expect({{0,93,true},{64,93,true},{127,93,true}});
        seq.setMuted(true); seq.setMuted(false); seq.pulse();
        expect({{0,0,false},{64,0,false},{127,0,false}}); // Unmute waits for the next step.
        seq.step(); expect({{0,93,true},{64,93,true},{127,93,true}});
        seq.setLength(2); seq.pulse(); expect({{0,0,false},{64,0,false},{127,0,false}});
    }
}

void chordBoundsAndLiveOwnership() {
    NoteOutput output(send);
    Sequencer seq(output);
    assert(seq.setStep(0, 60, 90));
    assert(seq.setStep(0, 64, 90, true) && seq.setStep(0, 67, 90, true));
    assert(seq.setStep(0, 71, 90, true) && seq.setStep(0, 60, 90, true));
    assert(seq.getStep(0)->count == 4 && !seq.setStep(0, 72, 90, true));
    Sequencer::Step valid = *seq.getStep(0), invalid = valid;
    invalid.count = 5; assert(!seq.setStep(0, invalid));
    invalid = valid; invalid.notes[2] = 60; assert(!seq.setStep(0, invalid));
    invalid = valid; invalid.notes[2] = 128; assert(!seq.setStep(0, invalid));
    invalid = valid; invalid.chance = 101; assert(!seq.setStep(0, invalid));
    invalid = valid; invalid.ratchet = 4; assert(!seq.setStep(0, invalid));
    invalid = valid; invalid.lockCount = 5; assert(!seq.setStep(0, invalid));
    invalid = valid; invalid.lockCount = 2; assert(!seq.setStep(0, invalid)); // Duplicate CC 0.
    invalid.lockCount = 1; invalid.locks[0].control = 120; assert(!seq.setStep(0, invalid));
    invalid.locks[0].control = 119; invalid.locks[0].value = 128; assert(!seq.setStep(0, invalid));
    assert(seq.getStep(0)->count == 4 && seq.getStep(0)->chance == 100);
    valid.ratchet = 6; seq.setStep(0, valid);
    output.press(64, 80); seq.start();
    expect({{64,80,true},{60,90,true},{67,90,true},{71,90,true}});
    seq.pulse();
    expect({{60,0,false},{67,0,false},{71,0,false},{60,90,true},{67,90,true},{71,90,true}});
    seq.stop();
    expect({{60,0,false},{67,0,false},{71,0,false}});
    output.release(64); expect({{64,0,false}});
    seq.setStep(0, 72, 77); // Normal entry replaces the chord, retaining ratchet settings.
    assert(seq.getStep(0)->count == 1 && seq.getStep(0)->ratchet == 6);
}

void gatesReleaseBeforeTheNextStep() {
    for (int gate = 1; gate <= 6; ++gate) {
        NoteOutput output(send);
        Sequencer seq(output);
        seq.setStep(0, 60, 79); seq.setStep(0, 67, 79, true);
        auto value = *seq.getStep(0); value.gate = gate;
        assert(seq.setStep(0, value));
        value.gate = 0; assert(!seq.setStep(0, value));
        value.gate = 7; assert(!seq.setStep(0, value));
        seq.start(); expect({{60,79,true},{67,79,true}});
        for (int pulse = 1; pulse < 6; ++pulse) {
            seq.pulse();
            assert(seq.s == 1); // A release is independent of step advancement.
            if (pulse == gate) expect({{60,0,false},{67,0,false}});
            else expect({});
        }
        seq.step();
        if (gate == 6) expect({{60,0,false},{67,0,false}});
        else expect({});
        seq.stop(); expect({});
    }
}

void ratchetGatesAndEditsUseTheActiveSnapshot() {
    NoteOutput output(send);
    Sequencer seq(output);
    seq.setLength(1); seq.setStep(0, 60, 83);
    auto value = *seq.getStep(0); value.gate = 2; value.ratchet = 2;
    seq.setStep(0, value); seq.start(); expect({{60,83,true}});
    value.gate = 5; value.velocity = 117; seq.setStep(0, value);
    seq.pulse(); expect({});
    seq.pulse(); expect({{60,0,false}}); // Short gate ends before ratchet at pulse 3.
    seq.pulse(); expect({{60,83,true}});
    seq.pulse(); expect({});
    seq.pulse(); expect({{60,0,false}}); // Second hit still uses old gate/velocity.
    seq.step(); expect({{60,117,true}});
    seq.pulse(); seq.pulse(); expect({});
    seq.pulse(); expect({{60,0,false},{60,117,true}}); // Long gate cut by next ratchet.
    seq.pulse(); seq.pulse(); expect({});
    seq.stop(); expect({{60,0,false}});
    // 3x and 6x intervals are shorter than the gate: one off then on per hit.
    for (int ratchet : {3, 6}) {
        value.gate = 5; value.ratchet = ratchet; seq.setStep(0, value);
        seq.start(); expect({{60,117,true}});
        for (int pulse = 1; pulse < 6; ++pulse) {
            seq.pulse();
            if (ratchet == 6 || pulse == 2 || pulse == 4)
                expect({{60,0,false},{60,117,true}});
            else expect({});
        }
        seq.stop(); expect({{60,0,false}});
    }
    value.gate = 1; value.ratchet = 3; seq.setStep(0, value);
    seq.start(); expect({{60,117,true}});
    seq.pulse(); expect({{60,0,false}});
    seq.pulse(); expect({{60,117,true}});
    seq.pulse(); expect({{60,0,false}});
    seq.pulse(); expect({{60,117,true}});
    seq.pulse(); expect({{60,0,false}});
    seq.stop(); expect({});
}

void gateCleanupAndLiveOwners() {
    for (int operation = 0; operation < 4; ++operation) {
        NoteOutput output(send);
        Sequencer seq(output);
        seq.setStep(0, 60, 93); seq.setStep(0, 64, 93, true);
        auto value = *seq.getStep(0); value.gate = 2; value.ratchet = 2;
        seq.setStep(0, value);
        output.press(64, 71);
        seq.start(); expect({{64,71,true},{60,93,true}});
        if (operation == 0) seq.stop();
        else if (operation == 1) seq.setMuted(true);
        else if (operation == 2) seq.setLength(3);
        else { seq.stop(); output.panic(); } // Route/mode change ordering.
        if (operation == 3) expect({{60,0,false},{64,0,false}});
        else expect({{60,0,false}});
        for (int pulse = 0; pulse < 5; ++pulse) seq.pulse();
        expect({}); // No delayed off or ratchet after cancellation.
        output.release(64);
        if (operation == 3) expect({});
        else expect({{64,0,false}});
    }
    NoteOutput output(send); Sequencer seq(output);
    seq.setStep(0, 60, 90); auto value = *seq.getStep(0); value.gate = 1;
    seq.setStep(0, value); seq.start(); output.press(60, 65);
    expect({{60,90,true}});
    seq.pulse(); expect({}); // Gate releases only the sequencer's ownership.
    seq.stop(); output.release(60); expect({{60,0,false}});
}

void gateTimingUsesActualClockPulses() {
    for (int bpm : {20, 137, 300}) for (uint32_t start : {0U, UINT32_MAX - 4000U}) {
        NoteOutput output(send); Sequencer seq(output); Clock clk;
        seq.setStep(0, 72, 101); auto value = *seq.getStep(0); value.gate = 2;
        seq.setStep(0, value); seq.resume(); clk.setBPM(bpm, start); clk.start(start);
        assert(clk.poll(start) == Clock::Step); seq.step(); expect({{72,101,true}});
        // Independently ceil pulse deadlines: checks fractional periods and wrap.
        const uint32_t rate = uint32_t(bpm * 24);
        for (uint32_t pulse = 1; pulse <= 3; ++pulse) {
            const uint32_t deadline = uint32_t((60000000ULL * pulse + rate - 1) / rate);
            assert(clk.poll(start + deadline - 1) == Clock::None); expect({});
            assert(clk.poll(start + deadline) == Clock::Pulse); seq.pulse();
            if (pulse == 2) expect({{72,0,false}}); else expect({});
        }
        seq.stop(); expect({});
    }
    for (Clock::Source source : {Clock::BLE, Clock::TRS}) {
        NoteOutput output(send); Sequencer seq(output); Clock clk;
        seq.setStep(0, 75, 87); auto value = *seq.getStep(0); value.gate = 2;
        seq.setStep(0, value); seq.resume(); clk.setSource(source); clk.start(0);
        assert(clk.externalPulse(1000) == Clock::Step); seq.step(); expect({{75,87,true}});
        assert(clk.poll(100000) == Clock::None); expect({}); // Never guess a gate deadline.
        assert(clk.externalPulse(125000) == Clock::Pulse); seq.pulse(); expect({});
        assert(clk.externalPulse(130000) == Clock::Pulse); seq.pulse(); expect({{75,0,false}});
        seq.stop(); expect({});
        // A missing master still releases a long gated note through the timeout.
        value.gate = 6; seq.setStep(0, value); seq.reset(); seq.resume(); clk.start(200000);
        assert(clk.externalPulse(201000) == Clock::Step); seq.step(); expect({{75,87,true}});
        assert(clk.poll(700999) == Clock::None);
        assert(clk.poll(701000) == Clock::Overrun); seq.stop(); expect({{75,0,false}});
    }
}

int main() {
    gatesReleaseBeforeTheNextStep();
    ratchetGatesAndEditsUseTheActiveSnapshot();
    gateCleanupAndLiveOwners();
    gateTimingUsesActualClockPulses();
    restsVelocitiesAndWraparound();
    stopContinueStartAndEdits();
    liveAndSequencedNotesShareOwnership();
    boundedPatternsAndPanic();
    chanceAndLocksApplyOnceToTheWholeStep();
    chordRatchetTimingAndCancellation();
    chordBoundsAndLiveOwnership();
    puts("Sequencer note safety regression tests passed");
}
