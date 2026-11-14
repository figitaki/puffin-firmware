#include "NoteKeyboard.h"
#include "Sequencer.h"
#include <cassert>
#include <cstdio>
#include <vector>

struct Event { int note, velocity, channel; bool on; };
static std::vector<Event> events;
static void send(uint8_t note, uint8_t velocity, bool on, uint8_t channel) {
    events.push_back({note, velocity, channel, on});
}

int main() {
    NoteOutput output(send);
    NoteKeyboard keyboard(output);
    assert(keyboard.octave() == 0);
    assert(keyboard.press(60, 93, 2) == 60);
    keyboard.shiftOctave(1);
    assert(keyboard.press(65, 71, 4) == 77);
    keyboard.shiftOctave(-2);
    keyboard.release(60); keyboard.release(65);
    assert(events.size() == 4);
    assert(events[0].note == 60 && events[0].channel == 2 && events[0].velocity == 93 && events[0].on);
    assert(events[1].note == 77 && events[1].channel == 4 && events[1].velocity == 71 && events[1].on);
    assert(events[2].note == 60 && events[2].channel == 2 && !events[2].on);
    assert(events[3].note == 77 && events[3].channel == 4 && !events[3].on);
    events.clear();

    // Different physical keys can land on the same pitch after an octave shift.
    keyboard.shiftOctave(1);
    assert(keyboard.press(65, 89, 1) == 65);
    assert(keyboard.press(65, 89, 1) == -1);
    keyboard.shiftOctave(1);
    assert(keyboard.press(53, 72, 1) == 65);
    keyboard.release(65);
    assert(events.size() == 1 && keyboard.active(5));
    keyboard.release(53);
    assert(events.size() == 2 && !events.back().on && !keyboard.active(5));
    events.clear();

    // Panic releases each captured channel/pitch and makes late key-ups harmless.
    keyboard.press(60, 91, 1); keyboard.press(72, 101, 3);
    keyboard.releaseAll();
    const size_t count = events.size();
    keyboard.release(60); keyboard.release(72); keyboard.releaseAll();
    assert(count == 4 && events.size() == count && !keyboard.active(0));
    assert(events[2].note == 72 && events[2].channel == 1 && !events[2].on);
    assert(events[3].note == 84 && events[3].channel == 3 && !events[3].on);

    // Both ends stay in MIDI range without dropping edge keys.
    for (int direction : {-1, 1}) {
        for (int i = 0; i < 12; ++i) keyboard.shiftOctave(direction);
        assert(keyboard.octave() == direction * 4);
        for (int key : {53, 72}) {
            assert(keyboard.press(key, 127, 1) == key + direction * 48);
            keyboard.release(key);
        }
    }
    events.clear();
    for (int key : {0, 52, 73, 127}) assert(keyboard.press(key, 90, 1) == -1);
    assert(keyboard.press(60, 0, 1) == -1 && keyboard.press(60, 128, 1) == -1);
    assert(keyboard.press(60, 90, 0) == -1 && keyboard.press(60, 90, 17) == -1);
    assert(events.empty());

    // A held sequence owner must survive a live-key release at the same pitch.
    keyboard.shiftOctave(-4);
    Sequencer seq(output); seq.setStep(0, 60, 88); seq.start();
    keyboard.press(60, 95, 1); keyboard.shiftOctave(1); keyboard.release(60);
    assert(events.size() == 1 && events[0].on);
    seq.stop(); assert(events.size() == 2 && !events.back().on);
    puts("Note keyboard octave and held-note safety tests passed");
}
