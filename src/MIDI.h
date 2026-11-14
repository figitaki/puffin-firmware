/* __/\\\\\\\\\\\\\________________________/\\\\\________/\\\\\_____________________         */
/*  _\/\\\/////////\\\____________________/\\\///_______/\\\///______________________        */
/*  _\/\\\_______\/\\\___________________/\\\__________/\\\_______/\\\_______________        */
/*  _\/\\\\\\\\\\\\\/___/\\\____/\\\__/\\\\\\\\\____/\\\\\\\\\___\///___/\\/\\\\\\___        */
/*   _\/\\\/////////____\/\\\___\/\\\_\////\\\//____\////\\\//_____/\\\_\/\\\////\\\__       */
/*    _\/\\\_____________\/\\\___\/\\\____\/\\\_________\/\\\______\/\\\_\/\\\__\//\\\_      */
/*     _\/\\\_____________\/\\\___\/\\\____\/\\\_________\/\\\______\/\\\_\/\\\___\/\\\_     */
/*      _\/\\\_____________\//\\\\\\\\\_____\/\\\_________\/\\\______\/\\\_\/\\\___\/\\\_    */
/*       _\///_______________\/////////______\///__________\///_______\///__\///____\///__   */

//  Puffin Keyboard/MIDI Sequencer
//  Designed by Colby Janecka in Sunny California

// MIDI.h
//  This file defines information and functions for the MIDI interface of the keyboard


//#define CS_USE_NIMBLE
#include <Seq.h>
#include "MultiTrackSequencer.h"
#include <Control_Surface.h>
#include <MIDI_Interfaces/BluetoothMIDI_Interface.hpp>


// Instantiate a MIDI over BLE interface
BluetoothMIDI_Interface midi_ble;
// Instantiate a 5-pin DIN MIDI interface (on the TX and RX pins of Serial1)
HardwareSerialMIDI_Interface midi_ser {Serial0, MIDI_BAUD};

MIDI_Interface* midi_out = &midi_ble;

//MIDI TEST NOTE SENDING
// MIDI address of the note to send
const MIDIAddress noteAddress {MIDI_Notes::C[4], Channel_1};
// The velocity of the note events
const uint8_t velocity = 0x7F;

bool midiMode;
bool bridgeBLEtoSER = true;
bool serialOutput = false;
bool stepEditing = false;

bool activeNotes[12] = {false};
uint8_t liveChannels[128] = {}; // Capture press-time channel across track changes.

Clock clk;

void sendOwnedNote(uint8_t note, uint8_t velocity, bool on, uint8_t channel){
  MIDIAddress address {note, Channel::createChannel(channel)};
  if(on) midi_out->sendNoteOn(address, velocity);
  else midi_out->sendNoteOff(address, velocity);
}

NoteOutput noteOutput(sendOwnedNote);
MultiTrackSequencer seq(noteOutput);

// Channel/SysEx traffic retains the existing bidirectional bridge. Only the
// selected clock input may forward transport, preventing competing masters
// from leaking a second clock into the generated internal-clock output.
class ClockInputPipe : public MIDI_Pipe {
public:
  explicit ClockInputPipe(Clock::Source source) : source(source) {}
  void mapForwardMIDI(RealTimeMessage message) override {
    switch(message.getMessageType()) {
      case MIDIMessageType::TimingClock:
      case MIDIMessageType::Start:
      case MIDIMessageType::Continue:
      case MIDIMessageType::Stop:
        if(clk.source() != source) return;
        break;
      default: break;
    }
    sourceMIDItoSink(message);
  }
private:
  Clock::Source source;
};

ClockInputPipe bleToSerial(Clock::BLE), serialToBle(Clock::TRS);

void stopTransport(bool sendStop){
  clk.stop();
  seq.stop();
  if(sendStop) {
    midi_out->sendRealTime(MIDIMessageType::Stop);
  }
  midi_out->sendNow();
}

void startTransport(bool sendStart){
  seq.reset();
  seq.resume(); // Step zero sounds on the first clock, not on Start itself.
  clk.start(micros());
  if(sendStart) {
    midi_out->sendRealTime(MIDIMessageType::Start);
  }
  midi_out->sendNow();
}

void continueTransport(bool sendContinue){
  if(!clk.resume(micros())) return;
  seq.resume();
  if(sendContinue) {
    midi_out->sendRealTime(MIDIMessageType::Continue);
    midi_out->sendNow();
  }
}

void updateSequencerClock(){
  const Clock::Tick tick = clk.poll(micros());
  if(tick == Clock::Overrun) { stopTransport(true); return; }
  if(tick == Clock::None) return;
  midi_out->sendRealTime(MIDIMessageType::TimingClock);
  if(tick == Clock::Step) seq.step();
  midi_out->sendNow();
}

void panicMIDI(){
  if(!midiMode) return; // The MIDI interfaces are not begun in typing mode.
  stopTransport(clk.running());
  noteOutput.panic();
  for(auto &channel : liveChannels) channel = 0;
  for(auto &note : activeNotes) note = false;
  midi_out->sendNow();
  if(serialOutput) Serial0.flush();
}

void setupSequencer(){
  seq.setStep(0,60,0x6F);
}

void setupBleSerPipe(){

}

void setMidiMode(bool on){
  panicMIDI();
  EEPROM.begin(4);                                      //Begin EEPROM, allow us to store
  EEPROM.write(1,on);
  EEPROM.commit();
  esp_sleep_enable_timer_wakeup(1);
  esp_deep_sleep_start(); 
}

bool readMIDIModeEEPROM(){
  EEPROM.begin(4);                                      //Begin EEPROM, allow us to store
  midiMode = EEPROM.read(1);                     //Read selected address from storage
  return midiMode;
}
bool getMIDIMode(){
  return midiMode;
}
bool bleMIDIConnected(){
  return midi_ble.isConnected();
}

void playNote(int note, int vel){
  if(note < 0 || note > 127 || vel < 1 || vel > 127 || liveChannels[note]) return;
  liveChannels[note] = seq.channel();
  noteOutput.press(note, vel, liveChannels[note]);
}

void stopNote(int note){
  if(note < 0 || note > 127 || !liveChannels[note]) return;
  noteOutput.release(note, liveChannels[note]);
  liveChannels[note] = 0;
}

// Process the correct midi function triggered from MIDIMap.h
void processMIDIPress(int function){

  //Serial.println("MIDI FUNCTION: " + String(function));
  int currentBPM = clk.internalBPM();

  // if function is between 300 and 320, play that respective note
  if(293 < function && function < 314){
    const uint8_t velocity = 0x5F;
    int noteToPlay = function-241;
    activeNotes[noteToPlay%12] = true;
    playNote(noteToPlay, velocity);
    if(stepEditing) seq.setStep(seq.editStep(),noteToPlay,velocity);
    else if(seq.playing()) seq.setStep(-1,noteToPlay,velocity);
  }
  switch (function){
    case SW_OUTPUT:
      panicMIDI(); // Release on the old route before selecting the new one.
      serialOutput = !serialOutput;
      midi_out = serialOutput ? static_cast<MIDI_Interface*>(&midi_ser)
                              : static_cast<MIDI_Interface*>(&midi_ble);
      break;
    case CLOCK_START:
      startTransport(true);
      break;
    case CLOCK_STOP:
      stopTransport(true);
      break;
    case CLOCK_CONTINUE:
      continueTransport(true);
      break;
    case CLOCK_SOURCE:
      stopTransport(true);
      clk.setSource(static_cast<Clock::Source>((clk.source() + 1) % 3));
      break;
    case TRACK_1:
    case TRACK_2:
    case TRACK_3:
    case TRACK_4:
      seq.selectTrack(function - TRACK_1);
      break;
    case TRACK_MUTE:
      seq.setMuted(!seq.muted());
      midi_out->sendNow();
      break;
    case TRACK_SHORTER:
      if(seq.setLength(seq.length() - 1)) stopTransport(true);
      break;
    case TRACK_LONGER:
      if(seq.setLength(seq.length() + 1)) stopTransport(true);
      break;
    case EDIT_TOGGLE:
      stepEditing = !stepEditing;
      break;
    case EDIT_PREVIOUS:
    case EDIT_NEXT:
    case EDIT_PAGE_BACK:
    case EDIT_PAGE_NEXT:
      if(stepEditing) {
        const int direction = function == EDIT_PREVIOUS || function == EDIT_PAGE_BACK ? -1 : 1;
        int target = (seq.editStep() + direction + seq.length()) % seq.length();
        if(function == EDIT_PAGE_BACK || function == EDIT_PAGE_NEXT) {
          const int pages = (seq.length() + 15) / 16;
          const int page = (seq.editStep() / 16 + direction + pages) % pages;
          target = min(page * 16 + seq.editStep() % 16, seq.length() - 1);
        }
        seq.selectStep(target);
      }
      break;
    case BPM_INC:
      clk.setBPM(currentBPM + 1, micros());
      break;
    case BPM_DEC:
      clk.setBPM(currentBPM - 1, micros());
      break;
    case MIDI_MODE:
      setMidiMode(false);
      break;
    case PLAY_CHORD:
      playChord();
      break;
    case SEQ_CLEAR:
      if(stepEditing) seq.setStep(seq.editStep(),0,0);
      else seq.clear();
      //chord.constructAtRoot()
  }
}

void processMIDIRelease(int function){

  // if function is between 300 and 320, play that respective note
  if(293 < function && function < 314){
    const uint8_t velocity = 0x7F;
    int noteToPlay = function-241;
    activeNotes[noteToPlay%12] = false;
    stopNote(noteToPlay);
  }
}

// Custom MIDI callback that prints incoming messages.
struct MyMIDI_Callbacks : MIDI_Callbacks {
 
  // Callback for channel messages (notes, control change, pitch bend, etc.).
  void onChannelMessage(MIDI_Interface &, ChannelMessage cm) override {
    // Print the message
    Serial << F("Channel message: ")                                        //
           << hex << cm.header << ' ' << cm.data1 << ' ' << cm.data2 << dec //
           << F(" on cable ") << cm.cable.getOneBased() << endl;
  }

  // Callback for real-time messages
  void onRealTimeMessage(MIDI_Interface &input, RealTimeMessage rt) override {
    const Clock::Source source = &input == &midi_ble ? Clock::BLE : Clock::TRS;
    if(clk.source() != source) return;
    switch(rt.getMessageType()){
      case MIDIMessageType::TimingClock:
        switch(clk.externalPulse(micros())) {
          case Clock::Step: seq.step(); break;
          case Clock::Overrun: stopTransport(true); break;
          default: break;
        }
        break;
      case MIDIMessageType::Start:
        startTransport(false); // The pipe already forwarded this transport.
        break;
      case MIDIMessageType::Continue:
        continueTransport(false);
        break;
      case MIDIMessageType::Stop:
        stopTransport(false);
        break;
    }
  }
 
} callback;
