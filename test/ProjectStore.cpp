#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "ProjectStore.h"

class Files : public ProjectFiles {
public:
    std::vector<uint8_t> slots[2];
    std::vector<uint8_t> substituteReadback;
    bool readFailure = false, syncFailure = false, damageReadback = false;
    size_t writeLimit = ProjectCodec::Size;
    int writes = 0;
    ReadResult read(unsigned slot, uint8_t *data, size_t capacity, size_t &size) override {
        if (readFailure) return Failed;
        const auto &source = writes && !substituteReadback.empty() ? substituteReadback : slots[slot];
        size = source.size() < capacity ? source.size() : capacity;
        if (source.empty()) return Missing;
        memcpy(data, source.data(), size);
        if (damageReadback && writes) data[size - 1] ^= 1;
        return Read;
    }
    bool write(unsigned slot, const uint8_t *data, size_t size) override {
        ++writes;
        const size_t count = size < writeLimit ? size : writeLimit;
        slots[slot].assign(data, data + count);
        return count == size && !syncFailure;
    }
};

ProjectSnapshot pattern() {
    ProjectSnapshot value;
    value.projectID[0] = 17;
    value.bpm = 137;
    value.tracks[0].length = 19;
    value.tracks[1].muted = true;
    value.tracks[2].steps[63].notes[0] = 0;
    value.tracks[2].steps[63].count = 1;
    value.tracks[2].steps[63].velocity = 73;
    return value;
}

void saveLoadAndRecovery() {
    Files files;
    ProjectStore store(files);
    auto value = pattern();
    ProjectSnapshot loaded;
    assert(store.load(loaded) == ProjectStore::Empty);
    assert(store.dirty(value));
    assert(store.save(value) == ProjectStore::Saved);
    assert(!store.dirty(value));
    assert(store.save(value) == ProjectStore::Saved && files.writes == 1);
    value.bpm = 141;
    assert(store.dirty(value));
    assert(store.save(value) == ProjectStore::Saved);
    assert(!store.dirty(value) && files.writes == 2);
    value.tracks[3].length = 1;
    assert(store.dirty(value));
    value.tracks[3].length = 16;
    assert(!store.dirty(value));
    ProjectStore reboot(files);
    assert(reboot.load(loaded) == ProjectStore::Loaded);
    assert(ProjectCodec::sameContent(value, loaded));
    files.slots[1][100] ^= 1;
    assert(reboot.load(loaded) == ProjectStore::Recovered);
    assert(loaded.bpm == 137 && loaded.tracks[2].steps[63].notes[0] == 0);
    assert(reboot.save(loaded) == ProjectStore::Saved);
    assert(reboot.load(loaded) == ProjectStore::Loaded);
}

void interruptionAtEveryByte() {
    const auto old = pattern();
    auto next = old;
    next.bpm = 211;
    std::vector<uint8_t> committed(ProjectCodec::Size);
    assert(ProjectCodec::encode(old, 7, committed.data(), committed.size()));
    for (size_t boundary = 0; boundary <= ProjectCodec::Size; ++boundary) {
        Files files;
        files.slots[0] = committed;
        files.writeLimit = boundary;
        files.syncFailure = true; // Including a full write without confirmed sync.
        ProjectStore writer(files);
        assert(writer.save(next) == ProjectStore::SaveFailed);
        assert(writer.dirty(next));
        assert(files.slots[0] == committed);
        ProjectStore reboot(files);
        ProjectSnapshot recovered;
        const auto result = reboot.load(recovered);
        assert(result == ProjectStore::Loaded || result == ProjectStore::Recovered);
        assert(ProjectCodec::sameContent(recovered, boundary == ProjectCodec::Size ? next : old));
    }
}

void rejectWithoutMutation() {
    Files files;
    ProjectStore store(files);
    auto value = pattern();
    assert(store.save(value) == ProjectStore::Saved);
    auto edited = value;
    edited.bpm = 222;
    files.slots[1] = files.slots[0];
    files.slots[1][4] = 3;
    files.slots[1].resize(ProjectCodec::Size + 100, 0); // Larger future records stay protected.
    const auto unknown = files.slots[1];
    assert(store.load(edited) == ProjectStore::Unsupported && edited.bpm == 222);
    assert(store.save(edited, true) == ProjectStore::Unsupported);
    assert(files.writes == 1 && files.slots[1] == unknown);
    files.slots[1].clear();
    files.readFailure = true;
    assert(store.load(edited) == ProjectStore::Unavailable && edited.bpm == 222);
    assert(store.save(edited, true) == ProjectStore::Unavailable && files.writes == 1);
    files.readFailure = false;
    files.slots[0][0] ^= 1;
    assert(store.load(edited) == ProjectStore::Corrupt && edited.bpm == 222);
    assert(store.save(edited) == ProjectStore::Corrupt && files.writes == 1);
    assert(store.save(edited, true) == ProjectStore::Saved);
    assert(!store.dirty(edited));
}

void ambiguousAndFailedVerification() {
    Files files;
    auto value = pattern();
    auto other = value;
    other.bpm = 99;
    files.slots[0].resize(ProjectCodec::Size);
    files.slots[1].resize(ProjectCodec::Size);
    assert(ProjectCodec::encode(value, 9, files.slots[0].data(), files.slots[0].size()));
    assert(ProjectCodec::encode(value, 9, files.slots[1].data(), files.slots[1].size()));
    ProjectStore store(files);
    assert(store.load(value) == ProjectStore::Loaded);
    assert(ProjectCodec::encode(other, 9, files.slots[1].data(), files.slots[1].size()));
    assert(store.load(value) == ProjectStore::Ambiguous && value.bpm == 137);
    assert(store.save(value, true) == ProjectStore::Ambiguous && !files.writes);
    files.slots[1].clear();
    files.damageReadback = true;
    assert(store.save(other) == ProjectStore::SaveFailed);
    assert(store.dirty(other));
    files.damageReadback = false;
    assert(store.load(value) == ProjectStore::Loaded && value.bpm == 99);
    assert(ProjectCodec::encode(value, UINT64_MAX, files.slots[0].data(), files.slots[0].size()));
    other.bpm = 100;
    assert(store.save(other) == ProjectStore::SaveFailed);
}

// Literal v1 pattern() fixture, independent of the production encoder and v2.
static std::vector<uint8_t> legacyPattern(uint64_t generation) {
    std::vector<uint8_t> wire(4430, 0);
    const uint8_t header[] = {'P','F','N','P',1,0,32,0,46,17};
    memcpy(wire.data(), header, sizeof(header));
    for (int i = 0; i < 8; ++i) wire[12 + i] = uint8_t(generation >> (8 * i));
    wire[32] = 17; wire[48] = 1; wire[52] = 137;
    for (int t = 0; t < 4; ++t) {
        const size_t track = 54 + t * 1094;
        wire[track] = uint8_t(t + 1); wire[track + 4] = t == 0 ? 19 : 16;
        wire[track + 5] = t == 1 ? 1 : 0;
        for (int s = 0; s < 64; ++s) {
            wire[track + 6 + s * 17 + 6] = 100;
            wire[track + 6 + s * 17 + 7] = 1;
        }
    }
    wire[60 + 2 * 1094 + 63 * 17 + 4] = 1;
    wire[60 + 2 * 1094 + 63 * 17 + 5] = 73;
    const uint32_t crc = ProjectCodec::crc32(wire.data(), wire.size(), true);
    for (int i = 0; i < 4; ++i) wire[24 + i] = uint8_t(crc >> (8 * i));
    return wire;
}

static void migrationAndMixedVersions() {
    const auto old = legacyPattern(7);
    for (unsigned legacySlot = 0; legacySlot < 2; ++legacySlot) {
        Files files; files.slots[legacySlot] = old;
        ProjectStore store(files);
        ProjectSnapshot value;
        assert(store.load(value) == ProjectStore::Loaded);
        assert(!files.writes && files.slots[legacySlot] == old && files.slots[1 - legacySlot].empty());
        assert(ProjectCodec::sameContent(value, pattern()) && !store.dirty(value));
        assert(store.save(value) == ProjectStore::Saved && files.writes == 1);
        assert(files.slots[legacySlot] == old && files.slots[1 - legacySlot][4] == 2);
        uint64_t generation = 0; ProjectSnapshot decoded;
        assert(ProjectCodec::decode(files.slots[1 - legacySlot].data(), 4686, decoded, generation) == ProjectCodec::Valid);
        assert(generation == 8 && ProjectCodec::sameContent(value, decoded));
        assert(store.save(value) == ProjectStore::Saved && files.writes == 1);
        value.tracks[0].steps[63].gate = 1; // Hidden gate-only edit is musical.
        assert(store.dirty(value));
        assert(store.save(value) == ProjectStore::Saved && files.writes == 2);
        assert(store.load(decoded) == ProjectStore::Loaded && ProjectCodec::sameContent(value, decoded));
    }
    // Generation, not schema, chooses the winner; equivalent ties prefer v2.
    for (uint64_t v2generation = 6; v2generation <= 8; ++v2generation) {
        for (unsigned legacySlot = 0; legacySlot < 2; ++legacySlot) {
            Files files; files.slots[legacySlot] = old;
            files.slots[1 - legacySlot].resize(4686);
            auto newer = pattern();
            if (v2generation != 7) newer.tracks[2].steps[63].gate = 2;
            assert(ProjectCodec::encode(newer, v2generation, files.slots[1 - legacySlot].data(), 4686));
            ProjectStore store(files); ProjectSnapshot value;
            assert(store.load(value) == ProjectStore::Loaded && !files.writes);
            assert(ProjectCodec::sameContent(value, v2generation < 7 ? pattern() : newer));
            if (v2generation == 7) assert(store.save(value) == ProjectStore::Saved && !files.writes);
            newer.tracks[2].steps[63].gate = 3;
            assert(ProjectCodec::encode(newer, 7, files.slots[1 - legacySlot].data(), 4686));
            const auto before = value;
            assert(store.load(value) == ProjectStore::Ambiguous && ProjectCodec::sameContent(value, before));
            assert(store.save(value, true) == ProjectStore::Ambiguous && !files.writes);
        }
    }
}

static void migrationFailures() {
    const auto old = legacyPattern(7);
    // Every partial migration leaves the original v1 slot byte-for-byte intact.
    // Repeat for unchanged migration and for a hidden gate edit.
    for (size_t attempt = 0; attempt < 2 * (ProjectCodec::Size + 1); ++attempt) {
        const size_t boundary = attempt % (ProjectCodec::Size + 1);
        const bool changeGate = attempt > ProjectCodec::Size;
        Files files; files.slots[0] = old;
        ProjectStore writer(files); ProjectSnapshot value;
        assert(writer.load(value) == ProjectStore::Loaded);
        auto edited = value;
        if (changeGate) edited.tracks[2].steps[63].gate = 1;
        files.writeLimit = boundary; files.syncFailure = true;
        assert(writer.save(edited) == ProjectStore::SaveFailed);
        assert(writer.dirty(edited) == changeGate && !writer.dirty(value));
        assert(files.slots[0] == old);
        ProjectStore reboot(files); ProjectSnapshot recovered;
        const auto result = reboot.load(recovered);
        assert(result == ProjectStore::Loaded || result == ProjectStore::Recovered);
        assert(ProjectCodec::sameContent(recovered, boundary == 4686 ? edited : value));
    }
    // Failed readback must not advance the musical baseline or touch v1.
    Files files; files.slots[0] = old;
    ProjectStore store(files); ProjectSnapshot value;
    assert(store.load(value) == ProjectStore::Loaded);
    value.tracks[2].steps[63].gate = 3;
    files.damageReadback = true;
    assert(store.save(value) == ProjectStore::SaveFailed && store.dirty(value));
    assert(files.slots[0] == old);
    files.damageReadback = false;
    files.slots[1].back() ^= 1;
    ProjectSnapshot recovered;
    assert(store.load(recovered) == ProjectStore::Recovered && ProjectCodec::sameContent(recovered, pattern()));
    assert(store.save(recovered) == ProjectStore::Saved && files.slots[0] == old);
    assert(store.save(recovered) == ProjectStore::Saved && files.writes == 2);
    // An equivalent v1 response is not successful verification of a v2 write.
    Files stale; stale.slots[0] = old; stale.substituteReadback = legacyPattern(8);
    ProjectStore staleStore(stale);
    assert(staleStore.save(pattern()) == ProjectStore::SaveFailed);
    assert(staleStore.dirty(pattern()) && stale.slots[0] == old && stale.writes == 1);
}

int main() {
    saveLoadAndRecovery();
    interruptionAtEveryByte();
    rejectWithoutMutation();
    ambiguousAndFailedVerification();
    migrationAndMixedVersions();
    migrationFailures();
    puts("Project store recovery tests passed (every byte interruption boundary)");
}
