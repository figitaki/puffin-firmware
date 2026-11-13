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
    p.swing = 2;
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
            v.gate = uint8_t(1 + (s + t) % 6);
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
    chord.velocity = 91; chord.chance = 100; chord.ratchet = 2; chord.gate = 6;
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
    captured.capture(seq, source.bpm, source.swing);
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
    assert(captured.swing == 0);
}

static void canonicalAndWire() {
    ProjectSnapshot p; p.projectID[0] = 1;
    std::vector<uint8_t> wire(ProjectCodec::Size), other(ProjectCodec::Size);
    assert(ProjectCodec::crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xcbf43926u);
    assert(ProjectCodec::encode(p, UINT64_C(0x0102030405060708), wire.data(), wire.size()));
    const uint8_t header[] = {'P','F','N','P',3,0,32,0,47,18,0,0,8,7,6,5,4,3,2,1,0,0,0,0,
                              0xe0,0x71,0x4f,0xb0,0,0,0,0};
    // Independent fixture: Python struct.pack LE plus zlib.crc32 => 0xb04f71e0.
    assert(!memcmp(wire.data(), header, sizeof(header)));
    assert(wire[32] == 1 && wire[48] == 1 && wire[52] == 120);
    assert(wire[54] == 1 && wire[58] == 16 && wire[60 + 6] == 100 && wire[60 + 7] == 1);
    assert(wire.size() == 4687 && ProjectCodec::PayloadSize == 4655 && wire.back() == 0);
    for (int t = 0; t < 4; ++t)
        for (int s = 0; s < 64; ++s) assert(wire[60 + t * 1158 + s * 18 + 17] == 6);
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
    dirty = p; dirty.tracks[0].steps[63].gate = 5;
    assert(dirty.valid() && !ProjectCodec::sameContent(p, dirty));
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
    for (unsigned swing = 0; swing <= 255; ++swing) {
        auto candidate = good; candidate.swing = uint8_t(swing);
        assert(candidate.valid() == (swing <= 2));
        assert(ProjectCodec::sameContent(candidate, good) == (swing == good.swing));
        auto encoded = wire;
        assert(ProjectCodec::encode(candidate, 7, encoded.data(), encoded.size()) == (swing <= 2));
        if (swing > 2) {
            assert(encoded == wire);
            bad = wire; bad.back() = uint8_t(swing); checksum(bad);
            reject(bad, ProjectCodec::Invalid);
        } else {
            assert(encoded.back() == swing);
            ProjectSnapshot out; uint64_t generation = 0;
            assert(ProjectCodec::decode(encoded.data(), encoded.size(), out, generation) == ProjectCodec::Valid);
            assert(ProjectCodec::sameContent(out, candidate) && generation == 7);
        }
    }
    bad = wire; bad.push_back(0);
    bad[4] = 4; reject(bad, ProjectCodec::Unsupported);
    bad.resize(ProjectCodec::Size + 100, 0); reject(bad, ProjectCodec::Unsupported);
    bad = wire; bad[4] = 4; bad.resize(6); reject(bad, ProjectCodec::Unsupported);
    bad = wire; bad[0] = 'X'; reject(bad, ProjectCodec::Invalid);
    bad = wire; bad.back() ^= 1; reject(bad, ProjectCodec::Invalid);
    const size_t offsets[] = {6,8,20,28,48,54,58,59,60+4,60+5,60+6,60+7,60+8,60+9,60+16,
                              60+63*18+6, 54+3*1158+6+63*18+7, 60+17, 60+63*18+17};
    const uint8_t values[] = {31,0,1,1,2,2,0,2,5,128,101,4,120,128,5,101,0,0,7};
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
    for (int kind = 0; kind < 13; ++kind) {
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
        case 10: s.gate = 0; break;
        case 11: s.gate = 7; break;
        case 12: invalid.swing = 3; break;
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

static void everyGate() {
    auto good = asymmetric();
    std::vector<uint8_t> wire(ProjectCodec::Size);
    assert(ProjectCodec::encode(good, 7, wire.data(), wire.size()));
    for (int t = 0; t < 4; ++t) {
        for (int s = 0; s < 64; ++s) {
            const size_t offset = 60 + t * 1158 + s * 18 + 17;
            assert(wire[offset] == good.tracks[t].steps[s].gate);
            for (uint8_t invalid : {uint8_t(0), uint8_t(7), uint8_t(255)}) {
                auto malformed = good;
                malformed.tracks[t].steps[s].gate = invalid;
                assert(!malformed.valid() && !ProjectCodec::sameContent(good, malformed));
                auto untouched = wire;
                assert(!ProjectCodec::encode(malformed, 7, untouched.data(), untouched.size()));
                assert(untouched == wire);
                auto bad = wire; bad[offset] = invalid; checksum(bad);
                ProjectSnapshot out = good;
                uint64_t generation = 123;
                assert(ProjectCodec::decode(bad.data(), bad.size(), out, generation) == ProjectCodec::Invalid);
                assert(ProjectCodec::sameContent(good, out) && generation == 123);
            }
        }
    }
    for (uint8_t gate = 1; gate <= 6; ++gate) {
        for (auto &t : good.tracks) for (auto &s : t.steps) s.gate = gate;
        assert(good.valid() && ProjectCodec::encode(good, 9, wire.data(), wire.size()));
        ProjectSnapshot out; uint64_t generation = 0;
        assert(ProjectCodec::decode(wire.data(), wire.size(), out, generation) == ProjectCodec::Valid);
        assert(ProjectCodec::sameContent(good, out) && generation == 9);
    }
}

// Independent legacy writer: never calls encode or removes bytes from v3.
static std::vector<uint8_t> legacyFixture(const ProjectSnapshot &p, unsigned version = 1) {
    std::vector<uint8_t> wire;
    auto append = [&](uint64_t n, int bytes) {
        for (int i = 0; i < bytes; ++i) wire.push_back(uint8_t(n >> (8 * i)));
    };
    append(0x504e4650, 4); append(version, 2); append(32, 2);
    append(version == 1 ? 4398 : 4654, 4);
    append(UINT64_C(0x0102030405060708), 8); append(0, 4); append(0, 4); append(0, 4);
    for (auto b : p.projectID) append(b, 1);
    append(p.patternID, 4); append(p.bpm, 2);
    for (const auto &t : p.tracks) {
        append(t.id, 4); append(t.length, 1); append(t.muted, 1);
        for (const auto &s : t.steps) {
            for (int i = 0; i < 4; ++i) append(i < s.count ? s.notes[i] : 0, 1);
            append(s.count, 1); append(s.velocity, 1); append(s.chance, 1); append(s.ratchet, 1);
            for (int i = 0; i < 4; ++i) {
                append(i < s.lockCount ? s.locks[i].control : 0, 1);
                append(i < s.lockCount ? s.locks[i].value : 0, 1);
            }
            append(s.lockCount, 1);
            if (version == 2) append(s.gate, 1);
        }
    }
    assert(wire.size() == (version == 1 ? 4430U : 4686U));
    checksum(wire);
    return wire;
}

static void legacyMigration(unsigned version) {
    ProjectSnapshot blank; blank.projectID[0] = 1;
    auto wire = legacyFixture(blank, version);
    // Independent Python struct.pack/zlib fixtures retained for both old schemas.
    const uint32_t crc = version == 1 ? 0x0fdbe1d0u : 0xd9b5a861u;
    for (int i = 0; i < 4; ++i) assert(wire[24 + i] == uint8_t(crc >> (8 * i)));
    auto expected = asymmetric();
    wire = legacyFixture(expected, version);
    expected.swing = 0;
    if (version == 1) for (auto &t : expected.tracks) for (auto &s : t.steps) s.gate = 6;
    ProjectSnapshot out = asymmetric();
    uint64_t generation = 99;
    assert(ProjectCodec::decode(wire.data(), wire.size(), out, generation) == ProjectCodec::Valid);
    assert(generation == UINT64_C(0x0102030405060708));
    assert(ProjectCodec::sameContent(expected, out));
    assert(out.swing == 0);
    std::vector<uint8_t> upgraded(ProjectCodec::Size);
    assert(ProjectCodec::encode(out, generation, upgraded.data(), upgraded.size()));
    const size_t trackSize = version == 1 ? 1094 : 1158;
    const size_t stepSize = version == 1 ? 17 : 18;
    for (int t = 0; t < 4; ++t) {
        assert(!memcmp(wire.data() + 54 + t * trackSize, upgraded.data() + 54 + t * 1158, 6));
        for (int s = 0; s < 64; ++s) {
            assert(!memcmp(wire.data() + 60 + t * trackSize + s * stepSize,
                           upgraded.data() + 60 + t * 1158 + s * 18, stepSize));
            assert(upgraded[60 + t * 1158 + s * 18 + 17] == expected.tracks[t].steps[s].gate);
        }
    }
    auto reject = [&](const std::vector<uint8_t> &bad) {
        assert(ProjectCodec::decode(bad.data(), bad.size(), out, generation) == ProjectCodec::Invalid);
        assert(ProjectCodec::sameContent(expected, out));
        assert(generation == UINT64_C(0x0102030405060708));
    };
    for (size_t n = 0; n < wire.size(); ++n)
        reject(std::vector<uint8_t>(wire.begin(), wire.begin() + n));
    auto bad = wire; bad.push_back(0); reject(bad);
    bad = wire; bad[60 + 3 * trackSize + 63 * stepSize + 16] = 5; checksum(bad); reject(bad);
    bad = wire; bad[4] = version == 1 ? 2 : 3; checksum(bad); reject(bad);
    bad = upgraded; bad[4] = 1; checksum(bad); reject(bad);
}

int main() {
    roundtripAndPlayback(); canonicalAndWire(); invalidIsAtomic(); everyGate();
    legacyMigration(1); legacyMigration(2);
    puts("Project snapshot regression tests passed");
}
