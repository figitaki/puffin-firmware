#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <vector>
#include "Sequencer.h"

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

int main() {
    restsVelocitiesAndWraparound();
    stopContinueStartAndEdits();
    liveAndSequencedNotesShareOwnership();
    boundedPatternsAndPanic();
    puts("Sequencer note safety regression tests passed");
}
