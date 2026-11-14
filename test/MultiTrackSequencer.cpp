#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <vector>
#include "MultiTrackSequencer.h"

struct Event { int channel, note, velocity; bool on; };
std::vector<Event> events;
void send(uint8_t note, uint8_t velocity, bool on, uint8_t channel) {
    events.push_back({channel, note, velocity, on});
}
void expect(std::initializer_list<Event> expected) {
    assert(events.size() == expected.size());
    unsigned int i = 0;
    for (const auto &event : expected) {
        const auto &actual = events[i++];
        assert(actual.channel == event.channel && actual.note == event.note);
        assert(actual.velocity == event.velocity && actual.on == event.on);
    }
    events.clear();
}

void independentLengthsAndChannels() {
    NoteOutput output(send);
    MultiTrackSequencer seq(output);
    const int lengths[] = {2, 3, 4, 1};
    for (int track = 0; track < 4; ++track) {
        assert(seq.selectTrack(track));
        assert(seq.channel() == track + 1);
        assert(seq.setLength(lengths[track]));
    }
    seq.selectTrack(0); seq.setStep(0, 60, 91); seq.setStep(1, 62, 92);
    seq.selectTrack(1); seq.setStep(0, 60, 73); seq.setStep(1, 65, 74); seq.setStep(2, 67, 75);
    seq.selectTrack(2); seq.setStep(0, 72, 40); seq.setStep(3, 75, 41);
    seq.selectTrack(3); seq.setStep(0, 127, 1);
    assert(!seq.selectTrack(-1) && !seq.selectTrack(4));
    assert(seq.selectedTrack() == 3);
    seq.reset(); seq.resume(); seq.step();
    expect({{1,60,91,true},{2,60,73,true},{3,72,40,true},{4,127,1,true}});
    seq.step();
    expect({{1,60,0,false},{1,62,92,true},{2,60,0,false},{2,65,74,true},
            {3,72,0,false},{4,127,0,false},{4,127,1,true}});
    seq.step();
    expect({{1,62,0,false},{1,60,91,true},{2,65,0,false},{2,67,75,true},
            {4,127,0,false},{4,127,1,true}});
    const int positions[] = {1, 0, 3, 0};
    for (int track = 0; track < 4; ++track) {
        seq.selectTrack(track);
        assert(seq.nextStep() == positions[track]);
        assert(seq.length() == lengths[track]);
    }
    seq.stop(); seq.stop(); seq.step();
    expect({{1,60,0,false},{2,67,0,false},{4,127,0,false}});
}

void mutePreservesPhaseAndLiveOwner() {
    NoteOutput output(send);
    MultiTrackSequencer seq(output);
    seq.setLength(3);
    seq.setStep(0,60,80); seq.setStep(1,61,81); seq.setStep(2,62,82);
    seq.selectTrack(1); seq.setLength(2);
    seq.setStep(0,60,90); seq.setStep(1,65,91);
    seq.reset(); seq.resume(); seq.step();
    expect({{1,60,80,true},{2,60,90,true}});
    output.press(60,88,1);
    seq.selectTrack(0); seq.setMuted(true); seq.setMuted(true);
    assert(seq.playing() && seq.muted());
    expect({}); // Same-channel live owner keeps this pitch sounding.
    seq.step();
    assert(seq.nextStep() == 2);
    expect({{2,60,0,false},{2,65,91,true}});
    output.release(60,1);
    expect({{1,60,0,false}});
    seq.setMuted(false);
    expect({}); // Unmute waits for the next step rather than retriggering now.
    seq.step();
    expect({{1,62,82,true},{2,65,0,false},{2,60,90,true}});
    assert(!seq.setLength(0) && !seq.setLength(97) && seq.playing());
    seq.selectTrack(1);
    assert(seq.setLength(1)); // Valid resize stops every track.
    expect({{2,60,0,false},{1,62,0,false}});
    assert(!seq.playing() && seq.nextStep() == 0);
    seq.selectTrack(0);
    assert(seq.nextStep() == 0 && seq.length() == 3);
    seq.clear(); // Clear only this selected track's next step.
    seq.resume(); seq.step();
    expect({{2,60,90,true}});
    seq.stop(); seq.resume(); seq.step();
    expect({{2,60,0,false},{1,61,81,true},{2,60,90,true}});
    seq.reset();
    expect({{1,61,0,false},{2,60,0,false}});
    assert(!seq.playing() && seq.nextStep() == 0);
}

void ownershipIsScopedToChannelAndPitch() {
    NoteOutput output(send);
    output.press(0,7,1); output.press(0,99,16); output.press(0,80,1);
    expect({{1,0,7,true},{16,0,99,true}});
    output.release(0,1); output.release(0,16);
    expect({{16,0,0,false}});
    output.press(127,50,0); output.press(127,50,17);
    output.release(0,0); output.release(0,17);
    expect({});
    output.press(127,126,16);
    output.panic(); output.panic(); output.release(0,1);
    expect({{16,127,126,true},{1,0,0,false},{16,127,0,false}});
}

int main() {
    independentLengthsAndChannels();
    mutePreservesPhaseAndLiveOwner();
    ownershipIsScopedToChannelAndPitch();
    puts("Multi-track sequencer regression tests passed");
}
