#include <Chord.h>
#include "Sequencer.h"

class Clock {
    public:
    bool ext = false;

    long bpm = 120;
    long tempo = 1000/(bpm/60);

    long prevmillis = 0;
    long interval = tempo/24;

    void setBPM(long bpmNew){
        bpm = (long)bpmNew;
        tempo = 1000/(bpm/60);
    }

    int getBPM(){
        return bpm;
    }

    bool clockUpdate(){
        unsigned long currentMillis = millis();
        if(currentMillis - prevmillis > interval) {
            // save the last time.
            prevmillis = currentMillis;
            return true;
        }
        return false;
    }
};

void playChord(){
    MyChord chord;
    chord.constructAtRoot(60, CHORD_MAJOR);
}
