#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <INA226_WE.h>
#include <Adafruit_BME680.h>
#include <Adafruit_Sensor.h>

// ============================================================
// WIFI & NTP CONFIGURATION
// ============================================================
#define PRIMARY_SSID     "SKYYRMR7"
#define PRIMARY_PASS     "K2xWvDFZkuCh"

#define FALLBACK_SSID    "CyDen-Guests"
#define FALLBACK_PASS    "29336Visit2#02"

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;

const int   daylightOffset_sec = 3600;

// ============================================================
// I2C & OLED CONFIGURATION
// ============================================================
#define SDA_PIN       21
#define SCL_PIN       22
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_I2C_ADDRESS 0x3D
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================================================
// SENSOR & PWM PINS CONFIGURATION
// ============================================================
#define C12880MA_CLK_PIN      16
#define C12880MA_ST_PIN       17
#define C12880MA_VIDEO_PIN    34
#define SD_CS_PIN             5

#define PWM_PIN               15

#define GY30_ADDRESS          0x23

#define SPECTROGRAPH_CHANNELS 288

// ============================================================
// HAMAMATSU C12880MA WAVELENGTH CALIBRATION (Serial: 21A02890)
// ============================================================
const float CAL_A0 =  3.02539467e+02;
const float CAL_B1 =  2.70472314e+00;
const float CAL_B2 = -1.10776346e-03;
const float CAL_B3 = -8.62290205e-06;
const float CAL_B4 =  1.42568072e-08;
const float CAL_B5 = -2.66396197e-12;
float calibratedWavelengths[SPECTROGRAPH_CHANNELS];

// ============================================================
// SENSOR OBJECTS & STATE VARIABLES
// ============================================================
INA226_WE ina226;
Adafruit_BME680 bme;

uint16_t spectrographData[SPECTROGRAPH_CHANNELS];
uint16_t currentMaxIntensity = 0;
uint16_t currentMinIntensity = 4095;

// Auto-Integration State (Safe target = 2100 to avoid 2700+ optical clipping plateau)
bool autoIntegrationEnabled = true;
const uint16_t TARGET_MAX_INTENSITY = 2100;

const unsigned long CLK_PERIOD_US = 2;
const unsigned long CLK_HALF_US   = 1;

unsigned long currentIntegrationTimeUs = 20000;
const unsigned long minIntegrationTimeUs = 11000;
const unsigned long maxIntegrationTimeUs = 1000000;

// MPPT & Environmental Variables
float V_old = 0.0;

float I_old = 0.0;

float P_old = 0.0;

int current_duty_cycle = 100;
unsigned long last_mppt_time = 0;
const unsigned long MPPT_INTERVAL_MS = 1000;

bool mpptEnabled = true;
const float LUX_MPPT_THRESHOLD = 50.0;
const int DUTY_PARK = 50;

const int PWM_FREQUENCY = 1000;
const int PWM_RESOLUTION_BITS = 12;

bool sdCardInitialized = false;
volatile unsigned int sdCardWriteCount = 0;
const char* ALL_SENSORS_DATA_FILENAME = "/SENSOR_LOG.CSV";

// 30-Second SD Buffer Structure with Saturation Status Flag
struct SensorLogRecord {
String timestamp;
float voltage;
float current;
float power;
int dutyCycle;
float temp;
float pressure;
float humidity;
float gas;
float lux;
unsigned long integrationTimeUs;
uint16_t maxAdc;
String saturationStatus;
uint16_t spectrum[SPECTROGRAPH_CHANNELS];
};

const int BUFFER_SIZE = 30;
SensorLogRecord sdBuffer[BUFFER_SIZE];
int bufferIndex = 0;
unsigned long lastBatchFlushTime = 0;

float bmeTemp, bmePressure, bmeHumidity, bmeGasResistance;
float gy30Illuminance;

unsigned long lastPageChangeTime = 0;
int currentPage = 0;

// ============================================================
// FUNCTION PROTOTYPES
// ============================================================
void connectWiFi();
String getFormattedTimestamp();
void clockPulse();
void readSpectrometer();
void updateAutoIntegration();
void computeWavelengths();
void displaySpectrometerStatsPage();
void displaySpectrographPage();
void displayMPPTPage(float V, float I, float P, int dc);
void displayEnvironmentalPage();
void writeCSVHeader();
void bufferOrFlushSensorData(float mpptV, float mpptI, float mpptP, int mpptDC,
float bmeT, float bmeP, float bmeH, float bmeG,
float gy30Lux);
void flushBufferToSD();
void streamCSVToSerial(const char* recordType, float mpptV, float mpptI, float mpptP, int mpptDC,
float bmeT, float bmeP, float bmeH, float bmeG,
float gy30Lux);
void executeIVSweep();
void handleSerialCommands();

String getSaturationStatus() {
return (currentMaxIntensity >= 2700) ? "SATURATED" : "UNSATURATED";
}

// ============================================================
// SETUP
// ============================================================
void setup() {
Serial.begin(115200);
delay(500);

Serial.println(F(""));
Serial.println(F(" ESP32 C12880MA + MPPT + WAVELENGTH + SD "));
Serial.println(F(""));

computeWavelengths();

Wire.begin(SDA_PIN, SCL_PIN);
if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
Serial.println(F("[ERROR] OLED allocation failed at 0x3D!"));
}
display.clearDisplay();
display.setTextColor(SSD1306_WHITE);
display.setTextSize(1);
display.setCursor(0, 0);
display.println(F("Initializing System..."));
display.display();
delay(1000);

pinMode(C12880MA_CLK_PIN, OUTPUT);
pinMode(C12880MA_ST_PIN, OUTPUT);
digitalWrite(C12880MA_CLK_PIN, LOW);
digitalWrite(C12880MA_ST_PIN, LOW);

// FIX 1: Set correct 11dB attenuation for full 0-3.3V ADC dynamic range
analogSetPinAttenuation(C12880MA_VIDEO_PIN, ADC_11db);
analogReadResolution(12);

if (!ina226.init()) {
Serial.println(F("[ERROR] Failed to find INA226 sensor!"));
} else {
ina226.setResistorRange(10.0, 0.1);
ina226.setConversionTime(CONV_TIME_1100);
ina226.setAverage(AVERAGE_16);
ina226.setMeasureMode(CONTINUOUS);
}

if (!bme.begin()) {
Serial.println(F("[ERROR] Could not find a valid BME680 sensor!"));
} else {
bme.setTemperatureOversampling(BME680_OS_8X);
bme.setHumidityOversampling(BME680_OS_2X);
bme.setPressureOversampling(BME680_OS_4X);
bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
bme.setGasHeater(320, 150);
}

Wire.beginTransmission(GY30_ADDRESS);
if (Wire.endTransmission() == 0) {
Wire.beginTransmission(GY30_ADDRESS);
Wire.write(0x01);
Wire.endTransmission();
Wire.beginTransmission(GY30_ADDRESS);
Wire.write(0x10);
Wire.endTransmission();
}

if (!SD.begin(SD_CS_PIN)) {
Serial.println(F("[WARNING] SD Card Mount Failed."));
} else {
sdCardInitialized = true;
writeCSVHeader();
}

analogWriteResolution(PWM_PIN, PWM_RESOLUTION_BITS);
analogWriteFrequency(PWM_PIN, PWM_FREQUENCY);
analogWrite(PWM_PIN, current_duty_cycle);

connectWiFi();
lastBatchFlushTime = millis();
Serial.println(F("[INIT] Setup complete. Entering main loop."));
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
unsigned long loop_start_time = millis();

handleSerialCommands();

readSpectrometer();
if (autoIntegrationEnabled) {
updateAutoIntegration();
}

if (bme.performReading()) {
bmeTemp = bme.temperature;
bmePressure = bme.pressure;
bmeHumidity = bme.humidity;
bmeGasResistance = bme.gas_resistance;
}

if (Wire.requestFrom(GY30_ADDRESS, 2) == 2) {
uint16_t rawLux = Wire.read();
rawLux = (rawLux << 8) | Wire.read();
gy30Illuminance = rawLux / 1.2;
} else {
gy30Illuminance = -1.0;
}

if (gy30Illuminance <= LUX_MPPT_THRESHOLD) {
if (mpptEnabled) {
analogWrite(PWM_PIN, DUTY_PARK);
V_old = 0.0;
I_old = 0.0;
P_old = 0.0;
mpptEnabled = false;
}
} else {
if (!mpptEnabled) {
analogWrite(PWM_PIN, DUTY_PARK);
V_old = 0.0;
I_old = 0.0;
P_old = 0.0;
mpptEnabled = true;
}
}

// MPPT P&O Loop
if (mpptEnabled && (loop_start_time - last_mppt_time >= MPPT_INTERVAL_MS)) {
last_mppt_time = loop_start_time;

float V_raw = ina226.getBusVoltage_V();
float I_raw = ina226.getCurrent_mA() / 1000.0;
float P_raw = V_raw * I_raw;

if (P_raw < 0) P_raw = 0;
if (I_raw < 0) I_raw = 0;
if (V_raw < 0) V_raw = 0;

const float alpha = 0.4f;
float V_new = (alpha * V_raw) + ((1.0f - alpha) * V_old);
float I_new = (alpha * I_raw) + ((1.0f - alpha) * I_old);
float P_new = (alpha * P_raw) + ((1.0f - alpha) * P_old);

float delta_P = P_new - P_old;
const float POWER_DEADBAND = 0.0000015f; 

static int last_direction = 1; 
static int lock_counter = 0;

if (abs(delta_P) < POWER_DEADBAND) {
  lock_counter++;
  if (lock_counter > 3) {
    V_old = V_new;
    I_old = I_new;
    P_old = P_new;
    
    streamCSVToSerial("LIVE", V_old, I_old, P_old, current_duty_cycle,
                      bmeTemp, bmePressure, bmeHumidity, bmeGasResistance,
                      gy30Illuminance);
    
    bufferOrFlushSensorData(V_old, I_old, P_old, current_duty_cycle,
                            bmeTemp, bmePressure, bmeHumidity, bmeGasResistance,
                            gy30Illuminance);
    
    // FIX 2: Removed blocking delay(1000 - elapsed) that caused main loop stalling
    return; 
  }
} else {
  lock_counter = 0; 
  
  if (delta_P > 0) {
    if ((V_new > V_old) != (P_new > P_old)) {
      last_direction = -1;
    } else {
      last_direction = 1;
    }
  } else {
    if ((V_new > V_old) != (P_new > P_old)) {
      last_direction = 1;
    } else {
      last_direction = -1;
    }
  }

  const int FINE_STEP = 10; 
  current_duty_cycle += (last_direction * FINE_STEP);

  int max_duty_cycle = (1 << PWM_RESOLUTION_BITS) - 1;
  current_duty_cycle = constrain(current_duty_cycle, FINE_STEP, max_duty_cycle - FINE_STEP);
  analogWrite(PWM_PIN, current_duty_cycle);
}

V_old = V_new;
I_old = I_new;
P_old = P_new;


}

streamCSVToSerial("LIVE", V_old, I_old, P_old, current_duty_cycle,
bmeTemp, bmePressure, bmeHumidity, bmeGasResistance,
gy30Illuminance);

if (gy30Illuminance > LUX_MPPT_THRESHOLD) {
bufferOrFlushSensorData(V_old, I_old, P_old, current_duty_cycle,
bmeTemp, bmePressure, bmeHumidity, bmeGasResistance,
gy30Illuminance);
}

if (millis() - lastPageChangeTime >= 5000) {
currentPage = (currentPage + 1) % 4;
lastPageChangeTime = millis();
}

if (currentPage == 0) {
displaySpectrometerStatsPage();
} else if (currentPage == 1) {
displaySpectrographPage();
} else if (currentPage == 2) {
displayMPPTPage(V_old, I_old, P_old, current_duty_cycle);
} else {
displayEnvironmentalPage();
}

// FIX 2: Removed forced end-of-loop delay to keep execution responsive
}

// ============================================================
// ON-CHIP WAVELENGTH CALIBRATION COMPUTATION
// ============================================================
void computeWavelengths() {
for (int i = 0; i < SPECTROGRAPH_CHANNELS; i++) {
float pix = (float)i;
calibratedWavelengths[i] = CAL_A0 +
CAL_B1 * pix +
CAL_B2 * (pix * pix) +
CAL_B3 * (pix * pix * pix) +
CAL_B4 * (pix * pix * pix * pix) +
CAL_B5 * (pix * pix * pix * pix * pix);
}
}

// ============================================================
// AUTO-INTEGRATION ROUTINE (Stable Deadband & Safe Gain)
// ============================================================
void updateAutoIntegration() {
if (currentMaxIntensity < 50) {
currentIntegrationTimeUs = min(maxIntegrationTimeUs, currentIntegrationTimeUs * 2);
return;
}

// FIX 3: Added deadband and smoothed gain factor to stop oscillation
long error = (long)TARGET_MAX_INTENSITY - (long)currentMaxIntensity;
if (abs(error) > 150) {
float adjustmentRatio = (float)TARGET_MAX_INTENSITY / (float)currentMaxIntensity;
long newTime = currentIntegrationTimeUs + (long)((float)currentIntegrationTimeUs * (adjustmentRatio - 1.0) * 0.5);
currentIntegrationTimeUs = constrain(newTime, minIntegrationTimeUs, maxIntegrationTimeUs);
}
}

// ============================================================
// 30-SECOND BATCH BUFFERING & SD WRITE REDUCTION
// ============================================================
void bufferOrFlushSensorData(float mpptV, float mpptI, float mpptP, int mpptDC,
float bmeT, float bmeP, float bmeH, float bmeG,
float gy30Lux) {
sdBuffer[bufferIndex].timestamp = getFormattedTimestamp();
sdBuffer[bufferIndex].voltage = mpptV;
sdBuffer[bufferIndex].current = mpptI;
sdBuffer[bufferIndex].power = mpptP;
sdBuffer[bufferIndex].dutyCycle = mpptDC;
sdBuffer[bufferIndex].temp = bmeT;
sdBuffer[bufferIndex].pressure = bmeP;
sdBuffer[bufferIndex].humidity = bmeH;
sdBuffer[bufferIndex].gas = bmeG;
sdBuffer[bufferIndex].lux = gy30Lux;
sdBuffer[bufferIndex].integrationTimeUs = currentIntegrationTimeUs;
sdBuffer[bufferIndex].maxAdc = currentMaxIntensity;
sdBuffer[bufferIndex].saturationStatus = getSaturationStatus();

for (int i = 0; i < SPECTROGRAPH_CHANNELS; i++) {
sdBuffer[bufferIndex].spectrum[i] = spectrographData[i];
}

bufferIndex++;

if (bufferIndex >= BUFFER_SIZE || (millis() - lastBatchFlushTime >= 30000)) {
flushBufferToSD();
}
}

void flushBufferToSD() {
if (!sdCardInitialized || bufferIndex == 0) return;

File dataFile = SD.open(ALL_SENSORS_DATA_FILENAME, FILE_APPEND);
if (dataFile) {
for (int b = 0; b < bufferIndex; b++) {
dataFile.print('"');
dataFile.print(sdBuffer[b].timestamp);
dataFile.print('"');
dataFile.print(',');
dataFile.print(sdBuffer[b].voltage, 4);
dataFile.print(',');
dataFile.print(sdBuffer[b].current, 6);
dataFile.print(',');
dataFile.print(sdBuffer[b].power, 6);
dataFile.print(',');
dataFile.print(sdBuffer[b].dutyCycle);
dataFile.print(',');
dataFile.print(sdBuffer[b].temp, 1);
dataFile.print(',');
dataFile.print(sdBuffer[b].pressure / 100.0, 1);
dataFile.print(',');
dataFile.print(sdBuffer[b].humidity, 1);
dataFile.print(',');
dataFile.print(sdBuffer[b].gas, 0);
dataFile.print(',');
dataFile.print(sdBuffer[b].lux, 1);
dataFile.print(',');
dataFile.print(sdBuffer[b].integrationTimeUs);
dataFile.print(',');
dataFile.print(sdBuffer[b].maxAdc);
dataFile.print(',');
dataFile.print(sdBuffer[b].saturationStatus);

  for (int i = 0; i < SPECTROGRAPH_CHANNELS; i++) {
    dataFile.print(',');
    dataFile.print(sdBuffer[b].spectrum[i]);
  }
  dataFile.println();
  sdCardWriteCount++;
}
dataFile.close();


}

bufferIndex = 0;
lastBatchFlushTime = millis();
}

// ============================================================
// SERIAL COMMAND & SWEEP HANDLER
// ============================================================
void handleSerialCommands() {
if (Serial.available() <= 0) return;

String input = Serial.readStringUntil('\n');
input.trim();
input.toUpperCase();

if (input.equals("SWEEP")) {
executeIVSweep();
}
}

void executeIVSweep() {
Serial.println(F("[SWEEP_START] Initiating full I-V curve sweep..."));
mpptEnabled = false;

int max_dc = (1 << PWM_RESOLUTION_BITS) - 1;
int step = 100;

for (int dc = 0; dc <= max_dc; dc += step) {
analogWrite(PWM_PIN, dc);
delay(50);

float sweepV = ina226.getBusVoltage_V();
float sweepI = ina226.getCurrent_mA() / 1000.0;
float sweepP = sweepV * sweepI;

readSpectrometer();

streamCSVToSerial("SWEEP_POINT", sweepV, sweepI, sweepP, dc,
                  bmeTemp, bmePressure, bmeHumidity, bmeGasResistance,
                  gy30Illuminance);


}

analogWrite(PWM_PIN, current_duty_cycle);
mpptEnabled = true;
Serial.println(F("[SWEEP_END] I-V curve sweep complete."));
}

// ============================================================
// WIFI & NTP FUNCTIONS
// ============================================================
void connectWiFi() {
WiFi.begin(PRIMARY_SSID, PRIMARY_PASS);
unsigned long startAttemptTime = millis();
while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
delay(500);
}
if (WiFi.status() != WL_CONNECTED) {
WiFi.begin(FALLBACK_SSID, FALLBACK_PASS);
startAttemptTime = millis();
while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
delay(500);
}
}
if (WiFi.status() == WL_CONNECTED) {
configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}
}

String getFormattedTimestamp() {
struct tm timeinfo;
struct timeval tv;
gettimeofday(&tv, NULL);
if (!getLocalTime(&timeinfo)) {
return String(millis());
}
char timeBuffer[64];
int millisVal = (tv.tv_usec / 1000);
snprintf(timeBuffer, sizeof(timeBuffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, millisVal);
return String(timeBuffer);
}

// ============================================================
// C12880MA TIMING & READOUT ROUTINE
// ============================================================
void clockPulse() {
digitalWrite(C12880MA_CLK_PIN, HIGH);
delayMicroseconds(CLK_HALF_US);
digitalWrite(C12880MA_CLK_PIN, LOW);
delayMicroseconds(CLK_HALF_US);
}

void readSpectrometer() {
const unsigned long integrationClockComponent = 48UL * CLK_PERIOD_US;
unsigned long stHighUs = 0;

if (currentIntegrationTimeUs > integrationClockComponent) {
stHighUs = currentIntegrationTimeUs - integrationClockComponent;
} else {
stHighUs = 1;
}

digitalWrite(C12880MA_CLK_PIN, LOW);
digitalWrite(C12880MA_ST_PIN, LOW);
delayMicroseconds(5);

digitalWrite(C12880MA_ST_PIN, HIGH);

// FIX 4: Break long delayMicroseconds into yields for exposures >= 15ms
if (stHighUs >= 15000) {
delay(stHighUs / 1000);
delayMicroseconds(stHighUs % 1000);
} else {
delayMicroseconds(stHighUs);
}

digitalWrite(C12880MA_ST_PIN, LOW);

clockPulse();

for (int i = 0; i < 86; i++) {
clockPulse();
}

currentMaxIntensity = 0;
currentMinIntensity = 4095;

for (int i = 0; i < SPECTROGRAPH_CHANNELS; i++) {
spectrographData[i] = analogRead(C12880MA_VIDEO_PIN);

if (spectrographData[i] > currentMaxIntensity) {
  currentMaxIntensity = spectrographData[i];
}
if (spectrographData[i] < currentMinIntensity) {
  currentMinIntensity = spectrographData[i];
}

clockPulse();


}

digitalWrite(C12880MA_ST_PIN, HIGH);
for (int i = 0; i < 7; i++) {
clockPulse();
}

digitalWrite(C12880MA_CLK_PIN, LOW);
digitalWrite(C12880MA_ST_PIN, LOW);
}

// ============================================================
// DISPLAY & SERIAL ROUTINES
// ============================================================
void displaySpectrometerStatsPage() {
display.clearDisplay();
display.setTextColor(SSD1306_WHITE);
display.setTextSize(1);

display.setCursor(0, 0);
display.print(F("Max/Min: "));
display.print(currentMaxIntensity);
display.print(F("/"));
display.print(currentMinIntensity);

display.setCursor(0, 16);
display.print(F("Auto Int: "));
if (currentIntegrationTimeUs >= 1000000UL) {
  display.print(currentIntegrationTimeUs / 1000000.0, 1);
  display.print(F("s"));
} else {
  display.print(currentIntegrationTimeUs / 1000.0, 1);
  display.print(F("ms"));
}

display.setCursor(0, 32);
display.print(F("SD Writes: "));
if (sdCardInitialized) {
  display.print(sdCardWriteCount);
} else {
  display.print(F("N/A"));
}

display.display();


}

void displaySpectrographPage() {
display.clearDisplay();
display.setTextColor(SSD1306_WHITE);
display.setTextSize(1);

display.setCursor(0, 0);
display.print(F("Auto:"));

if (currentIntegrationTimeUs >= 1000000UL) {
  display.print(currentIntegrationTimeUs / 1000000.0, 1);
  display.print(F("s"));
} else {
  display.print(currentIntegrationTimeUs / 1000.0, 1);
  display.print(F("ms"));
}

display.setCursor(68, 0);
display.print(F("Max:"));
display.print(currentMaxIntensity);

for (int x = 0; x < 128; x++) {
  int specIndex = map(x, 0, 127, 0, SPECTROGRAPH_CHANNELS - 1);
  int barHeight = 0;

  if (currentMaxIntensity > currentMinIntensity) {
    barHeight = map(spectrographData[specIndex], currentMinIntensity, currentMaxIntensity, 0, 45);
  }
  barHeight = constrain(barHeight, 0, 45);

  display.drawFastVLine(x, 50 - barHeight, barHeight, SSD1306_WHITE);
}
display.display();


}

void displayMPPTPage(float V, float I, float P, int dc) {
display.clearDisplay();
display.setTextColor(SSD1306_WHITE);
display.setTextSize(1);

display.setCursor(0, 0);
display.println(F("MPPT Data:"));

display.setCursor(0, 16);
display.print(F("V: "));
display.print(V, 4);
display.println(F(" V"));

display.setCursor(0, 32);
display.print(F("I: "));
display.print(I * 1000000, 2);
display.println(F(" uA"));

display.setCursor(0, 48);
display.print(F("P: "));
display.print(P * 1000000, 2);
display.println(F(" uW"));

display.display();


}

void displayEnvironmentalPage() {
display.clearDisplay();
display.setTextColor(SSD1306_WHITE);
display.setTextSize(1);

display.setCursor(0, 0);
display.println(F("Env. Sensors:"));

display.setCursor(0, 16);
display.print(F("Temp: "));
display.print(bmeTemp, 1);
display.println(F(" C"));

display.setCursor(0, 32);
display.print(F("Hum: "));
display.print(bmeHumidity, 1);
display.println(F(" %"));

display.setCursor(0, 48);
display.print(F("Lux: "));
display.print(gy30Illuminance, 1);
display.println(F(" lx"));

display.display();


}

void writeCSVHeader() {
if (!sdCardInitialized) return;

File dataFile = SD.open(ALL_SENSORS_DATA_FILENAME, FILE_APPEND);
if (dataFile) {
    if (dataFile.size() == 0) {
        dataFile.print("Timestamp,");
        dataFile.print("MPPT_Voltage_V,MPPT_Current_A,MPPT_Power_W,MPPT_DutyCycle,");
        dataFile.print("BME_Temp_C,BME_Pressure_hPa,BME_Humidity_perc,BME_Gas_Ohm,");
        dataFile.print("GY30_Illuminance_Lux,");
        dataFile.print("IntegrationTime_us,Max_ADC,Saturation_Status");
        
        for (int i = 0; i < SPECTROGRAPH_CHANNELS; i++) {
            dataFile.print(',');
            dataFile.print(calibratedWavelengths[i], 2);
            dataFile.print("nm");
        }
        dataFile.println();
    }
    dataFile.close();
}


}

void streamCSVToSerial(const char* recordType, float mpptV, float mpptI, float mpptP, int mpptDC,
float bmeT, float bmeP, float bmeH, float bmeG,
float gy30Lux) {
Serial.print(recordType);
Serial.print(',');
Serial.print('"'); Serial.print(getFormattedTimestamp()); Serial.print('"');
Serial.print(','); Serial.print(mpptV, 4);
Serial.print(','); Serial.print(mpptI, 6);
Serial.print(','); Serial.print(mpptP, 6);
Serial.print(','); Serial.print(mpptDC);
Serial.print(','); Serial.print(bmeT, 1);
Serial.print(','); Serial.print(bmeP / 100.0, 1);
Serial.print(','); Serial.print(bmeH, 1);
Serial.print(','); Serial.print(bmeG, 0);
Serial.print(','); Serial.print(gy30Lux, 1);
Serial.print(','); Serial.print(currentIntegrationTimeUs);
Serial.print(','); Serial.print(currentMaxIntensity);
Serial.print(','); Serial.print(getSaturationStatus());

for (int i = 0; i < SPECTROGRAPH_CHANNELS; i++) {
    Serial.print(',');
    Serial.print(spectrographData[i]);
}
Serial.println();
