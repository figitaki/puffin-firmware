#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <vector>
#include "DisplayTransfer.h"

struct Bus {
    std::vector<std::vector<uint8_t>> packets;
    uint32_t clock = 100000;
    int error = 0;

    void setClock(uint32_t value) { clock = value; }
    void beginTransmission(uint8_t address) {
        assert(clock == 400000 && address == 0x3C);
        packets.push_back({});
    }
    void write(uint8_t byte) { packets.back().push_back(byte); }
    void write(const uint8_t *bytes, size_t size) {
        packets.back().insert(packets.back().end(), bytes, bytes + size);
    }
    int endTransmission() {
        assert(packets.back().size() <= 33);
        return error;
    }
};

void fullFrameAndRestart() {
    DisplayTransfer transfer;
    Bus bus;
    uint8_t buffer[512];
    for (int i = 0; i < 512; ++i) buffer[i] = uint8_t(i ^ (i >> 3));
    transfer.update(bus, 0x3C, buffer);
    assert(bus.packets.empty());
    assert(transfer.beginFrame());
    for (int chunk = 0; chunk < 17; ++chunk) {
        assert(transfer.busy());
        assert(!transfer.beginFrame()); // Cannot reset a partial frame.
        transfer.update(bus, 0x3C, buffer);
        assert(bus.packets.size() == size_t(chunk + 1));
        assert(bus.clock == 100000);
    }
    assert(!transfer.busy());
    assert((bus.packets[0] == std::vector<uint8_t>{0x00, 0x22, 0, 3, 0x21, 0, 127}));
    std::vector<uint8_t> received;
    for (size_t i = 1; i < bus.packets.size(); ++i) {
        assert(bus.packets[i].size() == 33 && bus.packets[i][0] == 0x40);
        received.insert(received.end(), bus.packets[i].begin() + 1, bus.packets[i].end());
    }
    assert(received == std::vector<uint8_t>(buffer, buffer + 512));
    transfer.update(bus, 0x3C, buffer);
    assert(bus.packets.size() == 17);

    // Fail both the address command and an in-progress data transfer.
    for (int failAt : {0, 5}) {
        assert(transfer.beginFrame());
        for (int i = 0; i < failAt; ++i) transfer.update(bus, 0x3C, buffer);
        bus.error = 2;
        transfer.update(bus, 0x3C, buffer);
        assert(!transfer.busy() && bus.clock == 100000);
        bus.error = 0;
        assert(transfer.beginFrame());
        transfer.update(bus, 0x3C, buffer);
        assert(bus.packets.back() == bus.packets[0]);
        while (transfer.busy()) transfer.update(bus, 0x3C, buffer);
    }
}

int main() {
    fullFrameAndRestart();
    puts("Display transfer regression tests passed");
}
