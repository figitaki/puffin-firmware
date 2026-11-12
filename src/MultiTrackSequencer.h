#pragma once

#include "Sequencer.h"

// Shared transport and selected-track editing. Each polyphonic track owns
// its position, length and mute state; MIDI channels are fixed at 1 through 4.
class MultiTrackSequencer {
public:
    static const int TrackCount = 4;

    explicit MultiTrackSequencer(NoteOutput &output, Sequencer::Random random = nullptr)
        : tracks{Sequencer(output, 1, random), Sequencer(output, 2, random),
                 Sequencer(output, 3, random), Sequencer(output, 4, random)} {}

    int selectedTrack() const { return selected; }
    int channel() const { return selected + 1; }
    int nextStep() const { return tracks[selected].s; }
    int editStep() const { return editSteps[selected]; }
    int length() const { return tracks[selected].getLength(); }
    int pageCount() const { return (length() + Sequencer::StepsPerPage - 1) / Sequencer::StepsPerPage; }
    int editPage() const { return editStep() / Sequencer::StepsPerPage; }
    bool muted() const { return tracks[selected].isMuted(); }
    // All tracks' run states change together, including while muted.
    bool playing() const { return tracks[0].play; }

    bool selectTrack(int index) {
        if (index < 0 || index >= TrackCount) return false;
        selected = index;
        return true;
    }

    bool setStep(int index, int note, int velocity, bool append = false) {
        return tracks[selected].setStep(index, note, velocity, append);
    }

    bool setStep(int index, const Sequencer::Step &value) {
        return tracks[selected].setStep(index, value);
    }

    const Sequencer::Step *getStep(int index) const {
        return tracks[selected].getStep(index);
    }

    bool getStep(int index, uint8_t &note, uint8_t &velocity) const {
        return tracks[selected].getStep(index, note, velocity);
    }

    bool selectStep(int index) {
        if (index < 0 || index >= length()) return false;
        editSteps[selected] = index;
        return true;
    }

    bool setLength(int value) {
        if (!tracks[selected].setLength(value)) return false;
        if (editSteps[selected] >= value) editSteps[selected] = value - 1;
        stop(); // Never resize only one running member of the transport.
        return true;
    }

    bool setPages(int count) {
        if (count < 1 || count > Sequencer::MaxPages ||
            count * Sequencer::StepsPerPage == length()) return false;
        return setLength(count * Sequencer::StepsPerPage);
    }

    bool selectPage(int page) {
        if (page < 0 || page >= pageCount()) return false;
        const int target = page * Sequencer::StepsPerPage + editStep() % Sequencer::StepsPerPage;
        return selectStep(target < length() ? target : length() - 1);
    }

    void clear(int index = -1) { tracks[selected].clear(index); }
    void setMuted(bool value) { tracks[selected].setMuted(value); }
    void step() { for (auto &track : tracks) track.step(); }
    void pulse() { for (auto &track : tracks) track.pulse(); }
    void stop() { for (auto &track : tracks) track.stop(); }
    void reset() { for (auto &track : tracks) track.reset(); }
    void resume() { for (auto &track : tracks) track.resume(); }

private:
    Sequencer tracks[TrackCount];
    int editSteps[TrackCount] = {};
    int selected = 0;
};
