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
#include "Clock.h"
#include "NoteKeyboard.h"
#include "MultiTrackSequencer.h"
#include "ProjectStorage.h"
#include "PatternSync.h"
#include <esp_system.h>
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
bool chordEntry = false;
enum StepView { StepNotes, StepTrigger, StepControl, StepVelocity, StepGate, StepSwing };
StepView stepView = StepNotes;
uint8_t editControl = 74; // MIDI brightness/filter cutoff by convention.
bool stepFull = false;


Clock clk;

void sendOwnedNote(uint8_t note, uint8_t velocity, bool on, uint8_t channel){
  MIDIAddress address {note, Channel::createChannel(channel)};
  if(on) midi_out->sendNoteOn(address, velocity);
  else midi_out->sendNoteOff(address, velocity);
}

void sendLockedControl(uint8_t control, uint8_t value, uint8_t channel){
  midi_out->sendControlChange({control, Channel::createChannel(channel)}, value);
}

NoteOutput noteOutput(sendOwnedNote, sendLockedControl);
NoteKeyboard noteKeyboard(noteOutput);
MultiTrackSequencer seq(noteOutput, esp_random);

LittleFSProjectFiles projectFiles;
ProjectStore projectStore(projectFiles);
ProjectSnapshot project;
PatternSync patternSync;
PatternSync::Reply patternReply;
bool projectDirty = true, projectBusy = false, projectChanged = false;
bool projectTransportPaused = false;
int projectPending = 0, projectPrompt = 0;
const char *projectNotice = nullptr;
uint32_t projectNoticeAt = 0;

void showProjectNotice(const char *text) {
  projectNotice = text;
  projectNoticeAt = millis();
}

void updateProjectDirty() {
  project.capture(seq, uint16_t(clk.internalBPM()), uint8_t(clk.swing()));
  projectDirty = projectStore.dirty(project);
  patternSync.observe(project);
}

void projectResult(ProjectStore::Result result) {
  switch(result) {
    case ProjectStore::Loaded: showProjectNotice("PROJECT LOADED"); break;
    case ProjectStore::Recovered: showProjectNotice("SAVE RECOVERED"); break;
    case ProjectStore::Saved: showProjectNotice("PROJECT SAVED"); break;
    case ProjectStore::Empty: showProjectNotice("NO SAVED PROJECT"); break;
    case ProjectStore::Corrupt: showProjectNotice("SAVES DAMAGED"); break;
    case ProjectStore::Unavailable: showProjectNotice("STORAGE ERROR"); break;
    case ProjectStore::Unsupported: showProjectNotice("NEW FW REQUIRED"); break;
    case ProjectStore::Ambiguous: showProjectNotice("SAVES CONFLICT"); break;
    default: showProjectNotice("SAVE FAILED"); break;
  }
}

// Channel/unrelated SysEx traffic retains the bidirectional bridge; PF pattern
// requests are consumed locally and replies return to their ingress. Only the
// selected clock input may forward transport, preventing competing masters
// from leaking a second clock into the generated internal-clock output.
class ClockInputPipe : public MIDI_Pipe {
public:
  explicit ClockInputPipe(Clock::Source source) : source(source) {}
  void mapForwardMIDI(SysExMessage message) override {
    if(message.isFirstChunk()) suppressPattern = PatternSync::addressed(message.data, message.length);
    const bool suppress = suppressPattern;
    if(message.isLastChunk()) suppressPattern = false;
    if(!suppress) sourceMIDItoSink(message);
  }
  void mapForwardMIDI(RealTimeMessage message) override {
    if(projectBusy || projectTransportPaused) return;
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
  bool suppressPattern = false;
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
  if(clk.midiPulse()) midi_out->sendRealTime(MIDIMessageType::TimingClock);
  if(tick == Clock::Step) seq.step(clk.stepDuration(), clk.pulseScale());
  else seq.pulse();
  midi_out->sendNow();
}

void panicMIDI(){
  if(!midiMode) return; // The MIDI interfaces are not begun in typing mode.
  stopTransport(clk.running());
  noteKeyboard.releaseAll();
  noteOutput.panic();
  midi_out->sendNow();
  if(serialOutput) Serial0.flush();
}

void setupSequencer(){
  // A new identity is used only if no saved project can be restored.
  esp_fill_random(project.projectID, sizeof(project.projectID));
  project.projectID[6] = (project.projectID[6] & 0x0f) | 0x40;
  project.projectID[8] = (project.projectID[8] & 0x3f) | 0x80;
  if(!projectFiles.begin()) {
    projectResult(ProjectStore::Unavailable);
    return; // Never format automatically, including on the first boot.
  }
  const auto result = projectStore.load(project);
  if(result == ProjectStore::Loaded || result == ProjectStore::Recovered) {
    project.apply(seq);
    clk.setBPM(project.bpm, micros());
    clk.setSwing(project.swing);
  }
  // Normal boot is silent: a mounted empty filesystem or a verified erased
  // partition simply starts a new pattern. Explicit Load still reports Empty.
  if(result != ProjectStore::Empty && result != ProjectStore::Loaded) projectResult(result);
  updateProjectDirty();
}

// Called after the entire key-event batch, so panic also releases notes from
// keys pressed in the same scan as Save/Load. Held keys need a fresh press.
void updateProjectStorage(){
  if(projectChanged) {
    updateProjectDirty();
    projectChanged = false;
  }
  const int action = projectPending;
  projectPending = 0;
  if(!action) return;
  if(action == PROJECT_CANCEL) {
    projectPrompt = 0;
    showProjectNotice("CANCELLED");
    return;
  }
  if(clk.running() || seq.playing()) {
    projectPrompt = 0;
    showProjectNotice("STOP PLAYBACK");
    return;
  }
  int operation = action;
  bool confirmed = false;
  if(action == PROJECT_YES) {
    if(!projectPrompt) return;
    operation = projectPrompt;
    confirmed = true;
    projectPrompt = 0;
  }
  if(operation == PROJECT_INIT && !confirmed) {
    projectPrompt = PROJECT_INIT;
    return;
  }
  updateProjectDirty();
  if(operation == PROJECT_LOAD && projectDirty && !confirmed) {
    projectPrompt = PROJECT_LOAD;
    return;
  }
  projectPrompt = 0;
  projectBusy = true;
  projectTransportPaused = true;
  panicMIDI();
  if(operation == PROJECT_INIT) {
    if(projectFiles.initialize()) {
      projectStore.forget();
      showProjectNotice("STORAGE READY");
    } else showProjectNotice("STORAGE ERROR");
  } else if(operation == PROJECT_SAVE && projectFiles.uninitialized()) {
    showProjectNotice("STORAGE NOT SETUP"); // Explicit Fn+C confirmation, never automatic erasure.
  } else if(operation == PROJECT_SAVE) {
    const auto result = projectStore.save(project, confirmed);
    if(result == ProjectStore::Corrupt && !confirmed) projectPrompt = PROJECT_SAVE;
    else projectResult(result);
  } else if(operation == PROJECT_LOAD) {
    const auto result = projectStore.load(project);
    if(result == ProjectStore::Loaded || result == ProjectStore::Recovered) {
      project.apply(seq);
      clk.setSource(clk.source()); // Reset phase, retaining the device's source.
      clk.setBPM(project.bpm, micros());
      clk.setSwing(project.swing);
      chordEntry = false;
      stepView = StepNotes;
      stepFull = false;
    }
    projectResult(result);
  }
  // Polling is bounded and may leave older transport in another input batch.
  // Keep external transport paused until a local Start/Continue re-arms it.
  MIDI_Interface::updateAll();
  projectBusy = false;
  updateProjectDirty();
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

// Process the correct midi function triggered from MIDIMap.h
void processMIDIPress(int function){
  if(projectBusy) return;
  if(function >= PROJECT_SAVE && function <= PROJECT_CANCEL) {
    if(!projectPending) projectPending = function;
    return;
  }
  projectPrompt = 0; // Unrelated actions cancel destructive confirmations.
  projectNotice = nullptr;
  projectChanged = true;

  //Serial.println("MIDI FUNCTION: " + String(function));
  int currentBPM = clk.internalBPM();

  // if function is between 300 and 320, play that respective note
  if(293 < function && function < 314){
    const uint8_t storedVelocity = stepEditing ? seq.getStep(seq.editStep())->velocity : 0;
    const uint8_t velocity = storedVelocity ? storedVelocity : 95;
    const int noteToPlay = noteKeyboard.press(function - 241, velocity, seq.channel());
    if(noteToPlay < 0) return;
    if(stepEditing) stepFull = !seq.setStep(seq.editStep(),noteToPlay,velocity,chordEntry);
    else if(seq.playing()) seq.setStep(-1,noteToPlay,velocity);
  }
  switch (function){
    case OCTAVE_DOWN:
    case OCTAVE_UP:
      noteKeyboard.shiftOctave(function == OCTAVE_DOWN ? -1 : 1);
      {
        static char notice[20];
        snprintf(notice, sizeof(notice), "KEY OCTAVE %+d", noteKeyboard.octave());
        showProjectNotice(notice);
      }
      break;
    case SW_OUTPUT:
      panicMIDI(); // Release on the old route before selecting the new one.
      serialOutput = !serialOutput;
      midi_out = serialOutput ? static_cast<MIDI_Interface*>(&midi_ser)
                              : static_cast<MIDI_Interface*>(&midi_ble);
      break;
    case CLOCK_START:
      projectTransportPaused = false;
      startTransport(true);
      break;
    case CLOCK_STOP:
      stopTransport(true);
      break;
    case CLOCK_CONTINUE:
      projectTransportPaused = false;
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
      stepFull = false;
      break;
    case TRACK_MUTE:
      seq.setMuted(!seq.muted());
      midi_out->sendNow();
      break;
    case TRACK_SHORTER:
      if(seq.setPages(seq.pageCount() - 1)) stopTransport(true);
      break;
    case TRACK_LONGER:
      if(seq.setPages(seq.pageCount() + 1)) stopTransport(true);
      break;
    case EDIT_SET_END:
      if(stepEditing && seq.editStep() + 1 != seq.length() &&
         seq.setLength(seq.editStep() + 1)) stopTransport(true);
      break;
    case EDIT_TOGGLE:
      stepEditing = !stepEditing;
      stepFull = false;
      break;
    case EDIT_DETAILS:
      if(stepEditing) stepView = static_cast<StepView>((stepView + 1) % 6);
      break;
    case SWING_CYCLE:
      if(stepEditing) {
        clk.setSwing((clk.swing() + 1) % 3);
        stepFull = false;
        stepView = StepSwing;
      }
      break;
    case GATE_DOWN:
    case GATE_UP:
      if(stepEditing) {
        Sequencer::Step value = *seq.getStep(seq.editStep());
        value.gate = constrain(int(value.gate) + (function == GATE_UP ? 1 : -1), 1, 6);
        seq.setStep(seq.editStep(), value);
        stepFull = false;
        stepView = StepGate;
      }
      break;
    case VELOCITY_DOWN:
    case VELOCITY_UP:
      if(stepEditing) {
        Sequencer::Step value = *seq.getStep(seq.editStep());
        // Zero remains a valid stored silent value, but the editor uses 1–127.
        const int previous = value.velocity ? value.velocity : 95;
        value.velocity = constrain(previous + (function == VELOCITY_UP ? 1 : -1), 1, 127);
        seq.setStep(seq.editStep(), value);
        stepFull = false;
        stepView = StepVelocity;
      }
      break;
    case CHANCE_DOWN:
    case CHANCE_UP:
    case RATCHET_DOWN:
    case RATCHET_UP:
    case LOCK_CC_DOWN:
    case LOCK_CC_UP:
    case LOCK_VALUE_DOWN:
    case LOCK_VALUE_UP:
    case LOCK_CLEAR:
      if(stepEditing) {
        Sequencer::Step value = *seq.getStep(seq.editStep());
        stepFull = false;
        if(function == CHANCE_DOWN || function == CHANCE_UP) {
          value.chance = constrain(int(value.chance) + (function == CHANCE_UP ? 10 : -10), 0, 100);
          stepView = StepTrigger;
        } else if(function == RATCHET_DOWN || function == RATCHET_UP) {
          const int counts[] = {1, 2, 3, 6};
          int index = 0;
          while(counts[index] != value.ratchet) ++index;
          index = constrain(index + (function == RATCHET_UP ? 1 : -1), 0, 3);
          value.ratchet = counts[index];
          stepView = StepTrigger;
        } else {
          stepView = StepControl;
          if(function == LOCK_CC_DOWN || function == LOCK_CC_UP) {
            editControl = constrain(int(editControl) + (function == LOCK_CC_UP ? 1 : -1), 0, 119);
          } else {
            int slot = 0;
            while(slot < value.lockCount && value.locks[slot].control != editControl) ++slot;
            if(function == LOCK_CLEAR) {
              if(slot < value.lockCount) {
                for(int i = slot; i + 1 < value.lockCount; ++i) value.locks[i] = value.locks[i + 1];
                --value.lockCount;
              }
            } else if(slot < Sequencer::MaxLocks) {
              const int previous = slot < value.lockCount ? value.locks[slot].value : 64;
              value.locks[slot].control = editControl;
              value.locks[slot].value = constrain(previous + (function == LOCK_VALUE_UP ? 1 : -1), 0, 127);
              if(slot == value.lockCount) ++value.lockCount;
            } else stepFull = true;
          }
        }
        seq.setStep(seq.editStep(), value);
      }
      break;
    case EDIT_PREVIOUS:
    case EDIT_NEXT:
    case EDIT_PAGE_BACK:
    case EDIT_PAGE_NEXT:
      if(stepEditing) {
        stepFull = false;
        const int direction = function == EDIT_PREVIOUS || function == EDIT_PAGE_BACK ? -1 : 1;
        if(function == EDIT_PAGE_BACK || function == EDIT_PAGE_NEXT) {
          seq.selectPage((seq.editPage() + direction + seq.pageCount()) % seq.pageCount());
        } else {
          seq.selectStep((seq.editStep() + direction + seq.length()) % seq.length());
        }
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
      if(stepEditing) {
        chordEntry = !chordEntry;
        stepView = StepTrigger;
        stepFull = false;
      }
      break;
    case SEQ_CLEAR:
      if(stepEditing) { seq.clear(seq.editStep()); stepFull = false; }
      else seq.clear();
  }
}

void processMIDIRelease(int function){

  // if function is between 300 and 320, play that respective note
  if(293 < function && function < 314){
    noteKeyboard.release(function - 241);
  }
}

// Custom MIDI callback that prints incoming messages.
struct MyMIDI_Callbacks : MIDI_Callbacks {
  void onSysExMessage(MIDI_Interface &input, SysExMessage message) override {
    if(projectBusy || !message.isCompleteMessage() ||
       !PatternSync::addressed(message.data, message.length)) return;
    updateProjectDirty();
    PatternSync::Status status;
    status.running = clk.running() || seq.playing();
    status.clockSource = uint8_t(clk.source());
    for(int i = 0; i < 4; ++i) status.positions[i] = uint8_t(seq.nextStep(i));
    patternSync.receive(message.data, message.length, &input == &midi_ble ? 0 : 1,
                        millis(), project, status, patternReply);
    if(patternReply.appliedTrack >= 0) {
      seq.replaceTrack(patternReply.appliedTrack, project.tracks[patternReply.appliedTrack]);
      projectPrompt = 0;
      updateProjectDirty();
    }
    if(patternReply.size) {
      input.sendSysEx(patternReply.bytes, uint16_t(patternReply.size), message.cable);
      input.sendNow();
    }
  }
 
  // Callback for channel messages (notes, control change, pitch bend, etc.).
  void onChannelMessage(MIDI_Interface &, ChannelMessage cm) override {
    // Print the message
    Serial << F("Channel message: ")                                        //
           << hex << cm.header << ' ' << cm.data1 << ' ' << cm.data2 << dec //
           << F(" on cable ") << cm.cable.getOneBased() << endl;
  }

  // Callback for real-time messages
  void onRealTimeMessage(MIDI_Interface &input, RealTimeMessage rt) override {
    if(projectBusy || projectTransportPaused) return;
    const Clock::Source source = &input == &midi_ble ? Clock::BLE : Clock::TRS;
    if(clk.source() != source) return;
    switch(rt.getMessageType()){
      case MIDIMessageType::TimingClock:
        switch(clk.externalPulse(micros())) {
          case Clock::Step: seq.step(clk.stepDuration(), clk.pulseScale()); midi_out->sendNow(); break;
          case Clock::Pulse: seq.pulse(); midi_out->sendNow(); break;
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
