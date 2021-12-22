/*
  Author: Jason Reposa
  Created: 12-22-2021
  Wasn't happy with any of the libraries out there, so I made my own. Very simple.
*/

class OurNextion {
  using NextionEvent = void (*)(uint8_t eventType, String eventData);

  private:
    NextionEvent EventCallback;

  public:
    // Event Types
    const uint8_t BUTTON_PRESS = 1;

    // called once in Arduino setup()
    void setup(NextionEvent TheEventCallback) {
      EventCallback = TheEventCallback;
      // RX and TX on the pro micro to communicate between the HMI / Nextion touch screen display
       Serial1.begin(9600);
       // not sure if this will help lag, but you can try a higher BAUD
       // https://forum.arduino.cc/t/nextion-baud-rate-help-please/560081/14
       // but don't forget to turn it back if you don't keep it
//      Serial1.begin(9600);
//      Serial1.print("baud=38400");
//      Serial1.write(0xff);
//      Serial1.write(0xff);
//      Serial1.write(0xff);
//      Serial1.end();
//      delay(1000);
//      Serial1.begin(38400);

      // This makes it much faster.
      Serial1.setTimeout(100);
      // wait 1 second for Serial1
      while (!Serial1 && millis() < 1000);
    }

    // called in the Arduino loop()
    void listen() {
      // We 'print" the command in the press of each button.
      // E.g. `print "fill"` is the filling button

      // To use component IDs instead, use the following template. The ID is in the 3rd read().
      // 65 0 9 0 FF FF FF - Filling Start Button

      String output;
      while (Serial1.available() > 0) {
        output = Serial1.readString();

        // we only currently support button presses
        EventCallback(BUTTON_PRESS, output);
      }
    }

    // Messages going to the Nextion
    void finish() {
      Serial1.write(0xff);
      Serial1.write(0xff);
      Serial1.write(0xff);
    }

    void setVariable(String variable, uint8_t value) {
      Serial1.print(variable);
      Serial1.print(".val=");
      Serial1.print(value);
      finish();
    }

    void setVariable(String variable, uint16_t value) {
      Serial1.print(variable);
      Serial1.print(".val=");
      Serial1.print(value);
      finish();
    }

    void setVariable(String variable, float value) {
      Serial1.print(variable);
      Serial1.print(".val=");
      Serial1.print(value);
      finish();
    }

    void setVariable(String variable, String value) {
      Serial1.print(variable);
      Serial1.print(".val=");
      Serial1.print(value);
      finish();
    }

    void addDataWaveform(uint8_t id, uint8_t channel, uint8_t value) {
      Serial1.print("add ");
      Serial1.print(id);
      Serial1.print(",");
      Serial1.print(channel);
      Serial1.print(",");
      Serial1.print(value);
      finish();
    }
};
