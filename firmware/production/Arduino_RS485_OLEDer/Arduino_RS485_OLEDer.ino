#include <Wire.h>
#include <Adafruit_MCP4728.h>
#include <Adafruit_ADS1X15.h>
#include <stdlib.h>
#include <string.h>
#include <avr/wdt.h>

// ============================================================
// NMSE OLEDer RS-485 firmware
// ============================================================
//
// Заливать можно на обе Arduino один и тот же файл.
// Отличие только здесь:
//   первая плата: #define DEVICE_ID 1
//   вторая плата: #define DEVICE_ID 2
//
// Команды по RS-485 должны начинаться с адреса платы:
//   1,PING
//   2,READ
//   1,READ,3
//   2,SET,4,0.800,0.200
//   1,PHGAIN,3,16
//   1,ICGAIN,3,4
//   1,ZERO
//
// Ответы тоже начинаются с адреса:
//   1,OK,PONG
//   2,DATA,1,...
//
// Для MAX485/подобных модулей:
//   Arduino TX  -> DI
//   Arduino RX  -> RO
//   Arduino D2  -> DE и /RE, соединённые вместе
//   A -> A общей шины
//   B -> B общей шины
//   GND общий с остальными платами и USB-RS485 адаптером

// ================= DEVICE / RS-485 =================

#define DEVICE_ID 12
#define RS485_DIR_PIN 2
#define REQUIRE_ADDRESS 1
#define PREFIX_RESPONSES 1
#define I2C_TIMEOUT_US 25000UL
#define WATCHDOG_ENABLE 1

// Если нужно видеть BOOT/READY в одиночном Serial Monitor, можно поставить 1.
// На общей RS-485 шине лучше оставить 0, чтобы платы не говорили сами без запроса.
#define SEND_BOOT_MESSAGES 0

// ================= I2C ADDRESSES =================

#define PCA_ADDR 0x70
#define ADS_ADDR 0x48
#define DAC_ADDR 0x60

// ================= PCA9548A CHANNELS =================

#define ADS_12_MUX 0
#define DAC_12_MUX 1
#define ADS_34_MUX 2
#define DAC_34_MUX 3
#define PHOTO_MUX  4

// ================= CONSTANTS =================

#define DAC_VREF      5.0f
#define VOLT_SCALE    3.0f
#define CURRENT_GAIN  10.0f
#define SHUNT_OHMS    50.0f

// ================= OBJECTS =================

Adafruit_MCP4728 mcp;
Adafruit_ADS1115 ads;
bool adsReady = false;
bool dacReady = false;
adsGain_t measurementGain = GAIN_TWOTHIRDS;
adsGain_t photoGainBySample[4] = {
  GAIN_TWOTHIRDS,
  GAIN_TWOTHIRDS,
  GAIN_TWOTHIRDS,
  GAIN_TWOTHIRDS
};
adsGain_t currentGainBySample[4] = {
  GAIN_TWOTHIRDS,
  GAIN_TWOTHIRDS,
  GAIN_TWOTHIRDS,
  GAIN_TWOTHIRDS
};

// ================= RS-485 HELPERS =================

void rs485Listen() {
  digitalWrite(RS485_DIR_PIN, LOW);
  delayMicroseconds(50);
}

void rs485Transmit() {
  digitalWrite(RS485_DIR_PIN, HIGH);
  delayMicroseconds(50);
}

void rs485FlushAndListen() {
  Serial.flush();
  delayMicroseconds(150);
  rs485Listen();
}

void serviceWatchdog() {
#if WATCHDOG_ENABLE
  wdt_reset();
#endif
}

void responsePrefix() {
#if PREFIX_RESPONSES
  Serial.print(DEVICE_ID);
  Serial.print(F(","));
#endif
}

void responseLine(const __FlashStringHelper *text) {
  responsePrefix();
  Serial.println(text);
}

void responseLineText(const char *text) {
  responsePrefix();
  Serial.println(text);
}

bool isNumberToken(const char *s) {
  if (s == NULL || *s == '\0') return false;
  if (*s == '@') s++;
  if (*s == '\0') return false;
  while (*s) {
    if (*s < '0' || *s > '9') return false;
    s++;
  }
  return true;
}

int parseAddressToken(const char *s) {
  if (s == NULL) return -1;
  if (*s == '@') s++;
  return atoi(s);
}

// ================= ADS1115 PGA / GAIN =================

bool parseAdsGain(const char *token, adsGain_t *gain) {
  if (token == NULL || gain == NULL) return false;

  if (
    strcmp(token, "2/3") == 0 ||
    strcmp(token, "TWOTHIRDS") == 0 ||
    strcmp(token, "TWO_THIRDS") == 0 ||
    strcmp(token, "GAIN_TWOTHIRDS") == 0 ||
    strcmp(token, "6.144") == 0 ||
    strcmp(token, "6144") == 0 ||
    strcmp(token, "0") == 0
  ) {
    *gain = GAIN_TWOTHIRDS;
    return true;
  }

  if (
    strcmp(token, "1") == 0 ||
    strcmp(token, "ONE") == 0 ||
    strcmp(token, "GAIN_ONE") == 0 ||
    strcmp(token, "4.096") == 0 ||
    strcmp(token, "4096") == 0
  ) {
    *gain = GAIN_ONE;
    return true;
  }

  if (
    strcmp(token, "2") == 0 ||
    strcmp(token, "TWO") == 0 ||
    strcmp(token, "GAIN_TWO") == 0 ||
    strcmp(token, "2.048") == 0 ||
    strcmp(token, "2048") == 0
  ) {
    *gain = GAIN_TWO;
    return true;
  }

  if (
    strcmp(token, "4") == 0 ||
    strcmp(token, "FOUR") == 0 ||
    strcmp(token, "GAIN_FOUR") == 0 ||
    strcmp(token, "1.024") == 0 ||
    strcmp(token, "1024") == 0
  ) {
    *gain = GAIN_FOUR;
    return true;
  }

  if (
    strcmp(token, "8") == 0 ||
    strcmp(token, "EIGHT") == 0 ||
    strcmp(token, "GAIN_EIGHT") == 0 ||
    strcmp(token, "0.512") == 0 ||
    strcmp(token, "512") == 0
  ) {
    *gain = GAIN_EIGHT;
    return true;
  }

  if (
    strcmp(token, "16") == 0 ||
    strcmp(token, "SIXTEEN") == 0 ||
    strcmp(token, "GAIN_SIXTEEN") == 0 ||
    strcmp(token, "0.256") == 0 ||
    strcmp(token, "256") == 0
  ) {
    *gain = GAIN_SIXTEEN;
    return true;
  }

  return false;
}

float adsGainFullScaleVolts(adsGain_t gain) {
  switch (gain) {
    case GAIN_TWOTHIRDS: return 6.144f;
    case GAIN_ONE:       return 4.096f;
    case GAIN_TWO:       return 2.048f;
    case GAIN_FOUR:      return 1.024f;
    case GAIN_EIGHT:     return 0.512f;
    case GAIN_SIXTEEN:   return 0.256f;
  }
  return 6.144f;
}

void printAdsGainName(adsGain_t gain) {
  switch (gain) {
    case GAIN_TWOTHIRDS: Serial.print(F("2/3")); break;
    case GAIN_ONE:       Serial.print(F("1")); break;
    case GAIN_TWO:       Serial.print(F("2")); break;
    case GAIN_FOUR:      Serial.print(F("4")); break;
    case GAIN_EIGHT:     Serial.print(F("8")); break;
    case GAIN_SIXTEEN:   Serial.print(F("16")); break;
  }
}

// ================= PCA =================

void selectMux(uint8_t ch) {
  if (ch > 7) return;

  Wire.beginTransmission(PCA_ADDR);
  Wire.write(1 << ch);
  Wire.endTransmission();

  delay(2);
}

bool selectMuxChecked(uint8_t ch) {
  if (ch > 7) return false;

  Wire.beginTransmission(PCA_ADDR);
  Wire.write(1 << ch);
  byte err = Wire.endTransmission();

  delay(2);
  return err == 0;
}

void i2cInit() {
  Wire.begin();
  Wire.setClock(100000);
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(I2C_TIMEOUT_US, true);
#endif

  selectMux(ADS_12_MUX);
  adsReady = ads.begin(ADS_ADDR);
  if (adsReady) {
    ads.setGain(measurementGain);
  }

  selectMux(DAC_12_MUX);
  dacReady = mcp.begin(DAC_ADDR);
}

// ================= SAMPLE MAP =================

bool getMap(
  uint8_t sample,
  uint8_t *dacMux,
  uint8_t *dacVch,
  uint8_t *dacIch,
  uint8_t *adsMux,
  uint8_t *adsVch,
  uint8_t *adsIch,
  uint8_t *photoCh
) {
  switch (sample) {
    case 1:
      *dacMux = DAC_12_MUX;
      *dacVch = 0;
      *dacIch = 1;
      *adsMux = ADS_12_MUX;
      *adsVch = 0;
      *adsIch = 1;
      *photoCh = 0;
      return true;

    case 2:
      *dacMux = DAC_12_MUX;
      *dacVch = 2;
      *dacIch = 3;
      *adsMux = ADS_12_MUX;
      *adsVch = 2;
      *adsIch = 3;
      *photoCh = 1;
      return true;

    case 3:
      *dacMux = DAC_34_MUX;
      *dacVch = 0;
      *dacIch = 1;
      *adsMux = ADS_34_MUX;
      *adsVch = 0;
      *adsIch = 1;
      *photoCh = 2;
      return true;

    case 4:
      *dacMux = DAC_34_MUX;
      *dacVch = 2;
      *dacIch = 3;
      *adsMux = ADS_34_MUX;
      *adsVch = 2;
      *adsIch = 3;
      *photoCh = 3;
      return true;
  }

  return false;
}

// ================= DAC =================

uint16_t voltToCode(float v) {
  if (v < 0.0f) v = 0.0f;
  if (v > DAC_VREF) v = DAC_VREF;

  return (uint16_t)((v / DAC_VREF) * 4095.0f);
}

void writeDacChannel(uint8_t ch, uint16_t code) {
  if (!dacReady) return;
  if (ch == 0) mcp.setChannelValue(MCP4728_CHANNEL_A, code);
  if (ch == 1) mcp.setChannelValue(MCP4728_CHANNEL_B, code);
  if (ch == 2) mcp.setChannelValue(MCP4728_CHANNEL_C, code);
  if (ch == 3) mcp.setChannelValue(MCP4728_CHANNEL_D, code);
}

void setSampleDac(uint8_t sample, char mode, float v) {
  uint8_t dacMux, dacVch, dacIch, adsMux, adsVch, adsIch, photoCh;

  if (!getMap(sample, &dacMux, &dacVch, &dacIch, &adsMux, &adsVch, &adsIch, &photoCh)) {
    responseLine(F("ERR,BAD_SAMPLE"));
    return;
  }

  uint8_t dacCh;

  if (mode == 'V') {
    dacCh = dacVch;
  } else if (mode == 'I') {
    dacCh = dacIch;
  } else {
    responseLine(F("ERR,BAD_MODE"));
    return;
  }

  uint16_t code = voltToCode(v);

  if (!dacReady) {
    responseLine(F("ERR,DAC_NOT_READY"));
    return;
  }

  if (!selectMuxChecked(dacMux)) {
    responseLine(F("ERR,I2C_MUX"));
    return;
  }

  writeDacChannel(dacCh, code);

  responsePrefix();
  Serial.print(F("OK,DAC,"));
  Serial.print(sample);
  Serial.print(F(","));
  Serial.print(mode);
  Serial.print(F(","));
  Serial.print(v, 5);
  Serial.print(F(","));
  Serial.println(code);
}

void setSampleVI(uint8_t sample, float vDac, float iDac) {
  setSampleDac(sample, 'V', vDac);
  setSampleDac(sample, 'I', iDac);
}

void zeroAll() {
  for (uint8_t s = 1; s <= 4; s++) {
    setSampleDac(s, 'V', 0.0f);
    setSampleDac(s, 'I', 0.0f);
  }

  responseLine(F("OK,ZERO"));
}

// ================= ADS =================

bool readAdsVoltsWithGain(uint8_t mux, uint8_t ch, adsGain_t gain, float *volts) {
  if (!adsReady) return false;
  if (!selectMuxChecked(mux)) return false;

  ads.setGain(gain);
  int16_t raw = ads.readADC_SingleEnded(ch);
  *volts = ads.computeVolts(raw);
  return true;
}

bool readAdsVolts(uint8_t mux, uint8_t ch, float *volts) {
  return readAdsVoltsWithGain(mux, ch, measurementGain, volts);
}

void readSample(uint8_t sample) {
  uint8_t dacMux, dacVch, dacIch, adsMux, adsVch, adsIch, photoCh;

  if (!getMap(sample, &dacMux, &dacVch, &dacIch, &adsMux, &adsVch, &adsIch, &photoCh)) {
    responseLine(F("ERR,BAD_SAMPLE"));
    return;
  }

  float uRaw = 0.0f;
  float iRaw = 0.0f;
  float pRaw = 0.0f;

  serviceWatchdog();
  if (!readAdsVolts(adsMux, adsVch, &uRaw)) {
    responsePrefix();
    Serial.print(F("ERR,ADS_READ,U,"));
    Serial.println(sample);
    return;
  }

  serviceWatchdog();
  if (!readAdsVoltsWithGain(adsMux, adsIch, currentGainBySample[sample - 1], &iRaw)) {
    responsePrefix();
    Serial.print(F("ERR,ADS_READ,I,"));
    Serial.println(sample);
    return;
  }

  serviceWatchdog();
  if (!readAdsVoltsWithGain(PHOTO_MUX, photoCh, photoGainBySample[sample - 1], &pRaw)) {
    responsePrefix();
    Serial.print(F("ERR,ADS_READ,P,"));
    Serial.println(sample);
    return;
  }

  float uApprox = uRaw * VOLT_SCALE;
  float iApprox_mA = iRaw / CURRENT_GAIN / SHUNT_OHMS * 1000.0f;

  // Формат:
  // DEVICE_ID,DATA,localSample,millis,Uraw_V,Iraw_V,PhotoRaw_V,Uapprox_V,Iapprox_mA

  responsePrefix();
  Serial.print(F("DATA,"));
  Serial.print(sample);
  Serial.print(F(","));
  Serial.print(millis());
  Serial.print(F(","));
  Serial.print(uRaw, 6);
  Serial.print(F(","));
  Serial.print(iRaw, 6);
  Serial.print(F(","));
  Serial.print(pRaw, 6);
  Serial.print(F(","));
  Serial.print(uApprox, 6);
  Serial.print(F(","));
  Serial.println(iApprox_mA, 6);
}

void readAllSamples() {
  for (uint8_t s = 1; s <= 4; s++) {
    serviceWatchdog();
    readSample(s);
  }

  responseLine(F("END"));
}

void setPhotoGain(uint8_t sample, const char *gainToken) {
  if (sample < 1 || sample > 4) {
    responseLine(F("ERR,BAD_SAMPLE"));
    return;
  }

  adsGain_t gain;
  if (!parseAdsGain(gainToken, &gain)) {
    responseLine(F("ERR,BAD_GAIN"));
    return;
  }

  photoGainBySample[sample - 1] = gain;

  responsePrefix();
  Serial.print(F("OK,PHGAIN,"));
  Serial.print(sample);
  Serial.print(F(","));
  printAdsGainName(gain);
  Serial.print(F(",FS="));
  Serial.println(adsGainFullScaleVolts(gain), 3);
}

void setCurrentAdcGain(uint8_t sample, const char *gainToken) {
  if (sample < 1 || sample > 4) {
    responseLine(F("ERR,BAD_SAMPLE"));
    return;
  }

  adsGain_t gain;
  if (!parseAdsGain(gainToken, &gain)) {
    responseLine(F("ERR,BAD_GAIN"));
    return;
  }

  currentGainBySample[sample - 1] = gain;

  responsePrefix();
  Serial.print(F("OK,ICGAIN,"));
  Serial.print(sample);
  Serial.print(F(","));
  printAdsGainName(gain);
  Serial.print(F(",FS="));
  Serial.println(adsGainFullScaleVolts(gain), 3);
}

// ================= LOW LEVEL DEBUG =================

void readRawAds(uint8_t mux, uint8_t ch) {
  if (mux > 7 || ch > 3) {
    responseLine(F("ERR,BAD_ARG"));
    return;
  }

  float v = 0.0f;
  if (!readAdsVolts(mux, ch, &v)) {
    responseLine(F("ERR,ADS_READ"));
    return;
  }

  responsePrefix();
  Serial.print(F("RAWADS,"));
  Serial.print(mux);
  Serial.print(F(","));
  Serial.print(ch);
  Serial.print(F(","));
  Serial.println(v, 6);
}

void scanMux() {
  for (uint8_t mux = 0; mux < 8; mux++) {
    serviceWatchdog();
    if (!selectMuxChecked(mux)) {
      responsePrefix();
      Serial.print(F("ERR,MUX,"));
      Serial.println(mux);
      continue;
    }

    responsePrefix();
    Serial.print(F("MUX,"));
    Serial.println(mux);

    for (byte addr = 1; addr < 127; addr++) {
      if (addr == PCA_ADDR) continue;

      Wire.beginTransmission(addr);
      byte err = Wire.endTransmission();

      if (err == 0) {
        responsePrefix();
        Serial.print(F("I2C,"));
        Serial.print(mux);
        Serial.print(F(",0x"));
        if (addr < 16) Serial.print(F("0"));
        Serial.println(addr, HEX);
      }
    }
  }

  responseLine(F("END"));
}

// ================= HELP =================

void help() {
  responseLine(F("Commands with address:"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",PING"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",HELP"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",SCAN"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",READ"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",READ,1"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",VDAC,1,0.800"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",IDAC,1,0.200"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",SET,1,0.800,0.200"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",PHGAIN,1,16"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",ICGAIN,1,4"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",ZERO"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",I2CINIT"));
  responsePrefix();
  Serial.print(DEVICE_ID);
  Serial.println(F(",STATUS"));
}

// ================= COMMAND PROCESSING =================

void processCommand(char *cmd, char *t1, char *t2, char *t3) {
  if (cmd == NULL) return;

  if (strcmp(cmd, "PING") == 0) {
    responseLine(F("OK,PONG"));
  }

  else if (strcmp(cmd, "HELP") == 0) {
    help();
  }

  else if (strcmp(cmd, "SCAN") == 0) {
    scanMux();
  }

  else if (strcmp(cmd, "I2CINIT") == 0) {
    i2cInit();
    responsePrefix();
    Serial.print(F("OK,I2CINIT,ADS="));
    Serial.print(adsReady ? 1 : 0);
    Serial.print(F(",DAC="));
    Serial.println(dacReady ? 1 : 0);
  }

  else if (strcmp(cmd, "STATUS") == 0) {
    responsePrefix();
    Serial.print(F("STATUS,ADS="));
    Serial.print(adsReady ? 1 : 0);
    Serial.print(F(",DAC="));
    Serial.print(dacReady ? 1 : 0);
    Serial.print(F(",PHGAIN="));
    for (uint8_t s = 0; s < 4; s++) {
      if (s > 0) Serial.print(F(";"));
      printAdsGainName(photoGainBySample[s]);
    }
    Serial.print(F(",ICGAIN="));
    for (uint8_t s = 0; s < 4; s++) {
      if (s > 0) Serial.print(F(";"));
      printAdsGainName(currentGainBySample[s]);
    }
    Serial.print(F(",MS="));
    Serial.println(millis());
  }

  else if (strcmp(cmd, "RD") == 0 || strcmp(cmd, "READ") == 0) {
    if (t1 == NULL) {
      readAllSamples();
    } else {
      readSample((uint8_t)atoi(t1));
    }
  }

  else if (strcmp(cmd, "RA") == 0) {
    readAllSamples();
  }

  else if (strcmp(cmd, "VDAC") == 0 || strcmp(cmd, "V") == 0) {
    if (t1 == NULL || t2 == NULL) {
      responseLine(F("ERR,USE_VDAC"));
    } else {
      setSampleDac((uint8_t)atoi(t1), 'V', atof(t2));
    }
  }

  else if (strcmp(cmd, "IDAC") == 0 || strcmp(cmd, "I") == 0) {
    if (t1 == NULL || t2 == NULL) {
      responseLine(F("ERR,USE_IDAC"));
    } else {
      setSampleDac((uint8_t)atoi(t1), 'I', atof(t2));
    }
  }

  else if (strcmp(cmd, "SET") == 0 || strcmp(cmd, "VI") == 0) {
    if (t1 == NULL || t2 == NULL || t3 == NULL) {
      responseLine(F("ERR,USE_SET"));
    } else {
      setSampleVI((uint8_t)atoi(t1), atof(t2), atof(t3));
    }
  }

  else if (strcmp(cmd, "PHGAIN") == 0 || strcmp(cmd, "PGAIN") == 0 || strcmp(cmd, "PHOTO_GAIN") == 0) {
    if (t1 == NULL || t2 == NULL) {
      responseLine(F("ERR,USE_PHGAIN"));
    } else {
      setPhotoGain((uint8_t)atoi(t1), t2);
    }
  }

  else if (strcmp(cmd, "ICGAIN") == 0 || strcmp(cmd, "IADCGAIN") == 0 || strcmp(cmd, "CURRENT_GAIN") == 0) {
    if (t1 == NULL || t2 == NULL) {
      responseLine(F("ERR,USE_ICGAIN"));
    } else {
      setCurrentAdcGain((uint8_t)atoi(t1), t2);
    }
  }

  else if (strcmp(cmd, "ZERO") == 0) {
    zeroAll();
  }

  else if (strcmp(cmd, "RAWADS") == 0) {
    if (t1 == NULL || t2 == NULL) {
      responseLine(F("ERR,USE_RAWADS"));
    } else {
      readRawAds((uint8_t)atoi(t1), (uint8_t)atoi(t2));
    }
  }

  else {
    responseLine(F("ERR,UNKNOWN"));
  }
}

// ================= SETUP =================

void setup() {
#if WATCHDOG_ENABLE
  wdt_disable();
#endif

  pinMode(RS485_DIR_PIN, OUTPUT);
  rs485Listen();

  Serial.begin(9600);
  Serial.setTimeout(80);

  delay(300);

#if SEND_BOOT_MESSAGES
  delay(DEVICE_ID * 80);
  rs485Transmit();
  responseLine(F("BOOT"));
#endif

  i2cInit();

#if SEND_BOOT_MESSAGES
  if (adsReady) responseLine(F("OK,ADS"));
  else responseLine(F("ERR,ADS_INIT"));

  if (dacReady) responseLine(F("OK,DAC"));
  else responseLine(F("ERR,DAC_INIT"));

  responseLine(F("READY"));
  rs485FlushAndListen();
#endif

#if WATCHDOG_ENABLE
  wdt_enable(WDTO_2S);
#endif
}

// ================= LOOP =================

void loop() {
  serviceWatchdog();

  if (!Serial.available()) return;

  char line[80];

  int len = Serial.readBytesUntil('\n', line, sizeof(line) - 1);
  line[len] = '\0';

  for (int i = 0; i < len; i++) {
    if (line[i] == '\r' || line[i] == '\n') {
      line[i] = '\0';
      break;
    }
  }

  for (int i = 0; line[i]; i++) {
    if (line[i] >= 'a' && line[i] <= 'z') {
      line[i] = line[i] - 'a' + 'A';
    }
  }

  char *t0 = strtok(line, " ,\t");
  char *t1 = strtok(NULL, " ,\t");
  char *t2 = strtok(NULL, " ,\t");
  char *t3 = strtok(NULL, " ,\t");
  char *t4 = strtok(NULL, " ,\t");

  if (t0 == NULL) return;

  char *cmd = t0;
  char *a1 = t1;
  char *a2 = t2;
  char *a3 = t3;

  if (isNumberToken(t0)) {
    int addr = parseAddressToken(t0);
    if (addr != DEVICE_ID) {
      return; // чужая команда, молчим
    }

    cmd = t1;
    a1 = t2;
    a2 = t3;
    a3 = t4;
  } else {
#if REQUIRE_ADDRESS
    return; // на RS-485 без адреса не отвечаем, чтобы две платы не спорили
#endif
  }

  rs485Transmit();
  processCommand(cmd, a1, a2, a3);
  rs485FlushAndListen();
}
