#include <Chord.h>
#include "Sequencer.h"
#include "Clock.h"

void playChord(){
    MyChord chord;
    chord.constructAtRoot(60, CHORD_MAJOR);
}
