#pragma once

#include "ProjectStore.h"
#include <LittleFS.h>
#include <esp_partition.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

// POSIX I/O exposes fsync/close errors that Arduino File::flush() discards.
class LittleFSProjectFiles : public ProjectFiles {
public:
    bool begin() {
        ready = LittleFS.begin(false);
        blank = false;
        if (!ready) {
            // A never-formatted partition is not a damaged saved project. Only
            // classify it as empty after checking EVERY byte; do not autoformat.
            const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                             ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
            if (!partition) return false;
            uint8_t bytes[256];
            for (size_t offset = 0; offset < partition->size; offset += sizeof(bytes)) {
                const size_t count = partition->size - offset < sizeof(bytes)
                                   ? partition->size - offset : sizeof(bytes);
                if (esp_partition_read(partition, offset, bytes, count) != ESP_OK) return false;
                for (size_t i = 0; i < count; ++i) if (bytes[i] != 0xff) return false;
            }
            blank = true;
        }
        return ready || blank;
    }
    bool available() const { return ready; }
    bool uninitialized() const { return blank; }

    bool initialize() {
        LittleFS.end();
        ready = blank = false;
        if (!LittleFS.format()) return false;
        return begin();
    }

    ReadResult read(unsigned slot, uint8_t *data, size_t capacity, size_t &size) override {
        size = 0;
        if (slot > 1) return Failed;
        if (blank) return Missing;
        if (!ready) return Failed;
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
    bool ready = false, blank = false;
    static const char *path(unsigned slot) {
        return slot ? "/littlefs/project-b.pfn" : "/littlefs/project-a.pfn";
    }
};
