#pragma once

#include "ProjectSnapshot.h"
#include <limits.h>

// One local project, with two independently checked generations. File adapters
// must report durable-write errors; readback alone is not a durability barrier.
class ProjectFiles {
public:
    enum ReadResult { Missing, Read, Failed };
    virtual ~ProjectFiles() {}
    virtual ReadResult read(unsigned slot, uint8_t *data, size_t capacity, size_t &size) = 0;
    virtual bool write(unsigned slot, const uint8_t *data, size_t size) = 0;
};

class ProjectStore {
public:
    enum Result { Empty, Loaded, Recovered, Corrupt, Unavailable, Unsupported,
                  Ambiguous, Saved, SaveFailed };

    explicit ProjectStore(ProjectFiles &files) : files(files) {}

    Result load(ProjectSnapshot &out) {
        const Result result = inspect();
        if (result == Loaded || result == Recovered) {
            out = candidates[newest];
            baseline = out;
            hasBaseline = true;
        }
        return result;
    }

    Result save(const ProjectSnapshot &value, bool replaceCorrupt = false) {
        if (!value.valid()) return SaveFailed;
        const Result result = inspect();
        if (result == Unsupported || result == Unavailable || result == Ambiguous ||
            (result == Corrupt && !replaceCorrupt)) return result;
        if (newest >= 0 && generations[newest] == UINT64_MAX) return SaveFailed;
        // Loading v1/v2/v3 is read-only. The first deliberate Save upgrades even
        // unchanged music, retaining the old slot until v4 is verified.
        if (result == Loaded && versions[newest] == 4 &&
            ProjectCodec::sameContent(value, candidates[newest])) {
            baseline = value;
            hasBaseline = true;
            return Saved;
        }
        const unsigned target = newest < 0 ? 0U : unsigned(1 - newest);
        const uint64_t generation = newest < 0 ? 1 : generations[newest] + 1;
        if (!ProjectCodec::encode(value, generation, buffer, ProjectCodec::Size) ||
            !files.write(target, buffer, ProjectCodec::Size)) return SaveFailed;
        size_t size = 0;
        uint64_t verifiedGeneration = 0;
        if (files.read(target, buffer, sizeof(buffer), size) != ProjectFiles::Read ||
            size != ProjectCodec::Size ||
            ProjectCodec::decode(buffer, size, candidates[target], verifiedGeneration) != ProjectCodec::Valid ||
            verifiedGeneration != generation ||
            !ProjectCodec::sameContent(value, candidates[target])) return SaveFailed;
        baseline = candidates[target];
        hasBaseline = true;
        return Saved;
    }

    bool dirty(const ProjectSnapshot &value) const {
        return !hasBaseline || !ProjectCodec::sameContent(value, baseline);
    }

    void forget() { hasBaseline = false; }

private:
    ProjectFiles &files;
    // These buffers live with the store, not on the small Arduino task stack.
    ProjectSnapshot candidates[2], baseline;
    uint8_t buffer[ProjectCodec::Size + 1] = {};
    uint64_t generations[2] = {};
    uint8_t versions[2] = {};
    int newest = -1;
    bool hasBaseline = false;

    Result inspect() {
        newest = -1;
        bool invalid = false, unavailable = false, unsupported = false;
        for (unsigned slot = 0; slot < 2; ++slot) {
            size_t size = 0;
            const auto read = files.read(slot, buffer, sizeof(buffer), size);
            if (read == ProjectFiles::Missing) continue;
            if (read == ProjectFiles::Failed || size > sizeof(buffer)) {
                unavailable = true;
                continue;
            }
            const auto decoded = ProjectCodec::decode(buffer, size, candidates[slot], generations[slot]);
            if (decoded == ProjectCodec::Unsupported) { unsupported = true; continue; }
            if (decoded != ProjectCodec::Valid) { invalid = true; continue; }
            versions[slot] = buffer[4];
            if (newest < 0 || generations[slot] > generations[newest]) newest = int(slot);
            else if (generations[slot] == generations[newest] &&
                     !ProjectCodec::sameContent(candidates[slot], candidates[newest])) {
                // With two slots, no later record can disambiguate this pair.
                return Ambiguous;
            }
            else if (generations[slot] == generations[newest] &&
                     versions[slot] > versions[newest]) newest = int(slot);
        }
        if (unsupported) return Unsupported;
        if (unavailable) return Unavailable;
        if (newest < 0) return invalid ? Corrupt : Empty;
        return invalid ? Recovered : Loaded;
    }
};
