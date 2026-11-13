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
            if ((tick == Clock::Step || tick == Clock::Pulse) && clock.midiPulse()) ++pulses;
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
    // Partial 48-PPQN tick at 120 BPM, then double tempo: retain its credit.
    assert(clock.setBPM(240, 5000));
    assert(clock.poll(7708) == Clock::None);
    assert(clock.poll(7709) == Clock::Pulse && !clock.midiPulse());

    assert(clock.setBPM(120, 10000));
    clock.start(10000);
    assert(clock.poll(10000) == Clock::Step);
    assert(clock.poll(30833) == Clock::Pulse); // Just less than 2 overdue ticks.
    clock.start(50000);
    assert(clock.poll(50000) == Clock::Step);
    assert(clock.poll(70834) == Clock::Overrun);
    assert(!clock.running());
    assert(clock.poll(70835) == Clock::None);
    assert(clock.resume(75000));
    assert(!clock.resume(75000));
    assert(clock.poll(75000) == Clock::Pulse && !clock.midiPulse()); // Retained half-clock phase.
    clock.start(80000);
    assert(clock.poll(80000) == Clock::Step && clock.midiPulse());
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

void swingPairsKeepWireClockStraight() {
    for (int swing : {0, 1, 2}) for (int bpm : {20, 137, 300}) {
        Clock clock;
        const uint32_t origin = UINT32_MAX - 4000U;
        assert(!clock.setSwing(-1) && !clock.setSwing(3));
        assert(clock.setSwing(swing) && clock.swing() == swing);
        clock.setBPM(bpm, origin); clock.start(origin);
        const int longStep = swing == 0 ? 12 : swing == 1 ? 14 : 16;
        for (int i = 0; i <= 96; ++i) {
            const uint32_t deadline = uint32_t((60000000ULL * i + bpm * 48 - 1) / (bpm * 48));
            if (i) assert(clock.poll(origin + deadline - 1) == Clock::None);
            const auto tick = clock.poll(origin + deadline);
            assert(tick == (i % 24 == 0 || i % 24 == longStep ? Clock::Step : Clock::Pulse));
            assert(clock.midiPulse() == (i % 2 == 0));
            assert(clock.pulseScale() == 2);
            if (tick == Clock::Step)
                assert(clock.stepDuration() == (i % 24 == 0 ? longStep : 24 - longStep));
        }
    }
    for (auto source : {Clock::BLE, Clock::TRS}) {
        Clock clock; clock.setSwing(2); clock.setSource(source); clock.start(0);
        for (int i = 0; i < 48; ++i) {
            assert(clock.externalPulse(i * 19000) == (i % 6 == 0 ? Clock::Step : Clock::Pulse));
            assert(clock.midiPulse() && clock.pulseScale() == 1 && clock.stepDuration() == 6);
        }
        assert(clock.swing() == 2); // Stored preference survives external bypass.
    }
}

void swingChangesLatchAtPairBoundary() {
    Clock clock; clock.setBPM(125, 0); clock.start(0); // Exactly 10000 us/tick.
    assert(clock.poll(0) == Clock::Step && clock.stepDuration() == 12);
    clock.setSwing(2);
    for (int i = 1; i < 24; ++i)
        assert(clock.poll(i * 10000) == (i == 12 ? Clock::Step : Clock::Pulse));
    assert(clock.poll(240000) == Clock::Step && clock.stepDuration() == 16);
    clock.setSwing(0);
    clock.stop(); clock.resume(250000);
    assert(clock.poll(250000) == Clock::Pulse && !clock.midiPulse());
    for (int i = 2; i < 24; ++i)
        assert(clock.poll(240000 + i * 10000) == (i == 16 ? Clock::Step : Clock::Pulse));
    assert(clock.poll(480000) == Clock::Step && clock.stepDuration() == 12);
    clock.setSwing(1); clock.start(500000);
    assert(clock.poll(500000) == Clock::Step && clock.stepDuration() == 14);
}

int main() {
    swingPairsKeepWireClockStraight();
    swingChangesLatchAtPairBoundary();
    exactTempoAndWrap();
    tempoChangeAndStallBoundary();
    externalTempoTransportAndTimeout();
    externalTempoKeepsLastValidEstimate();
    puts("Clock transport regression tests passed");
}
