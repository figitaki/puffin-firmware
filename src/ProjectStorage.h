#pragma once

#include "ProjectStore.h"
#include <LittleFS.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

// POSIX I/O exposes fsync/close errors that Arduino File::flush() discards.
class LittleFSProjectFiles : public ProjectFiles {
public:
    bool begin() { ready = LittleFS.begin(false); return ready; }
    bool available() const { return ready; }

    bool initialize() {
        LittleFS.end();
        ready = false;
        if (!LittleFS.format()) return false;
        return begin();
    }

    ReadResult read(unsigned slot, uint8_t *data, size_t capacity, size_t &size) override {
        size = 0;
        if (!ready || slot > 1) return Failed;
        const int fd = ::open(path(slot), O_RDONLY);
        if (fd < 0) return errno == ENOENT ? Missing : Failed;
        bool okay = true;
        while (size < capacity) {
            const ssize_t count = ::read(fd, data + size, capacity - size);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { okay = false; break; }
            if (!count) break;
            size += size_t(count);
        }
        if (::close(fd) != 0) okay = false;
        return okay ? Read : Failed;
    }

    bool write(unsigned slot, const uint8_t *data, size_t size) override {
        if (!ready || slot > 1) return false;
        const int fd = ::open(path(slot), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return false;
        size_t written = 0;
        bool okay = true;
        while (written < size) {
            const ssize_t count = ::write(fd, data + written, size - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { okay = false; break; }
            written += size_t(count);
        }
        if (::fsync(fd) != 0) okay = false;
        if (::close(fd) != 0) okay = false;
        return okay;
    }

private:
    bool ready = false;
    static const char *path(unsigned slot) {
        return slot ? "/littlefs/project-b.pfn" : "/littlefs/project-a.pfn";
    }
};
