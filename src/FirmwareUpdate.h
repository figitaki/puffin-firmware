#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Storage must verify the digest and image before selecting the new boot slot.
class FirmwareStorage {
public:
    virtual ~FirmwareStorage() = default;
    virtual uint32_t capacity() const = 0;
    virtual bool begin(uint32_t size) = 0;
    virtual bool write(const uint8_t *data, size_t size) = 0;
    virtual bool finish(const uint8_t digest[32]) = 0;
    virtual void abort() = 0;
};

// Single-connection maintenance protocol. The BLE adapter serializes calls.
class FirmwareUpdate {
public:
    enum State { Ready, Authorized, Receiving, Complete, Error };
    enum Failure { None, Authentication, Locked, Protocol, Size, Offset, Flash, Verification, Timeout };

    FirmwareUpdate(FirmwareStorage &storage, const char pin[7], uint32_t now)
        : storage(storage), openedAt(now), lastActivity(now) { memcpy(code, pin, 6); }

    State state() const { return current; }
    Failure error() const { return failure; }
    uint32_t written() const { return received; }
    uint32_t total() const { return expected; }
    bool streaming() const { return stream; }
    bool progressDue() const { return !stream || packetsSinceStatus >= 8 || received == expected; }
    void progressPublished() { packetsSinceStatus = 0; }

    static uint32_t read32(const uint8_t *data) {
        return uint32_t(data[0]) | uint32_t(data[1]) << 8 |
               uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
    }

    void status(uint8_t bytes[12]) const {
        bytes[0] = 1; bytes[1] = uint8_t(current); bytes[2] = uint8_t(failure); bytes[3] = 0;
        for (int i = 0; i < 4; ++i) {
            bytes[4 + i] = uint8_t(received >> (8 * i));
            bytes[8 + i] = uint8_t(expected >> (8 * i));
        }
    }

    void poll(uint32_t now) {
        if (current == Complete || attempts >= 5) return;
        if (current == Receiving) {
            if (uint32_t(now - lastActivity) >= 30000) fail(Timeout);
        } else if (uint32_t(now - openedAt) >= 300000) fail(Timeout);
    }

    void control(const uint8_t *data, size_t size, uint32_t now) {
        poll(now);
        if (current == Complete || attempts >= 5 ||
            (current != Receiving && uint32_t(now - openedAt) >= 300000)) return;
        if (!size) { fail(Protocol); return; }
        if (data[0] == 5 && size == 1) { reset(); return; }
        if (data[0] == 1 && size == 7 && current != Receiving) {
            if (memcmp(data + 1, code, 6)) {
                ++attempts;
                fail(attempts >= 5 ? Locked : Authentication);
            } else {
                reset();
                current = Authorized;
                lastActivity = now;
            }
        } else if (data[0] == 6 && size == 1 && current == Authorized) {
            stream = true; // Explicit opt-in; old clients keep per-packet acknowledgments.
        } else if (data[0] == 3 && size == 18 && current == Authorized && data[1] < 2) {
            memcpy(digest + data[1] * 16, data + 2, 16);
            digestParts |= uint8_t(1 << data[1]);
        } else if (data[0] == 2 && size == 5 && current == Authorized && digestParts == 3) {
            expected = read32(data + 1);
            if (!expected || expected > storage.capacity()) { fail(Size); return; }
            if (!storage.begin(expected)) { fail(Flash); return; }
            received = 0;
            current = Receiving;
            lastActivity = now;
        } else if (data[0] == 4 && size == 1 && current == Receiving && received == expected) {
            if (!storage.finish(digest)) { fail(Verification); return; }
            current = Complete;
            failure = None;
        } else fail(Protocol);
    }

    void data(const uint8_t *bytes, size_t size, uint32_t now) {
        poll(now);
        if (current == Complete || current == Error) return;
        if (current != Receiving || size < 5 || size > (stream ? 512U : 180U)) { fail(Protocol); return; }
        if (read32(bytes) != received) { fail(Offset); return; }
        const size_t count = size - 4;
        if (count > expected - received) { fail(Size); return; }
        if (!storage.write(bytes + 4, count)) { fail(Flash); return; }
        received += uint32_t(count);
        if (packetsSinceStatus < 8) ++packetsSinceStatus;
        lastActivity = now;
    }

    void disconnect(uint32_t now) {
        if (current == Complete) return;
        reset();
        if (attempts >= 5) fail(Locked);
        else poll(now);
    }

private:
    FirmwareStorage &storage;
    State current = Ready;
    Failure failure = None;
    char code[6];
    uint8_t digest[32] = {}, digestParts = 0, attempts = 0, packetsSinceStatus = 0;
    bool stream = false;
    uint32_t expected = 0, received = 0, openedAt, lastActivity;

    void reset() {
        storage.abort();
        current = Ready;
        failure = None;
        expected = received = 0;
        digestParts = packetsSinceStatus = 0;
        stream = false;
    }

    void fail(Failure value) {
        storage.abort();
        current = Error;
        failure = value;
        digestParts = 0;
    }
};
