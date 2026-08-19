/*
  =====================================================================
  VEGA ARIES V2.0 - Environmental Sensor Node (BME680 + WiFi/LoRa-Zigbee + MQTT)
  =====================================================================

  WHAT CHANGED IN THIS REVISION (read this first)
  ---------------------------------------------------
  FIX FOR "SEND FAILED - mqttClient.publish() returned false, state=0":

      state=0 means MQTT_CONNECTED - the link to the broker is fine.
      The failure is happening one layer deeper: PubSubClient was
      splitting your ~430-byte JSON payload into small 64-byte chunks
      (MQTT_MAX_TRANSFER_SIZE) and writing them one at a time. On this
      board's WiFiNINA-clone module, that kind of fragmented/chunked
      write is unreliable - one of the chunk writes fails partway
      through, and publish() correctly reports that as a failure
      instead of pretending it succeeded.

      FIX: switch from the single-call mqttClient.publish(topic, buf,
      len) to the STREAMING publish API:
          mqttClient.beginPublish(topic, len, false);
          mqttClient.write((const uint8_t*)payload, len);
          mqttClient.endPublish();
      This sends the payload as one continuous write instead of many
      small fragmented ones, which is far more reliable on this
      module. MQTT_MAX_TRANSFER_SIZE has been removed since it no
      longer does anything useful here - streaming publish does not
      use PubSubClient's internal chunking logic at all.

      A short retry loop was also added: if the very first attempt
      still fails (e.g. one bad write on a noisy hotspot link), it
      retries up to 2 more times with a brief tcp settle delay before
      giving up and moving on to the next reading, rather than
      abandoning that sample the instant one write hiccups.

  Everything else (transport switch, BME680 math, compile-error fixes
  from earlier revisions, topic structure) is UNCHANGED and still
  applies.
  =====================================================================
*/

#include <Wire.h>
#include <SPI.h>

// =====================================================================
// >>>>>>>>>>>>>>>>>> EDIT HERE #1: TRANSPORT MODE SWITCH <<<<<<<<<<<<<<<<
//   Exactly ONE of the next two lines must be un-commented.
// =====================================================================
#define TRANSPORT_WIFI
// #define TRANSPORT_LORA_ZIGBEE

#if defined(TRANSPORT_WIFI) && defined(TRANSPORT_LORA_ZIGBEE)
  #error "Pick only ONE transport - comment out either TRANSPORT_WIFI or TRANSPORT_LORA_ZIGBEE, not both."
#endif
#if !defined(TRANSPORT_WIFI) && !defined(TRANSPORT_LORA_ZIGBEE)
  #error "Pick a transport - uncomment either #define TRANSPORT_WIFI or #define TRANSPORT_LORA_ZIGBEE."
#endif

#ifdef TRANSPORT_WIFI
  #include <WiFiNINA.h>
  // Buffer still needs to be large enough to hold the whole payload
  // (~430-450 bytes today) even though we now stream it - PubSubClient
  // still uses this internally for tracking/other operations.
  #define MQTT_MAX_PACKET_SIZE 768
  // NOTE: MQTT_MAX_TRANSFER_SIZE intentionally REMOVED. Streaming
  // publish (beginPublish/write/endPublish) bypasses PubSubClient's
  // internal chunked-write path entirely, so this define no longer
  // has any effect on the actual publish and was left in only by
  // accident before. Leaving it out avoids confusion.
  #include <PubSubClient.h>
#endif

#include <math.h>

// --- Old min()/max() macro clash with ArduinoJson's std::string_view fix ---
#undef min
#undef max
#include <ArduinoJson.h>

// =====================================================================
// ------------------------------ CONFIG --------------------------------
// =====================================================================

#ifdef TRANSPORT_WIFI
char ssid[] = "cdac-iot";
char pass[] = "4444333221";

// Set this to the ACTUAL static IP of your Raspberry Pi on the
// hotspot/hostapd network.
const char* BROKER_IP   = "10.1.1.2";
const uint16_t BROKER_PORT = 1883;

// Fixed, organization-wide topic for sensor data (per CDAC naming
// convention). ALL boards publish sensor readings here - the
// board_name/mac fields inside the JSON are how you tell boards
// apart downstream.
const char* FIXED_DATA_TOPIC = "/CDAC/HQ/LAB-I/VEGA-IoT";
#endif

#ifdef TRANSPORT_LORA_ZIGBEE
#define LORA_UART_BAUD 9600
const char* LORA_NODE_ID = "0001";
#endif

#define SEA_LEVEL_PRESSURE 1013.25 // standard hPa, only used for altitude estimate
#define PRESSURE_OFFSET_HPA 0.0    // fixed ADDITIVE calibration offset, NOT a multiplier

#define BME680_I2C_ADDR 0x76
const char* FIRMWARE_VERSION = "1.1.1-2026-08-13";
const char* BOARD_MODEL = "VEGA_ARIES_V2";

const unsigned long PUBLISH_INTERVAL_MS = 10000;

#define BATTERY_ADC_PIN A0
#define BATTERY_DIVIDER_RATIO 2.0
#define ADC_REF_VOLTAGE 3.3
#define ADC_RESOLUTION 4095.0  // 1023.0 if your core is 10-bit instead of 12-bit

// How many times to retry a failed publish before giving up on that
// reading and moving on to the next cycle.
#define PUBLISH_MAX_RETRIES 3

// =====================================================================

#ifdef TRANSPORT_WIFI
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
#endif

TwoWire Wire(8);

#ifdef TRANSPORT_WIFI
char boardName[24];
char mqttClientId[32];
char macStr[18];

char topicData[64];
char topicStatus[64];
char topicFirmware[64];
#endif

unsigned long bootMillis = 0;

JsonDocument outboundDoc;

// --- Global Calibration Variables ---
uint16_t par_t1; int16_t par_t2; int8_t par_t3; int32_t t_fine;
uint16_t par_p1; int16_t par_p2; int8_t par_p3; int16_t par_p4;
int16_t  par_p5; int8_t  par_p6; int8_t par_p7; int16_t par_p8;
int16_t  par_p9; uint8_t par_p10;

uint16_t par_h1;
uint16_t par_h2;
int8_t par_h3;
int8_t par_h4;
int8_t par_h5;
uint8_t par_h6;
int8_t par_h7;

int8_t par_g1; int16_t par_g2; int8_t par_g3;
uint8_t  res_heat_range; int8_t res_heat_val;

// =====================================================================
// ---------------------- Small helpers ----------------------
// =====================================================================

bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

void epochToISO8601(uint32_t epoch, char* buf, size_t len) {
  uint32_t seconds = epoch;
  int sec = seconds % 60; seconds /= 60;
  int minute = seconds % 60; seconds /= 60;
  int hour = seconds % 24; seconds /= 24;
  uint32_t days = seconds;

  int year = 1970;
  while (true) {
    int diy = isLeapYear(year) ? 366 : 365;
    if (days >= (uint32_t)diy) { days -= diy; year++; } else break;
  }

  static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  int month = 0;
  while (true) {
    int dim = mdays[month];
    if (month == 1 && isLeapYear(year)) dim = 29;
    if (days >= (uint32_t)dim) { days -= dim; month++; } else break;
  }
  int day = days + 1;

  snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month + 1, day, hour, minute, sec);
}

extern "C" char* sbrk(int incr);
int freeMemory() {
  char stackDummy;
  return (int)&stackDummy - (int)sbrk(0);
}

#ifdef TRANSPORT_WIFI
void buildIdentity() {
  byte mac[6];
  WiFi.macAddress(mac);
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);

  snprintf(boardName, sizeof(boardName), "VEGA-%02X%02X%02X", mac[2], mac[1], mac[0]);
  snprintf(mqttClientId, sizeof(mqttClientId), "%s", boardName);

  snprintf(topicData,     sizeof(topicData),     "%s", FIXED_DATA_TOPIC);
  snprintf(topicStatus,    sizeof(topicStatus),    "iot/vega/%s/status", boardName);
  snprintf(topicFirmware, sizeof(topicFirmware), "firmware/update/%s", boardName);
}
#endif

// =====================================================================
// ------------------- BME680 calibration + compensation ----------------
// =====================================================================

void readCalibrationData() {
  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0xE9); Wire.endTransmission();
  Wire.requestFrom(BME680_I2C_ADDR, 4);
  par_t1 = (uint16_t)(Wire.read() | (Wire.read() << 8));
  par_t2 = (int16_t)(Wire.read() | (Wire.read() << 8));

  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0xEE); Wire.endTransmission();
  Wire.requestFrom(BME680_I2C_ADDR, 1);
  par_t3 = (int8_t)Wire.read();

  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x8E); Wire.endTransmission();
  Wire.requestFrom(BME680_I2C_ADDR, 12);
  par_p1 = (uint16_t)(Wire.read() | (Wire.read() << 8));
  par_p2 = (int16_t)(Wire.read() | (Wire.read() << 8));
  par_p3 = (int8_t)Wire.read();
  par_p4 = (int16_t)(Wire.read() | (Wire.read() << 8));
  par_p5 = (int16_t)(Wire.read() | (Wire.read() << 8));
  par_p6 = (int8_t)Wire.read();
  par_p7 = (int8_t)Wire.read();

  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x9C); Wire.endTransmission();
  Wire.requestFrom(BME680_I2C_ADDR, 5);
  par_p8 = (int16_t)(Wire.read() | (Wire.read() << 8));
  par_p9 = (int16_t)(Wire.read() | (Wire.read() << 8));
  par_p10 = (uint8_t)Wire.read();

  uint8_t hbuf[8];
  Wire.beginTransmission(BME680_I2C_ADDR);
  Wire.write(0xE1);
  Wire.endTransmission();
  Wire.requestFrom(BME680_I2C_ADDR, 8);
  for (int i = 0; i < 8; i++) hbuf[i] = Wire.read();

  par_h1 = ((uint16_t)hbuf[1] << 4) | (hbuf[0] & 0x0F);
  par_h2 = ((uint16_t)hbuf[2] << 4) | (hbuf[0] >> 4);
  par_h3 = (int8_t)hbuf[3];
  par_h4 = (int8_t)hbuf[4];
  par_h5 = (int8_t)hbuf[5];
  par_h6 = hbuf[6];
  par_h7 = (int8_t)hbuf[7];

  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0xEB); Wire.endTransmission();
  Wire.requestFrom(BME680_I2C_ADDR, 3);
  par_g1 = (int8_t)Wire.read();
  par_g2 = (int16_t)(Wire.read() | (Wire.read() << 8));

  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0xEE); Wire.endTransmission();
  Wire.requestFrom(BME680_I2C_ADDR, 2);
  Wire.read(); // Skip overlapping T3
  par_g3 = (int8_t)Wire.read();

  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x02); Wire.endTransmission();
  Wire.requestFrom(BME680_I2C_ADDR, 1);
  res_heat_range = (Wire.read() & 0x30) >> 4;

  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x00); Wire.endTransmission();
  Wire.requestFrom(BME680_I2C_ADDR, 1);
  res_heat_val = (int8_t)Wire.read();
}

float calcTemperature(uint32_t temp_adc) {
  int64_t var1 = ((int64_t)temp_adc >> 3) - ((int64_t)par_t1 << 1);
  int64_t var2 = (var1 * (int64_t)par_t2) >> 11;
  int64_t var3 = ((((var1 >> 1) * (var1 >> 1)) >> 12) * ((int64_t)par_t3 << 4)) >> 14;
  t_fine = (int32_t)(var2 + var3);
  float calc_temp = (float)(((t_fine * 5) + 128) >> 8) / 100.0;
  if (calc_temp < 0) calc_temp += 45.0;
  return calc_temp;
}

float calcPressure(uint32_t pres_adc) {
  int64_t var1 = ((int64_t)t_fine >> 1) - 64000;
  int64_t var2 = ((((var1 >> 2) * (var1 >> 2)) >> 11) * (int64_t)par_p6) >> 2;
  var2 = var2 + ((var1 * (int64_t)par_p5) << 1);
  var2 = (var2 >> 2) + ((int64_t)par_p4 << 16);
  var1 = (((((var1 >> 2) * (var1 >> 2)) >> 13) * ((int64_t)par_p3 << 5)) >> 3) + (((int64_t)par_p2 * var1) >> 1);
  var1 = var1 >> 18;
  var1 = ((32768 + var1) * (int64_t)par_p1) >> 15;

  if (var1 == 0) return 0.0;

  int64_t p = 1048576 - (int64_t)pres_adc;
  p = (p - (var2 >> 12)) * 3125;

  if (p < 0) {
    p = (p << 1) / (int64_t)var1;
  } else {
    p = (p / (int64_t)var1) << 1;
  }

  int64_t var3 = (((p >> 8) * (p >> 8)) >> 19);
  var1 = (((p >> 3) * (int64_t)par_p9) >> 13);
  var2 = ((int64_t)par_p8 * (p >> 2)) >> 13;
  int64_t var4 = ((int64_t)par_p10 * 5);
  int64_t var5 = var3 + var1 + var2 + var4 + ((int64_t)par_p7 << 7);

  p = p + (var5 >> 4);
  return (float)p / 100.0;
}

float calcGasResistance(uint16_t gas_adc, uint8_t gas_range) {
  static const uint32_t lookupTable1[16] = {
    2147483647U,2147483647U,2147483647U,2147483647U,
    2147483647U,2126008810U,2147483647U,2130303777U,
    2147483647U,2147483647U,2143188679U,2136746228U,
    2147483647U,2126008810U,2147483647U,2147483647U
  };
  static const uint32_t lookupTable2[16] = {
    4096000000U,2048000000U,1024000000U,512000000U,
    255744255U,127110228U,64000000U,32258064U,
    16016016U,8000000U,4000000U,2000000U,
    1000000U,500000U,250000U,125000U
  };

  int64_t var1 = ((1340 + (5 * (int64_t)par_g1)) * lookupTable1[gas_range]) >> 16;
  int64_t var2 = (((int64_t)((int32_t)gas_adc << 15) - (1 << 24)) + var1);
  int64_t var3 = ((lookupTable2[gas_range] * (int64_t)var1) >> 9);

  if (var2 == 0) return 0.0;
  return (float)((var3 + (var2 >> 1)) / var2);
}

uint8_t calcHeaterResistance(uint16_t target_temp) {
  if (target_temp > 400) target_temp = 400;
  int32_t var1 = (((int32_t)target_temp * 1302) >> 10) + 140288;
  int32_t var2 = (((int32_t)res_heat_val * 1238) >> 12) + 24576;
  int32_t var3 = var1 / (var2 >> 10);
  int32_t var4 = var3 - ((int32_t)par_g1 << 2);
  int32_t var5 = (var4 * 120) >> 8;
  uint8_t heat_res = (uint8_t)((var5 - ((int32_t)res_heat_range << 4)) >> 4);
  return heat_res;
}

float calcAltitude(float pressure) {
  if (pressure <= 0) return 0.0;
  return 44330.0 * (1.0 - pow((pressure / SEA_LEVEL_PRESSURE), 0.1903));
}

float calcHumidity(uint16_t hum_adc) {
  float var1 = ((float)t_fine) - 76800.0;
  float var2 = (float)(hum_adc - (par_h4 * 64.0 + par_h5 / 16384.0 * var1));
  float var3 = par_h2 / 65536.0;
  float var4 = 1.0 + (par_h3 / 67108864.0) * var1;
  float var5 = 1.0 + (par_h6 / 67108864.0) * var1 * var4;
  float hum = var2 * var3 * var4 * var5;
  hum = hum * (1.0 - par_h1 * hum / 524288.0);
  if (hum > 100.0) hum = 100.0;
  if (hum < 0.0) hum = 0.0;
  return hum;
}

// =====================================================================
// ------------------------ transport dispatch --------------------------
// =====================================================================

bool sendPayload(const char* payload, size_t len) {
#ifdef TRANSPORT_WIFI
  if (!mqttClient.connected()) {
    Serial.print("SEND SKIPPED - MQTT not connected, state=");
    Serial.println(mqttClient.state());
    return false;
  }

  // STREAMING PUBLISH FIX: send as one continuous write instead of
  // PubSubClient's small chunked writes, which this board's WiFiNINA-
  // clone module handles unreliably. Retry a few times in case one
  // attempt hits a transient write hiccup.
  for (int attempt = 1; attempt <= PUBLISH_MAX_RETRIES; attempt++) {
    if (!mqttClient.beginPublish(topicData, len, false)) {
      Serial.print("SEND FAILED - beginPublish() refused, state=");
      Serial.print(mqttClient.state());
      Serial.print(" attempt="); Serial.println(attempt);
    } else {
      size_t written = mqttClient.write((const uint8_t*)payload, len);
      bool ended = mqttClient.endPublish();

      if (written == len && ended) {
        return true; // success
      }

      Serial.print("SEND FAILED - streamed="); Serial.print(written);
      Serial.print("/"); Serial.print(len);
      Serial.print(" endPublish="); Serial.print(ended);
      Serial.print(" attempt="); Serial.println(attempt);
    }

    if (attempt < PUBLISH_MAX_RETRIES) {
      delay(150); // let the TCP/SPI link settle before retrying
      mqttClient.loop();
    }
  }
  return false; // all retries exhausted
#endif
#ifdef TRANSPORT_LORA_ZIGBEE
  Serial1.println(payload);
  return true;
#endif
}

// =====================================================================
// ---------------------------- MQTT callback ----------------------------
// =====================================================================

#ifdef TRANSPORT_WIFI
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.println("--------------------------------");
  Serial.print("Topic : "); Serial.println(topic);
  Serial.print("Message : "); Serial.println(message);

  if (String(topic) == topicFirmware) {
    Serial.println("New Firmware Update Available!");

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
      Serial.println("JSON Parsing Failed!");
      return;
    }

    int firmwareId = doc["firmwareId"];
    String version = doc["version"].as<String>();
    String firmwareFile = doc["file"].as<String>();

    Serial.println("========== OTA INFO ==========");
    Serial.print("Firmware ID : "); Serial.println(firmwareId);
    Serial.print("Version     : "); Serial.println(version);
    Serial.print("File Name   : "); Serial.println(firmwareFile);
    Serial.print("Current Version : "); Serial.println(FIRMWARE_VERSION);

    if (version == FIRMWARE_VERSION) {
      Serial.println("Device already has the latest firmware.");
      return;
    }
    Serial.println("New firmware detected! (OTA apply not implemented yet)");
    Serial.println("==============================");
  }
}
#endif

// =====================================================================
// -------------------------------- setup ---------------------------------
// =====================================================================

void setup() {
  delay(2000);
  Serial.begin(115200);
  Serial.println("=====================================================");
  Serial.print("VEGA ARIES V2.0 firmware "); Serial.println(FIRMWARE_VERSION);
  Serial.println("=====================================================");
  Serial.println("Starting...");

#ifdef TRANSPORT_WIFI
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connecting WiFi...");
    WiFi.begin(ssid, pass);
    delay(5000);
  }
  Serial.println("WiFi Connected");

  buildIdentity();
  Serial.print("Board name   : "); Serial.println(boardName);
  Serial.print("MAC address  : "); Serial.println(macStr);
  Serial.print("Data topic   : "); Serial.println(topicData);
  Serial.print("Status topic : "); Serial.println(topicStatus);

  mqttClient.setServer(BROKER_IP, BROKER_PORT);
  mqttClient.setCallback(mqttCallback);

  Serial.print("MQTT_MAX_PACKET_SIZE (compiled): "); Serial.println(MQTT_MAX_PACKET_SIZE);
  Serial.println("Publish mode: STREAMING (beginPublish/write/endPublish)");

  Serial.print("Board IP: "); Serial.println(WiFi.localIP());
  Serial.print("Gateway: "); Serial.println(WiFi.gatewayIP());
#endif

#ifdef TRANSPORT_LORA_ZIGBEE
  Serial1.begin(LORA_UART_BAUD);
  Serial.print("LoRa/Zigbee node id: "); Serial.println(LORA_NODE_ID);
  Serial.println("Sending JSON lines out over Serial1 to the radio module.");
#endif

  Wire.begin();
  Serial.println("I2C Started");
  Serial.println(F("--- BME680 Dashboard: Temp, Pressure, Humidity, Gas Resistance ---"));

  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0xD0);
  if (Wire.endTransmission() == 0) {
    Wire.requestFrom(BME680_I2C_ADDR, 1);
    byte chipID = Wire.read();
    Serial.print(F("Sensor initialized. Chip ID: 0x")); Serial.println(chipID, HEX);

    readCalibrationData();

    uint8_t heat_reg_val = calcHeaterResistance(320);
    Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x5A); Wire.write(heat_reg_val); Wire.endTransmission();
    Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x64); Wire.write(0x59); Wire.endTransmission();
    Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x71); Wire.write(0x10); Wire.endTransmission();
  } else {
    Serial.println(F("Sensor connection lost. Please check wiring pins."));
    while (1);
  }

  bootMillis = millis();
}

// =====================================================================
// --------------------------------- loop ----------------------------------
// =====================================================================

#ifdef TRANSPORT_WIFI
void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi dropped - reconnecting...");
  WiFi.disconnect();
  delay(500);
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi reconnected");
    buildIdentity();
  } else {
    Serial.println("\nWiFi still down after 15s - will retry next loop");
  }
}

void reconnectMQTT() {
  if (mqttClient.connected()) return;

  Serial.println("Connecting MQTT...");
  bool ok = mqttClient.connect(mqttClientId, topicStatus, 0, true, "offline");

  if (ok) {
    Serial.println("MQTT Connected");
    mqttClient.subscribe(topicFirmware);
    mqttClient.publish(topicStatus, "online", true);
    Serial.print("Subscribed to "); Serial.println(topicFirmware);
  } else {
    Serial.print("MQTT Failed, state=");
    Serial.println(mqttClient.state());
  }
}
#endif

void loop() {
#ifdef TRANSPORT_WIFI
  ensureWiFiConnected();
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();
#endif

  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x72); Wire.write(0x01); Wire.endTransmission();
  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x74); Wire.write(0x25); Wire.endTransmission();

  delay(200); // Wait for measurement

  Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x1F);
  if (Wire.endTransmission() == 0) {
    Wire.requestFrom(BME680_I2C_ADDR, 6);
    uint32_t p_msb = Wire.read(); uint32_t p_lsb = Wire.read(); uint32_t p_xlsb = Wire.read();
    uint32_t raw_press = (p_msb << 12) | (p_lsb << 4) | (p_xlsb >> 4);

    uint32_t t_msb = Wire.read(); uint32_t t_lsb = Wire.read(); uint32_t t_xlsb = Wire.read();
    uint32_t raw_temp = (t_msb << 12) | (t_lsb << 4) | (t_xlsb >> 4);

    Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x25); Wire.endTransmission();
    Wire.requestFrom(BME680_I2C_ADDR, 2);
    uint16_t raw_hum = ((uint16_t)Wire.read() << 8) | Wire.read();

    Wire.beginTransmission(BME680_I2C_ADDR); Wire.write(0x2A); Wire.endTransmission();
    Wire.requestFrom(BME680_I2C_ADDR, 2);
    uint16_t g_msb = Wire.read(); uint8_t g_lsb = Wire.read();
    uint16_t raw_gas = (uint16_t)((g_msb << 2) | (g_lsb >> 6));
    uint8_t gas_range = g_lsb & 0x0F;

    if (raw_temp != 524288 && raw_press != 524288) {
      float t = calcTemperature(raw_temp);
      float p = calcPressure(raw_press) + PRESSURE_OFFSET_HPA;
      float humidity = calcHumidity(raw_hum);
      float gas = calcGasResistance(raw_gas, gas_range);
      float altitude = calcAltitude(p);

      unsigned long uptimeSec = (millis() - bootMillis) / 1000UL;
      int freeRam = freeMemory();

      float batteryVoltage = -1.0;
#if BATTERY_ADC_PIN >= 0
      int adcRaw = analogRead(BATTERY_ADC_PIN);
      batteryVoltage = (adcRaw / ADC_RESOLUTION) * ADC_REF_VOLTAGE * BATTERY_DIVIDER_RATIO;
#endif

      outboundDoc.clear();
      JsonDocument& doc = outboundDoc;
      doc["board_model"] = BOARD_MODEL;
      doc["firmware"]    = FIRMWARE_VERSION;

#ifdef TRANSPORT_WIFI
      doc["board_name"] = boardName;
      doc["mac"]        = macStr;

      unsigned long epoch = WiFi.getTime();
      char isoTime[25];
      bool timeSynced = (epoch != 0);
      if (timeSynced) {
        epochToISO8601(epoch, isoTime, sizeof(isoTime));
      } else {
        snprintf(isoTime, sizeof(isoTime), "unsynced");
      }
      doc["timestamp_epoch"] = (uint32_t)epoch;
      doc["timestamp"]       = isoTime;
      doc["time_synced"]     = timeSynced;
      doc["wifi_rssi_dbm"]   = WiFi.RSSI();
#endif
#ifdef TRANSPORT_LORA_ZIGBEE
      doc["node_id"] = LORA_NODE_ID;
#endif

      doc["temperature_c"]      = round(t * 100) / 100.0;
      doc["humidity_pct"]       = round(humidity * 100) / 100.0;
      doc["pressure_hpa"]       = round(p * 100) / 100.0;
      doc["altitude_m"]         = round(altitude * 100) / 100.0;
      doc["gas_resistance_ohm"] = (long)gas;

      doc["uptime_sec"]     = uptimeSec;
      doc["free_ram_bytes"] = freeRam;
      if (batteryVoltage >= 0) {
        doc["battery_voltage"] = round(batteryVoltage * 100) / 100.0;
      }
      doc["status"] = "ok";

      char payload[768];
      size_t n = serializeJson(doc, payload, sizeof(payload));

      bool sent = sendPayload(payload, n);
      Serial.print(sent ? "SENT: " : "GAVE UP AFTER RETRIES: ");
      Serial.println(payload);
    }
  }

  unsigned long waited = 0;
  while (waited < PUBLISH_INTERVAL_MS) {
#ifdef TRANSPORT_WIFI
    mqttClient.loop();
#endif
    delay(100);
    waited += 100;
  }
}
