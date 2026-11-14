#pragma once

#include <stdint.h>

// Puffin's 128 x 32 SSD1306, in the horizontal address mode set by begin().
// One I2C transaction per update lets the main loop scan between chunks.
class DisplayTransfer {
public:
    bool busy() const { return chunk >= 0; }

    bool beginFrame() {
        if (busy()) return false;
        chunk = 0;
        return true;
    }

    // Keep the framebuffer unchanged until busy() becomes false. The ProS3
    // Wire buffer holds 128 bytes; each data transaction here uses only 33.
    template<class Bus>
    void update(Bus &bus, uint8_t address, const uint8_t *buffer) {
        if (!busy()) return;
        bus.setClock(400000);
        bus.beginTransmission(address);
        if (chunk == 0) {
            const uint8_t commands[] = {0x00, 0x22, 0, 3, 0x21, 0, 127};
            bus.write(commands, sizeof(commands));
        } else {
            bus.write(uint8_t(0x40));
            bus.write(buffer + (chunk - 1) * 32, 32);
        }
        const bool ok = bus.endTransmission() == 0;
        bus.setClock(100000); // Same restore clock as Adafruit_SSD1306.
        // On a bus error, abandon this frame; the next refresh starts with
        // a complete address command instead of continuing at a wrong pixel.
        if (!ok || chunk == 16) chunk = -1;
        else ++chunk;
    }

private:
    int chunk = -1;
};
