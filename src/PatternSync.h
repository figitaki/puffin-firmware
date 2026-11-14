#pragma once

#include "ProjectSnapshot.h"
#include <limits.h>

// Experimental PF SysEx v1, profile 1: first-note/velocity projection.
// Main-loop only. Call observe after every local musical edit and before receive.
class PatternSync {
public:
    static const size_t MaxRequest = 47, MaxReply = 147, CacheSize = 32;
    enum Error { Malformed = 1, Unsupported, Busy, Conflict, Range, Expired, BootChanged };
    struct Status {
        bool running = false;
        uint8_t clockSource = 0;
        uint8_t positions[4] = {127,127,127,127};
    };
    struct Reply {
        uint8_t bytes[MaxReply] = {};
        size_t size = 0;
        int appliedTrack = -1;
    };

    void begin(const uint8_t (&nonce)[8], const ProjectSnapshot &state) {
        memcpy(boot, nonce, 8);
        for (auto &byte : boot) byte &= 127;
        baseline = state;
        revision = 0;
        exhausted = false;
        staged = false;
        nextToken = 0;
        for (auto &entry : cache) entry.used = false;
    }

    void observe(const ProjectSnapshot &state) {
        if (!ProjectCodec::sameContent(baseline, state)) {
            advance();
            baseline = state;
        }
    }
    uint32_t currentRevision() const { return revision; }
    static bool addressed(const uint8_t *data, size_t size) {
        return data && size >= 4 && data[0] == 0xf0 && data[1] == 0x7d &&
               data[2] == 0x50 && data[3] == 0x46;
    }

    // Full F0..F7 frames. Replies go back only to the originating MIDI input.
    void receive(const uint8_t *data, size_t size, uint8_t source, uint32_t now,
                 ProjectSnapshot &state, const Status &status, Reply &reply) {
        reply.size = 0;
        reply.appliedTrack = -1;
        if (!addressed(data, size) || size < 11) return;
        // Fixed address zero; broadcast is discovery-only.
        if (data[5] != 0 && !(data[5] == 127 && data[6] == 1)) return;
        // Defined replies are never requests, even when echoed by a MIDI route.
        if ((data[6] >= 0x41 && data[6] <= 0x47) || data[6] == 0x7f) return;
        const uint8_t command = data[6] & 127;
        header(reply, command + 0x40, data);
        uint8_t sum = 0;
        bool valid = data[size - 1] == 0xf7;
        for (size_t i = 1; i + 1 < size; ++i) {
            valid = valid && data[i] < 128;
            sum = uint8_t((sum + data[i]) & 127);
        }
        if (!valid || sum || size > MaxRequest) { error(reply, command, Malformed); return; }
        if (data[4] != 1) { error(reply, command, Unsupported); return; }
        observe(state);
        if (staged && uint32_t(now - touched) >= 10000) staged = false;
        Entry *slot = nullptr;
        for (auto &entry : cache) {
            if (entry.used && uint32_t(now - entry.at) >= 30000) entry.used = false;
            if (!entry.used) { if (!slot) slot = &entry; continue; }
            if (entry.source == source && entry.request[7] == data[7] && entry.request[8] == data[8]) {
                if (entry.requestSize != size || memcmp(entry.request, data, size)) {
                    error(reply, command, Malformed);
                } else {
                    memcpy(reply.bytes, entry.response, entry.responseSize);
                    reply.size = entry.responseSize;
                    // Only a successful retry belonging to this live stage is activity.
                    if (staged && owner == source && (command == 3 || command == 4) &&
                        reply.bytes[6] == command + 0x40 && read(reply.bytes + 9, 2) == token)
                        touched = now;
                }
                return;
            }
        }
        // Never discard an unexpired COMMIT reply to make room for another request.
        if (!slot) { error(reply, command, Busy); return; }
        const uint8_t *p = data + 9;
        const size_t length = size - 11;
        switch (command) {
        case 1: // HELLO
            if (length) { error(reply, command, Malformed); break; }
            append(reply, 1); append(reply, 4); append(reply, 4); append(reply, 16); append(reply, 11);
            for (uint8_t byte : boot) append(reply, byte);
            number(reply, revision, 5);
            finish(reply);
            break;
        case 2: // READ_TRACK, including hidden slots
            if (length != 1) { error(reply, command, Malformed); break; }
            if (p[0] >= 4) { error(reply, command, Range); break; }
            append(reply, p[0]); number(reply, revision, 5);
            append(reply, state.tracks[p[0]].length); append(reply, state.tracks[p[0]].muted);
            for (const auto &step : state.tracks[p[0]].steps) {
                append(reply, step.count ? step.notes[0] : 0); append(reply, step.count ? step.velocity : 0);
            }
            finish(reply);
            break;
        case 3: // BEGIN_WRITE
            if (length != 16) { error(reply, command, Malformed); break; }
            if (memcmp(p, boot, 8)) { error(reply, command, BootChanged); break; }
            if (p[12] > 15) { error(reply, command, Range); break; }
            if (read(p + 8, 5) != revision) { error(reply, command, Conflict); break; }
            if (p[13] >= 4 || p[14] < 1 || p[14] > 64 || p[15] > 1) {
                error(reply, command, Range); break;
            }
            if (staged || exhausted || nextToken == 16383) { error(reply, command, Busy); break; }
            track = p[13]; staging = state.tracks[track];
            staging.length = p[14]; staging.muted = p[15] != 0;
            expected = revision; owner = source; token = ++nextToken; touched = now; staged = true;
            number(reply, token, 2); finish(reply);
            break;
        case 4: { // WRITE_CHUNK: validate entire request before mutating staging
            if (length < 4 || length != size_t(4 + 2 * p[3])) { error(reply, command, Malformed); break; }
            if (!stage(reply, command, source, p)) break;
            if (p[3] < 1 || p[3] > 16 || p[2] + p[3] > 64) { error(reply, command, Range); break; }
            bool okay = true;
            for (int i = 0; i < p[3]; ++i) {
                Sequencer::Step value = staging.steps[p[2] + i];
                projectPair(value, p[4 + 2*i], p[5 + 2*i]);
                okay = okay && Sequencer::validStep(value);
            }
            if (!okay) { error(reply, command, Range); break; }
            for (int i = 0; i < p[3]; ++i)
                projectPair(staging.steps[p[2] + i], p[4 + 2*i], p[5 + 2*i]);
            touched = now;
            number(reply, token, 2); append(reply, p[2]); append(reply, p[3]); finish(reply);
            break;
        }
        case 5: // COMMIT_WRITE
            if (length != 2) { error(reply, command, Malformed); break; }
            if (!stage(reply, command, source, p)) break;
            if (expected != revision) { staged = false; error(reply, command, Conflict); break; }
            if (status.running || exhausted) { error(reply, command, Busy); break; }
            state.tracks[track] = staging;
            staged = false;
            // A successful transaction advances even if its musical content is unchanged.
            advance(); baseline = state;
            reply.appliedTrack = track;
            number(reply, revision, 5); finish(reply);
            break;
        case 6:
            if (length != 2) { error(reply, command, Malformed); break; }
            if (!stage(reply, command, source, p)) break;
            staged = false; number(reply, token, 2); finish(reply);
            break;
        case 7:
            if (length) { error(reply, command, Malformed); break; }
            number(reply, revision, 5); append(reply, status.clockSource); append(reply, status.running);
            for (uint8_t position : status.positions) append(reply, status.running ? position : 127);
            number(reply, state.bpm, 2); finish(reply);
            break;
        default: error(reply, command, Unsupported); break;
        }
        slot->used = true; slot->at = now; slot->source = source;
        slot->requestSize = size; memcpy(slot->request, data, size);
        slot->responseSize = reply.size; memcpy(slot->response, reply.bytes, reply.size);
    }

private:
    struct Entry {
        bool used = false;
        uint32_t at = 0;
        uint8_t source = 0, request[MaxRequest] = {}, response[MaxReply] = {};
        size_t requestSize = 0, responseSize = 0;
    } cache[CacheSize];
    ProjectSnapshot baseline;
    ProjectSnapshot::Track staging;
    uint8_t boot[8] = {}, track = 0, owner = 0;
    uint32_t revision = 0, expected = 0, touched = 0;
    uint16_t token = 0, nextToken = 0;
    bool staged = false, exhausted = false;

    void advance() {
        if (revision == UINT32_MAX) exhausted = true;
        else { ++revision; if (revision == UINT32_MAX) exhausted = true; }
    }
    static void projectPair(Sequencer::Step &step, uint8_t note, uint8_t velocity) {
        if ((step.count ? step.notes[0] : 0) == note && (step.count ? step.velocity : 0) == velocity) return;
        step.notes[0] = note; step.velocity = velocity;
        if (!step.count) step.count = 1;
    }
    bool stage(Reply &reply, uint8_t command, uint8_t source, const uint8_t *p) {
        if (!staged || read(p, 2) != token) { error(reply, command, Expired); return false; }
        if (owner != source) { error(reply, command, Busy); return false; }
        return true;
    }
    static uint32_t read(const uint8_t *p, int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i) value |= uint32_t(p[i]) << (7 * i);
        return value;
    }
    static void append(Reply &reply, uint8_t value) { reply.bytes[reply.size++] = value; }
    static void number(Reply &reply, uint32_t value, int count) {
        for (int i = 0; i < count; ++i) { append(reply, value & 127); value >>= 7; }
    }
    static void header(Reply &reply, uint8_t command, const uint8_t *request) {
        const uint8_t prefix[] = {0xf0,0x7d,0x50,0x46,1,0,command,
                                  uint8_t(request[7] & 127),uint8_t(request[8] & 127)};
        memcpy(reply.bytes, prefix, sizeof(prefix)); reply.size = sizeof(prefix);
    }
    static void finish(Reply &reply) {
        unsigned sum = 0;
        for (size_t i = 1; i < reply.size; ++i) sum += reply.bytes[i];
        append(reply, uint8_t(-sum) & 127); append(reply, 0xf7);
    }
    void error(Reply &reply, uint8_t command, Error code) const {
        reply.size = 9; reply.bytes[6] = 0x7f;
        append(reply, command); append(reply, code); number(reply, revision, 5); finish(reply);
    }
};
