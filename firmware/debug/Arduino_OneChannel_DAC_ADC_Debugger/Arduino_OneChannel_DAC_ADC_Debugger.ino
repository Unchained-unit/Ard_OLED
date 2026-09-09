#include <Wire.h>
#include <Adafruit_MCP4728.h>
#include <Adafruit_ADS1X15.h>
#include <stdlib.h>
#include <string.h>

/*
  NMSE OLEDer: one-channel manual DAC/ADC debugger

  Direct I2C wiring (default):
    Arduino A4/SDA -> MCP4728 SDA + ADS1115 SDA
    Arduino A5/SCL -> MCP4728 SCL + ADS1115 SCL
    Arduino 5V     -> MCP4728 VCC + ADS1115 VDD
    Arduino GND    -> common GND

  USB debug with MAX485 still connected:
    MAX485 DE and /RE -> D2
    Disconnect RS-485 A/B during USB debugging.

  Serial Monitor: 9600 baud. Any line ending is accepted.

  Main commands:
    STATUS
    SCAN
    ARM
    DAC A 0.200
    ADC 0 20
    ADCALL 20
    GAIN 16
    MONITOR 1000
    STOP
    SWEEP A 0 0.0 1.0 0.1 300
    ZERO
*/

#define SERIAL_BAUD           9600
#define RS485_DIR_PIN         2
#define DEBUG_OVER_USB        1

#define USE_PCA9548A          0
#define PCA_ADDR              0x70
#define ADC_PCA_CHANNEL       0
#define DAC_PCA_CHANNEL       1

#define ADS_ADDR              0x48
#define DAC_ADDR              0x60
#define I2C_TIMEOUT_US        25000UL
#define ARM_TIMEOUT_MS        60000UL

Adafruit_ADS1115 ads;
Adafruit_MCP4728 dac;

float dacReferenceV = 5.000f;

// ===== ONE-CHANNEL ANALOG CALIBRATION =====
// These values may be edited here for permanent defaults or changed at runtime
// with SHUNT, IGAIN, USCALE, UZERO, IZERO, PSCALE and PZERO commands.
float shuntOhms = 25.0f;
float currentSenseGain = 10.0f;
float voltageFeedbackScale = 3.0f;  // U_sample = U_ADC * 3 for 30k/10k subtractor
float voltageZeroV = 0.0f;          // ADC voltage at real U=0; subtracted before scale
float currentZeroV = 0.0f;          // ADC voltage at real I=0; subtracted before conversion
float photoScale = 1.0f;
float photoZeroV = 0.0f;

uint8_t voltageAdcChannel = 0;      // ADS1115 A0: differential voltage amplifier
uint8_t currentAdcChannel = 1;      // ADS1115 A1: shunt amplifier output
uint8_t photoAdcChannel = 2;        // ADS1115 A2: photo channel
uint8_t voltageDacChannel = 0;      // MCP4728 A: voltage setpoint
uint8_t currentDacChannel = 1;      // MCP4728 B: current setpoint

adsGain_t selectedGain = GAIN_TWOTHIRDS;
bool adsReady = false;
bool dacReady = false;
bool outputArmed = false;
unsigned long armedAtMs = 0;

bool monitorEnabled = false;
unsigned long monitorIntervalMs = 1000;
unsigned long monitorLastMs = 0;

char commandLine[96];
uint8_t commandLength = 0;
unsigned long lastByteMs = 0;

void setUsbDebugDirection() {
  pinMode(RS485_DIR_PIN, OUTPUT);
#if DEBUG_OVER_USB
  // DE=1 and /RE=1: MAX485 RO is high impedance and cannot fight USB-UART RX.
  digitalWrite(RS485_DIR_PIN, HIGH);
#else
  digitalWrite(RS485_DIR_PIN, LOW);
#endif
}

bool selectPca(uint8_t channel) {
#if USE_PCA9548A
  if (channel > 7) return false;
  Wire.beginTransmission(PCA_ADDR);
  Wire.write((uint8_t)(1U << channel));
  byte error = Wire.endTransmission();
  delay(2);
  return error == 0;
#else
  (void)channel;
  return true;
#endif
}

bool addressPresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void printHex(uint8_t address) {
  Serial.print(F("0x"));
  if (address < 16) Serial.print('0');
  Serial.print(address, HEX);
}

void scanI2c() {
  Serial.println(F("I2C_SCAN,BEGIN"));
  uint8_t found = 0;
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      found++;
      Serial.print(F("I2C_FOUND,"));
      printHex(address);
      Serial.println();
    }
  }
  Serial.print(F("I2C_SCAN,END,COUNT="));
  Serial.println(found);
}

bool initAds() {
  if (!selectPca(ADC_PCA_CHANNEL)) return false;
  if (!addressPresent(ADS_ADDR)) return false;
  adsReady = ads.begin(ADS_ADDR);
  if (adsReady) ads.setGain(selectedGain);
  return adsReady;
}

bool initDac() {
  if (!selectPca(DAC_PCA_CHANNEL)) return false;
  if (!addressPresent(DAC_ADDR)) return false;
  dacReady = dac.begin(DAC_ADDR);
  return dacReady;
}

void printGain() {
  if (selectedGain == GAIN_TWOTHIRDS) Serial.print(F("2/3"));
  else if (selectedGain == GAIN_ONE) Serial.print(F("1"));
  else if (selectedGain == GAIN_TWO) Serial.print(F("2"));
  else if (selectedGain == GAIN_FOUR) Serial.print(F("4"));
  else if (selectedGain == GAIN_EIGHT) Serial.print(F("8"));
  else if (selectedGain == GAIN_SIXTEEN) Serial.print(F("16"));
}

float gainRangeV() {
  if (selectedGain == GAIN_TWOTHIRDS) return 6.144f;
  if (selectedGain == GAIN_ONE) return 4.096f;
  if (selectedGain == GAIN_TWO) return 2.048f;
  if (selectedGain == GAIN_FOUR) return 1.024f;
  if (selectedGain == GAIN_EIGHT) return 0.512f;
  return 0.256f;
}

bool setGainFromText(const char *text) {
  if (!text) return false;
  if (!strcmp(text, "2/3") || !strcmp(text, "0") || !strcmp(text, "6.144")) selectedGain = GAIN_TWOTHIRDS;
  else if (!strcmp(text, "1") || !strcmp(text, "4.096")) selectedGain = GAIN_ONE;
  else if (!strcmp(text, "2") || !strcmp(text, "2.048")) selectedGain = GAIN_TWO;
  else if (!strcmp(text, "4") || !strcmp(text, "1.024")) selectedGain = GAIN_FOUR;
  else if (!strcmp(text, "8") || !strcmp(text, "0.512")) selectedGain = GAIN_EIGHT;
  else if (!strcmp(text, "16") || !strcmp(text, "0.256")) selectedGain = GAIN_SIXTEEN;
  else return false;
  if (adsReady) ads.setGain(selectedGain);
  return true;
}

int8_t parseDacChannel(const char *text) {
  if (!text || text[1] != '\0') return -1;
  if (text[0] == 'A' || text[0] == '0') return 0;
  if (text[0] == 'B' || text[0] == '1') return 1;
  if (text[0] == 'C' || text[0] == '2') return 2;
  if (text[0] == 'D' || text[0] == '3') return 3;
  return -1;
}

void setDacCode(uint8_t channel, uint16_t code) {
  if (channel == 0) dac.setChannelValue(MCP4728_CHANNEL_A, code);
  else if (channel == 1) dac.setChannelValue(MCP4728_CHANNEL_B, code);
  else if (channel == 2) dac.setChannelValue(MCP4728_CHANNEL_C, code);
  else if (channel == 3) dac.setChannelValue(MCP4728_CHANNEL_D, code);
}

bool writeDacVoltage(uint8_t channel, float volts, bool printResult) {
  if (channel > 3 || volts < 0.0f || volts > dacReferenceV) return false;
  if (!selectPca(DAC_PCA_CHANNEL)) return false;
  if (!dacReady && !initDac()) return false;

  uint16_t code = (uint16_t)(volts / dacReferenceV * 4095.0f + 0.5f);
  if (code > 4095) code = 4095;
  setDacCode(channel, code);
  delay(2);

  if (printResult) {
    Serial.print(F("DAC_SET,CH="));
    Serial.print((char)('A' + channel));
    Serial.print(F(",REQUEST_V="));
    Serial.print(volts, 6);
    Serial.print(F(",CODE="));
    Serial.print(code);
    Serial.print(F(",IDEAL_V="));
    Serial.println((float)code * dacReferenceV / 4095.0f, 6);
  }
  return true;
}

void zeroAllDac() {
  if (selectPca(DAC_PCA_CHANNEL) && (dacReady || initDac())) {
    for (uint8_t channel = 0; channel < 4; channel++) setDacCode(channel, 0);
  }
  outputArmed = false;
  Serial.println(F("DAC_ZERO,ALL_CHANNELS,LOCKED"));
}

struct Measurement {
  bool ok;
  float meanV;
  float minV;
  float maxV;
  int16_t minCode;
  int16_t maxCode;
};

Measurement measureAdc(uint8_t channel, uint8_t count);

Measurement measureAdc(uint8_t channel, uint8_t count) {
  Measurement m;
  m.ok = false;
  m.meanV = 0;
  m.minV = 1000;
  m.maxV = -1000;
  m.minCode = 32767;
  m.maxCode = -32768;
  if (channel > 3) return m;
  if (!selectPca(ADC_PCA_CHANNEL)) return m;
  if (!adsReady && !initAds()) return m;
  if (count < 1) count = 1;
  if (count > 100) count = 100;

  ads.setGain(selectedGain);
  float sum = 0;
  for (uint8_t i = 0; i < count; i++) {
    int16_t code = ads.readADC_SingleEnded(channel);
    float volts = ads.computeVolts(code);
    sum += volts;
    if (volts < m.minV) m.minV = volts;
    if (volts > m.maxV) m.maxV = volts;
    if (code < m.minCode) m.minCode = code;
    if (code > m.maxCode) m.maxCode = code;
  }
  m.meanV = sum / count;
  m.ok = true;
  return m;
}

void printMeasurement(uint8_t channel, uint8_t count) {
  Measurement m = measureAdc(channel, count);
  Serial.print(F("ADC,CH=A"));
  Serial.print(channel);
  if (!m.ok) {
    Serial.println(F(",FAIL"));
    return;
  }
  Serial.print(F(",GAIN="));
  printGain();
  Serial.print(F(",RANGE_V="));
  Serial.print(gainRangeV(), 3);
  Serial.print(F(",N="));
  Serial.print(count);
  Serial.print(F(",MEAN_V="));
  Serial.print(m.meanV, 7);
  Serial.print(F(",MIN_V="));
  Serial.print(m.minV, 7);
  Serial.print(F(",MAX_V="));
  Serial.print(m.maxV, 7);
  Serial.print(F(",P2P_MV="));
  Serial.print((m.maxV - m.minV) * 1000.0f, 4);
  Serial.print(F(",CODE_MIN="));
  Serial.print(m.minCode);
  Serial.print(F(",CODE_MAX="));
  Serial.print(m.maxCode);
  Serial.print(F(",SAT="));
  Serial.println(m.maxCode >= 32760 ? 1 : 0);
}

void readAllAdc(uint8_t count) {
  for (uint8_t channel = 0; channel < 4; channel++) printMeasurement(channel, count);
  Serial.println(F("ADC_ALL,END"));
}

void printCalibration() {
  Serial.print(F("CAL,SHUNT_OHM=")); Serial.print(shuntOhms, 6);
  Serial.print(F(",I_GAIN=")); Serial.print(currentSenseGain, 6);
  Serial.print(F(",U_SCALE=")); Serial.print(voltageFeedbackScale, 6);
  Serial.print(F(",U_ZERO_V=")); Serial.print(voltageZeroV, 7);
  Serial.print(F(",I_ZERO_V=")); Serial.print(currentZeroV, 7);
  Serial.print(F(",PHOTO_SCALE=")); Serial.print(photoScale, 6);
  Serial.print(F(",PHOTO_ZERO_V=")); Serial.print(photoZeroV, 7);
  Serial.print(F(",ADC_MAP=U:A")); Serial.print(voltageAdcChannel);
  Serial.print(F(";I:A")); Serial.print(currentAdcChannel);
  Serial.print(F(";P:A")); Serial.print(photoAdcChannel);
  Serial.print(F(",DAC_MAP=U:")); Serial.print((char)('A' + voltageDacChannel));
  Serial.print(F(";I:")); Serial.println((char)('A' + currentDacChannel));
}

void printPhysicalMeasurement(uint8_t count) {
  Measurement u = measureAdc(voltageAdcChannel, count);
  Measurement i = measureAdc(currentAdcChannel, count);
  Measurement p = measureAdc(photoAdcChannel, count);
  if (!u.ok || !i.ok || !p.ok) {
    Serial.println(F("MEASURE,FAIL"));
    return;
  }

  float sampleVoltageV = (u.meanV - voltageZeroV) * voltageFeedbackScale;
  float sampleCurrentMa = (i.meanV - currentZeroV) / currentSenseGain / shuntOhms * 1000.0f;
  float brightness = (p.meanV - photoZeroV) * photoScale;

  Serial.print(F("MEASURE,N=")); Serial.print(count);
  Serial.print(F(",U_ADC_V=")); Serial.print(u.meanV, 7);
  Serial.print(F(",U_SAMPLE_V=")); Serial.print(sampleVoltageV, 7);
  Serial.print(F(",I_ADC_V=")); Serial.print(i.meanV, 7);
  Serial.print(F(",I_SAMPLE_MA=")); Serial.print(sampleCurrentMa, 7);
  Serial.print(F(",PHOTO_ADC_V=")); Serial.print(p.meanV, 7);
  Serial.print(F(",BRIGHTNESS=")); Serial.println(brightness, 7);
}

bool checkOutputUnlocked() {
  if (!outputArmed || millis() - armedAtMs > ARM_TIMEOUT_MS) {
    outputArmed = false;
    Serial.println(F("ERR,LOCKED,USE_ARM"));
    return false;
  }
  return true;
}

void setPhysicalVoltage(float requestedV) {
  if (!checkOutputUnlocked()) return;
  if (voltageFeedbackScale <= 0.0f || requestedV < 0.0f) {
    Serial.println(F("ERR,BAD_U_SCALE_OR_VOLTAGE"));
    return;
  }
  float dacV = requestedV / voltageFeedbackScale + voltageZeroV;
  if (!writeDacVoltage(voltageDacChannel, dacV, false)) {
    Serial.println(F("ERR,SETU_OUT_OF_DAC_RANGE"));
    return;
  }
  Serial.print(F("SETU,REQUEST_SAMPLE_V=")); Serial.print(requestedV, 6);
  Serial.print(F(",DAC_CH=")); Serial.print((char)('A' + voltageDacChannel));
  Serial.print(F(",DAC_SET_V=")); Serial.println(dacV, 6);
}

void setPhysicalCurrent(float requestedMa) {
  if (!checkOutputUnlocked()) return;
  if (shuntOhms <= 0.0f || currentSenseGain <= 0.0f || requestedMa < 0.0f) {
    Serial.println(F("ERR,BAD_SHUNT_GAIN_OR_CURRENT"));
    return;
  }
  float dacV = requestedMa / 1000.0f * shuntOhms * currentSenseGain + currentZeroV;
  if (!writeDacVoltage(currentDacChannel, dacV, false)) {
    Serial.println(F("ERR,SETI_OUT_OF_DAC_RANGE"));
    return;
  }
  Serial.print(F("SETI,REQUEST_MA=")); Serial.print(requestedMa, 6);
  Serial.print(F(",DAC_CH=")); Serial.print((char)('A' + currentDacChannel));
  Serial.print(F(",DAC_SET_V=")); Serial.println(dacV, 6);
}

void printStatus() {
  adsReady = initAds();
  dacReady = initDac();
  Serial.print(F("STATUS,ADS1115="));
  Serial.print(adsReady ? F("OK") : F("FAIL"));
  Serial.print(F(",MCP4728="));
  Serial.print(dacReady ? F("OK") : F("FAIL"));
  Serial.print(F(",GAIN="));
  printGain();
  Serial.print(F(",ADC_RANGE_V="));
  Serial.print(gainRangeV(), 3);
  Serial.print(F(",DAC_VREF="));
  Serial.print(dacReferenceV, 4);
  Serial.print(F(",ARMED="));
  Serial.println(outputArmed ? 1 : 0);
  printCalibration();
}

void runSweep(uint8_t dacChannel, uint8_t adcChannel, float startV,
              float stopV, float stepV, unsigned long settleMs) {
  if (!outputArmed || millis() - armedAtMs > ARM_TIMEOUT_MS) {
    outputArmed = false;
    Serial.println(F("ERR,LOCKED,USE_ARM"));
    return;
  }
  if (dacChannel > 3 || adcChannel > 3 || startV < 0 || stopV > dacReferenceV ||
      stopV < startV || stepV < 0.001f || settleMs < 10 || settleMs > 10000UL) {
    Serial.println(F("ERR,BAD_SWEEP_ARGUMENT"));
    return;
  }
  uint16_t points = (uint16_t)((stopV - startV) / stepV + 1.5f);
  if (points < 1 || points > 100) {
    Serial.println(F("ERR,MAX_100_POINTS"));
    return;
  }

  Serial.println(F("SWEEP,BEGIN"));
  Serial.println(F("SWEEP_COLUMNS,DAC_CH,ADC_CH,DAC_SET_V,ADC_MEAN_V,ADC_MIN_V,ADC_MAX_V,ADC_P2P_MV"));
  for (uint16_t point = 0; point < points; point++) {
    float setV = startV + point * stepV;
    if (setV > stopV) setV = stopV;
    if (!writeDacVoltage(dacChannel, setV, false)) {
      Serial.println(F("ERR,DAC_WRITE"));
      break;
    }
    delay(settleMs);
    Measurement m = measureAdc(adcChannel, 10);
    Serial.print(F("SWEEP_POINT,"));
    Serial.print((char)('A' + dacChannel));
    Serial.print(F(",A"));
    Serial.print(adcChannel);
    Serial.print(',');
    Serial.print(setV, 6);
    if (m.ok) {
      Serial.print(','); Serial.print(m.meanV, 7);
      Serial.print(','); Serial.print(m.minV, 7);
      Serial.print(','); Serial.print(m.maxV, 7);
      Serial.print(','); Serial.println((m.maxV - m.minV) * 1000.0f, 4);
    } else Serial.println(F(",FAIL,FAIL,FAIL,FAIL"));
  }
  zeroAllDac();
  Serial.println(F("SWEEP,END,DAC_ZEROED"));
}

void printHelp() {
  Serial.println(F("NMSE ONE-CHANNEL DAC/ADC DEBUGGER"));
  Serial.println(F("STATUS                 check ADS1115 and MCP4728"));
  Serial.println(F("SCAN                   I2C scanner"));
  Serial.println(F("GAIN 2/3|1|2|4|8|16   select ADS1115 gain"));
  Serial.println(F("VREF 5.000             measured MCP4728 supply/reference"));
  Serial.println(F("ARM                    unlock output for 60 seconds"));
  Serial.println(F("DAC A 0.200            set DAC A to 0.2 V"));
  Serial.println(F("ADC 0 20               read ADS A0 twenty times"));
  Serial.println(F("ADCALL 20              read every ADS input"));
  Serial.println(F("MEASURE 20             converted U, I and brightness"));
  Serial.println(F("CAL                    show shunt, gains, offsets and map"));
  Serial.println(F("SHUNT 25               shunt resistance in ohms"));
  Serial.println(F("IGAIN 10               current registration amplifier gain"));
  Serial.println(F("USCALE 3               differential voltage scale"));
  Serial.println(F("UZERO 0.0 | IZERO 0.0  zero offsets in ADC volts"));
  Serial.println(F("PSCALE 1 | PZERO 0     photo scale and zero"));
  Serial.println(F("MAP 0 1 2              ADS channels: U I PHOTO"));
  Serial.println(F("DACMAP A B             DAC channels: U setpoint, I setpoint"));
  Serial.println(F("SETU 1.0               set real sample voltage using USCALE"));
  Serial.println(F("SETI 10.0              set real current using shunt and IGAIN"));
  Serial.println(F("MONITOR 1000           read every input each second"));
  Serial.println(F("STOP                   stop monitor"));
  Serial.println(F("SWEEP A 0 0 1 0.1 300 DAC A -> ADC A0"));
  Serial.println(F("ZERO                   zero and lock all DAC outputs"));
}

void normalizeCommand(char *line) {
  for (uint8_t i = 0; line[i]; i++) {
    if (line[i] == ',' || line[i] == '\t') line[i] = ' ';
    if (line[i] >= 'a' && line[i] <= 'z') line[i] -= 32;
  }
}

void processCommand(char *line) {
  normalizeCommand(line);
  char *token[8];
  for (uint8_t i = 0; i < 8; i++) token[i] = strtok(i == 0 ? line : NULL, " ");
  if (!token[0]) return;

  if (!strcmp(token[0], "PING")) Serial.println(F("OK,PONG,ONE_CHANNEL_DEBUGGER"));
  else if (!strcmp(token[0], "HELP") || !strcmp(token[0], "?")) printHelp();
  else if (!strcmp(token[0], "STATUS")) printStatus();
  else if (!strcmp(token[0], "CAL") || !strcmp(token[0], "CONFIG")) printCalibration();
  else if (!strcmp(token[0], "SCAN")) scanI2c();
  else if (!strcmp(token[0], "GAIN")) {
    if (token[1] && setGainFromText(token[1])) {
      Serial.print(F("OK,GAIN=")); printGain();
      Serial.print(F(",RANGE_V=")); Serial.println(gainRangeV(), 3);
    } else Serial.println(F("ERR,GAIN_USE_2/3_1_2_4_8_16"));
  } else if (!strcmp(token[0], "VREF")) {
    if (!token[1] || atof(token[1]) < 2.7f || atof(token[1]) > 5.5f) Serial.println(F("ERR,VREF_RANGE_2.7_TO_5.5"));
    else {
      dacReferenceV = atof(token[1]);
      Serial.print(F("OK,DAC_VREF=")); Serial.println(dacReferenceV, 5);
    }
  } else if (!strcmp(token[0], "ARM")) {
    outputArmed = true;
    armedAtMs = millis();
    Serial.println(F("OK,OUTPUT_ARMED_FOR_60_SECONDS"));
  } else if (!strcmp(token[0], "DAC")) {
    int8_t channel = parseDacChannel(token[1]);
    if (!outputArmed || millis() - armedAtMs > ARM_TIMEOUT_MS) {
      outputArmed = false;
      Serial.println(F("ERR,LOCKED,USE_ARM"));
    } else if (channel < 0 || !token[2] || !writeDacVoltage(channel, atof(token[2]), true)) {
      Serial.println(F("ERR,USE_DAC_A_TO_D_VOLTS"));
    }
  } else if (!strcmp(token[0], "ADC")) {
    if (!token[1] || atoi(token[1]) < 0 || atoi(token[1]) > 3) Serial.println(F("ERR,ADC_CHANNEL_0_TO_3"));
    else printMeasurement(atoi(token[1]), token[2] ? atoi(token[2]) : 1);
  } else if (!strcmp(token[0], "ADCALL")) {
    readAllAdc(token[1] ? atoi(token[1]) : 10);
  } else if (!strcmp(token[0], "MEASURE")) {
    uint8_t count = token[1] ? atoi(token[1]) : 20;
    if (count < 1 || count > 100) Serial.println(F("ERR,MEASURE_COUNT_1_TO_100"));
    else printPhysicalMeasurement(count);
  } else if (!strcmp(token[0], "SHUNT")) {
    float value = token[1] ? atof(token[1]) : 0.0f;
    if (value <= 0.0f || value > 10000.0f) Serial.println(F("ERR,SHUNT_RANGE"));
    else { shuntOhms = value; printCalibration(); }
  } else if (!strcmp(token[0], "IGAIN")) {
    float value = token[1] ? atof(token[1]) : 0.0f;
    if (value <= 0.0f || value > 1000.0f) Serial.println(F("ERR,IGAIN_RANGE"));
    else { currentSenseGain = value; printCalibration(); }
  } else if (!strcmp(token[0], "USCALE")) {
    float value = token[1] ? atof(token[1]) : 0.0f;
    if (value <= 0.0f || value > 1000.0f) Serial.println(F("ERR,USCALE_RANGE"));
    else { voltageFeedbackScale = value; printCalibration(); }
  } else if (!strcmp(token[0], "UZERO")) {
    if (!token[1]) Serial.println(F("ERR,USE_UZERO_VOLTS"));
    else { voltageZeroV = atof(token[1]); printCalibration(); }
  } else if (!strcmp(token[0], "IZERO")) {
    if (!token[1]) Serial.println(F("ERR,USE_IZERO_VOLTS"));
    else { currentZeroV = atof(token[1]); printCalibration(); }
  } else if (!strcmp(token[0], "PSCALE")) {
    float value = token[1] ? atof(token[1]) : 0.0f;
    if (value <= 0.0f) Serial.println(F("ERR,PSCALE_MUST_BE_POSITIVE"));
    else { photoScale = value; printCalibration(); }
  } else if (!strcmp(token[0], "PZERO")) {
    if (!token[1]) Serial.println(F("ERR,USE_PZERO_VOLTS"));
    else { photoZeroV = atof(token[1]); printCalibration(); }
  } else if (!strcmp(token[0], "MAP")) {
    if (!token[1] || !token[2] || !token[3] || atoi(token[1]) > 3 || atoi(token[2]) > 3 || atoi(token[3]) > 3) {
      Serial.println(F("ERR,USE_MAP_U_I_PHOTO_CHANNEL_0_TO_3"));
    } else {
      voltageAdcChannel = atoi(token[1]);
      currentAdcChannel = atoi(token[2]);
      photoAdcChannel = atoi(token[3]);
      printCalibration();
    }
  } else if (!strcmp(token[0], "DACMAP")) {
    int8_t uChannel = parseDacChannel(token[1]);
    int8_t iChannel = parseDacChannel(token[2]);
    if (uChannel < 0 || iChannel < 0) Serial.println(F("ERR,USE_DACMAP_A_TO_D_A_TO_D"));
    else {
      voltageDacChannel = uChannel;
      currentDacChannel = iChannel;
      printCalibration();
    }
  } else if (!strcmp(token[0], "SETU")) {
    if (!token[1]) Serial.println(F("ERR,USE_SETU_VOLTS"));
    else setPhysicalVoltage(atof(token[1]));
  } else if (!strcmp(token[0], "SETI")) {
    if (!token[1]) Serial.println(F("ERR,USE_SETI_MA"));
    else setPhysicalCurrent(atof(token[1]));
  } else if (!strcmp(token[0], "MONITOR")) {
    unsigned long intervalMs = token[1] ? atol(token[1]) : 1000;
    if (intervalMs < 100 || intervalMs > 60000UL) Serial.println(F("ERR,MONITOR_MS_100_TO_60000"));
    else {
      monitorIntervalMs = intervalMs;
      monitorLastMs = 0;
      monitorEnabled = true;
      Serial.println(F("OK,MONITOR_STARTED"));
    }
  } else if (!strcmp(token[0], "STOP")) {
    monitorEnabled = false;
    Serial.println(F("OK,MONITOR_STOPPED"));
  } else if (!strcmp(token[0], "SWEEP")) {
    int8_t dacChannel = parseDacChannel(token[1]);
    if (dacChannel < 0 || !token[2] || !token[3] || !token[4] || !token[5] || !token[6]) {
      Serial.println(F("ERR,USE_SWEEP_DAC_ADC_START_STOP_STEP_SETTLE"));
    } else {
      runSweep(dacChannel, atoi(token[2]), atof(token[3]), atof(token[4]), atof(token[5]), atol(token[6]));
    }
  } else if (!strcmp(token[0], "ZERO") || !strcmp(token[0], "LOCK")) zeroAllDac();
  else Serial.println(F("ERR,UNKNOWN,TYPE_HELP"));
}

void serviceMonitor() {
  if (!monitorEnabled) return;
  unsigned long now = millis();
  if (monitorLastMs && now - monitorLastMs < monitorIntervalMs) return;
  monitorLastMs = now;
  Serial.print(F("MONITOR,T_MS="));
  Serial.println(now);
  readAllAdc(1);
  printPhysicalMeasurement(1);
}

void setup() {
  setUsbDebugDirection();
  Serial.begin(SERIAL_BAUD);
  Serial.setTimeout(80);

  Wire.begin();
  Wire.setClock(100000);
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(I2C_TIMEOUT_US, true);
#endif
  delay(400);

  adsReady = initAds();
  dacReady = initDac();
  Serial.println(F("NMSE_ONE_CHANNEL_DEBUGGER,BOOT"));
  zeroAllDac();
  printStatus();
  Serial.println(F("READY,TYPE_HELP"));
}

void loop() {
  while (Serial.available()) {
    char character = Serial.read();
    if (character == '\r' || character == '\n') {
      commandLine[commandLength] = 0;
      if (commandLength) processCommand(commandLine);
      commandLength = 0;
    } else if (commandLength < sizeof(commandLine) - 1) {
      commandLine[commandLength++] = character;
      lastByteMs = millis();
    } else commandLength = 0;
  }

  // Accept commands even when Serial Monitor is set to "No line ending".
  if (commandLength && millis() - lastByteMs >= 150) {
    commandLine[commandLength] = 0;
    processCommand(commandLine);
    commandLength = 0;
  }

  if (outputArmed && millis() - armedAtMs > ARM_TIMEOUT_MS) outputArmed = false;
  serviceMonitor();
}
