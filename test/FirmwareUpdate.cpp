#include <assert.h>
#include <stdio.h>
#include <vector>
#include "FirmwareUpdate.h"

struct Storage : FirmwareStorage {
    std::vector<uint8_t> inactive;
    bool selected = false, active = false, validImage = true, flashOK = true;
    uint32_t size = 0, limit = 263;
    uint32_t capacity() const override { return limit; }
    bool begin(uint32_t value) override {
        if (!flashOK) return false;
        size = value; inactive.clear(); active = true; return true;
    }
    bool write(const uint8_t *bytes, size_t count) override {
        if (!active || !flashOK) return false;
        inactive.insert(inactive.end(), bytes, bytes + count); return true;
    }
    bool finish(const uint8_t digest[32]) override {
        if (!active || !validImage || inactive.size() != size) return false;
        // Protocol fixtures use an asymmetric expected digest; SHA/image
        // verification itself is provided by mbedTLS/ESP-IDF on the board.
        for (int i = 0; i < 32; ++i) if (digest[i] != i * 7 + 3) return false;
        active = false; selected = true; return true;
    }
    void abort() override { if (active) inactive.clear(); active = false; }
};

void authorize(FirmwareUpdate &update, uint32_t now = 0) {
    const uint8_t auth[] = {1,'4','8','2','6','0','1'};
    update.control(auth, sizeof(auth), now);
    assert(update.state() == FirmwareUpdate::Authorized);
}
void digest(FirmwareUpdate &update, uint32_t now = 0, bool corrupt = false) {
    for (int half = 1; half >= 0; --half) { // Both orders are legal.
        uint8_t command[18] = {3, uint8_t(half)};
        for (int i = 0; i < 16; ++i) command[2 + i] = uint8_t((half * 16 + i) * 7 + 3);
        if (corrupt && half == 1) command[17] ^= 1;
        update.control(command, sizeof(command), now);
    }
}
void begin(FirmwareUpdate &update, uint32_t count, uint32_t now = 0) {
    const uint8_t command[] = {2, uint8_t(count), uint8_t(count >> 8), uint8_t(count >> 16), uint8_t(count >> 24)};
    update.control(command, sizeof(command), now);
}
void packet(FirmwareUpdate &update, uint32_t offset, int count, uint32_t now = 0) {
    std::vector<uint8_t> bytes = {uint8_t(offset),uint8_t(offset >> 8),uint8_t(offset >> 16),uint8_t(offset >> 24)};
    for (int i = 0; i < count; ++i) bytes.push_back(uint8_t((offset + i) * 11 + 9));
    update.data(bytes.data(), bytes.size(), now);
}
void finish(FirmwareUpdate &update, uint32_t now = 0) {
    const uint8_t command = 4; update.control(&command, 1, now);
}

void completeImageAndWireStatus() {
    Storage storage;
    FirmwareUpdate update(storage,"482601",0);
    authorize(update); digest(update); begin(update,263);
    packet(update,0,176,1); packet(update,176,87,2);
    assert(update.written() == 263 && !storage.selected);
    for (unsigned int i = 0; i < storage.inactive.size(); ++i)
        assert(storage.inactive[i] == uint8_t(i * 11 + 9));
    uint8_t status[12]; update.status(status);
    const uint8_t expected[] = {1,2,0,0,7,1,0,0,7,1,0,0};
    assert(!memcmp(status,expected,12));
    finish(update,3);
    assert(storage.selected && update.state() == FirmwareUpdate::Complete);
    const uint8_t abort = 5;
    update.control(&abort,1,4); update.disconnect(5); update.poll(1000000);
    assert(storage.selected && update.state() == FirmwareUpdate::Complete);
}

void authenticationAndSessionBoundaries() {
    Storage storage;
    FirmwareUpdate update(storage,"482601",0);
    begin(update,10);
    assert(update.error() == FirmwareUpdate::Protocol && !storage.active && !storage.selected);
    authorize(update); begin(update,10); // Digest is mandatory.
    assert(update.error() == FirmwareUpdate::Protocol);
    authorize(update); digest(update); begin(update,263); packet(update,0,17);
    update.disconnect(1);
    assert(update.state() == FirmwareUpdate::Ready && !storage.active && storage.inactive.empty());
    authorize(update,2); begin(update,263,2); // Reconnect cannot reuse the prior digest.
    assert(update.error() == FirmwareUpdate::Protocol);
    const uint8_t badPin[] = {1,'4','8','2','6','0','2'};
    for (int i = 0; i < 5; ++i) {
        update.control(badPin,sizeof(badPin),3);
        assert(update.error() == (i == 4 ? FirmwareUpdate::Locked : FirmwareUpdate::Authentication));
        update.disconnect(4); // Reconnecting does not reset attempts.
    }
    const uint8_t auth[] = {1,'4','8','2','6','0','1'};
    update.control(auth,sizeof(auth),5);
    assert(update.error() == FirmwareUpdate::Locked && !storage.selected);
}

void malformedOffsetsBoundsAndVerification() {
    for (int mode = 0; mode < 9; ++mode) {
        Storage storage;
        FirmwareUpdate update(storage,"482601",0);
        authorize(update); digest(update,0,mode == 5);
        begin(update,mode == 0 ? 264 : mode == 1 ? 0 : 20);
        if (mode < 2) assert(update.error() == FirmwareUpdate::Size);
        else if (mode == 2) { packet(update,1,10); assert(update.error() == FirmwareUpdate::Offset); }
        else if (mode == 3) { packet(update,0,21); assert(update.error() == FirmwareUpdate::Size); }
        else if (mode == 4) { packet(update,0,10); finish(update); assert(update.error() == FirmwareUpdate::Protocol); }
        else if (mode == 5 || mode == 6) {
            storage.validImage = mode != 6;
            packet(update,0,20); finish(update);
            assert(update.error() == FirmwareUpdate::Verification);
        } else if (mode == 7) {
            storage.flashOK = false; packet(update,0,10);
            assert(update.error() == FirmwareUpdate::Flash);
        } else {
            packet(update,0,10); packet(update,0,10); // Duplicate packet cannot be appended twice.
            assert(update.error() == FirmwareUpdate::Offset);
        }
        assert(!storage.selected && !storage.active);
    }
    Storage storage;
    FirmwareUpdate update(storage,"482601",0);
    authorize(update); digest(update); begin(update,263);
    packet(update,0,177); assert(update.error() == FirmwareUpdate::Protocol);
    authorize(update); digest(update); storage.flashOK = false; begin(update,263);
    assert(update.error() == FirmwareUpdate::Flash && !storage.selected);
}

void cancelAndTimeoutsAcrossMillisWrap() {
    Storage storage;
    const uint32_t opened = UINT32_MAX - 1000;
    FirmwareUpdate update(storage,"482601",opened);
    authorize(update,opened); digest(update,opened); begin(update,20,opened);
    packet(update,0,7,opened + 10);
    update.poll(opened + 30009);
    assert(update.state() == FirmwareUpdate::Receiving);
    update.poll(opened + 30010);
    assert(update.error() == FirmwareUpdate::Timeout && !storage.active && !storage.selected);
    authorize(update,opened + 30011); digest(update,opened + 30011); begin(update,20,opened + 30011);
    const uint8_t abort = 5; update.control(&abort,1,opened + 30012);
    assert(update.state() == FirmwareUpdate::Ready && update.written() == 0 && update.total() == 0);
    update.poll(opened + 300000);
    assert(update.error() == FirmwareUpdate::Timeout);
    const uint8_t auth[] = {1,'4','8','2','6','0','1'};
    update.control(auth,sizeof(auth),opened + 300001);
    assert(update.error() == FirmwareUpdate::Timeout && !storage.selected);
    // An active transfer may finish after the five-minute admission window.
    FirmwareUpdate ongoing(storage,"482601",0);
    authorize(ongoing,299999); digest(ongoing,299999); begin(ongoing,20,299999);
    packet(ongoing,0,20,300001); finish(ongoing,300002);
    assert(ongoing.state() == FirmwareUpdate::Complete && storage.selected);
}

void streamingIsNegotiatedBoundedAndVerified() {
    const uint8_t stream = 6;
    for (int payload : {16, 178, 508}) {
        Storage storage; storage.limit = 10000;
        FirmwareUpdate update(storage, "482601", 0);
        update.control(&stream, 1, 0);
        assert(update.error() == FirmwareUpdate::Protocol && !update.streaming());
        authorize(update); update.control(&stream, 1, 0); digest(update);
        assert(update.streaming());
        const uint32_t total = uint32_t(payload * 9 + 7);
        begin(update, total); update.progressPublished();
        for (int i = 0; i < 9; ++i) {
            packet(update, uint32_t(i * payload), payload, uint32_t(i));
            assert(update.state() == FirmwareUpdate::Receiving && !storage.selected);
            assert(update.progressDue() == (i == 7));
            if (update.progressDue()) update.progressPublished();
        }
        packet(update, uint32_t(payload * 9), 7, 10);
        assert(update.progressDue() && update.written() == total && !storage.selected);
        for (uint32_t i = 0; i < total; ++i) assert(storage.inactive[i] == uint8_t(i * 11 + 9));
        finish(update, 11);
        assert(storage.selected && update.state() == FirmwareUpdate::Complete);
    }
    Storage storage; storage.limit = 10000;
    FirmwareUpdate update(storage, "482601", 0);
    authorize(update); update.control(&stream, 1, 0); digest(update); begin(update, 1000);
    packet(update, 0, 509); assert(update.error() == FirmwareUpdate::Protocol && !storage.selected);
    update.disconnect(1); assert(!update.streaming());
    authorize(update); digest(update); begin(update, 1000);
    packet(update, 0, 177); assert(update.error() == FirmwareUpdate::Protocol); // Legacy limit restored.
    authorize(update); update.control(&stream, 1, 0); digest(update); begin(update, 1000);
    packet(update, 0, 178); packet(update, 0, 178);
    assert(update.error() == FirmwareUpdate::Offset && !storage.selected);
    authorize(update); update.control(&stream, 1, 0); digest(update); begin(update, 1000);
    packet(update, 0, 508); finish(update);
    assert(update.error() == FirmwareUpdate::Protocol && !storage.selected);
    authorize(update); update.control(&stream, 1, 0); digest(update, 0, true); begin(update, 508);
    packet(update, 0, 508); finish(update);
    assert(update.error() == FirmwareUpdate::Verification && !storage.selected);
}

int main() {
    streamingIsNegotiatedBoundedAndVerified();
    completeImageAndWireStatus(); authenticationAndSessionBoundaries();
    malformedOffsetsBoundsAndVerification(); cancelAndTimeoutsAcrossMillisWrap();
    puts("Firmware update protocol regression tests passed");
}
