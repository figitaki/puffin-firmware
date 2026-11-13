#include <assert.h>
#include <stdio.h>
#include <vector>
#include "ProjectSnapshot.h"

struct Event { int number, value, channel; bool on; };
static std::vector<Event> events;
static void note(uint8_t n, uint8_t v, bool on, uint8_t ch) {
    events.push_back({n, v, ch, on});
}
static void control(uint8_t cc, uint8_t v, uint8_t ch) {
    events.push_back({128 + cc, v, ch, false});
}
static uint32_t randomZero() { return 0; }
static void checksum(std::vector<uint8_t> &wire) {
    const uint32_t crc = ProjectCodec::crc32(wire.data(), wire.size(), true);
    for (int i = 0; i < 4; ++i) wire[24 + i] = uint8_t(crc >> (8 * i));
}

static ProjectSnapshot asymmetric() {
    ProjectSnapshot p;
    for (int i = 0; i < 16; ++i) p.projectID[i] = uint8_t(i * 7 + 1);
    p.bpm = 237;
    const uint8_t lengths[] = {1, 19, 37, 64};
    const uint8_t ratchets[] = {1, 2, 3, 6};
    for (int t = 0; t < 4; ++t) {
        auto &track = p.tracks[t];
        track.length = lengths[t];
        track.muted = t == 1;
        for (int s = 0; s < 64; ++s) {
            auto &v = track.steps[s];
            v.count = uint8_t((s + t) % 5);
            v.velocity = uint8_t((s * 3 + t * 11) % 128);
            v.chance = uint8_t((s + 13 * t) % 101);
            v.ratchet = ratchets[(s + t) % 4];
            v.lockCount = uint8_t((s + t + 2) % 5);
            for (int n = 0; n < 4; ++n) {
                v.notes[n] = uint8_t((s + t * 19 + n * 7) % 128);
                v.locks[n].control = uint8_t((s + t + n * 23) % 120);
                v.locks[n].value = uint8_t((s * 2 + t + n) % 128);
            }
        }
    }
    auto &chord = p.tracks[0].steps[0];
    chord.count = 2; chord.notes[0] = 60; chord.notes[1] = 67;
    chord.velocity = 91; chord.chance = 100; chord.ratchet = 2;
    chord.lockCount = 1; chord.locks[0].control = 74; chord.locks[0].value = 21;
    auto &automation = p.tracks[3].steps[0];
    automation.count = 0; automation.velocity = 0; automation.chance = 100;
    automation.lockCount = 1;
    automation.locks[0].control = 119; automation.locks[0].value = 127;
    assert(p.valid());
    return p;
}

static void roundtripAndPlayback() {
    ProjectSnapshot source = asymmetric(), restored;
    std::vector<uint8_t> wire(ProjectCodec::Size);
    assert(ProjectCodec::encode(source, UINT64_C(0xfedcba9876543210), wire.data(), wire.size()));
    uint64_t generation = 0;
    assert(ProjectCodec::decode(wire.data(), wire.size(), restored, generation) == ProjectCodec::Valid);
    assert(generation == UINT64_C(0xfedcba9876543210));
    assert(ProjectCodec::sameContent(source, restored));
    assert(restored.tracks[0].steps[63].notes[0] == source.tracks[0].steps[63].notes[0]);
    NoteOutput output(note, control);
    MultiTrackSequencer seq(output, randomZero);
    seq.setStep(0, 99, 80); seq.resume(); seq.step();
    assert(seq.selectTrack(3) && seq.selectStep(9));
    events.clear();
    assert(restored.apply(seq));
    assert(events.size() == 1 && events[0].number == 99 && !events[0].on);
    assert(!seq.playing() && seq.selectedTrack() == 0);
    for (int t = 0; t < 4; ++t) {
        seq.selectTrack(t);
        assert(seq.editStep() == 0 && seq.nextStep() == 0);
        assert(seq.length() == restored.tracks[t].length);
        assert(seq.muted() == restored.tracks[t].muted);
    }
    assert(seq.exportTrack(-1, restored.tracks[0]) == false);
    assert(seq.exportTrack(4, restored.tracks[0]) == false);
    ProjectSnapshot captured = source;
    captured.capture(seq, source.bpm);
    assert(ProjectCodec::sameContent(captured, source));
    events.clear(); seq.pulse(); assert(events.empty());
    seq.resume(); seq.step();
    assert(events.size() >= 3);
    assert(events[0].number == 202 && events[0].value == 21 && events[0].channel == 1);
    assert(events[1].number == 60 && events[1].value == 91 && events[1].on);
    assert(events[2].number == 67 && events[2].on);
    for (const auto &e : events) assert(e.channel != 2);
    bool automationSeen = false;
    for (const auto &e : events)
        if (e.channel == 4 && e.number == 247 && e.value == 127) automationSeen = true;
    assert(automationSeen);
    events.clear(); seq.pulse(); seq.pulse(); seq.pulse();
    int chordEvents = 0;
    for (const auto &e : events) if (e.channel == 1) ++chordEvents;
    assert(chordEvents == 4); // Restored ratchet releases/retriggers the chord.
    seq.stop(); events.clear();
    captured.patternID = 9; captured.capture(seq, 20);
    assert(captured.patternID == 9 && !memcmp(captured.projectID, source.projectID, 16));
}

static void canonicalAndWire() {
    ProjectSnapshot p; p.projectID[0] = 1;
    std::vector<uint8_t> wire(ProjectCodec::Size), other(ProjectCodec::Size);
    assert(ProjectCodec::crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xcbf43926u);
    assert(ProjectCodec::encode(p, UINT64_C(0x0102030405060708), wire.data(), wire.size()));
    const uint8_t header[] = {'P','F','N','P',1,0,32,0,46,17,0,0,8,7,6,5,4,3,2,1,0,0,0,0,
                              0xd0,0xe1,0xdb,0x0f,0,0,0,0};
    // Independent fixture: Python struct.pack LE plus zlib.crc32 => 0x0fdbe1d0.
    assert(!memcmp(wire.data(), header, sizeof(header)));
    assert(wire[32] == 1 && wire[48] == 1 && wire[52] == 120);
    assert(wire[54] == 1 && wire[58] == 16 && wire[60 + 6] == 100 && wire[60 + 7] == 1);
    ProjectSnapshot dirty = p;
    dirty.tracks[0].steps[63].notes[3] = 255;
    dirty.tracks[0].steps[63].locks[3].control = 255;
    assert(dirty.valid() && ProjectCodec::sameContent(p, dirty));
    assert(ProjectCodec::encode(dirty, UINT64_C(0x0102030405060708), other.data(), other.size()));
    assert(wire == other);
    wire[60] = 255; wire[68] = 255; checksum(wire);
    uint64_t g = 0;
    assert(ProjectCodec::decode(wire.data(), wire.size(), dirty, g) == ProjectCodec::Valid);
    assert(dirty.tracks[0].steps[0].notes[0] == 0 && dirty.tracks[0].steps[0].locks[0].control == 0);
    dirty.tracks[3].steps[63].chance = 99;
    assert(!ProjectCodec::sameContent(p, dirty));
    dirty = p; dirty.bpm = 300; assert(dirty.valid());
    dirty.bpm = 20; assert(dirty.valid());
}

static void invalidIsAtomic() {
    const ProjectSnapshot good = asymmetric();
    std::vector<uint8_t> wire(ProjectCodec::Size);
    assert(ProjectCodec::encode(good, 7, wire.data(), wire.size()));
    auto reject = [&](const std::vector<uint8_t> &bad, ProjectCodec::Result result) {
        ProjectSnapshot out = good;
        uint64_t generation = 987;
        assert(ProjectCodec::decode(bad.data(), bad.size(), out, generation) == result);
        assert(ProjectCodec::sameContent(good, out) && generation == 987);
    };
    for (size_t n = 0; n < wire.size(); ++n)
        reject(std::vector<uint8_t>(wire.begin(), wire.begin() + n), ProjectCodec::Invalid);
    auto bad = wire; bad.push_back(0); reject(bad, ProjectCodec::Invalid);
    bad[4] = 2; reject(bad, ProjectCodec::Unsupported);
    bad.resize(ProjectCodec::Size + 100, 0); reject(bad, ProjectCodec::Unsupported);
    bad = wire; bad[4] = 2; bad.resize(6); reject(bad, ProjectCodec::Unsupported);
    bad = wire; bad[0] = 'X'; reject(bad, ProjectCodec::Invalid);
    bad = wire; bad.back() ^= 1; reject(bad, ProjectCodec::Invalid);
    const size_t offsets[] = {6,8,20,28,48,54,58,59,60+4,60+5,60+6,60+7,60+8,60+9,60+16,
                              60+63*17+6, 54+3*1094+6+63*17+7};
    const uint8_t values[] = {31,0,1,1,2,2,0,2,5,128,101,4,120,128,5,101,0};
    for (size_t i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i) {
        bad = wire; bad[offsets[i]] = values[i]; checksum(bad); reject(bad, ProjectCodec::Invalid);
    }
    bad = wire; for (int i = 12; i < 20; ++i) bad[i] = 0;
    checksum(bad); reject(bad, ProjectCodec::Invalid);
    bad = wire; for (int i = 32; i < 48; ++i) bad[i] = 0;
    checksum(bad); reject(bad, ProjectCodec::Invalid);
    bad = wire; bad[61] = bad[60]; checksum(bad); reject(bad, ProjectCodec::Invalid);
    NoteOutput output(note, control); MultiTrackSequencer seq(output);
    assert(good.apply(seq)); seq.resume(); seq.step(); seq.selectTrack(2); seq.selectStep(8);
    ProjectSnapshot before = good; before.capture(seq, good.bpm);
    events.clear();
    for (int kind = 0; kind < 10; ++kind) {
        ProjectSnapshot invalid = good;
        auto &s = invalid.tracks[3].steps[63];
        switch (kind) {
        case 0: invalid.projectID[0] = 0; memset(invalid.projectID, 0, 16); break;
        case 1: invalid.patternID = 2; break;
        case 2: invalid.bpm = 301; break;
        case 3: invalid.tracks[3].id = 3; break;
        case 4: invalid.tracks[3].length = 65; break;
        case 5: s.count = 5; break;
        case 6: s.count = 2; s.notes[0] = s.notes[1] = 60; break;
        case 7: s.ratchet = 0; break;
        case 8: s.lockCount = 2; s.locks[0].control = s.locks[1].control = 74; break;
        case 9: s.count = 1; s.notes[0] = 128; break;
        }
        assert(!invalid.apply(seq));
        ProjectSnapshot after = good; after.capture(seq, good.bpm);
        assert(ProjectCodec::sameContent(before, after));
        assert(seq.playing() && seq.selectedTrack() == 2 && seq.editStep() == 8 && seq.nextStep() == 1);
        assert(events.empty());
        auto untouched = wire;
        assert(!ProjectCodec::encode(invalid, 7, untouched.data(), untouched.size()) && untouched == wire);
    }
    assert(!ProjectCodec::encode(good, 0, wire.data(), wire.size()));
    seq.stop(); events.clear();
}

int main() {
    roundtripAndPlayback(); canonicalAndWire(); invalidIsAtomic();
    puts("Project snapshot regression tests passed");
}
