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

template <typename Pattern>
void expectStep(const Pattern &seq, int index, int note, int velocity) {
    uint8_t actualNote = 19, actualVelocity = 23;
    assert(seq.getStep(index, actualNote, actualVelocity));
    assert(actualNote == note && actualVelocity == velocity);
}

template <typename Pattern>
void expectUnreadableStep(const Pattern &seq, int index) {
    uint8_t note = 19, velocity = 23;
    assert(!seq.getStep(index, note, velocity));
    assert(note == 19 && velocity == 23);
}

void readsRespectActiveLength() {
    NoteOutput output(send);
    Sequencer seq(output);
    expectStep(seq, 0, 0, 0);
    assert(seq.setStep(95, 127, 127));
    expectStep(seq, 95, 127, 127);
    expectUnreadableStep(seq, -1); // Unlike setStep, reads have no sentinel.
    expectUnreadableStep(seq, -2);
    expectUnreadableStep(seq, 96);
    assert(seq.setLength(2));
    assert(seq.setStep(0, 0, 7));
    assert(seq.setStep(1, 60, 0));
    expectStep(seq, 0, 0, 7); // MIDI note zero is not a rest by itself.
    expectStep(seq, 1, 60, 0); // Rest reads retain the stored pitch.
    expectUnreadableStep(seq, 2);
    expectUnreadableStep(seq, 95); // Stored but outside the active length.
    assert(seq.setLength(96));
    expectStep(seq, 95, 127, 127);
    expect({});
}

void independentEditCursorsAndResize() {
    NoteOutput output(send);
    MultiTrackSequencer seq(output);
    const MultiTrackSequencer &view = seq;
    const int lengths[] = {5, 3, 96, 1};
    const int cursors[] = {3, 1, 95, 0};
    for (int track = 0; track < 4; ++track) {
        assert(seq.selectTrack(track));
        assert(view.editStep() == 0);
        assert(seq.setLength(lengths[track]));
        assert(seq.selectStep(cursors[track]));
        assert(seq.setStep(seq.editStep(), 60 + track, 90 + track));
        assert(!seq.selectStep(-1) && !seq.selectStep(lengths[track]));
        assert(!seq.selectStep(96));
        assert(view.editStep() == cursors[track] && seq.nextStep() == 0);
        expectUnreadableStep(view, -1);
        expectUnreadableStep(view, lengths[track]);
        if (cursors[track] != 0) expectStep(view, 0, 0, 0);
    }
    seq.resume();
    seq.step(); seq.step();
    expect({{4,63,93,true},{2,61,91,true},{4,63,0,false},{4,63,93,true}});
    const int positions[] = {2, 2, 2, 0};
    for (int track = 0; track < 4; ++track) {
        assert(seq.selectTrack(track));
        assert(view.editStep() == cursors[track]);
        assert(seq.nextStep() == positions[track]);
        expectStep(view, cursors[track], 60 + track, 90 + track);
    }
    assert(!seq.selectTrack(-1) && !seq.selectTrack(4));
    assert(seq.selectedTrack() == 3 && seq.editStep() == 0);
    seq.selectTrack(0);
    assert(!seq.setLength(0) && !seq.setLength(97));
    assert(seq.playing() && seq.editStep() == 3 && seq.nextStep() == 2);
    assert(seq.setLength(2)); // Shrink clamps just this cursor and stops all tracks.
    expect({{2,61,0,false},{4,63,0,false}});
    assert(!seq.playing() && seq.editStep() == 1 && seq.nextStep() == 0);
    expectUnreadableStep(view, 3);
    assert(seq.setLength(6));
    assert(seq.editStep() == 1); // Growing must not restore a previous cursor.
    assert(seq.setLength(3) && seq.editStep() == 1); // In-range cursors survive shrink.
    assert(seq.setLength(1) && seq.editStep() == 0);
    for (int track = 1; track < 4; ++track) {
        seq.selectTrack(track);
        assert(seq.editStep() == cursors[track]);
        assert(seq.nextStep() == positions[track]);
    }
    seq.stop(); seq.step(); seq.resume(); seq.step(); seq.reset();
    seq.resume(); seq.step(); // Start's reset/resume sequence must not select step zero.
    seq.stop();
    expect({{4,63,93,true},{4,63,0,false},{4,63,93,true},{4,63,0,false}});
    for (int track = 0; track < 4; ++track) {
        seq.selectTrack(track);
        assert(seq.editStep() == (track == 0 ? 0 : cursors[track]));
    }
}

void runningEditsPreservePlaybackAndReleases() {
    NoteOutput output(send);
    MultiTrackSequencer seq(output);
    assert(seq.setLength(3));
    assert(seq.setStep(0, 60, 90));
    assert(seq.setStep(1, 62, 91));
    assert(seq.setStep(2, 64, 92));
    seq.resume(); seq.step();
    expect({{1,60,90,true}});
    assert(seq.selectStep(0));
    assert(seq.setStep(seq.editStep(), 70, 80)); // Replace the sounding step.
    assert(seq.nextStep() == 1 && seq.playing());
    assert(seq.selectStep(2));
    assert(seq.setStep(seq.editStep(), 74, 82)); // Edit ahead, not nextStep().
    expectStep(seq, 0, 70, 80);
    expectStep(seq, 2, 74, 82);
    assert(seq.nextStep() == 1);
    expect({}); // Reads, selection and writes never emit or release notes.
    seq.step();
    expect({{1,60,0,false},{1,62,91,true}}); // Release original pitch, not 70.
    assert(seq.editStep() == 2 && seq.nextStep() == 2);
    seq.stop(); seq.step();
    expect({{1,62,0,false}});
    assert(seq.editStep() == 2 && seq.nextStep() == 2);
    seq.resume(); seq.step();
    expect({{1,74,82,true}});
    assert(seq.selectStep(1));
    assert(seq.setStep(-1, 77, 83)); // Legacy sentinel still writes next playback step.
    expectStep(seq, 0, 77, 83);
    expectStep(seq, 1, 62, 91);
    seq.clear(); // Likewise clear is playback-relative, not editor-relative.
    expectStep(seq, 0, 0, 0);
    expectStep(seq, 1, 62, 91);
    assert(seq.editStep() == 1 && seq.nextStep() == 0);
    seq.step();
    expect({{1,74,0,false}});
    assert(seq.editStep() == 1 && seq.nextStep() == 1);
    seq.reset(); seq.resume(); seq.step(); seq.step();
    expect({{1,62,91,true}});
    assert(seq.editStep() == 1 && seq.nextStep() == 2);
    seq.stop();
    expect({{1,62,0,false}});
}

int main() {
    independentLengthsAndChannels();
    mutePreservesPhaseAndLiveOwner();
    ownershipIsScopedToChannelAndPitch();
    readsRespectActiveLength();
    independentEditCursorsAndResize();
    runningEditsPreservePlaybackAndReleases();
    puts("Multi-track sequencer regression tests passed");
}
