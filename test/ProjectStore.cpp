#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "ProjectStore.h"

class Files : public ProjectFiles {
public:
    std::vector<uint8_t> slots[2];
    bool readFailure = false, syncFailure = false, damageReadback = false;
    size_t writeLimit = ProjectCodec::Size;
    int writes = 0;
    ReadResult read(unsigned slot, uint8_t *data, size_t capacity, size_t &size) override {
        if (readFailure) return Failed;
        size = slots[slot].size() < capacity ? slots[slot].size() : capacity;
        if (slots[slot].empty()) return Missing;
        memcpy(data, slots[slot].data(), size);
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
    files.slots[1][4] = 2;
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

int main() {
    saveLoadAndRecovery();
    interruptionAtEveryByte();
    rejectWithoutMutation();
    ambiguousAndFailedVerification();
    puts("Project store recovery tests passed (every byte interruption boundary)");
}
