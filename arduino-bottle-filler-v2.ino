/*
  Author: Jason Reposa
  Created: 12-12-2021
  Based on arduino-bottling-line source code
*/

#include <EnableInterrupt.h>
#include <util/atomic.h>
#include <EEPROM.h>
#include "OurNextion.h"

// Air cylinder solenoid - relay for lowering and raising filling heads
#define AIR_CYLINDER_RELAY_1 2  // relay 1

// CO2 solenoid - relay for purging the Oxygen from the bottles
#define CO2_PURGE_RELAY_1 3  // relay 2

// Beverage solenoid - relay for filling bottles with beverage
#define BEVERAGE_FILLING_RELAY_1 4  // relay 3
#define BEVERAGE_FILLING_RELAY_2 5  // relay 4
#define BEVERAGE_FILLING_RELAY_3 6  // relay 5
#define BEVERAGE_FILLING_RELAY_4 7  // relay 6

// water flow sensors
#define BEVERAGE_SENSOR_1 10
volatile uint8_t pulseCount1;
double beverageVolume1 = 0.0;

#define BEVERAGE_SENSOR_2 16
volatile uint8_t pulseCount2;
double beverageVolume2 = 0.0;

#define BEVERAGE_SENSOR_3 14
volatile uint8_t pulseCount3;
double beverageVolume3 = 0.0;

#define BEVERAGE_SENSOR_4 15
volatile uint8_t pulseCount4;
double beverageVolume4 = 0.0;

uint32_t startTime = 0;
uint32_t loopTime = 0;

uint32_t timeBetweenInterruptCheck = 100; // check flow meter every tenth of a second

// Area of 1/4 diameter (6.35 millimeters) = pi * (6.35/2)^2 = 31.6692174436
// Area of 3/8 diameter (9.52 millimeters) = pi * (9.52/2)^2 = 71.180949708
uint32_t pulsesPerLiter = 1380; // 1/4 DIGITEN - https://digiten.shop/products/digiten-1-4-quick-connect-0-3-10l-min-water-hall-effect-flow-sensor-meter
// uint32_t pulsesPerLiter = 450; // 3/8 DIGITEN - https://smile.amazon.com/ask/questions/TxNFD5HNWLKHEW/ref=ask_dp_dpmw_al_hza - https://www.digiten.shop/products/digiten-g3-8-quick-connect-water-flow-sensor-switch-flowmeter-counter-0-3-10l-min

// I've lost the math somewhere in here, but for now we'll hard code a new number
// double psiAdjustment = 230.0/355.0;  // TODO: overwrite with EEPROM?
// pulsesPerLiter / 60 minutes / 60 seconds * psiAdjustment * 1000.0 (convert to milliseconds?)
//double mLsPerPulseConstant = 1380.0 / 60.0 / 60.0 * (230.0/355.0) * 1000.0;
//double mLsPerPulseConstant = 1380.0 / 60.0 / 60.0;// * 1000.0;
//double mLsPerPulseConstant = 1380.0 / 60.0 / 60.0 * (230.0/355.0);// * 1000.0;
//double mLsPerPulseConstant = 248.3568075117;
// 1380 pulses per liter * 0.355 liters = 489.9 pulses to get to 355mLs

// 12 PSI - the hard coded number, until I figure out the math above again. The math below had to change as well.
double mLsPerPulseConstant = 0.4725;  // how many mL in each pulse?

// BOOL state machine, ha!
bool fillingInProcess = false;
bool fillingHead1Stopped = true;
bool fillingHead2Stopped = true;
bool fillingHead3Stopped = true;
bool fillingHead4Stopped = true;

// each address is 8 bits / 1 byte away (unless you use avrEeprom, which currently isn't working for me)
#define LOWERING_TIME_EEPROM_ADDRESS 0
#define PURGING_TIME_EEPROM_ADDRESS 1
#define PERCENT_ADJUST_EEPROM_ADDRESS 2
#define BEVERAGE_SIZE_EEPROM_ADDRESS 3  // 16-bit

uint16_t beverageSizeInML = 355;  // TODO: editable and overwrite with EEPROM (precision: 5)
uint8_t percentToAdjust = 100;  // editable and overwrite with EEPROM (precision: 1)
uint32_t loweringTimeInMillis = 3000;  // editable and overwrite with EEPROM (precision: 100)
uint32_t purgingTimeInMillis = 3000;  // editable and overwrite with EEPROM (precision: 100)

OurNextion nextion;

void setup() {
  nextion.setup(ProcessNextionData);

  Serial.begin(9600);
  // wait another 1 second for Serial
  // if you wait forever, the program won't start unless it's connected to a serial monitor
  while (!Serial && millis() < 2000);

  Serial.println("------------------------------------");
  Serial.println("SETUP");

  serial_nextion_clear();
  serial_nextion_println("Ready...");

  // Lowering Filling Heads TIME - load from EEPROM
  uint8_t loweringTimeValueFromEEPROM;
  EEPROM.get(LOWERING_TIME_EEPROM_ADDRESS, loweringTimeValueFromEEPROM);
  if (loweringTimeValueFromEEPROM) {
    updateLoweringTime(loweringTimeValueFromEEPROM);
    Serial.print("Found a lowering time value: "); Serial.println(loweringTimeInMillis);
  } else {
    updateAndSaveLoweringTime(loweringTimeInMillis / 100);  // 3 second default
  }

  // Purging Oxygen with CO2 TIME - load from EEPROM
  uint8_t purgingTimeValueFromEEPROM;
  EEPROM.get(PURGING_TIME_EEPROM_ADDRESS, purgingTimeValueFromEEPROM);
  if (purgingTimeValueFromEEPROM) {
    updatePurgingTime(purgingTimeValueFromEEPROM);
    Serial.print("Found a purging time value: "); Serial.println(purgingTimeInMillis);
  } else {
    updateAndSavePurgingTime(purgingTimeInMillis / 100);  // 3 second default
  }

  // Beverage Adjustment Percentage - load from EEPROM
  uint8_t beverageAdjustmentValueFromEEPROM;
  EEPROM.get(PERCENT_ADJUST_EEPROM_ADDRESS, beverageAdjustmentValueFromEEPROM);
  if (beverageAdjustmentValueFromEEPROM) {
    updateBeverageAdjustment(beverageAdjustmentValueFromEEPROM);
    Serial.print("Found a beverage adjustment value: "); Serial.println(percentToAdjust);
  } else {
    updateAndSaveBeverageAdjustment(percentToAdjust); // 100% default
  }

  // Beverage Size in MLs - load from EEPROM
  uint8_t beverageSizeValueFromEEPROM;
  EEPROM.get(BEVERAGE_SIZE_EEPROM_ADDRESS, beverageSizeValueFromEEPROM);
  if (beverageSizeValueFromEEPROM) {
    updateBeverageSize(beverageSizeValueFromEEPROM);
    Serial.print("Found a beverage size value: "); Serial.println(beverageSizeInML);
  } else {
    updateAndSaveBeverageSize(beverageSizeInML / 5);  // 355mL
  }

  // solenoid that opens and closes to raise and lower filling heads
  pinMode(AIR_CYLINDER_RELAY_1, OUTPUT);
  digitalWrite(AIR_CYLINDER_RELAY_1, HIGH);

  // solenoid that opens and closes to allow purging of oxygen from bottle
  pinMode(CO2_PURGE_RELAY_1, OUTPUT);
  digitalWrite(CO2_PURGE_RELAY_1, HIGH);

  // solenoid that opens and closes to allow transfer of beverage to bottle
  pinMode(BEVERAGE_FILLING_RELAY_1, OUTPUT);
  digitalWrite(BEVERAGE_FILLING_RELAY_1, HIGH);
  pinMode(BEVERAGE_FILLING_RELAY_2, OUTPUT);
  digitalWrite(BEVERAGE_FILLING_RELAY_2, HIGH);
  pinMode(BEVERAGE_FILLING_RELAY_3, OUTPUT);
  digitalWrite(BEVERAGE_FILLING_RELAY_3, HIGH);
  pinMode(BEVERAGE_FILLING_RELAY_4, OUTPUT);
  digitalWrite(BEVERAGE_FILLING_RELAY_4, HIGH);

  pinMode(BEVERAGE_SENSOR_1, INPUT);
  enableInterrupt(BEVERAGE_SENSOR_1, FlowPulse1, RISING);
  pinMode(BEVERAGE_SENSOR_2, INPUT);
  enableInterrupt(BEVERAGE_SENSOR_2, FlowPulse2, RISING);
  pinMode(BEVERAGE_SENSOR_3, INPUT);
  enableInterrupt(BEVERAGE_SENSOR_3, FlowPulse3, RISING);
  pinMode(BEVERAGE_SENSOR_4, INPUT);
  enableInterrupt(BEVERAGE_SENSOR_4, FlowPulse4, RISING);

  sei(); // turn on interrupts
}

void loop() {
  // check touch screen for any actions
  nextion.listen();

  // if we are filling things, check the filling heads
  if (fillingInProcess) {
    checkFillingHeads();
  }
}


void startFillingProcess() {
  Serial.println("START FILLING");
  serial_nextion_println("Filling process started...");
  fillingInProcess = true;

  // sometimes a little more pulses come in after closing the solenoid valve, so reset these values.
  beverageVolume1 = 0.0;
  beverageVolume2 = 0.0;
  beverageVolume3 = 0.0;
  beverageVolume4 = 0.0;

  serial_nextion_println("Lowering fill heads...");
  // display purging on waveform, for fun
  lowerFillingHeads();
  uint8_t increment = loweringTimeInMillis / 120;
  for (int i = 0; i <= 120; i = i + 1) {
    nextion.addDataWaveform(3, 0, i);
    delay(increment);  // TODO: replace blocking code?
  }
  purgeCO2();

  serial_nextion_println("Purging Oxygen from bottles...");

  // display purging on waveform, for fun
  increment = purgingTimeInMillis / 120;
  for (int i = 0; i <= 120; i = i + 1) {
    nextion.addDataWaveform(3, 1, i);
    delay(increment);  // TODO: replace blocking code?
  }

  stopCO2();
  serial_nextion_println("Filling bottles...");
  startFilling();

  // record start time to see how long each filling head takes
  startTime = millis();
}

void checkFillingHeads() {
  uint32_t currentTime = millis();
  // checks every 100 milliseconds
  if (currentTime > loopTime + timeBetweenInterruptCheck) {
    loopTime = currentTime;

    // wait to stop the bottle filling when the flow meter detects it has reach 12 ounces
    checkFlowMeter1(currentTime);
    checkFlowMeter2(currentTime);
    checkFlowMeter3(currentTime);
    checkFlowMeter4(currentTime);
  }

  // if we are in the filling process, but all the filling heads have stopped, raise the fill heads
  if (fillingHead1Stopped &&
      fillingHead2Stopped &&
      fillingHead3Stopped &&
      fillingHead4Stopped) {
    // code starts here
    Serial.println("FILLING FINISHED");
    serial_nextion_println("Filling Finished!");
    fillingInProcess = false;
    raiseFillingHeads();
    nextion.gotoPage("ready");
  }
}

// sensor 1
void checkFlowMeter1(uint32_t currentTime) {
  if (fillingHead1Stopped) { return; }

  // only using an int for pulseCount, so no need for an ATOMIC block
  beverageVolume1 = beverageVolume1 + (pulseCount1 * mLsPerPulseConstant);
  if (pulseCount1 > 0) {
    Serial.print("Flow From Sensor 1: "); Serial.print(pulseCount1); Serial.print(" : "); Serial.print(beverageVolume1); Serial.println(" mL;");
  }

  // reset counter
  pulseCount1 = 0;

  // stop filling, if we are full
  if (beverageVolume1 >= beverageSizeInML) {
    Serial.print("CLOSING VALVE after "); Serial.print((currentTime - startTime) / 1000.0); Serial.println(" seconds;");
    serial_nextion_println("Filling Head 1 Complete");
    stopFilling1();
  }
}

// sensor 2
void checkFlowMeter2(uint32_t currentTime) {
  if (fillingHead2Stopped) { return; }

  // only using an int for pulseCount, so no need for an ATOMIC block
  beverageVolume2 = beverageVolume2 + (pulseCount2 * mLsPerPulseConstant);
  if (pulseCount2 > 0) {
    Serial.print("Flow From Sensor 2: "); Serial.print(pulseCount2); Serial.print(" : "); Serial.print(beverageVolume2); Serial.println(" mL;");
  }

  // reset counter
  pulseCount2 = 0;

  // stop filling, if we are full
  if (beverageVolume2 >= beverageSizeInML) {
    Serial.print("CLOSING VALVE after "); Serial.print((currentTime - startTime) / 1000.0); Serial.println(" seconds;");
    serial_nextion_println("Filling Head 2 Complete");
    stopFilling2();
  }
}

// sensor 3
void checkFlowMeter3(uint32_t currentTime) {
  if (fillingHead3Stopped) { return; }

  // only using an int for pulseCount, so no need for an ATOMIC block
  beverageVolume3 = beverageVolume3 + (pulseCount3 * mLsPerPulseConstant);
  if (pulseCount3 > 0) {
    Serial.print("Flow From Sensor 3: "); Serial.print(pulseCount3); Serial.print(" : "); Serial.print(beverageVolume3); Serial.println(" mL;");
  }

  // reset counter
  pulseCount3 = 0;

  // stop filling, if we are full
  if (beverageVolume3 >= beverageSizeInML) {
    Serial.print("CLOSING VALVE after "); Serial.print((currentTime - startTime) / 1000.0); Serial.println(" seconds;");
    serial_nextion_println("Filling Head 3 Complete");
    stopFilling3();
  }
}

// sensor 4
void checkFlowMeter4(uint32_t currentTime) {
  if (fillingHead4Stopped) { return; }

  // only using an int for pulseCount, so no need for an ATOMIC block
  beverageVolume4 = beverageVolume4 + (pulseCount4 * mLsPerPulseConstant);
  if (pulseCount4 > 0) {
    Serial.print("Flow From Sensor 4: "); Serial.print(pulseCount4); Serial.print(" : "); Serial.print(beverageVolume4); Serial.println(" mL;");
  }

  // reset counter
  pulseCount4 = 0;

  // stop filling, if we are full
  if (beverageVolume4 >= beverageSizeInML) {
    Serial.print("CLOSING VALVE after "); Serial.print((currentTime - startTime) / 1000.0); Serial.println(" seconds;");
    serial_nextion_println("Filling Head 4 Complete");
    stopFilling4();
  }
}

void FlowPulse1() {
  pulseCount1++;
}

void FlowPulse2() {
  pulseCount2++;
}

void FlowPulse3() {
  pulseCount3++;
}

void FlowPulse4() {
  pulseCount4++;
}

// STEP 1
// The first step in the bottle filling process is to lower the filling heads. The air cylinder gets triggered to extend down.
void lowerFillingHeads() {
  digitalWrite(AIR_CYLINDER_RELAY_1, LOW);
}

// STEP 2
// After the filling heads have been lowered, start releasing CO2 to purge oxygen from the bottles
void purgeCO2() {
  digitalWrite(CO2_PURGE_RELAY_1, LOW);
}

void stopCO2() {
  digitalWrite(CO2_PURGE_RELAY_1, HIGH);
}

// STEP 3
// After the oxygen has been purged, begin filling bottles with beverage
void startFilling() {
  fillingHead1Stopped = false;
  fillingHead2Stopped = false;
  fillingHead3Stopped = false;
  fillingHead4Stopped = false;
  digitalWrite(BEVERAGE_FILLING_RELAY_1, LOW);
  digitalWrite(BEVERAGE_FILLING_RELAY_2, LOW);
  digitalWrite(BEVERAGE_FILLING_RELAY_3, LOW);
  digitalWrite(BEVERAGE_FILLING_RELAY_4, LOW);
}

// STEP 4
// After the bottle is filled with beverage, stop filling
void stopFilling1() {
  beverageVolume1 = 0.0;
  fillingHead1Stopped = true;
  digitalWrite(BEVERAGE_FILLING_RELAY_1, HIGH);
}
void stopFilling2() {
  beverageVolume2 = 0.0;
  fillingHead2Stopped = true;
  digitalWrite(BEVERAGE_FILLING_RELAY_2, HIGH);
}
void stopFilling3() {
  beverageVolume3 = 0.0;
  fillingHead3Stopped = true;
  digitalWrite(BEVERAGE_FILLING_RELAY_3, HIGH);
}
void stopFilling4() {
  beverageVolume4 = 0.0;
  fillingHead4Stopped = true;
  digitalWrite(BEVERAGE_FILLING_RELAY_4, HIGH);
}

// STEP 5
// After filling is complete, raise the filling heads using the air cylinder
void raiseFillingHeads() {
  digitalWrite(AIR_CYLINDER_RELAY_1, HIGH);
}

// if we double hit the start button, that triggers an emergency stop
void emergencyStop() {
  Serial.println("EMERGENCY STOP PRESSED");
  serial_nextion_println("EMERGENCY STOP");

  // immediately turn off filling heads
  digitalWrite(BEVERAGE_FILLING_RELAY_1, HIGH);
  digitalWrite(BEVERAGE_FILLING_RELAY_2, HIGH);
  digitalWrite(BEVERAGE_FILLING_RELAY_3, HIGH);
  digitalWrite(BEVERAGE_FILLING_RELAY_4, HIGH);

  // next, just in case turn of CO2 purging if it's running
  digitalWrite(CO2_PURGE_RELAY_1, HIGH);

  // finally, raise filling heads
  digitalWrite(AIR_CYLINDER_RELAY_1, HIGH);
  fillingInProcess = false;
}

// Nextion Stuff

void updateLoweringTime(uint8_t newTime) {
  loweringTimeInMillis = newTime * 100;  // convert it from tenths of a second to milliseconds
  // update interface
  nextion.setVariable("ready.x0", newTime);
}

void updateAndSaveLoweringTime(uint8_t newTime) {
  updateLoweringTime(newTime);

  Serial.print("New Lowering Time: "); Serial.println(newTime);
  // write it to memory
  EEPROM.put(LOWERING_TIME_EEPROM_ADDRESS, newTime);
}

void updatePurgingTime(uint8_t newTime) {
  purgingTimeInMillis = newTime * 100;  // convert it from tenths of a second to milliseconds
  // update interface
  nextion.setVariable("ready.x1", newTime);
}

void updateAndSavePurgingTime(uint8_t newTime) {
  updatePurgingTime(newTime);

  Serial.print("New Purging Time: "); Serial.println(newTime);
  // write it to memory
  EEPROM.put(PURGING_TIME_EEPROM_ADDRESS, newTime);
}

void updateBeverageAdjustment(uint8_t newAdjustment) {
  percentToAdjust = newAdjustment;  // convert it from tenths of a second to milliseconds
  // update interface
  nextion.setVariable("ready.n0", newAdjustment);
}

void updateAndSaveBeverageAdjustment(uint8_t newAdjustment) {
  updateBeverageAdjustment(newAdjustment);

  Serial.print("New Beverage Adjustment: "); Serial.println(newAdjustment);
  // write it to memory
  EEPROM.put(PERCENT_ADJUST_EEPROM_ADDRESS, newAdjustment);
}

void updateBeverageSize(uint16_t newSize) {
  beverageSizeInML = newSize * 5;
  // update interface
  nextion.setVariable("ready.n1", newSize * 5);
}

void updateAndSaveBeverageSize(uint16_t newSize) {
  updateBeverageSize(newSize);

  Serial.print("New Beverage Size: "); Serial.println(newSize);
  // write it to memory
  EEPROM.put(BEVERAGE_SIZE_EEPROM_ADDRESS, newSize);
}

// Nextion Variables / Buttons
// x0 - Lowering Time Float
// x1 - CO2 Purge Time Float
// n0 - Percent Adjustment for Beverage (starts at 100%)
// t7 - Text Output / Messages back to user
// s0 - 4 channel graph/waveform
// fill - `print "fill"` - Fill Button
// stop - `print "stop"` - Stop Button
// b1 - `print "more lowering"` - More Time for Lowering - increases x0
// b2 - `print "less lowering"` - Less Time for Lowering - decreases x0
// b3 - `print "more purge"` - More Time for CO2 Purge - increases x1
// b4 - `print "less purge"` - Less Time for CO2 Purge - decreases x1
// b5 - `print "more bev"` - More Adjustment for Beverage - increases n0
// b6 - `print "less bev"` - Less Adjustment for Beverage - decreases n0
// b9 - `print "cip"` - CIP


void ProcessNextionData(uint8_t eventType, String eventData) {
  if (nextion.BUTTON_PRESS == eventType) {
    if (eventData == "fill") {;
      // if we are already in the filling process, do an emergency stop? (or ignore it?)
      if (fillingInProcess) {
        serial_nextion_println("Filling already in progress...");
          // emergencyStop();
        } else {
          startFillingProcess();
        }
    } else if (eventData == "stop") {
      emergencyStop();
    } else if (eventData == "cip") {
      // CIP
    } else if (eventData == "more lowering") {
      Serial.println("LOWERING TIME INCREASE PRESSED");
      updateAndSaveLoweringTime(loweringTimeInMillis / 100 + 1);
    } else if (eventData == "less lowering") {
      Serial.println("LOWERING TIME DECREASE PRESSED");
      updateAndSaveLoweringTime(loweringTimeInMillis / 100 - 1);
    } else if (eventData == "more purge") {
      Serial.println("PURGING TIME INCREASE PRESSED");
      updateAndSavePurgingTime(purgingTimeInMillis / 100 + 1);
    } else if (eventData == "less purge") {
      Serial.println("PURGING TIME DECREASE PRESSED");
      updateAndSavePurgingTime(purgingTimeInMillis / 100 - 1);
    } else if (eventData == "more adj") {
      Serial.println("MORE ADJ INCREASE PRESSED");
      updateAndSaveBeverageAdjustment(percentToAdjust + 1);
    } else if (eventData == "less adj") {
      Serial.println("LESS ADJ DECREASE PRESSED");
      updateAndSaveBeverageAdjustment(percentToAdjust - 1);
    } else if (eventData == "more bev") {
      Serial.println("MORE BEV INCREASE PRESSED");
      updateAndSaveBeverageSize(beverageSizeInML / 5 + 1);
    } else if (eventData == "less bev") {
      Serial.println("LESS BEV DECREASE PRESSED");
      updateAndSaveBeverageSize(beverageSizeInML / 5 - 1);
    } else {
      serial_nextion_print("Unknown Button Press: "); serial_nextion_println(eventData);
    }
  }
}

String nextionSerialPrint = "";
uint8_t nextionSerialPrintLineCount = 0;
void serial_nextion_print(String printThis) {
  // Serial.print(printThis);
  nextionSerialPrint += printThis;

  // if it's too long, drop the first line.
  if (nextionSerialPrintLineCount > 5) {
    nextionSerialPrint.remove(0, nextionSerialPrint.indexOf("\\r")+2);
    // keep it under 255
    if (nextionSerialPrintLineCount > 200) { nextionSerialPrintLineCount = 7; }
  }
  nextion.setText("filling.t7", nextionSerialPrint);
}
void serial_nextion_println(String printThis) {
  serial_nextion_print(printThis + "\\r");
  nextionSerialPrintLineCount = nextionSerialPrintLineCount + 1;
}
void serial_nextion_clear() {
  nextion.setText("filling.t7", "");
}
