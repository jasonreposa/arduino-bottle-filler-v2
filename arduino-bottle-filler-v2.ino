/*
  Author: Jason Reposa
  Created: 12-12-2021
  Based on arduino-bottling-line source code
*/

#include <EnableInterrupt.h>
#include <util/atomic.h>
#include <EEPROM.h>

// each address is 8 bits / 1 byte away (unless you use avrEeprom, which currently isn't working for me)
#define LOWERING_TIME_EEPROM_ADDRESS 0
#define PURGING_TIME_EEPROM_ADDRESS 1

// Air cylinder solenoid - relay for lowering and raising filling heads
#define AIR_CYLINDER_RELAY_1 2

// CO2 solenoid - relay for purging the Oxygen from the bottles
#define CO2_PURGE_RELAY_1 3

// Beverage solenoid - relay for filling bottles with beverage
#define BEVERAGE_FILLING_RELAY_1 4
#define BEVERAGE_FILLING_RELAY_2 5
#define BEVERAGE_FILLING_RELAY_3 6
#define BEVERAGE_FILLING_RELAY_4 7

// water flow sensors
#define BEVERAGE_SENSOR_1 10
volatile uint32_t pulseCount1;
double beverageVolume1 = 0.0;

#define BEVERAGE_SENSOR_2 16
volatile uint32_t pulseCount2;
double beverageVolume2 = 0.0;

#define BEVERAGE_SENSOR_3 14
volatile uint32_t pulseCount3;
double beverageVolume3 = 0.0;

#define BEVERAGE_SENSOR_4 15
volatile uint32_t pulseCount4;
double beverageVolume4 = 0.0;

uint32_t startTime = 0;
uint32_t loopTime = 0;

uint32_t timeBetweenInterruptCheck = 100; // check flow meter every tenth of a second

// real stuff
// Area of 1/4 diameter (6.35 millimeters) = pi * (6.35/2)^2 = 31.6692174436
// Area of 3/8 diameter (9.52 millimeters) = pi * (9.52/2)^2 = 71.180949708
uint32_t pulsesPerLiter = 1380; // 1/4 DIGITEN - https://digiten.shop/products/digiten-1-4-quick-connect-0-3-10l-min-water-hall-effect-flow-sensor-meter
// uint32_t pulsesPerLiter = 450; // 3/8 DIGITEN - https://smile.amazon.com/ask/questions/TxNFD5HNWLKHEW/ref=ask_dp_dpmw_al_hza - https://www.digiten.shop/products/digiten-g3-8-quick-connect-water-flow-sensor-switch-flowmeter-counter-0-3-10l-min

double ouncesToFill = 355.0;  // i'm comfortable with this for now
double psiAdjustment = 230.0/355.0;  // TODO: overwrite with EEPROM?
// pulsesPerLiter / 60 minutes / 60 seconds * psiAdjustment * 1000.0 (convert to milliseconds?)
//double twelveOunceConstant = 1380.0 / 60.0 / 60.0 * (230.0/355.0) * 1000.0;
double twelveOunceConstant = 248.3568075117;

// BOOL state machine, ha!
bool fillingInProcess = false;
bool fillingHead1Stopped = true;
bool fillingHead2Stopped = true;
bool fillingHead3Stopped = true;
bool fillingHead4Stopped = true;

uint32_t loweringTimeInMillis = 3000;  // overwrite with EEPROM
uint32_t purgingTimeInMillis = 3000;  // overwrite with EEPROM

void setup() {
  // RX and TX on the pro micro to communicate between the HMI / Nextion touch screen display
  Serial1.begin(9600);
  // wait 1 second for Serial1
  while (!Serial1 && millis() < 1000);

  Serial.begin(9600);
  // wait another 1 second for Serial
  // if you wait forever, the program won't start unless it's connected to a serial monitor
  while (!Serial && millis() < 2000);

  Serial.println("------------------------------------");
  Serial.println("SETUP");

  // Lowering Filling Heads TIME - load from EEPROM
  uint8_t loweringTimeValueFromEEPROM = EEPROM.read(LOWERING_TIME_EEPROM_ADDRESS);
  if (loweringTimeValueFromEEPROM && loweringTimeValueFromEEPROM != 255) {
    updateLoweringTime(loweringTimeValueFromEEPROM);
    Serial.print("Found a lowering time value: "); Serial.println(loweringTimeInMillis);
  }

  // Purging Oxygen with CO2 TIME - load from EEPROM
  uint8_t purgingTimeValueFromEEPROM = EEPROM.read(PURGING_TIME_EEPROM_ADDRESS);
  if (purgingTimeValueFromEEPROM && purgingTimeValueFromEEPROM != 255) {
    updatePurgingTime(purgingTimeValueFromEEPROM);
    Serial.print("Found a purging time value: "); Serial.println(purgingTimeInMillis);
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
  checkTouchScreen();

  // if we are filling things, check the filling heads
  if (fillingInProcess) {
    checkFillingHeads();
  }
}


void checkTouchScreen() {
  // 65 0 2 0 FF FF FF - Capping Start Button
  // 65 0 8 0 FF FF FF - Filling Start Button
  while (Serial1.available() > 0) {
    int serial_datum = Serial1.read();
    //    Serial.print(serial_datum);
    //    Serial.print(" - ");
    //    Serial.println(serial_datum, HEX);

    // really unsafe. the data comes over one int at a time in the loop(),
    // so if we ever have a conflict with a number like 101, 255, 0 or a
    // button with the same id, this would need to be rewritten.
    if (serial_datum == 2) {
//      Serial.println("Start Capping");
//      cappingProcess.onStartButtonPress(true);
    } else if (serial_datum == 8) {
      Serial.println("START PRESSED");
      if (fillingInProcess) {
        emergencyStop();
      } else {
        startFillingProcess();
      }
    } else if (serial_datum == 9) {
      // lowering time change UP
      Serial.println("LOWERING TIME INCREASE PRESSED");
      updateAndSaveLoweringTime(loweringTimeInMillis / 100 + 1);
    } else if (serial_datum == 10) {
      // lowering time change UP
      Serial.println("LOWERING TIME DECREASE PRESSED");
      updateAndSaveLoweringTime(loweringTimeInMillis / 100 - 1);
    } else if (serial_datum == 11) {
      // lowering time change UP
      Serial.println("PURGING TIME INCREASE PRESSED");
      updateAndSavePurgingTime(purgingTimeInMillis / 100 + 1);
    } else if (serial_datum == 12) {
      // lowering time change UP
      Serial.println("PURGING TIME DECREASE PRESSED");
      updateAndSavePurgingTime(purgingTimeInMillis / 100 - 1);
    }
  }
}

void startFillingProcess() {
  Serial.println("START FILLING");

  // sometimes a little more pulses come in after closing the solenoid valve, so reset these values.
  beverageVolume1 = 0.0;
  beverageVolume2 = 0.0;
  beverageVolume3 = 0.0;
  beverageVolume4 = 0.0;

  lowerFillingHeads();
  delay(loweringTimeInMillis);  // TODO: replace blocking code?
  purgeCO2();
  delay(purgingTimeInMillis);  // TODO: replace blocking code?
  stopCO2();
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
    fillingInProcess = false;
    raiseFillingHeads();
  }
}

// sensor 1
void checkFlowMeter1(uint32_t currentTime) {
  // An interrupt that changes more than 8-bit needs to be inside an ATOMIC block, otherwise the data could be corrupted.
  // Change this to an unsigned int to store values from 0-255, but make sure you don't fill too fast?
  // More than 255 pulses in 0.1 seconds would be pretty fast, if not impossible? Do the math before removing the ATOMIC block.
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    beverageVolume1 = beverageVolume1 + (pulseCount1 / twelveOunceConstant);
    if (pulseCount1 > 0) {
      Serial.print("Flow From Sensor 1: "); Serial.print(pulseCount1); Serial.print(" : "); Serial.print(beverageVolume1); Serial.println(" mL;");
    }

    // reset counter
    pulseCount1 = 0;
  }

  // stop filling, if we are full
  if (beverageVolume1 >= ouncesToFill) {
    Serial.print("CLOSING VALVE after "); Serial.print((currentTime - startTime) / 1000.0); Serial.println(" seconds;");
    stopFilling1();
  }
}

// sensor 2
void checkFlowMeter2(uint32_t currentTime) {
  // An interrupt that changes more than 8-bit needs to be inside an ATOMIC block, otherwise the data could be corrupted.
  // Change this to an unsigned int to store values from 0-255, but make sure you don't fill too fast?
  // More than 255 pulses in 0.1 seconds would be pretty fast, if not impossible? Do the math before removing the ATOMIC block.
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    beverageVolume2 = beverageVolume2 + (pulseCount2 / twelveOunceConstant);
    if (pulseCount2 > 0) {
      Serial.print("Flow From Sensor 2: "); Serial.print(pulseCount2); Serial.print(" : "); Serial.print(beverageVolume2); Serial.println(" mL;");
    }

    // reset counter
    pulseCount2 = 0;
  }

  // stop filling, if we are full
  if (beverageVolume2 >= ouncesToFill) {
    Serial.print("CLOSING VALVE after "); Serial.print((currentTime - startTime) / 1000.0); Serial.println(" seconds;");
    stopFilling2();
  }
}

// sensor 3
void checkFlowMeter3(uint32_t currentTime) {
  // An interrupt that changes more than 8-bit needs to be inside an ATOMIC block, otherwise the data could be corrupted.
  // Change this to an unsigned int to store values from 0-255, but make sure you don't fill too fast?
  // More than 255 pulses in 0.1 seconds would be pretty fast, if not impossible? Do the math before removing the ATOMIC block.
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    beverageVolume3 = beverageVolume3 + (pulseCount3 / twelveOunceConstant);
    if (pulseCount3 > 0) {
      Serial.print("Flow From Sensor 3: "); Serial.print(pulseCount3); Serial.print(" : "); Serial.print(beverageVolume3); Serial.println(" mL;");
    }

    // reset counter
    pulseCount3 = 0;
  }

  // stop filling, if we are full
  if (beverageVolume3 >= ouncesToFill) {
    Serial.print("CLOSING VALVE after "); Serial.print((currentTime - startTime) / 1000.0); Serial.println(" seconds;");
    stopFilling3();
  }
}

// sensor 4
void checkFlowMeter4(uint32_t currentTime) {
  // An interrupt that changes more than 8-bit needs to be inside an ATOMIC block, otherwise the data could be corrupted.
  // Change this to an unsigned int to store values from 0-255, but make sure you don't fill too fast?
  // More than 255 pulses in 0.1 seconds would be pretty fast, if not impossible? Do the math before removing the ATOMIC block.
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    beverageVolume4 = beverageVolume4 + (pulseCount4 / twelveOunceConstant);
    if (pulseCount4 > 0) {
      Serial.print("Flow From Sensor 4: "); Serial.print(pulseCount4); Serial.print(" : "); Serial.print(beverageVolume4); Serial.println(" mL;");
    }

    // reset counter
    pulseCount4 = 0;
  }

  // stop filling, if we are full
  if (beverageVolume4 >= ouncesToFill) {
    Serial.print("CLOSING VALVE after "); Serial.print((currentTime - startTime) / 1000.0); Serial.println(" seconds;");
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
  fillingInProcess = true;
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
  fillingHead1Stopped = true;
  digitalWrite(BEVERAGE_FILLING_RELAY_1, HIGH);
}
void stopFilling2() {
  fillingHead2Stopped = true;
  digitalWrite(BEVERAGE_FILLING_RELAY_2, HIGH);
}
void stopFilling3() {
  fillingHead3Stopped = true;
  digitalWrite(BEVERAGE_FILLING_RELAY_3, HIGH);
}
void stopFilling4() {
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

  // immediately turn off filling heads
  digitalWrite(BEVERAGE_FILLING_RELAY_1, HIGH);
  digitalWrite(BEVERAGE_FILLING_RELAY_2, HIGH);
  digitalWrite(BEVERAGE_FILLING_RELAY_3, HIGH);
  digitalWrite(BEVERAGE_FILLING_RELAY_4, HIGH);

  // next, just in case turn of CO2 purging if it's running
  digitalWrite(CO2_PURGE_RELAY_1, HIGH);

  // finally, raise filling heads
  digitalWrite(AIR_CYLINDER_RELAY_1, HIGH);
}

void updateLoweringTime(uint8_t newTime) {
  loweringTimeInMillis = newTime * 100;  // convert it from tenths of a second to milliseconds
  // update interface
  HMI_setTimer("x0", newTime);
}

void updateAndSaveLoweringTime(uint8_t newTime) {
  updateLoweringTime(newTime);

  Serial.print("New Lowering Time: "); Serial.println(newTime);
  // write it to memory
  EEPROM.write(LOWERING_TIME_EEPROM_ADDRESS, newTime);
}

void updatePurgingTime(uint8_t newTime) {
  purgingTimeInMillis = newTime * 100;  // convert it from tenths of a second to milliseconds
  // update interface
  HMI_setTimer("x1", newTime);
}

void updateAndSavePurgingTime(uint8_t newTime) {
  updatePurgingTime(newTime);

  Serial.print("New Purging Time: "); Serial.println(newTime);
  // write it to memory
  EEPROM.write(PURGING_TIME_EEPROM_ADDRESS, newTime);
}

// HMI functions
void HMI_setTimer(String hmiVariable, uint32_t newTime) {
  Serial1.print(hmiVariable);
  Serial1.print(".val=");
  Serial1.print(newTime);
  Serial1.write(0xff);
  Serial1.write(0xff);
  Serial1.write(0xff);
}
