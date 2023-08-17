/**
 * Vecode Cloud Firmware
 * Copyright (c) 2020 Vecode. All rights reserved.
 * @version 1.0
 * @author  biohzrdmx <github.com/biohzrdmx>
 */
#include "main.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <EasyButton.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_AHTX0.h>
#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include "Timer.h"
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <PubSubClient.h>

#include "NotoSans_Regular16pt7b.h"
#include "NotoSans_Regular24pt7b.h"
#include "NotoSans_Regular42pt7b.h"
#include "Sansation_Regular24.h"

// The font names are arrays references, thus must NOT be in quotes ""
#define AA_FONT_SMALL  NotoSansBold16
#define AA_FONT_MEDIUM NotoSansBold24
#define AA_FONT_LARGE  NotoSansBold42
#define AA_FONT_VECODE SansationRegular24

#define STATE_IDLE    0
#define STATE_CONFIG  1
#define STATE_SERVER  2
#define STATE_CONNECT 3
#define STATE_CLIENT  4
#define STATE_ERROR  99

#define PIN_BTN_RESET D4

#define FIRMWARE_VERSION  F("1.0")
#define FIRMWARE_HOSTNAME F("Vecode Cloud Device")

#define AP_SSID F("DEVICE")
#define AP_PASS F("12345678")

#define SEALEVELPRESSURE_HPA (1013.25)

struct Config {
  int brightness;
  int timeOffset;
  int updateInterval;
  String apiKey;
  String apiToken;
};

Config config;

int state = STATE_IDLE;
bool is_reset = false;
bool update = true;
bool is_clock = true;

float temp, pressure, altitude, humidity, heat_index;

String wifi_ssid;
String wifi_password;

String cloud_uid;

String device_name;
String device_type;
String device_version;
String device_serial;
String device_mac;

Timer timer_read, timer_clock;
EasyButton button_reset(PIN_BTN_RESET);
ESP8266WebServer server(80);
WiFiClient client;
Adafruit_BMP280 bmp;
Adafruit_AHTX0 aht;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
TFT_eSPI lcd = TFT_eSPI();
PubSubClient pubsub(client);

void setup() {
  delay(200);
  //
  Serial.begin(115200);
  EEPROM.begin(512);
  LittleFS.begin();
  //
  unsigned status;
  status = bmp.begin();
  if (!status) {
    Serial.println(F("Could not find a valid BMP280 sensor, check wiring or "
                      "try a different address!"));
    Serial.print("SensorID was: 0x"); Serial.println(bmp.sensorID(),16);
    Serial.print("        ID of 0xFF probably means a bad address, a BMP 180 or BMP 085\n");
    Serial.print("   ID of 0x56-0x58 represents a BMP 280,\n");
    Serial.print("        ID of 0x60 represents a BME 280.\n");
    Serial.print("        ID of 0x61 represents a BME 680.\n");
    while (1) delay(10);
  }
  // Default settings from datasheet.
  bmp.setSampling(Adafruit_BMP280::MODE_FORCED,     /* Operating Mode. */
                  Adafruit_BMP280::SAMPLING_X2,     /* Temp. oversampling */
                  Adafruit_BMP280::SAMPLING_X16,    /* Pressure oversampling */
                  Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                  Adafruit_BMP280::STANDBY_MS_500); /* Standby time. */
  //
  if (! aht.begin()) {
    Serial.println("Could not find AHT? Check wiring");
    while (1) delay(10);
  }
  Serial.println("AHT10 or AHT20 found");
  //
  button_reset.begin();
  button_reset.onPressed(on_pressed_reset);
  button_reset.onPressedFor(5000, on_hold_reset);
  //
  device_name = F("Aion 2");
  device_type = F("Sensor Clock");
  device_version = FIRMWARE_VERSION;
  device_serial = String( ESP.getChipId() );
  //
  lcd.init();
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(lcd.color565(252, 176, 64), TFT_BLACK);
  lcd.loadFont(AA_FONT_VECODE);
  lcd.drawCentreString(F("vecode"), 120, 108, 2);
  lcd.unloadFont();
  delay(1500);
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.loadFont(AA_FONT_MEDIUM);
  lcd.drawCentreString(device_name, 120, 96, 2);
  lcd.unloadFont();
  lcd.setTextColor(lcd.color565(189, 189, 189), TFT_BLACK);
  lcd.loadFont(AA_FONT_SMALL);
  lcd.drawCentreString(device_serial, 120, 150, 2);
  lcd.unloadFont();
  delay(2000);
  //
  Serial.println("Vecode Cloud Device Firmware v" + device_version);
  Serial.println("Serial No. " + device_serial);
  Serial.println(F("Copyright (c) 2022 Vecode. All rights reserved."));
  Serial.println("");
  //
  read_eeprom();
  //
  if ( wifi_ssid != "" ) {
    state = STATE_CONNECT;
  } else {
    state = STATE_CONFIG;
  }
  //
  switch (state) {
    case STATE_CONFIG:
      setup_ap();
    break;
    case STATE_CONNECT:
      setup_client();
    break;
  }
  device_mac = WiFi.macAddress();
}

void load_configuration(const char *filename, Config &config) {
  // Open file for reading
  File file = LittleFS.open(filename, "r");
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, file);
  if (error)
    Serial.println(F("Failed to read file, using default configuration"));
  // Copy values from the JsonDocument to the Config
  config.timeOffset = doc["timeOffset"] | 0;
  config.brightness = doc["brightness"] | 8;
  config.updateInterval = doc["updateInterval"] | 60000;
  config.apiKey = doc["apiKey"] | "";
  config.apiToken = doc["apiToken"] | "";
  file.close();
}

void save_configuration(const char *filename, const Config &config) {
  // Delete existing file, otherwise the configuration is appended to the file
  LittleFS.remove(filename);
  // Open file for writing
  File file = LittleFS.open(filename, "w");
  if (!file) {
    Serial.println(F("Failed to create file"));
    return;
  }
  StaticJsonDocument<512> doc;
  // Set the values in the document
  doc["timeOffset"] = config.timeOffset;
  doc["brightness"] = config.brightness;
  doc["updateInterval"] = config.updateInterval;
  doc["apiKey"] = config.apiKey;
  doc["apiToken"] = config.apiToken;
  // Serialize JSON to file
  if (serializeJson(doc, file) == 0) {
    Serial.println(F("Failed to write to file"));
  }
  // Close the file
  file.close();
}

void setup_ap() {
  Serial.println(F("Configuring access point..."));
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(10);
  server.on(F("/xhr/scan"), callback_xhr_scan);
  server.on(F("/xhr/connect"), callback_xhr_connect);
  Serial.println(F("Successfully set to AccessPoint mode"));
  Serial.println(WiFi.softAPIP());
  server.begin();
  //
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.loadFont(AA_FONT_SMALL);
  lcd.drawCentreString(F("Setup mode"), 120, 90, 2);
  lcd.setTextColor(lcd.color565(34, 139, 34), TFT_BLACK);
  lcd.drawCentreString(F("Connect to DEVICE"), 120, 112, 2);
  lcd.setTextColor(lcd.color565(189, 189, 189), TFT_BLACK);
  lcd.drawCentreString(WiFi.softAPIP().toString(), 120, 150, 2);
  lcd.unloadFont();
  //
  delay(300);
  state = STATE_SERVER;
}

void setup_client() {
  Serial.println(F("Configuring client..."));
  Serial.print("Connecting to " + wifi_ssid);
  //
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.loadFont(AA_FONT_SMALL);
  lcd.drawCentreString(F("Connecting..."), 120, 112, 2);
  lcd.unloadFont();
  //
  WiFi.mode(WIFI_STA);
  WiFi.hostname(FIRMWARE_HOSTNAME);
  WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
  while (WiFi.status() != WL_CONNECTED) {
    button_reset.read();
    delay(500);
    Serial.print(".");
    if (is_reset) {
      return;
    }
  }
  //
  lcd.fillScreen(TFT_BLACK);
  lcd.loadFont(AA_FONT_SMALL);
  lcd.setTextColor(lcd.color565(34, 139, 34), TFT_BLACK);
  lcd.drawCentreString(F("Connected"), 120, 112, 2);
  lcd.unloadFont();
  //
  Serial.println(F("Connected"));
  Serial.println(F("Successfully set to Client mode"));
  Serial.println(cloud_uid);
  Serial.println(WiFi.localIP());
  delay(300);
  //
  String ip = WiFi.localIP().toString();
  //
  HTTPClient http;
  http.begin(client, F("http://cloud.vecode.net/api/devices/register"));
  http.addHeader(F("Content-Type"), F("application/x-www-form-urlencoded"));
  String postData = "uid=" + cloud_uid + "&serial=" + device_serial + "&name=" + device_name + "&type=" + device_type + "&address=" + ip;
  int httpCode = http.POST(postData);
  http.end();
  if (httpCode == 200) {
    state = STATE_CLIENT;
    Serial.println(F("Success"));
    // Start server
    server.on(F("/xhr/ping"), callback_xhr_ping);
    server.on(F("/xhr/reset"), callback_xhr_reset);
    server.on(F("/xhr/rpc"), callback_xhr_rpc);
    Serial.println(F("Server listening"));
    server.begin();
    //
    load_configuration("/config.json", config);
    //
    timeClient.begin();
    timeClient.setUpdateInterval(3600000);
    timeClient.setTimeOffset(config.timeOffset);
    //
    pubsub.setServer("cloud.vecode.net", 1883);
    pubsub.setBufferSize(255);
    Serial.println(config.apiKey);
    Serial.println(config.apiToken);
    if ( !config.apiKey.isEmpty() && !config.apiToken.isEmpty() ) {
      Serial.println("Connecting to broker...");
      bool ret = pubsub.connect(device_serial.c_str(), config.apiKey.c_str(), config.apiToken.c_str());
      Serial.println(ret ? "Success!" : "Failed");
    }
    //
    update_sensor_data();
    timer_read.init(config.updateInterval);
    timer_clock.init(config.updateInterval / 10);
    //
    lcd.fillScreen(TFT_BLACK);
  } else {
    state = STATE_ERROR;
  }
}

void callback_xhr_scan() {
  switch( server.method() ) {
    case HTTP_GET:
    {
      String response;
      DynamicJsonDocument json(2048);
      JsonObject data = json.createNestedObject("data");
      JsonArray networks = data.createNestedArray("networks");
      json["result"] = "success";
      //
      lcd.fillScreen(TFT_BLACK);
      lcd.setTextColor(TFT_WHITE, TFT_BLACK);
      lcd.loadFont(AA_FONT_SMALL);
      lcd.drawCentreString(F("Scanning..."), 120, 112, 2);
      lcd.unloadFont();
      //
      Serial.println(F("Scanning..."));
      int n = WiFi.scanNetworks();
      Serial.println("Scan done, found " + String(n) + " networks");
      //
      if (n > 0) {
        for (int i = 0; i < n; i++) {
          JsonObject network = networks.createNestedObject();
          String ssid = WiFi.SSID(i);
          network["ssid"] = ssid;
          network["sec"] = WiFi.encryptionType(i);
          network["str"] = WiFi.RSSI(i);
          Serial.println(" - " + ssid);
        }
      }
      serializeJson(json, response);
      //
      server.send(200, F("application/json"), response);
      break;
    }
    default:
      server.send(405);
      break;
  }
}

void callback_xhr_connect() {
  switch( server.method() ) {
    case HTTP_POST:
    {
      String response;
      DynamicJsonDocument json(2048);
      //
      lcd.fillScreen(TFT_BLACK);
      lcd.setTextColor(TFT_WHITE, TFT_BLACK);
      lcd.loadFont(AA_FONT_SMALL);
      lcd.drawCentreString(F("Connecting..."), 120, 112, 2);
      lcd.unloadFont();
      //
      String ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
      String password = server.hasArg("password") ? server.arg("password") : "";
      String uid = server.hasArg("uid") ? server.arg("uid") : "";
      if (ssid.length() > 0 && password.length() > 0 && uid.length() > 0) {
        clear_eeprom();
        delay(10);
        write_eeprom(ssid, password, uid);
        json["result"] = F("success");
      } else {
        json["result"] = F("error");
      }
      serializeJson(json, response);
      //
      server.send(200, F("application/json"), response);
      //
      lcd.fillScreen(TFT_BLACK);
      lcd.setTextColor(TFT_WHITE, TFT_BLACK);
      lcd.loadFont(AA_FONT_SMALL);
      lcd.drawCentreString(F("Restarting..."), 120, 112, 2);
      lcd.unloadFont();
      //
      is_reset = true;
      Serial.println(F("Restarting..."));
      delay(1000);
      ESP.restart();
      break;
    }
    default:
      server.send(405);
      break;
  }
}

void callback_xhr_ping() {
  switch( server.method() ) {
    case HTTP_POST:
    {
      String key = server.hasArg("key") ? server.arg("key") : "";
      if (key == device_serial) {
        String response;
        DynamicJsonDocument json(1024);
        json["result"] = F("success");
        serializeJson(json, response);
        //
        server.send(200, F("application/json"), response);
      } else {
        server.send(403);
      }
      break;
    }
    default:
      server.send(405);
      break;
  }
}

void callback_xhr_reset() {
  switch( server.method() ) {
    case HTTP_POST:
    {
      String key = server.hasArg("key") ? server.arg("key") : "";
      if (key == device_serial) {
        String response;
        DynamicJsonDocument json(1024);
        json["result"] = F("success");
        serializeJson(json, response);
        //
        server.send(200, F("application/json"), response);
        //
        lcd.fillScreen(TFT_BLACK);
        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.loadFont(AA_FONT_SMALL);
        lcd.drawCentreString(F("Restarting..."), 120, 112, 2);
        lcd.unloadFont();
        //
        is_reset = true;
        Serial.println(F("Restarting..."));
        delay(100);
        ESP.restart();
      } else {
        server.send(403);
      }
      break;
    }
    default:
      server.send(405);
      break;
  }
}

void callback_xhr_rpc() {
  String key = server.hasArg("key") ? server.arg("key") : "";
  String cmd = server.hasArg("cmd") ? server.arg("cmd") : "";
  String response;
  DynamicJsonDocument json(1024);
  if (key == device_serial) {
    switch( server.method() ) {
      case HTTP_GET:
        json["result"] = F("error");
        if (cmd == "CFG") {
          File file = LittleFS.open("/config.json", "r");
          StaticJsonDocument<512> doc;
          DeserializationError error = deserializeJson(doc, file);
          if (error) {
            Serial.println(F("Failed to read file"));
          } else {
            json["result"] = F("success");
            json["data"] = doc;
          }
          file.close();
        } else if (cmd == "MODE") {
          JsonObject data = json.createNestedObject("data");
          data["mode"] = 0;
          json["result"] = F("success");
        } else if (cmd == "VALUES") {
          JsonObject data = json.createNestedObject("data");
          data["temp"] = temp;
          data["pressure"] = pressure;
          data["altitude"] = altitude;
          data["humidity"] = humidity;
          json["result"] = F("success");
        }
        serializeJson(json, response);
        //
        server.send(200, F("application/json"), response);
        break;
      case HTTP_POST:
        json["result"] = F("error");
        if (cmd == "CFG") {
          String timeOffset = server.hasArg("timeOffset") ? server.arg("timeOffset") : "";
          String brightness = server.hasArg("brightness") ? server.arg("brightness") : "";
          String updateInterval = server.hasArg("updateInterval") ? server.arg("updateInterval") : "";
          String apiKey = server.hasArg("apiKey") ? server.arg("apiKey") : "";
          String apiToken = server.hasArg("apiToken") ? server.arg("apiToken") : "";
          if (timeOffset.length()) config.timeOffset = timeOffset.toInt();
          if (updateInterval.length()) config.updateInterval = updateInterval.toInt();
          if (apiKey.length()) config.apiKey = apiKey;
          if (apiToken.length()) config.apiToken = apiToken;
          //
          if ( !pubsub.connected() && !config.apiKey.isEmpty() && !config.apiToken.isEmpty() ) {
            pubsub.connect(device_serial.c_str(), config.apiKey.c_str(), config.apiToken.c_str());
          }
          //
          timeClient.setTimeOffset(config.timeOffset);
          timer_read.init(config.updateInterval);
          timer_clock.init(config.updateInterval / 10);
          //
          is_clock = true;
          update = true;
          //
          save_configuration("/config.json", config);
          json["result"] = F("success");
        } else if (cmd == "MODE") {
          String new_mode = server.hasArg("mode") ? server.arg("mode") : "";
          timer_clock.restart();
          is_clock = true;
          update = true;
          json["result"] = F("success");
        }
        serializeJson(json, response);
        //
        server.send(200, F("application/json"), response);
        break;
      default:
        server.send(405);
        break;
    }
  } else {
    server.send(403);
  }
}

void read_eeprom() {
  Serial.println(F("Reading eeprom..."));
  for (int i = 0; i < 32; ++i) {
    if ( EEPROM.read(i) < 32 ) break;
    wifi_ssid += char( EEPROM.read(i) );
  }
  for (int i = 32; i < 96; ++i) {
    if ( EEPROM.read(i) < 32 ) break;
    wifi_password += char( EEPROM.read(i) );
  }
  for (int i = 96; i < 128; ++i) {
    if ( EEPROM.read(i) < 32 ) break;
    cloud_uid += char( EEPROM.read(i) );
  }
}

void write_eeprom(String ssid, String password, String cloud_uid) {
  Serial.println(F("Writing eeprom..."));
  for (uint i = 0; i < ssid.length(); ++i) {
    EEPROM.write(i, ssid[i]);
  }
  for (uint i = 0; i < password.length(); ++i) {
    EEPROM.write(32 + i, password[i]);
  }
  for (uint i = 0; i < cloud_uid.length(); ++i) {
    EEPROM.write(96 + i, cloud_uid[i]);
  }
  EEPROM.commit();
}

void clear_eeprom() {
  Serial.println(F("Clearing eeprom..."));
  for (int i = 0; i < 96; ++i) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
}

void on_hold_reset() {
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.loadFont(AA_FONT_SMALL);
  lcd.drawCentreString(F("Factory reset..."), 120, 112, 2);
  lcd.unloadFont();
  //
  is_reset = true;
  Serial.println(F("Restarting into config mode..."));
  clear_eeprom();
  delay(100);
  ESP.restart();
}

void on_pressed_reset() {
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.loadFont(AA_FONT_SMALL);
  lcd.drawCentreString(F("Restarting..."), 120, 112, 2);
  lcd.unloadFont();
  //
  is_reset = true;
  Serial.println(F("Restarting..."));
  delay(100);
  ESP.restart();
}

void update_sensor_data() {
  sensors_event_t humidity_evt, temp_evt;
  aht.getEvent(&humidity_evt, &temp_evt);
  //
  pressure = bmp.readPressure() / 100.0F;
  altitude = bmp.readAltitude(SEALEVELPRESSURE_HPA);
  temp = temp_evt.temperature;
  humidity = humidity_evt.relative_humidity;
  heat_index = compute_heat_index(temp, humidity, false);
  //
  Serial.println("Readings:");
  Serial.println(temp);
  Serial.println(heat_index);
  Serial.println(pressure);
  Serial.println(altitude);
  Serial.println(humidity);
  //
  if ( pubsub.connected() ) {
    String topic;
    topic = device_serial + "/temperature";
    pubsub.publish(topic.c_str(), ((String)temp).c_str());
    topic = device_serial + "/heat_index";
    pubsub.publish(topic.c_str(), ((String)heat_index).c_str());
    topic = device_serial + "/pressure";
    pubsub.publish(topic.c_str(), ((String)pressure).c_str());
    topic = device_serial + "/altitude";
    pubsub.publish(topic.c_str(), ((String)altitude).c_str());
    topic = device_serial + "/humidity";
    pubsub.publish(topic.c_str(), ((String)humidity).c_str());
  } else {
    Serial.println("Not connected to broker");
  }
}

float convert_cto_f(float c) {
  return c * 1.8 + 32;
  }

float convert_fto_c(float f) {
  return (f - 32) * 0.55555;
  }

float compute_heat_index(float temperature, float percentHumidity, bool isFahrenheit) {
  float hi;

  if (!isFahrenheit)
    temperature = convert_cto_f(temperature);

  hi = 0.5 * (temperature + 61.0 + ((temperature - 68.0) * 1.2) + (percentHumidity * 0.094));

  if (hi > 79) {
    hi = -42.379 + 2.04901523 * temperature + 10.14333127 * percentHumidity +
         -0.22475541 * temperature * percentHumidity +
         -0.00683783 * pow(temperature, 2) +
         -0.05481717 * pow(percentHumidity, 2) +
         0.00122874 * pow(temperature, 2) * percentHumidity +
         0.00085282 * temperature * pow(percentHumidity, 2) +
         -0.00000199 * pow(temperature, 2) * pow(percentHumidity, 2);

    if ((percentHumidity < 13) && (temperature >= 80.0) &&
        (temperature <= 112.0))
      hi -= ((13.0 - percentHumidity) * 0.25) *
            sqrt((17.0 - abs(temperature - 95.0)) * 0.05882);

    else if ((percentHumidity > 85.0) && (temperature >= 80.0) &&
             (temperature <= 87.0))
      hi += ((percentHumidity - 85.0) * 0.1) * ((87.0 - temperature) * 0.2);
  }

  return isFahrenheit ? hi : convert_fto_c(hi);
}

void loop() {
  char buffer[9] = "";
  timeClient.update();
  button_reset.read();
  timer_read.update();
  timer_clock.update();
  switch (state) {
    case STATE_SERVER:
      server.handleClient();
    break;
    case STATE_CLIENT:
      if (update) {

        lcd.fillScreen(TFT_BLACK);

        if (is_clock) {

            lcd.loadFont(AA_FONT_LARGE);

            sprintf(buffer, "%02d:%02d", timeClient.getHours(), timeClient.getMinutes());
            lcd.setTextColor(TFT_WHITE, TFT_BLACK);
            lcd.drawCentreString(buffer, 120, 102, 2);

            lcd.unloadFont();

        } else {

          float value = ((temp + 50.0f) / 100.0f) * 360;
          uint32_t color;

          if (temp < 10) {
            color = lcd.color565(72, 209, 204);
          } else if (temp < 25) {
            color = lcd.color565(34, 139, 34);
          } else if (temp < 30) {
            color = lcd.color565(218, 165, 32);
          } else {
            color = lcd.color565(178, 34, 34);
          }

          //

          lcd.loadFont(AA_FONT_LARGE);

          sprintf(buffer, "%.0fº C", temp);
          lcd.setTextColor(TFT_WHITE, TFT_BLACK, true);
          lcd.drawCentreString(buffer, 120, 60, 6);

          lcd.unloadFont();

          //

          lcd.loadFont(AA_FONT_SMALL);

          sprintf(buffer, "Feels like %.0fº C", heat_index);
          lcd.setTextColor(color, TFT_BLACK, true);
          lcd.drawCentreString(buffer, 120, 110, 2);

          sprintf(buffer, "%.0f hPa", pressure);
          lcd.setTextColor(TFT_WHITE, TFT_BLACK, true);
          lcd.drawString(buffer, 40, 140, 2);

          sprintf(buffer, "Hum. %.0f%%", humidity);
          lcd.setTextColor(TFT_WHITE, TFT_BLACK, true);
          lcd.drawRightString(buffer, 200, 140, 2);

          lcd.unloadFont();

          //

          lcd.loadFont(AA_FONT_MEDIUM);

          sprintf(buffer, "%02d:%02d", timeClient.getHours(), timeClient.getMinutes());
          lcd.setTextColor(TFT_WHITE, TFT_BLACK, true);
          lcd.drawCentreString(buffer, 120, 170, 4);

          lcd.unloadFont();

          //

          lcd.drawSmoothArc(120, 120, 110, 105, 0, value, color, TFT_BLACK, true);
        }

        update = false;
      }
      if ( !pubsub.connected() && !config.apiKey.isEmpty() && !config.apiToken.isEmpty() ) {
        pubsub.connect(device_serial.c_str(), config.apiKey.c_str(), config.apiToken.c_str());
      }
      pubsub.loop();
      server.handleClient();
    break;
    default:
      //
    break;
  }
  if ( is_clock && timer_clock.hasFinished() ) {
    is_clock = false;
    update = true;
  }
  if ( timer_read.hasFinished() ) {
    update_sensor_data();
    timer_read.restart();
    timer_clock.restart();
    is_clock = true;
    update = true;
  }
}
