#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <stdlib.h>
#include <string.h>

/*
  NMSE OLEDer - full diagnostic report for sample/channel 1

  This sketch is READ ONLY: it does not write to the MCP4728 DAC.

  Current board map taken from Arduino_RS485_OLEDer.ino:
    PCA9548A channel 0 -> ADS1115 for samples 1 and 2
      ADS A0 -> sample 1 voltage feedback
      ADS A1 -> sample 1 current/shunt amplifier output
      ADS A2 -> sample 2 voltage feedback (printed as an extra port)
      ADS A3 -> sample 2 current feedback (printed as an extra port)

    PCA9548A channel 4 -> ADS1115 for photodiodes
      ADS A0 -> sample 1 brightness
      ADS A1/A2/A3 -> brightness of samples 2/3/4 (also printed)

  Serial Monitor:
    115200 baud
    Any line ending is accepted.

  Main commands:
    REPORT 16       full report, average of 16 readings
    PORTS 16        raw codes and volts on every ADS1115 port
    MONITOR 1000    compact converted values once per second
    FULLMON 5000    full report every 5 seconds
    STOP
    CONFIG
*/

#define SERIAL_BAUD       115200
#define RS485_DIR_PIN     2
#define PCA_ADDR          0x70
#define ADS_ADDR          0x48
#define ADS_12_MUX        0
#define PHOTO_MUX         4
#define I2C_TIMEOUT_US    25000UL
#define AUTO_REPORT_BOOT  1

// ===== PHYSICAL CONFIGURATION FOR SAMPLE 1 =====
// Change these defaults if the real circuit is different.
float shuntOhms = 25.0f;
float currentAmplifierGain = 10.0f;
float voltageScale = 3.0f;

// ADC voltage measured when the real value is zero.
float voltageOffsetV = 0.0f;
float currentOffsetV = 0.0f;
float photoOffsetV = 0.0f;

// Final calibration multipliers.
float voltageCalibration = 1.0f;
float currentCalibration = 1.0f;
float photoCalibration = 1.0f;

// Separate ADS1115 PGA ranges for the three useful signals.
adsGain_t voltageAdcGain = GAIN_TWOTHIRDS;  // +/-6.144 V, 187.5 uV/bit
adsGain_t currentAdcGain = GAIN_TWOTHIRDS;
adsGain_t photoAdcGain = GAIN_TWOTHIRDS;

Adafruit_ADS1115 ads;
bool adsObjectReady = false;

char commandLine[96];
uint8_t commandLength = 0;
unsigned long lastByteMs = 0;

enum MonitorMode {
  MONITOR_OFF,
  MONITOR_COMPACT,
  MONITOR_FULL
};

MonitorMode monitorMode = MONITOR_OFF;
unsigned long monitorIntervalMs = 1000;
unsigned long monitorLastMs = 0;

struct Measurement {
  bool ok;
  float rawMean;
  int16_t rawMin;
  int16_t rawMax;
  float voltsMean;
  float voltsMin;
  float voltsMax;
};

struct ConvertedValues {
  bool ok;
  Measurement voltage;
  Measurement current;
  Measurement photo;
  float correctedVoltageAdcV;
  float sampleVoltageV;
  float correctedCurrentAdcV;
  float shuntVoltageV;
  float currentA;
  float currentMa;
  float brightness;
  float powerMw;
  float resistanceOhm;
};

void disableRs485ReceiverForUsbDebug() {
  pinMode(RS485_DIR_PIN, OUTPUT);
  // For the usual MAX485 wiring DE and /RE are tied together.
  // HIGH disables RO so it cannot fight the Arduino USB-UART receiver.
  // Disconnect A/B from the common bus while using this USB debugger.
  digitalWrite(RS485_DIR_PIN, HIGH);
}

bool selectMux(uint8_t channel) {
  if (channel > 7) return false;
  Wire.beginTransmission(PCA_ADDR);
  Wire.write((uint8_t)(1U << channel));
  byte error = Wire.endTransmission();
  delay(2);
  return error == 0;
}

bool addressPresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

float gainFullScaleV(adsGain_t gain) {
  if (gain == GAIN_TWOTHIRDS) return 6.144f;
  if (gain == GAIN_ONE) return 4.096f;
  if (gain == GAIN_TWO) return 2.048f;
  if (gain == GAIN_FOUR) return 1.024f;
  if (gain == GAIN_EIGHT) return 0.512f;
  return 0.256f;
}

float gainLsbUv(adsGain_t gain) {
  return gainFullScaleV(gain) / 32768.0f * 1000000.0f;
}

void printGain(adsGain_t gain) {
  if (gain == GAIN_TWOTHIRDS) Serial.print(F("2/3"));
  else if (gain == GAIN_ONE) Serial.print(F("1"));
  else if (gain == GAIN_TWO) Serial.print(F("2"));
  else if (gain == GAIN_FOUR) Serial.print(F("4"));
  else if (gain == GAIN_EIGHT) Serial.print(F("8"));
  else Serial.print(F("16"));
}

bool parseGain(const char *text, adsGain_t *gain) {
  if (!text || !gain) return false;
  if (!strcmp(text, "2/3") || !strcmp(text, "0") || !strcmp(text, "6.144")) *gain = GAIN_TWOTHIRDS;
  else if (!strcmp(text, "1") || !strcmp(text, "4.096")) *gain = GAIN_ONE;
  else if (!strcmp(text, "2") || !strcmp(text, "2.048")) *gain = GAIN_TWO;
  else if (!strcmp(text, "4") || !strcmp(text, "1.024")) *gain = GAIN_FOUR;
  else if (!strcmp(text, "8") || !strcmp(text, "0.512")) *gain = GAIN_EIGHT;
  else if (!strcmp(text, "16") || !strcmp(text, "0.256")) *gain = GAIN_SIXTEEN;
  else return false;
  return true;
}

bool initAdsOnMux(uint8_t muxChannel) {
  if (!selectMux(muxChannel)) return false;
  if (!addressPresent(ADS_ADDR)) return false;
  if (!adsObjectReady) adsObjectReady = ads.begin(ADS_ADDR);
  return adsObjectReady;
}

Measurement readPort(uint8_t muxChannel, uint8_t adsChannel,
                     adsGain_t gain, uint8_t count) {
  Measurement result;
  result.ok = false;
  result.rawMean = 0.0f;
  result.rawMin = 32767;
  result.rawMax = -32768;
  result.voltsMean = 0.0f;
  result.voltsMin = 1000.0f;
  result.voltsMax = -1000.0f;

  if (adsChannel > 3) return result;
  if (count < 1) count = 1;
  if (count > 64) count = 64;
  if (!initAdsOnMux(muxChannel)) return result;

  ads.setGain(gain);
  long rawSum = 0;
  float voltsSum = 0.0f;

  for (uint8_t n = 0; n < count; n++) {
    int16_t raw = ads.readADC_SingleEnded(adsChannel);
    float volts = ads.computeVolts(raw);
    rawSum += raw;
    voltsSum += volts;
    if (raw < result.rawMin) result.rawMin = raw;
    if (raw > result.rawMax) result.rawMax = raw;
    if (volts < result.voltsMin) result.voltsMin = volts;
    if (volts > result.voltsMax) result.voltsMax = volts;
  }

  result.rawMean = (float)rawSum / count;
  result.voltsMean = voltsSum / count;
  result.ok = true;
  return result;
}

void printPortLine(const __FlashStringHelper *deviceName, uint8_t muxChannel,
                   uint8_t adsChannel, const __FlashStringHelper *meaning,
                   adsGain_t gain, uint8_t count) {
  Measurement m = readPort(muxChannel, adsChannel, gain, count);
  Serial.print(F("PORT,DEVICE=")); Serial.print(deviceName);
  Serial.print(F(",MUX=")); Serial.print(muxChannel);
  Serial.print(F(",PIN=A")); Serial.print(adsChannel);
  Serial.print(F(",MEANING=")); Serial.print(meaning);
  if (!m.ok) {
    Serial.println(F(",ERROR=READ_FAILED"));
    return;
  }

  Serial.print(F(",GAIN=")); printGain(gain);
  Serial.print(F(",FS_V=+/-")); Serial.print(gainFullScaleV(gain), 3);
  Serial.print(F(",LSB_UV=")); Serial.print(gainLsbUv(gain), 3);
  Serial.print(F(",N=")); Serial.print(count);
  Serial.print(F(",RAW_MEAN=")); Serial.print(m.rawMean, 2);
  Serial.print(F(",RAW_MIN=")); Serial.print(m.rawMin);
  Serial.print(F(",RAW_MAX=")); Serial.print(m.rawMax);
  Serial.print(F(",V_MEAN=")); Serial.print(m.voltsMean, 7);
  Serial.print(F(",V_MIN=")); Serial.print(m.voltsMin, 7);
  Serial.print(F(",V_MAX=")); Serial.print(m.voltsMax, 7);
  Serial.print(F(",NOISE_P2P_MV=")); Serial.print((m.voltsMax - m.voltsMin) * 1000.0f, 4);
  Serial.print(F(",SATURATION="));
  Serial.println(m.rawMax >= 32760 ? F("YES") : F("NO"));
}

void printAllPorts(uint8_t count) {
  Serial.println(F("---------------- ALL ADC PORTS ----------------"));
  Serial.println(F("Measurement ADS1115 behind PCA9548A channel 0:"));
  printPortLine(F("MEAS_ADS"), ADS_12_MUX, 0, F("SAMPLE1_VOLTAGE"), voltageAdcGain, count);
  printPortLine(F("MEAS_ADS"), ADS_12_MUX, 1, F("SAMPLE1_CURRENT_AMP"), currentAdcGain, count);
  printPortLine(F("MEAS_ADS"), ADS_12_MUX, 2, F("SAMPLE2_VOLTAGE_EXTRA"), GAIN_TWOTHIRDS, count);
  printPortLine(F("MEAS_ADS"), ADS_12_MUX, 3, F("SAMPLE2_CURRENT_EXTRA"), GAIN_TWOTHIRDS, count);

  Serial.println(F("Photo ADS1115 behind PCA9548A channel 4:"));
  printPortLine(F("PHOTO_ADS"), PHOTO_MUX, 0, F("SAMPLE1_BRIGHTNESS"), photoAdcGain, count);
  printPortLine(F("PHOTO_ADS"), PHOTO_MUX, 1, F("SAMPLE2_BRIGHTNESS_EXTRA"), GAIN_TWOTHIRDS, count);
  printPortLine(F("PHOTO_ADS"), PHOTO_MUX, 2, F("SAMPLE3_BRIGHTNESS_EXTRA"), GAIN_TWOTHIRDS, count);
  printPortLine(F("PHOTO_ADS"), PHOTO_MUX, 3, F("SAMPLE4_BRIGHTNESS_EXTRA"), GAIN_TWOTHIRDS, count);
}

ConvertedValues readConverted(uint8_t count) {
  ConvertedValues v;
  v.ok = false;
  v.voltage = readPort(ADS_12_MUX, 0, voltageAdcGain, count);
  v.current = readPort(ADS_12_MUX, 1, currentAdcGain, count);
  v.photo = readPort(PHOTO_MUX, 0, photoAdcGain, count);
  if (!v.voltage.ok || !v.current.ok || !v.photo.ok) return v;

  v.correctedVoltageAdcV = v.voltage.voltsMean - voltageOffsetV;
  v.sampleVoltageV = v.correctedVoltageAdcV * voltageScale * voltageCalibration;

  v.correctedCurrentAdcV = v.current.voltsMean - currentOffsetV;
  v.shuntVoltageV = v.correctedCurrentAdcV / currentAmplifierGain;
  v.currentA = v.shuntVoltageV / shuntOhms * currentCalibration;
  v.currentMa = v.currentA * 1000.0f;

  v.brightness = (v.photo.voltsMean - photoOffsetV) * photoCalibration;
  v.powerMw = v.sampleVoltageV * v.currentMa;
  if (v.currentA > 0.0000001f) v.resistanceOhm = v.sampleVoltageV / v.currentA;
  else v.resistanceOhm = -1.0f;
  v.ok = true;
  return v;
}

void printConfig() {
  Serial.println(F("---------------- CONFIGURATION ----------------"));
  Serial.print(F("SHUNT_OHM=")); Serial.println(shuntOhms, 6);
  Serial.print(F("CURRENT_REGISTRATION_GAIN=")); Serial.println(currentAmplifierGain, 6);
  Serial.print(F("VOLTAGE_SCALE=")); Serial.println(voltageScale, 6);
  Serial.print(F("VOLTAGE_OFFSET_ADC_V=")); Serial.println(voltageOffsetV, 7);
  Serial.print(F("CURRENT_OFFSET_ADC_V=")); Serial.println(currentOffsetV, 7);
  Serial.print(F("PHOTO_OFFSET_ADC_V=")); Serial.println(photoOffsetV, 7);
  Serial.print(F("VOLTAGE_CAL_MULTIPLIER=")); Serial.println(voltageCalibration, 7);
  Serial.print(F("CURRENT_CAL_MULTIPLIER=")); Serial.println(currentCalibration, 7);
  Serial.print(F("PHOTO_CAL_MULTIPLIER=")); Serial.println(photoCalibration, 7);
  Serial.print(F("VOLTAGE_ADC_GAIN=")); printGain(voltageAdcGain);
  Serial.print(F(",FS_V=+/-")); Serial.println(gainFullScaleV(voltageAdcGain), 3);
  Serial.print(F("CURRENT_ADC_GAIN=")); printGain(currentAdcGain);
  Serial.print(F(",FS_V=+/-")); Serial.println(gainFullScaleV(currentAdcGain), 3);
  Serial.print(F("PHOTO_ADC_GAIN=")); printGain(photoAdcGain);
  Serial.print(F(",FS_V=+/-")); Serial.println(gainFullScaleV(photoAdcGain), 3);
}

void printFormulaReport(uint8_t count) {
  ConvertedValues v = readConverted(count);
  Serial.println(F("---------------- CONVERSION REPORT ----------------"));
  if (!v.ok) {
    Serial.println(F("ERROR: one or more ADC readings failed"));
    return;
  }

  Serial.println(F("[1] SAMPLE VOLTAGE"));
  Serial.println(F("Formula: U_sample = (U_ADC - U_offset) * U_scale * U_cal"));
  Serial.print(F("Substitution: (")); Serial.print(v.voltage.voltsMean, 7);
  Serial.print(F(" - ")); Serial.print(voltageOffsetV, 7);
  Serial.print(F(") * ")); Serial.print(voltageScale, 6);
  Serial.print(F(" * ")); Serial.print(voltageCalibration, 6);
  Serial.print(F(" = ")); Serial.print(v.sampleVoltageV, 7); Serial.println(F(" V"));

  Serial.println(F("[2] SAMPLE CURRENT"));
  Serial.println(F("Step A: V_amp_corrected = I_ADC - I_offset"));
  Serial.print(F("        ")); Serial.print(v.current.voltsMean, 7);
  Serial.print(F(" - ")); Serial.print(currentOffsetV, 7);
  Serial.print(F(" = ")); Serial.print(v.correctedCurrentAdcV, 7); Serial.println(F(" V"));
  Serial.println(F("Step B: V_shunt = V_amp_corrected / amplifier_gain"));
  Serial.print(F("        ")); Serial.print(v.correctedCurrentAdcV, 7);
  Serial.print(F(" / ")); Serial.print(currentAmplifierGain, 6);
  Serial.print(F(" = ")); Serial.print(v.shuntVoltageV, 7); Serial.println(F(" V"));
  Serial.println(F("Step C: I_mA = V_shunt / R_shunt * 1000 * I_cal"));
  Serial.print(F("        ")); Serial.print(v.shuntVoltageV, 7);
  Serial.print(F(" / ")); Serial.print(shuntOhms, 6);
  Serial.print(F(" * 1000 * ")); Serial.print(currentCalibration, 6);
  Serial.print(F(" = ")); Serial.print(v.currentMa, 7); Serial.println(F(" mA"));

  Serial.println(F("[3] BRIGHTNESS SIGNAL"));
  Serial.println(F("Formula: Brightness = (PHOTO_ADC - PHOTO_offset) * PHOTO_cal"));
  Serial.print(F("Substitution: (")); Serial.print(v.photo.voltsMean, 7);
  Serial.print(F(" - ")); Serial.print(photoOffsetV, 7);
  Serial.print(F(") * ")); Serial.print(photoCalibration, 6);
  Serial.print(F(" = ")); Serial.println(v.brightness, 7);

  Serial.println(F("[4] DERIVED OLED VALUES"));
  Serial.print(F("OLED_POWER_MW = U_V * I_mA = ")); Serial.println(v.powerMw, 7);
  Serial.print(F("OLED_RESISTANCE_OHM = U_V / I_A = "));
  if (v.resistanceOhm >= 0.0f) Serial.println(v.resistanceOhm, 3);
  else Serial.println(F("undefined (current is approximately zero)"));

  Serial.println(F("[5] COMPACT MACHINE-READABLE RESULT"));
  Serial.print(F("SUMMARY,T_MS=")); Serial.print(millis());
  Serial.print(F(",U_ADC_V=")); Serial.print(v.voltage.voltsMean, 7);
  Serial.print(F(",U_SAMPLE_V=")); Serial.print(v.sampleVoltageV, 7);
  Serial.print(F(",I_ADC_V=")); Serial.print(v.current.voltsMean, 7);
  Serial.print(F(",V_SHUNT_V=")); Serial.print(v.shuntVoltageV, 7);
  Serial.print(F(",I_MA=")); Serial.print(v.currentMa, 7);
  Serial.print(F(",PHOTO_ADC_V=")); Serial.print(v.photo.voltsMean, 7);
  Serial.print(F(",BRIGHTNESS=")); Serial.print(v.brightness, 7);
  Serial.print(F(",POWER_MW=")); Serial.println(v.powerMw, 7);

  if (v.correctedVoltageAdcV < -0.001f) Serial.println(F("WARNING: voltage is below its zero offset"));
  if (v.correctedCurrentAdcV < -0.001f) Serial.println(F("WARNING: current signal is below its zero offset"));
  if (v.current.rawMax >= 32760 || v.voltage.rawMax >= 32760 || v.photo.rawMax >= 32760) {
    Serial.println(F("WARNING: ADC saturation detected; select a wider ADS1115 range"));
  }
}

void printFullReport(uint8_t count) {
  if (count < 1) count = 1;
  if (count > 64) count = 64;
  Serial.println();
  Serial.println(F("============================================================"));
  Serial.print(F("NMSE OLED CHANNEL 1 FULL REPORT, T_MS=")); Serial.print(millis());
  Serial.print(F(", SAMPLES_PER_INPUT=")); Serial.println(count);
  printConfig();
  printAllPorts(count);
  printFormulaReport(count);
  Serial.println(F("====================== END REPORT =========================="));
  Serial.println();
}

void printCompact(uint8_t count) {
  ConvertedValues v = readConverted(count);
  if (!v.ok) {
    Serial.println(F("SUMMARY,ERROR=ADC_READ_FAILED"));
    return;
  }
  Serial.print(F("SUMMARY,T_MS=")); Serial.print(millis());
  Serial.print(F(",U_ADC_V=")); Serial.print(v.voltage.voltsMean, 7);
  Serial.print(F(",U_V=")); Serial.print(v.sampleVoltageV, 7);
  Serial.print(F(",I_ADC_V=")); Serial.print(v.current.voltsMean, 7);
  Serial.print(F(",V_SHUNT_V=")); Serial.print(v.shuntVoltageV, 7);
  Serial.print(F(",I_MA=")); Serial.print(v.currentMa, 7);
  Serial.print(F(",PHOTO_ADC_V=")); Serial.print(v.photo.voltsMean, 7);
  Serial.print(F(",BRIGHTNESS=")); Serial.print(v.brightness, 7);
  Serial.print(F(",POWER_MW=")); Serial.println(v.powerMw, 7);
}

void scanI2cByMux() {
  Serial.println(F("I2C SCAN BY PCA9548A CHANNEL"));
  for (uint8_t mux = 0; mux < 8; mux++) {
    if (!selectMux(mux)) {
      Serial.print(F("MUX=")); Serial.print(mux); Serial.println(F(",PCA_ERROR"));
      continue;
    }
    Serial.print(F("MUX=")); Serial.print(mux); Serial.print(F(",FOUND="));
    uint8_t count = 0;
    for (uint8_t address = 1; address < 127; address++) {
      if (address == PCA_ADDR) continue;
      if (addressPresent(address)) {
        if (count) Serial.print(';');
        Serial.print(F("0x"));
        if (address < 16) Serial.print('0');
        Serial.print(address, HEX);
        count++;
      }
    }
    if (!count) Serial.print(F("NONE"));
    Serial.println();
  }
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("REPORT [1..64]       full report; default 16 samples/input"));
  Serial.println(F("PORTS [1..64]        raw code and volts on all ADC ports"));
  Serial.println(F("MONITOR [100..60000] compact report interval in ms"));
  Serial.println(F("FULLMON [1000..60000] full report interval in ms"));
  Serial.println(F("STOP                  stop either monitor"));
  Serial.println(F("CONFIG                show every coefficient and ADC gain"));
  Serial.println(F("SCAN                  scan every PCA9548A branch"));
  Serial.println(F("SHUNT 25              shunt resistance, ohms"));
  Serial.println(F("IGAIN 10              current registration amplifier gain"));
  Serial.println(F("USCALE 3              voltage divider/amplifier scale"));
  Serial.println(F("UOFFSET 0             voltage-channel zero, ADC volts"));
  Serial.println(F("IOFFSET 0             current-channel zero, ADC volts"));
  Serial.println(F("POFFSET 0             photo-channel zero, ADC volts"));
  Serial.println(F("UCAL 1 | ICAL 1 | PCAL 1   final calibration multipliers"));
  Serial.println(F("UGAIN 2/3|1|2|4|8|16       voltage ADS1115 range"));
  Serial.println(F("CGAIN 2/3|1|2|4|8|16       current ADS1115 range"));
  Serial.println(F("PGAIN 2/3|1|2|4|8|16       photo ADS1115 range"));
}

void normalizeCommand(char *line) {
  for (uint8_t i = 0; line[i]; i++) {
    if (line[i] == ',' || line[i] == '\t') line[i] = ' ';
    if (line[i] >= 'a' && line[i] <= 'z') line[i] -= 32;
  }
}

bool setPositiveValue(const char *text, float *target) {
  if (!text || !target) return false;
  float value = atof(text);
  if (value <= 0.0f || value > 100000.0f) return false;
  *target = value;
  return true;
}

void processCommand(char *line) {
  normalizeCommand(line);
  char *command = strtok(line, " ");
  char *arg = strtok(NULL, " ");
  if (!command) return;

  if (!strcmp(command, "HELP") || !strcmp(command, "?")) printHelp();
  else if (!strcmp(command, "PING")) Serial.println(F("OK,PONG,CHANNEL1_FULL_REPORT"));
  else if (!strcmp(command, "REPORT")) printFullReport(arg ? atoi(arg) : 16);
  else if (!strcmp(command, "PORTS")) printAllPorts(arg ? atoi(arg) : 16);
  else if (!strcmp(command, "CONFIG")) printConfig();
  else if (!strcmp(command, "SCAN")) scanI2cByMux();
  else if (!strcmp(command, "STOP")) {
    monitorMode = MONITOR_OFF;
    Serial.println(F("OK,MONITOR_STOPPED"));
  }
  else if (!strcmp(command, "MONITOR") || !strcmp(command, "FULLMON")) {
    unsigned long interval = arg ? atol(arg) : (!strcmp(command, "FULLMON") ? 5000UL : 1000UL);
    if (interval < (!strcmp(command, "FULLMON") ? 1000UL : 100UL) || interval > 60000UL) {
      Serial.println(F("ERR,BAD_MONITOR_INTERVAL"));
    } else {
      monitorIntervalMs = interval;
      monitorLastMs = 0;
      monitorMode = !strcmp(command, "FULLMON") ? MONITOR_FULL : MONITOR_COMPACT;
      Serial.println(F("OK,MONITOR_STARTED"));
    }
  }
  else if (!strcmp(command, "SHUNT")) {
    if (!setPositiveValue(arg, &shuntOhms)) Serial.println(F("ERR,BAD_SHUNT"));
    else printConfig();
  }
  else if (!strcmp(command, "IGAIN")) {
    if (!setPositiveValue(arg, &currentAmplifierGain)) Serial.println(F("ERR,BAD_IGAIN"));
    else printConfig();
  }
  else if (!strcmp(command, "USCALE")) {
    if (!setPositiveValue(arg, &voltageScale)) Serial.println(F("ERR,BAD_USCALE"));
    else printConfig();
  }
  else if (!strcmp(command, "UOFFSET") || !strcmp(command, "IOFFSET") || !strcmp(command, "POFFSET")) {
    if (!arg) Serial.println(F("ERR,OFFSET_VALUE_REQUIRED"));
    else {
      float value = atof(arg);
      if (!strcmp(command, "UOFFSET")) voltageOffsetV = value;
      else if (!strcmp(command, "IOFFSET")) currentOffsetV = value;
      else photoOffsetV = value;
      printConfig();
    }
  }
  else if (!strcmp(command, "UCAL") || !strcmp(command, "ICAL") || !strcmp(command, "PCAL")) {
    float *target = !strcmp(command, "UCAL") ? &voltageCalibration :
                    (!strcmp(command, "ICAL") ? &currentCalibration : &photoCalibration);
    if (!setPositiveValue(arg, target)) Serial.println(F("ERR,BAD_CALIBRATION"));
    else printConfig();
  }
  else if (!strcmp(command, "UGAIN") || !strcmp(command, "CGAIN") || !strcmp(command, "PGAIN")) {
    adsGain_t newGain;
    if (!parseGain(arg, &newGain)) Serial.println(F("ERR,GAIN_USE_2/3_1_2_4_8_16"));
    else {
      if (!strcmp(command, "UGAIN")) voltageAdcGain = newGain;
      else if (!strcmp(command, "CGAIN")) currentAdcGain = newGain;
      else photoAdcGain = newGain;
      printConfig();
    }
  }
  else Serial.println(F("ERR,UNKNOWN_COMMAND,TYPE_HELP"));
}

void serviceSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      commandLine[commandLength] = 0;
      if (commandLength) processCommand(commandLine);
      commandLength = 0;
    } else if (commandLength < sizeof(commandLine) - 1) {
      commandLine[commandLength++] = c;
      lastByteMs = millis();
    } else commandLength = 0;
  }

  // Also accept Arduino Serial Monitor setting "No line ending".
  if (commandLength && millis() - lastByteMs >= 150) {
    commandLine[commandLength] = 0;
    processCommand(commandLine);
    commandLength = 0;
  }
}

void serviceMonitor() {
  if (monitorMode == MONITOR_OFF) return;
  unsigned long now = millis();
  if (monitorLastMs && now - monitorLastMs < monitorIntervalMs) return;
  monitorLastMs = now;
  if (monitorMode == MONITOR_FULL) printFullReport(8);
  else printCompact(4);
}

void setup() {
  disableRs485ReceiverForUsbDebug();
  Serial.begin(SERIAL_BAUD);
  Serial.setTimeout(80);

  Wire.begin();
  Wire.setClock(100000);
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(I2C_TIMEOUT_US, true);
#endif
  delay(500);

  Serial.println(F("NMSE OLED CHANNEL 1 FULL REPORT DEBUGGER"));
  Serial.println(F("READ ONLY: this sketch does not change DAC outputs"));
  Serial.println(F("WARNING: disconnect RS485 A/B while debugging through USB"));
  Serial.println(F("Serial Monitor: 115200 baud"));

  bool measurementAdsFound = initAdsOnMux(ADS_12_MUX);
  bool photoAdsFound = initAdsOnMux(PHOTO_MUX);
  Serial.print(F("INIT,MEASUREMENT_ADS=")); Serial.print(measurementAdsFound ? F("OK") : F("FAIL"));
  Serial.print(F(",PHOTO_ADS=")); Serial.println(photoAdsFound ? F("OK") : F("FAIL"));
  printHelp();

#if AUTO_REPORT_BOOT
  printFullReport(8);
#endif
  Serial.println(F("READY"));
}

void loop() {
  serviceSerial();
  serviceMonitor();
}
