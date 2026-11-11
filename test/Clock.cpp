#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include "Clock.h"

void exactTempoAndWrap() {
    // Poll every 137 us at non-divisor tempos, including across micros wrap.
    // Count a full minute excluding the final boundary: BPM * 24 clocks,
    // BPM * 4 sixteenth notes, including the immediate first clock/step.
    for (int bpm : {20, 59, 73, 137, 300}) {
        Clock clock;
        const uint32_t origin = 0xFFFFF000U;
        assert(clock.setBPM(bpm, origin));
        clock.start(origin);
        int pulses = 0, steps = 0;
        for (uint32_t elapsed = 0; elapsed < 60000000; elapsed += 137) {
            const auto tick = clock.poll(origin + elapsed);
            assert(tick != Clock::Overrun);
            if (tick == Clock::Step) ++steps;
            if (tick == Clock::Step || tick == Clock::Pulse) ++pulses;
        }
        assert(pulses == bpm * 24 && steps == bpm * 4);
        assert(clock.poll(origin + 60000000) == Clock::Step);
        clock.stop();
        assert(clock.poll(origin + 60100000) == Clock::None);
    }
}

void tempoChangeAndStallBoundary() {
    Clock clock;
    assert(!clock.setBPM(19, 0) && !clock.setBPM(301, 0));
    assert(clock.getBPM() == 120);
    clock.start(0);
    assert(clock.poll(0) == Clock::Step);
    // Half a clock at 120 BPM, then double tempo: keep that half's phase.
    assert(clock.setBPM(240, 10000));
    assert(clock.poll(15416) == Clock::None);
    assert(clock.poll(15417) == Clock::Pulse);

    assert(clock.setBPM(120, 20000));
    clock.start(20000);
    assert(clock.poll(20000) == Clock::Step);
    assert(clock.poll(61666) == Clock::Pulse); // Just less than 2 overdue.
    clock.start(100000);
    assert(clock.poll(100000) == Clock::Step);
    assert(clock.poll(141667) == Clock::Overrun);
    assert(!clock.running());
    assert(clock.poll(141668) == Clock::None);
    assert(clock.resume(150000));
    assert(!clock.resume(150000));
    assert(clock.poll(150000) == Clock::Pulse); // Continue retains pulse phase.
    clock.start(160000);
    assert(clock.poll(160000) == Clock::Step); // Start resets it.
}

void externalTempoTransportAndTimeout() {
    for (auto source : {Clock::BLE, Clock::TRS}) {
        Clock clock;
        clock.setSource(source);
        assert(clock.getBPM() == 0);
        const uint32_t origin = 0xFFFF0000U;
        clock.start(origin);
        assert(clock.poll(origin) == Clock::None);
        for (uint32_t i = 0; i <= 24; ++i) {
            const uint32_t now = origin + i * 25000; // 100 BPM, 24 intervals.
            assert(clock.poll(now) == Clock::None);
            assert(clock.externalPulse(now) == (i % 6 == 0 ? Clock::Step : Clock::Pulse));
        }
        assert(clock.getBPM() == 100);
        clock.stop();
        assert(clock.externalPulse(origin + 625000) == Clock::None);
        assert(clock.resume(origin + 650000));
        assert(clock.externalPulse(origin + 650000) == Clock::Pulse);
        assert(clock.poll(origin + 1149999) == Clock::None);
        assert(clock.poll(origin + 1150000) == Clock::Overrun);
        assert(!clock.running());
        clock.start(origin + 1200000);
        assert(clock.externalPulse(origin + 1700000) == Clock::Overrun);
        assert(!clock.running()); // No intervening poll to notice the gap.
        clock.setSource(Clock::Internal);
        assert(clock.externalPulse(0) == Clock::None);
        assert(clock.getBPM() == 120);
    }
    Clock clock;
    clock.setSource(Clock::BLE);
    for (int i = 0; i < 25; ++i) clock.externalPulse(100); // No division by zero.
    assert(clock.getBPM() == 0);
}

void externalTempoKeepsLastValidEstimate() {
    for (auto source : {Clock::BLE, Clock::TRS}) {
        Clock clock;
        clock.setSource(source);
        uint32_t now = 0;
        clock.externalPulse(now);
        auto window = [&](uint32_t duration) {
            const uint32_t start = now;
            for (uint32_t i = 1; i <= 24; ++i) {
                now = start + uint32_t(uint64_t(i) * duration / 24);
                clock.externalPulse(now);
            }
        };
        window(600000); // 100 BPM, followed by zero-duration/invalid windows.
        assert(clock.getBPM() == 100);
        for (uint32_t duration : {0U, 100000U, 6000000U}) {
            window(duration);
            assert(clock.getBPM() == 100);
        }
        window(200000);
        assert(clock.getBPM() == 300);
        window(3000000);
        assert(clock.getBPM() == 20);
        window(800000);
        assert(clock.getBPM() == 75);
        clock.setSource(source == Clock::BLE ? Clock::TRS : Clock::BLE);
        assert(clock.getBPM() == 0); // Estimates belong to one clock input.
    }
}

int main() {
    exactTempoAndWrap();
    tempoChangeAndStallBoundary();
    externalTempoTransportAndTimeout();
    externalTempoKeepsLastValidEstimate();
    puts("Clock transport regression tests passed");
}
