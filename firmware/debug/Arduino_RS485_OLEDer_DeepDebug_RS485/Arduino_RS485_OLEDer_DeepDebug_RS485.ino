#include <Wire.h>
#include <Adafruit_MCP4728.h>
#include <Adafruit_ADS1X15.h>
#include <stdlib.h>
#include <string.h>

/*
  NMSE OLEDer - safe hardware diagnostic firmware

  Target: Arduino Uno/Nano (ATmega328P), 9600 baud.
  Use this sketch on ONE board at a time while debugging.

  Known project wiring:
    MAX485 RO -> RX (D0)
    MAX485 DI -> TX (D1)
    MAX485 DE and /RE -> D2
    I2C SDA -> A4, SCL -> A5
    PCA9548A -> 0x70
    ADS1115  -> 0x48
    MCP4728  -> 0x60

  PCA9548A map:
    0: ADS1115, samples 1-2 (U/I)
    1: MCP4728, samples 1-2
    2: ADS1115, samples 3-4 (U/I)
    3: MCP4728, samples 3-4
    4: ADS1115, photo 1-4

  Safety:
    - all detected DAC outputs are set to 0 V at boot;
    - a non-zero DAC write needs "ARM YES" first;
    - ARM expires automatically after 30 seconds;
    - DAC commands are limited to 0...5 V.
*/

#define SERIAL_BAUD          9600
#define RS485_DIR_PIN        2
// 1 = commands come from the Arduino USB connector (COM port of CH340).
// This disables MAX485 receiver output so it cannot fight the USB-UART on RX.
// Disconnect A/B or leave only this one board on the bus in this mode.
#define DEBUG_OVER_ARDUINO_USB 0
#define PCA_ADDR             0x70
#define ADS_ADDR             0x48
#define DAC_ADDR             0x60
#define I2C_TIMEOUT_US       25000UL
#define DAC_VREF_V           5.0f
#define SHUNT_OHMS           25.0f
#define CURRENT_SENSE_GAIN   10.0f
#define VOLTAGE_SCALE        3.0f
#define ARM_TIMEOUT_MS       30000UL

Adafruit_ADS1115 ads;
Adafruit_MCP4728 mcp;

bool hasPca = false;
bool dacArmed = false;
bool sweepArmed = false;
unsigned long armStartedMs = 0;

bool monitorEnabled = false;
uint8_t monitorSample = 0;  // 0 = all samples
unsigned long monitorIntervalMs = 1000;
unsigned long monitorLastMs = 0;

char inputLine[96];
uint8_t inputLength = 0;
unsigned long lastInputByteMs = 0;

void rs485Listen() {
#if DEBUG_OVER_ARDUINO_USB
  // DE=1 and /RE=1 (they are tied together): RO becomes high impedance,
  // therefore the onboard USB-UART can safely drive Arduino RX.
  digitalWrite(RS485_DIR_PIN, HIGH);
#else
  digitalWrite(RS485_DIR_PIN, LOW);
#endif
  delayMicroseconds(50);
}

void rs485Transmit() {
  digitalWrite(RS485_DIR_PIN, HIGH);
  delayMicroseconds(50);
}

void beginReply() {
  rs485Transmit();
}

void endReply() {
  Serial.flush();
  delayMicroseconds(200);
  rs485Listen();
}

bool i2cAddressPresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool selectMux(int8_t channel) {
  if (!hasPca) return true;
  if (channel < 0 || channel > 7) return false;

  Wire.beginTransmission(PCA_ADDR);
  Wire.write((uint8_t)(1U << channel));
  byte error = Wire.endTransmission();
  delay(2);
  return error == 0;
}

void disableMux() {
  if (!hasPca) return;
  Wire.beginTransmission(PCA_ADDR);
  Wire.write((uint8_t)0);
  Wire.endTransmission();
  delay(2);
}

void printHexAddress(uint8_t address) {
  Serial.print(F("0x"));
  if (address < 16) Serial.print('0');
  Serial.print(address, HEX);
}

uint8_t scanCurrentBus(bool printDevices) {
  uint8_t count = 0;
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    if (error == 0) {
      count++;
      if (printDevices) {
        Serial.print(F("I2C,FOUND,"));
        printHexAddress(address);
        Serial.println();
      }
    }
  }
  return count;
}

void scanRoot() {
  if (hasPca) disableMux();
  Serial.println(F("SCAN,ROOT,BEGIN"));
  uint8_t count = scanCurrentBus(true);
  Serial.print(F("SCAN,ROOT,END,COUNT="));
  Serial.println(count);
}

void scanMuxes() {
  if (!hasPca) {
    Serial.println(F("MUXSCAN,FAIL,PCA_NOT_FOUND"));
    scanRoot();
    return;
  }
  for (uint8_t mux = 0; mux < 8; mux++) {
    Serial.print(F("MUXSCAN,MUX="));
    Serial.print(mux);
    if (!selectMux(mux)) {
      Serial.println(F(",SELECT_FAIL"));
      continue;
    }
    Serial.println(F(",BEGIN"));
    uint8_t count = scanCurrentBus(true);
    Serial.print(F("MUXSCAN,MUX="));
    Serial.print(mux);
    Serial.print(F(",END,COUNT="));
    Serial.println(count);
  }
  disableMux();
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

void printGain(adsGain_t gain) {
  if (gain == GAIN_TWOTHIRDS) Serial.print(F("2/3"));
  else if (gain == GAIN_ONE) Serial.print(F("1"));
  else if (gain == GAIN_TWO) Serial.print(F("2"));
  else if (gain == GAIN_FOUR) Serial.print(F("4"));
  else if (gain == GAIN_EIGHT) Serial.print(F("8"));
  else if (gain == GAIN_SIXTEEN) Serial.print(F("16"));
}

struct AdsStats {
  bool ok;
  float meanV;
  float minV;
  float maxV;
  int16_t minCode;
  int16_t maxCode;
};

// Explicit prototype keeps the Arduino preprocessor from placing a prototype
// above the custom AdsStats type.
AdsStats measureAds(int8_t mux, uint8_t channel, adsGain_t gain, uint8_t count);

AdsStats measureAds(int8_t mux, uint8_t channel, adsGain_t gain, uint8_t count) {
  AdsStats s;
  s.ok = false;
  s.meanV = 0;
  s.minV = 1000;
  s.maxV = -1000;
  s.minCode = 32767;
  s.maxCode = -32768;

  if (channel > 3 || !selectMux(mux) || !i2cAddressPresent(ADS_ADDR)) return s;
  if (!ads.begin(ADS_ADDR)) return s;
  if (count < 1) count = 1;
  if (count > 100) count = 100;
  ads.setGain(gain);
  delay(2);

  float sum = 0;
  for (uint8_t i = 0; i < count; i++) {
    int16_t code = ads.readADC_SingleEnded(channel);
    float volts = ads.computeVolts(code);
    sum += volts;
    if (volts < s.minV) s.minV = volts;
    if (volts > s.maxV) s.maxV = volts;
    if (code < s.minCode) s.minCode = code;
    if (code > s.maxCode) s.maxCode = code;
  }
  s.meanV = sum / count;
  s.ok = true;
  return s;
}

void printStatsLine(int8_t mux, uint8_t channel, adsGain_t gain, uint8_t count) {
  AdsStats s = measureAds(mux, channel, gain, count);
  Serial.print(F("STATS,MUX="));
  Serial.print(mux);
  Serial.print(F(",CH="));
  Serial.print(channel);
  if (!s.ok) {
    Serial.println(F(",FAIL"));
    return;
  }
  Serial.print(F(",GAIN="));
  printGain(gain);
  Serial.print(F(",N="));
  Serial.print(count);
  Serial.print(F(",MEAN_V="));
  Serial.print(s.meanV, 7);
  Serial.print(F(",MIN_V="));
  Serial.print(s.minV, 7);
  Serial.print(F(",MAX_V="));
  Serial.print(s.maxV, 7);
  Serial.print(F(",P2P_MV="));
  Serial.print((s.maxV - s.minV) * 1000.0f, 4);
  Serial.print(F(",CODE_MIN="));
  Serial.print(s.minCode);
  Serial.print(F(",CODE_MAX="));
  Serial.print(s.maxCode);
  Serial.print(F(",SAT="));
  Serial.println(s.maxCode >= 32760 ? 1 : 0);
}

void rawAll(adsGain_t gain, uint8_t count) {
  Serial.println(F("RAWALL,BEGIN"));
  uint8_t devices = 0;
  if (hasPca) {
    for (uint8_t mux = 0; mux < 8; mux++) {
      if (!selectMux(mux) || !i2cAddressPresent(ADS_ADDR)) continue;
      devices++;
      for (uint8_t ch = 0; ch < 4; ch++) printStatsLine(mux, ch, gain, count);
    }
  } else if (i2cAddressPresent(ADS_ADDR)) {
    devices = 1;
    for (uint8_t ch = 0; ch < 4; ch++) printStatsLine(-1, ch, gain, count);
  }
  Serial.print(F("RAWALL,ADS_COUNT="));
  Serial.println(devices);
  Serial.println(F("RAWALL,END"));
}

long readVccMv() {
#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__)
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  delay(3);
  ADCSRA |= _BV(ADSC);
  while (bit_is_set(ADCSRA, ADSC)) {}
  uint16_t value = ADC;
  return value ? 1125300L / value : 0;
#else
  return -1;
#endif
}

void printRails() {
  Serial.print(F("RAILS,ARDUINO_AVCC_EST_MV="));
  Serial.print(readVccMv());
  Serial.println(F(",APPROXIMATE=1"));
}

bool sampleMap(uint8_t sample, int8_t *adsMux, uint8_t *uCh, uint8_t *iCh,
               int8_t *photoMux, uint8_t *photoCh, int8_t *dacMux,
               uint8_t *dacUCh, uint8_t *dacICh) {
  if (sample < 1 || sample > 4) return false;
  bool firstPair = sample <= 2;
  bool firstInPair = sample == 1 || sample == 3;
  *adsMux = hasPca ? (firstPair ? 0 : 2) : -1;
  *dacMux = hasPca ? (firstPair ? 1 : 3) : -1;
  *photoMux = hasPca ? 4 : -1;
  *uCh = firstInPair ? 0 : 2;
  *iCh = firstInPair ? 1 : 3;
  *dacUCh = *uCh;
  *dacICh = *iCh;
  *photoCh = sample - 1;
  return true;
}

void sampleStats(uint8_t sample, uint8_t count, adsGain_t photoGain) {
  int8_t am, pm, dm;
  uint8_t uc, ic, pc, duc, dic;
  if (!sampleMap(sample, &am, &uc, &ic, &pm, &pc, &dm, &duc, &dic)) {
    Serial.println(F("ERR,BAD_SAMPLE"));
    return;
  }
  AdsStats u = measureAds(am, uc, GAIN_TWOTHIRDS, count);
  AdsStats i = measureAds(am, ic, GAIN_TWOTHIRDS, count);
  AdsStats p = measureAds(pm, pc, photoGain, count);

  printStatsLine(am, uc, GAIN_TWOTHIRDS, count);
  printStatsLine(am, ic, GAIN_TWOTHIRDS, count);
  printStatsLine(pm, pc, photoGain, count);
  Serial.print(F("SAMPLE_SUMMARY,SAMPLE="));
  Serial.print(sample);
  if (!u.ok || !i.ok || !p.ok) {
    Serial.println(F(",FAIL"));
    return;
  }
  Serial.print(F(",U_ADC_V="));
  Serial.print(u.meanV, 7);
  Serial.print(F(",U_APPROX_V="));
  Serial.print(u.meanV * VOLTAGE_SCALE, 7);
  Serial.print(F(",I_ADC_V="));
  Serial.print(i.meanV, 7);
  Serial.print(F(",I_APPROX_MA="));
  Serial.print(i.meanV / CURRENT_SENSE_GAIN / SHUNT_OHMS * 1000.0f, 7);
  Serial.print(F(",PHOTO_ADC_V="));
  Serial.println(p.meanV, 7);
}

void readSampleOnce(uint8_t sample) {
  sampleStats(sample, 1, GAIN_TWOTHIRDS);
}

void readAll() {
  for (uint8_t s = 1; s <= 4; s++) readSampleOnce(s);
  Serial.println(F("END"));
}

void setDacCode(uint8_t channel, uint16_t code) {
  if (channel == 0) mcp.setChannelValue(MCP4728_CHANNEL_A, code);
  else if (channel == 1) mcp.setChannelValue(MCP4728_CHANNEL_B, code);
  else if (channel == 2) mcp.setChannelValue(MCP4728_CHANNEL_C, code);
  else if (channel == 3) mcp.setChannelValue(MCP4728_CHANNEL_D, code);
}

bool writeDac(int8_t mux, uint8_t channel, float volts, bool verbose) {
  if (channel > 3 || volts < 0 || volts > DAC_VREF_V) return false;
  if (!selectMux(mux) || !i2cAddressPresent(DAC_ADDR) || !mcp.begin(DAC_ADDR)) return false;
  uint16_t code = (uint16_t)(volts / DAC_VREF_V * 4095.0f + 0.5f);
  if (code > 4095) code = 4095;
  setDacCode(channel, code);
  delay(2);
  if (verbose) {
    Serial.print(F("DAC,MUX=")); Serial.print(mux);
    Serial.print(F(",CH=")); Serial.print(channel);
    Serial.print(F(",SET_V=")); Serial.print(volts, 6);
    Serial.print(F(",CODE=")); Serial.println(code);
  }
  return true;
}

bool zeroDac(int8_t mux) {
  if (!selectMux(mux) || !i2cAddressPresent(DAC_ADDR) || !mcp.begin(DAC_ADDR)) return false;
  for (uint8_t ch = 0; ch < 4; ch++) setDacCode(ch, 0);
  return true;
}

void zeroAll(bool verbose) {
  uint8_t found = 0;
  if (hasPca) {
    if (zeroDac(1)) found++;
    if (zeroDac(3)) found++;
  } else if (zeroDac(-1)) found++;
  dacArmed = false;
  sweepArmed = false;
  if (verbose) {
    Serial.print(F("ZERO,OK,DAC_DEVICES="));
    Serial.println(found);
  }
}

void checkDevice(int8_t mux, uint8_t address, const __FlashStringHelper *name) {
  bool ok = selectMux(mux) && i2cAddressPresent(address);
  Serial.print(F("CHECK,")); Serial.print(name);
  Serial.print(F(",MUX=")); Serial.print(mux);
  Serial.print(F(",ADDR=")); printHexAddress(address);
  Serial.print(','); Serial.println(ok ? F("OK") : F("FAIL"));
}

void autoTest() {
  Serial.println(F("AUTO,BEGIN"));
  Serial.print(F("CHECK,PCA9548A,ADDR=0x70,"));
  Serial.println(hasPca ? F("OK") : F("NOT_FOUND,DIRECT_MODE"));
  if (hasPca) {
    checkDevice(0, ADS_ADDR, F("ADS_12"));
    checkDevice(1, DAC_ADDR, F("DAC_12"));
    checkDevice(2, ADS_ADDR, F("ADS_34"));
    checkDevice(3, DAC_ADDR, F("DAC_34"));
    checkDevice(4, ADS_ADDR, F("ADS_PHOTO"));
  }
  zeroAll(true);
  readAll();
  Serial.println(F("AUTO,END"));
}

void deepTest(uint8_t count) {
  if (count < 2) count = 2;
  if (count > 50) count = 50;
  Serial.print(F("DEEP,BEGIN,N=")); Serial.println(count);
  zeroAll(true);
  printRails();
  if (hasPca) {
    checkDevice(0, ADS_ADDR, F("ADS_12"));
    checkDevice(1, DAC_ADDR, F("DAC_12"));
    checkDevice(2, ADS_ADDR, F("ADS_34"));
    checkDevice(3, DAC_ADDR, F("DAC_34"));
    checkDevice(4, ADS_ADDR, F("ADS_PHOTO"));
  }
  rawAll(GAIN_TWOTHIRDS, count);
  for (uint8_t s = 1; s <= 4; s++) sampleStats(s, count, GAIN_TWOTHIRDS);
  Serial.println(F("DEEP,END"));
}

void runSweep(uint8_t sample, char mode, float startV, float stopV,
              float stepV, unsigned long settleMs) {
  if (!dacArmed || !sweepArmed || millis() - armStartedMs > ARM_TIMEOUT_MS) {
    Serial.println(F("ERR,SWEEP_LOCKED,USE_ARM_SWEEP"));
    return;
  }
  if (sample < 1 || sample > 4 || (mode != 'U' && mode != 'I') ||
      startV < 0 || stopV > 5 || stopV < startV || stepV < 0.001f ||
      settleMs < 10 || settleMs > 10000UL) {
    Serial.println(F("ERR,BAD_SWEEP_ARGUMENT"));
    return;
  }
  uint16_t points = (uint16_t)((stopV - startV) / stepV + 1.5f);
  if (points < 1 || points > 100) {
    Serial.println(F("ERR,SWEEP_MAX_100_POINTS"));
    return;
  }
  int8_t am, pm, dm;
  uint8_t uc, ic, pc, duc, dic;
  sampleMap(sample, &am, &uc, &ic, &pm, &pc, &dm, &duc, &dic);
  uint8_t dch = mode == 'U' ? duc : dic;
  Serial.println(F("SWEEP_COLUMNS,SET_DAC_V,U_ADC_V,U_APPROX_V,I_ADC_V,I_APPROX_MA,PHOTO_ADC_V"));
  for (uint16_t n = 0; n < points; n++) {
    float setV = startV + n * stepV;
    if (setV > stopV) setV = stopV;
    if (!writeDac(dm, dch, setV, false)) {
      Serial.println(F("ERR,DAC_WRITE"));
      break;
    }
    delay(settleMs);
    AdsStats u = measureAds(am, uc, GAIN_TWOTHIRDS, 3);
    AdsStats i = measureAds(am, ic, GAIN_TWOTHIRDS, 3);
    AdsStats p = measureAds(pm, pc, GAIN_TWOTHIRDS, 3);
    Serial.print(F("SWEEP_POINT,")); Serial.print(setV, 6); Serial.print(',');
    if (u.ok) Serial.print(u.meanV, 7); else Serial.print(F("FAIL")); Serial.print(',');
    if (u.ok) Serial.print(u.meanV * VOLTAGE_SCALE, 7); else Serial.print(F("FAIL")); Serial.print(',');
    if (i.ok) Serial.print(i.meanV, 7); else Serial.print(F("FAIL")); Serial.print(',');
    if (i.ok) Serial.print(i.meanV / CURRENT_SENSE_GAIN / SHUNT_OHMS * 1000.0f, 7); else Serial.print(F("FAIL")); Serial.print(',');
    if (p.ok) Serial.println(p.meanV, 7); else Serial.println(F("FAIL"));
  }
  zeroAll(false);
  Serial.println(F("SWEEP,END,DAC_ZEROED_AND_LOCKED"));
}

void statusLine() {
  Serial.print(F("STATUS,PCA=")); Serial.print(hasPca ? 1 : 0);
  Serial.print(F(",DAC_ARMED=")); Serial.print(dacArmed ? 1 : 0);
  Serial.print(F(",SWEEP_ARMED=")); Serial.print(sweepArmed ? 1 : 0);
  Serial.print(F(",MONITOR=")); Serial.print(monitorEnabled ? 1 : 0);
  Serial.print(F(",UPTIME_MS=")); Serial.println(millis());
}

void help() {
  Serial.println(F("NMSE OLEDer Deep Debug v2"));
  Serial.println(F("PING | STATUS | RAILS | AUTO | DEEP 20"));
  Serial.println(F("SCAN | MUXSCAN | RAWALL 2/3 20"));
  Serial.println(F("ADS mux ch gain count"));
  Serial.println(F("SAMPLE n | SAMPLESTATS n count photo_gain | ALL"));
  Serial.println(F("MONITOR sample interval_ms | STOP"));
  Serial.println(F("ARM YES | DAC mux ch volts | ZERO | LOCK"));
  Serial.println(F("ARM SWEEP"));
  Serial.println(F("SWEEP sample U|I start stop step settle_ms"));
  Serial.println(F("GAIN: 2/3 1 2 4 8 16"));
}

void normalize(char *line) {
  for (uint8_t i = 0; line[i]; i++) {
    if (line[i] == ',' || line[i] == '\t') line[i] = ' ';
    if (line[i] >= 'a' && line[i] <= 'z') line[i] -= 32;
  }
}

void process(char *line) {
  normalize(line);
  char *t[7];
  for (uint8_t i = 0; i < 7; i++) t[i] = strtok(i == 0 ? line : NULL, " ");
  if (!t[0]) return;

  if (!strcmp(t[0], "PING")) Serial.println(F("OK,PONG,DEEP_DEBUG_V2"));
  else if (!strcmp(t[0], "HELP") || !strcmp(t[0], "?")) help();
  else if (!strcmp(t[0], "STATUS")) statusLine();
  else if (!strcmp(t[0], "RAILS")) printRails();
  else if (!strcmp(t[0], "AUTO")) autoTest();
  else if (!strcmp(t[0], "DEEP")) deepTest(t[1] ? atoi(t[1]) : 20);
  else if (!strcmp(t[0], "SCAN")) scanRoot();
  else if (!strcmp(t[0], "MUXSCAN")) scanMuxes();
  else if (!strcmp(t[0], "RAWALL")) {
    adsGain_t g = GAIN_TWOTHIRDS;
    if (t[1] && !parseGain(t[1], &g)) Serial.println(F("ERR,BAD_GAIN"));
    else rawAll(g, t[2] ? atoi(t[2]) : 20);
  } else if (!strcmp(t[0], "ADS")) {
    adsGain_t g;
    if (!t[1] || !t[2] || !t[3] || !parseGain(t[3], &g)) Serial.println(F("ERR,USE_ADS_MUX_CH_GAIN_COUNT"));
    else printStatsLine(atoi(t[1]), atoi(t[2]), g, t[4] ? atoi(t[4]) : 1);
  } else if (!strcmp(t[0], "SAMPLE")) {
    if (t[1]) readSampleOnce(atoi(t[1])); else Serial.println(F("ERR,USE_SAMPLE_1_TO_4"));
  } else if (!strcmp(t[0], "SAMPLESTATS")) {
    adsGain_t g = GAIN_TWOTHIRDS;
    if (!t[1] || (t[3] && !parseGain(t[3], &g))) Serial.println(F("ERR,USE_SAMPLESTATS_N_COUNT_GAIN"));
    else sampleStats(atoi(t[1]), t[2] ? atoi(t[2]) : 20, g);
  } else if (!strcmp(t[0], "ALL") || !strcmp(t[0], "READ")) readAll();
  else if (!strcmp(t[0], "MONITOR")) {
    if (!t[1] || !t[2] || atoi(t[1]) > 4 || atol(t[2]) < 100 || atol(t[2]) > 60000) Serial.println(F("ERR,MONITOR_SAMPLE_0_TO_4_MS_100_TO_60000"));
    else {
      monitorSample = atoi(t[1]); monitorIntervalMs = atol(t[2]);
      monitorLastMs = 0; monitorEnabled = true; Serial.println(F("OK,MONITOR_STARTED"));
    }
  } else if (!strcmp(t[0], "STOP")) {
    monitorEnabled = false; Serial.println(F("OK,MONITOR_STOPPED"));
  } else if (!strcmp(t[0], "ARM")) {
    if (t[1] && !strcmp(t[1], "YES")) {
      dacArmed = true; sweepArmed = false; armStartedMs = millis(); Serial.println(F("OK,DAC_ARMED_30S"));
    } else if (t[1] && !strcmp(t[1], "SWEEP")) {
      dacArmed = true; sweepArmed = true; armStartedMs = millis(); Serial.println(F("OK,SWEEP_ARMED_30S"));
    } else Serial.println(F("ERR,USE_ARM_YES_OR_ARM_SWEEP"));
  } else if (!strcmp(t[0], "DAC")) {
    if (!dacArmed || millis() - armStartedMs > ARM_TIMEOUT_MS) Serial.println(F("ERR,DAC_LOCKED"));
    else if (!t[1] || !t[2] || !t[3] || !writeDac(atoi(t[1]), atoi(t[2]), atof(t[3]), true)) Serial.println(F("ERR,DAC_WRITE"));
  } else if (!strcmp(t[0], "SWEEP")) {
    if (!t[1] || !t[2] || !t[3] || !t[4] || !t[5] || !t[6]) Serial.println(F("ERR,USE_SWEEP_SAMPLE_MODE_START_STOP_STEP_MS"));
    else runSweep(atoi(t[1]), t[2][0], atof(t[3]), atof(t[4]), atof(t[5]), atol(t[6]));
  } else if (!strcmp(t[0], "ZERO")) zeroAll(true);
  else if (!strcmp(t[0], "LOCK")) {
    dacArmed = false; sweepArmed = false; Serial.println(F("OK,DAC_LOCKED"));
  } else Serial.println(F("ERR,UNKNOWN,TYPE_HELP"));
}

void serviceMonitor() {
  if (!monitorEnabled) return;
  unsigned long now = millis();
  if (monitorLastMs && now - monitorLastMs < monitorIntervalMs) return;
  monitorLastMs = now;
  beginReply();
  if (monitorSample == 0) readAll(); else readSampleOnce(monitorSample);
  endReply();
}

void setup() {
  pinMode(RS485_DIR_PIN, OUTPUT);
  rs485Listen();
  Serial.begin(SERIAL_BAUD);
  Serial.setTimeout(80);
  Wire.begin();
  Wire.setClock(100000);
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(I2C_TIMEOUT_US, true);
#endif
  delay(350);
  hasPca = i2cAddressPresent(PCA_ADDR);
  Serial.println(F("NMSE_OLEDER_DEEP_DEBUG_V2,BOOT"));
  Serial.print(F("PCA9548A,")); Serial.println(hasPca ? F("FOUND") : F("NOT_FOUND,DIRECT_MODE"));
  zeroAll(true);
  Serial.println(F("READY,TYPE_HELP_OR_DEEP_20"));
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      inputLine[inputLength] = 0;
      if (inputLength) {
        beginReply(); process(inputLine); endReply();
      }
      inputLength = 0;
    } else if (inputLength < sizeof(inputLine) - 1) {
      inputLine[inputLength++] = c;
      lastInputByteMs = millis();
    }
    else inputLength = 0;
  }

  // Also accept Arduino Serial Monitor with "No line ending" selected.
  if (inputLength && millis() - lastInputByteMs >= 150) {
    inputLine[inputLength] = 0;
    beginReply(); process(inputLine); endReply();
    inputLength = 0;
  }
  if (dacArmed && millis() - armStartedMs > ARM_TIMEOUT_MS) {
    dacArmed = false;
    sweepArmed = false;
  }
  serviceMonitor();
}
