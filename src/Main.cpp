#include <Matrix.h>
#include <MIDIMap.h>
#include "KeyInput.h"
#include "KeyboardOutput.h"
#include <PuffinBrain.h>
#include <Control_Surface.h>
//#include "Seq.h"
#include <MIDI.h>
#include <BleKeyboard.h>  
#include <EEPROM.h>
#include <MIDI_Interfaces/BluetoothMIDI_Interface.hpp>
#include "Graphics.h"
#include <Adafruit_NeoPixel.h>
#include <Wire.h>


// Define LED Array

Adafruit_NeoPixel pixels(51, 34, NEO_GRB + NEO_KHZ800);
int LED_DELAY;
bool ledEnabled = false;

bool prod = false;

// Screen refresh counter
int refresh = 0;
int screenSpeed = 10000;
int extClockCount = 0;

//Setup, onces once on boot
void setup() {

  readMacEEPROM(readMIDIModeEEPROM());
  
  Wire.begin();
  boardStart();
  clk.setBPM(120);

  // Start Serial Output for monitoring/debugging
  Serial.begin(115200);
  // Allow for native usb to kick in
  delay(200);

  // Display Setup
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_I2C_ADDR);
  display.setRotation(2);
  //startupAnimation();
  display.clearDisplay();
  mainScreenUpdate();
  Kbd.setBatteryLevel(updateBattLevel());

  // Keyboard Matrix Setup
  //Initialize col output pins and set default state
  for(int i = 0; i < ColCount; i++)
  {
    pinMode(Cols[i],OUTPUT);  
    digitalWrite(Cols[i],LOW);
  }
  //Initialize row input pins
  for(int i = 0; i < RowCount; i++)
  {
    pinMode(Rows[i],INPUT_PULLDOWN);
  }

  // Initalize LEDS
  if (ledEnabled){
    //LED Setup
    pixels.begin();
    pixels.show();
    pixels.setBrightness(3);
    LED_DELAY = 100;
  }

  // check and start MIDI mode if necessary
  if(midiMode){

    setupSequencer();
    // MIDI 
    // Change the name of the BLE device (must be done before initializing it)
    std::string newName = std::string("Puffin MIDI []").insert(13, std::string(1, ('A' + getMacDevice())));
    midi_ble.setName(newName.c_str()); //midi_ble.setName("Puffin MIDI []");
    
    // Manually route MIDI input from the serial interface to the BLE interface,
    // and the MIDI input from the BLE interface to the serial interface
    if(bridgeBLEtoSER) {midi_ser | pipes | midi_ble;}
    // Initialize the MIDI interfaces
    MIDI_Interface::beginAll();
    midi_ble.setCallbacks(callback); // Attach the custom callback
    //midi_ble.begin();
    Serial.println("Started midi_ble");

  // Start Keyboard
  } else {
    //Begin bluetooth keyboard, without keyboard will not appear or connect
    Kbd.begin();
  }

}

// basic test animation for LEDS
int NUM = 51;
void animation1()
{
  for(int i=0;i<NUM;i++)
  {
    pixels.setPixelColor(i, pixels.Color(255,0,0));
    pixels.show();
    delay(100);
  }
  for(int i=NUM;i>=0;i--)
  {
    pixels.setPixelColor(i, pixels.Color(0,255,0));
    pixels.show();
    delay(100);
  }
  for(int i=0;i<NUM;i++)
  {
    pixels.setPixelColor(i, pixels.Color(0,255,255));
    pixels.show();
    delay(100);
  }
  for(int i=NUM;i>=0;i--)
  {
    pixels.setPixelColor(i, pixels.Color(255,255,0));
    pixels.show();
    delay(100);
  }
}

KeyInput keyInput;
// Reuse the exported translation table from the pinned BLE Keyboard library.
// ESP32 flash is memory-mapped; no second ASCII map needs to be maintained.
extern const uint8_t _asciimap[128];
KeyboardOutput keyboardOutput(_asciimap);
int color = 0;
int pixel = 0;
int LED = 0;

void sendTypingReport() {
  KeyReport report = {};
  keyboardOutput.report(report.modifiers, report.keys);
  Kbd.sendReport(&report);
}

void handleKeyEvent(const KeyInput::Event &event) {
  const int action = event.action;

  // Multiple physical switches may map to the same action. Dispatch its
  // first press and last release, not every switch's ownership transition.
  const int held = keyInput.heldCount(action);
  if (event.pressed ? held > 1 : held > 0) return;

  // These commands are shared; all other actions belong to one mode only.
  switch (action) {
    case 0:
    case FUNCTION_SW:
    case FUNCTION_SW2:
    case NULL_CON:
      return;
    case MIDI_MODE:
      if (event.pressed) setMidiMode(!midiMode);
      return;
    case CHANGE_ID0:
    case CHANGE_ID1:
    case CHANGE_ID2:
    case CHANGE_ID3:
      if (event.pressed) {
        panicMIDI();
        storeMacEEPROM(action - CHANGE_ID0);
      }
      return;
    case LED_B0:
      if (event.pressed) pixels.setBrightness(0);
      return;
    case LED_B1:
      if (event.pressed) {
        pixels.setBrightness(5);
        LED_DELAY = 500;
      }
      return;
    case LED_B2:
      if (event.pressed) {
        pixels.setBrightness(40);
        LED_DELAY = 20;
      }
      return;
  }

  if (midiMode) {
    if (event.pressed) processMIDIPress(action);
    else processMIDIRelease(action);
    return;
  }

  switch (action) {
    case NEXT:
      if (event.pressed) Kbd.press(KEY_MEDIA_NEXT_TRACK);
      else Kbd.release(KEY_MEDIA_NEXT_TRACK);
      break;
    case PREV:
      if (event.pressed) Kbd.press(KEY_MEDIA_PREVIOUS_TRACK);
      else Kbd.release(KEY_MEDIA_PREVIOUS_TRACK);
      break;
    case KEY_SS:
      if (event.pressed) {
        // GUI + left Shift + 5; preserve the physical report after the tap.
        keyboardOutput.tapUsage(_asciimap['5'] & 0x7F, 0x0A,
          [](uint8_t modifiers, const uint8_t (&keys)[6]) {
            KeyReport report = {};
            report.modifiers = modifiers;
            for (int i = 0; i < 6; ++i) report.keys[i] = keys[i];
            Kbd.sendReport(&report);
          });
      }
      break;
    default:
      if (event.pressed) {
        if (action == ' ') {
          spaceHit();
          firstSpace = false;
        } else {
          firstSpace = true;
        }
        keyboardOutput.press(action);
      } else {
        keyboardOutput.release(action);
      }
      sendTypingReport();
  }
}

//Loops to iterate through all functions
void loop() {

  //onboardLED(color);

  // Midi Loop Code
  if (getMIDIMode()){
    //if(bleMIDIConnected()){ getClock(); }
    // Continuously poll all interfaces and route the traffic between them
    MIDI_Interface::updateAll();
}

  // Scan a complete matrix each loop; debounce time is measured per switch,
  // not by slowing down MIDI processing or counting loop iterations.
  for (int col = 0; col < ColCount; ++col) {
    digitalWrite(Cols[col], HIGH);
    delayMicroseconds(5);
    const uint32_t now = millis();
    for (int row = 0; row < RowCount; ++row) {
      keyInput.sample(row, col, digitalRead(Rows[row]) == HIGH, now);
    }
    digitalWrite(Cols[col], LOW);
  }
  KeyInput::Event event;
  while (keyInput.nextEvent(midiMode ? MIDIMap : Layout, event)) {
    handleKeyEvent(event);
  }
  
  //Delay so it's not too fast
  if(not midiMode){ delay(1); screenSpeed = 10000; }

  EVERY_N_MILLIS(100){
    mainScreenUpdate();
  }
  // One short OLED transaction, then return to input/MIDI polling.
  displayTransfer.update(Wire, SCREEN_I2C_ADDR, display.getBuffer());

  // Update MIDI clock and output pulse if needed
  //if (clockUpdate()) { midi_ser.sendRealTime(clock); };

  // Update battery level every 30 seconds
  EVERY_N_SECONDS(30){
    Kbd.setBatteryLevel(updateBattLevel());
  }
  

  // Update WPM every 30 seconds
  if(not midiMode){
    EVERY_N_MILLISECONDS(200){
      updateWPM(10);
    }
  }



  if(ledEnabled){
    
    if(LED > LED_DELAY){
      color++;
      if (pixel >= 50){
        pixel = 0;
      } else {
        pixel += 1;
      }
      //pixels.setPixelColor(i, 255,0,0);
      pixels.setPixelColor(pixel, UMS3::colorWheel(color));
      pixels.show();
      LED = 0;
    } else {
      LED += 1;
    }
  }
}
