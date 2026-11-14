#pragma once

#include <stddef.h>
#include <string.h>
#include "MultiTrackSequencer.h"

// One project, one pattern, four stable track identities. Runtime transport,
// random state, and editor cursors are deliberately not persisted.
struct ProjectSnapshot {
    struct Track : Sequencer::State {
        uint32_t id = 1;
    };

    uint8_t projectID[16] = {};
    uint32_t patternID = 1;
    uint16_t bpm = 120;
    uint8_t swing = 0;
    Track tracks[4];

    ProjectSnapshot() {
        for (int i = 0; i < 4; ++i) tracks[i].id = uint32_t(i + 1);
    }

    bool valid() const {
        bool nonzero = false;
        for (uint8_t byte : projectID) nonzero = nonzero || byte != 0;
        if (!nonzero || patternID != 1 || bpm < 20 || bpm > 300 || swing > 2) return false;
        for (int i = 0; i < 4; ++i)
            if (tracks[i].id != uint32_t(i + 1) || !Sequencer::validState(tracks[i])) return false;
        return true;
    }

    void capture(const MultiTrackSequencer &seq, uint16_t tempo, uint8_t swing = 0) {
        bpm = tempo;
        this->swing = swing;
        for (int i = 0; i < 4; ++i) {
            seq.exportTrack(i, tracks[i]);
            tracks[i].id = uint32_t(i + 1);
        }
    }

    bool apply(MultiTrackSequencer &seq) const {
        if (!valid()) return false;
        return seq.replaceTracks(tracks);
    }
};

class ProjectCodec {
public:
    static const size_t HeaderSize = 32, PayloadSize = 4687, Size = 4719;
    static const size_t V3PayloadSize = 4655, V3Size = 4687;
    static const size_t V2PayloadSize = 4654, V2Size = 4686;
    static const size_t V1PayloadSize = 4398, V1Size = 4430;
    enum Result { Valid, Invalid, Unsupported };

    // CRC32/ISO-HDLC. zeroChecksum selects the record's zeroed CRC field;
    // false also makes this useful with standard independent CRC fixtures.
    static uint32_t crc32(const uint8_t *data, size_t size, bool zeroChecksum = false) {
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < size; ++i) {
            crc ^= zeroChecksum && i >= 24 && i < 28 ? 0 : data[i];
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
        }
        return crc ^ 0xffffffffu;
    }

    static bool encode(const ProjectSnapshot &snapshot, uint64_t generation,
                       uint8_t *data, size_t size) {
        if (!data || size != Size || !generation || !snapshot.valid()) return false;
        memset(data, 0, Size);
        memcpy(data, "PFNP", 4);
        put(data + 4, 4, 2);
        put(data + 6, HeaderSize, 2);
        put(data + 8, PayloadSize, 4);
        put(data + 12, generation, 8);
        uint8_t *p = data + HeaderSize;
        memcpy(p, snapshot.projectID, 16); p += 16;
        put(p, snapshot.patternID, 4); p += 4;
        put(p, snapshot.bpm, 2); p += 2;
        for (const auto &track : snapshot.tracks) {
            put(p, track.id, 4); p += 4;
            *p++ = track.length;
            *p++ = track.muted ? 1 : 0;
            for (const auto &step : track.steps) {
                for (int i = 0; i < 4; ++i) *p++ = i < step.count ? step.notes[i] : 0;
                *p++ = step.count;
                *p++ = step.velocity;
                *p++ = step.chance;
                *p++ = step.ratchet;
                for (int i = 0; i < 4; ++i) {
                    *p++ = i < step.lockCount ? step.locks[i].control : 0;
                    *p++ = i < step.lockCount ? step.locks[i].value : 0;
                }
                *p++ = step.lockCount;
                *p++ = step.gate;
            }
        }
        *p++ = snapshot.swing;
        // Track-major, LSB-first bitmap; all existing v3 payload offsets stay fixed.
        for (int t = 0; t < 4; ++t)
            for (int s = 0; s < 64; ++s)
                if (snapshot.tracks[t].steps[s].tie)
                    p[t * 8 + s / 8] |= uint8_t(1u << (s % 8));
        put(data + 24, crc32(data, Size, true), 4);
        return true;
    }

    static Result decode(const uint8_t *data, size_t size,
                         ProjectSnapshot &out, uint64_t &generation) {
        if (!data || size < 4 || memcmp(data, "PFNP", 4)) return Invalid;
        // Protect unknown versions even if the rest of their record is malformed.
        if (size < 6) return Invalid;
        const uint64_t version = get(data + 4, 2);
        if (version != 1 && version != 2 && version != 3 && version != 4) return Unsupported;
        const size_t recordSize = version == 1 ? V1Size : version == 2 ? V2Size :
                                  version == 3 ? V3Size : Size;
        const size_t stepSize = version == 1 ? 17 : 18;
        if (size != recordSize || get(data + 6, 2) != HeaderSize ||
            get(data + 8, 4) != recordSize - HeaderSize || !get(data + 12, 8) ||
            get(data + 20, 4) || get(data + 28, 4) ||
            get(data + 24, 4) != crc32(data, size, true)) return Invalid;
        // Validate first using only one small Step, then publish in a second
        // pass. The caller owns the stable input buffer for both passes.
        const uint8_t *p = data + HeaderSize;
        bool nonzero = false;
        for (int i = 0; i < 16; ++i) nonzero = nonzero || p[i] != 0;
        if (!nonzero || get(p + 16, 4) != 1 || get(p + 20, 2) < 20 ||
            get(p + 20, 2) > 300) return Invalid;
        p += 22;
        for (int t = 0; t < 4; ++t) {
            if (get(p, 4) != uint32_t(t + 1) || p[4] < 1 || p[4] > 64 || p[5] > 1)
                return Invalid;
            p += 6;
            for (int s = 0; s < 64; ++s, p += stepSize) {
                if (p[4] > 4 || p[16] > 4) return Invalid;
                Sequencer::Step step;
                readStep(p, step, version >= 2);
                if (!Sequencer::validStep(step)) return Invalid;
            }
        }
        if (version >= 3 && *p > 2) return Invalid;
        // Every bitmap bit represents a bool, so it has no invalid encodings.
        p = data + HeaderSize;
        memcpy(out.projectID, p, 16); p += 16;
        out.patternID = uint32_t(get(p, 4)); p += 4;
        out.bpm = uint16_t(get(p, 2)); p += 2;
        for (auto &track : out.tracks) {
            track.id = uint32_t(get(p, 4)); p += 4;
            track.length = *p++;
            track.muted = *p++ != 0;
            for (auto &step : track.steps) { readStep(p, step, version >= 2); p += stepSize; }
        }
        out.swing = version >= 3 ? *p++ : 0;
        if (version == 4)
            for (int t = 0; t < 4; ++t)
                for (int s = 0; s < 64; ++s)
                    out.tracks[t].steps[s].tie = (p[t * 8 + s / 8] & (1u << (s % 8))) != 0;
        generation = get(data + 12, 8);
        return Valid;
    }

    static bool sameContent(const ProjectSnapshot &a, const ProjectSnapshot &b) {
        if (memcmp(a.projectID, b.projectID, 16) || a.patternID != b.patternID ||
            a.bpm != b.bpm || a.swing != b.swing)
            return false;
        for (int t = 0; t < 4; ++t) {
            const auto &x = a.tracks[t];
            const auto &y = b.tracks[t];
            if (x.id != y.id || x.length != y.length || x.muted != y.muted) return false;
            for (int s = 0; s < 64; ++s) {
                const auto &u = x.steps[s];
                const auto &v = y.steps[s];
                if (u.count != v.count || u.velocity != v.velocity || u.chance != v.chance ||
                    u.ratchet != v.ratchet || u.lockCount != v.lockCount || u.gate != v.gate ||
                    u.tie != v.tie) return false;
                // Bound even malformed caller objects; equality is not validation.
                for (int i = 0; i < u.count && i < 4; ++i)
                    if (u.notes[i] != v.notes[i]) return false;
                for (int i = 0; i < u.lockCount && i < 4; ++i)
                    if (u.locks[i].control != v.locks[i].control ||
                        u.locks[i].value != v.locks[i].value) return false;
            }
        }
        return true;
    }

private:
    static void readStep(const uint8_t *p, Sequencer::Step &step, bool hasGate) {
        step = Sequencer::Step{};
        step.count = p[4]; step.velocity = p[5];
        step.chance = p[6]; step.ratchet = p[7]; step.lockCount = p[16];
        step.gate = hasGate ? p[17] : 6;
        for (int i = 0; i < step.count; ++i) step.notes[i] = p[i];
        for (int i = 0; i < step.lockCount; ++i) {
            step.locks[i].control = p[8 + i * 2];
            step.locks[i].value = p[9 + i * 2];
        }
    }

    static void put(uint8_t *p, uint64_t value, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) { p[i] = uint8_t(value); value >>= 8; }
    }
    static uint64_t get(const uint8_t *p, size_t bytes) {
        uint64_t value = 0;
        for (size_t i = 0; i < bytes; ++i) value |= uint64_t(p[i]) << (8 * i);
        return value;
    }
};
