#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// LCD
LiquidCrystal_I2C lcd(0x27, 16, 2);

// WIFI
const char* ssid = "4G-UFI-0988";
const char* password = "1234567890";


// WEBHOOK
const char* webhookURL = "https://johnmartincorpin.app.n8n.cloud/webhook/esp32";


// TIMER
unsigned long SYSTEM_INTERVAL = 1800000;
unsigned long lastSystemRun = 0;


float montalbanWL = 0;

String lastValidLidar = "0";


// RAIN
volatile unsigned int tipCount = 0;
volatile unsigned long lastTipTime = 0;
#define RAIN_PER_TIP 0.2794

// STATUS
bool wifiConnected = false;

// PINS
#define REED_PIN 4
#define LED_RAIN 18
#define LED_LIDAR 19
#define BTN_LIDAR 25
#define BTN_POST 26
#define RXD2 16
#define TXD2 17

// INTERRUPT
void IRAM_ATTR countTip() {
  unsigned long now = millis();
  if (now - lastTipTime > 200) {
    tipCount++;
    lastTipTime = now;
    digitalWrite(LED_RAIN, HIGH);
  }
}

// LCD DISPLAY
void updateLCD() {

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("WF=");
  lcd.print(wifiConnected ? "/" : "X");

  lcd.setCursor(7, 0);
  lcd.print("TP=");
  lcd.print(tipCount);

  lcd.setCursor(0, 1);
  lcd.print("P=");
  lcd.print(montalbanWL, 1);

  lcd.setCursor(8, 1);
  lcd.print("W=");
  lcd.print(lastValidLidar);
}


void readLidar() {

  Serial.println("\n[LIDAR] Requesting...");

  while (Serial2.available()) Serial2.read();

  Serial2.print("iSM\r\n");

  char buffer[50];
  int index = 0;

  unsigned long start = millis();

  while (millis() - start < 300) {

    if (Serial2.available()) {
      char c = Serial2.read();

      if (index < sizeof(buffer) - 1) {
        buffer[index++] = c;
      }
    }
  }

  buffer[index] = '\0';

  Serial.print("[LIDAR RAW]: ");
  Serial.println(buffer);

  String clean = "";

  for (int i = 0; i < index; i++) {
    char c = buffer[i];

    if (isDigit(c) || c == '.' || c == '-') {
      clean += c;
    }
  }

  if (clean.length() > 0) {
    lastValidLidar = clean;
    Serial.print("[LIDAR CLEAN]: ");
    Serial.println(lastValidLidar);
  } else {
    Serial.println("[LIDAR] Invalid cleaned data");
  }

  digitalWrite(LED_LIDAR, HIGH);
  delay(100);
  digitalWrite(LED_LIDAR, LOW);
}

// Fetching PAGASA API
void fetchWaterLevel() {

  Serial.println("\n[PAGASA] Fetching...");

  HTTPClient http;

  String url = "https://pasig-marikina-tullahanffws.pagasa.dost.gov.ph/water/main_list.do?_=" + String(millis());
  http.begin(url);

  http.addHeader("accept", "application/json");
  http.addHeader("isajax", "true");
  http.addHeader("x-requested-with", "XMLHttpRequest");

  int code = http.GET();

  if (code > 0) {

    String payload = http.getString();

    DynamicJsonDocument doc(35000);
    deserializeJson(doc, payload);

    JsonArray arr = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc["data"].as<JsonArray>();

    for (JsonObject obj : arr) {

      String name = obj["obsnm"] | "";

      if (name.indexOf("Montalban") >= 0) {

        String wlStr = obj["wl"] | "0";

        wlStr.replace("(*)", "");
        wlStr.replace("*", "");
        wlStr.trim();

        montalbanWL = wlStr.toFloat();
        break;
      }
    }
  }

  http.end();
}

// Posting to n8n
void postToWebhook() {

  Serial.println("[POST] Refreshing sensors...");

  Serial2.flush();
  while (Serial2.available()) Serial2.read();

  fetchWaterLevel();
  readLidar();

  delay(50); 

  float rain_mm = tipCount * RAIN_PER_TIP;

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("POSTING...");

  String payload = "{";
  payload += "\"tips\":" + String(tipCount) + ",";
  payload += "\"rain_mm\":" + String(rain_mm, 2) + ",";
  payload += "\"lidar\":\"" + lastValidLidar + "\",";
  payload += "\"pagasa\":" + String(montalbanWL, 2);
  payload += "}";

  Serial.println(payload);

  HTTPClient http;
  http.begin(webhookURL);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);

  Serial.print("POST CODE: ");
  Serial.println(code);

  http.end();

  tipCount = 0;

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("POST DONE");

  delay(1500);

  updateLCD();
}

// SYSTEM CYCLE
void runSystemCycle() {
  postToWebhook();
}

// SETUP
void setup() {

  Serial.begin(115200);

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  pinMode(REED_PIN, INPUT_PULLUP);
  pinMode(LED_RAIN, OUTPUT);
  pinMode(LED_LIDAR, OUTPUT);
  pinMode(BTN_LIDAR, INPUT_PULLUP);
  pinMode(BTN_POST, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(REED_PIN), countTip, FALLING);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  lcd.print("STARTING...");
  delay(1000);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  wifiConnected = true;

  Serial.println("WiFi Connected");

  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);

  lastSystemRun = millis() - SYSTEM_INTERVAL;
}

// LOOP
void loop() {

  wifiConnected = (WiFi.status() == WL_CONNECTED);

  unsigned long now = millis();

  if (now - lastSystemRun >= SYSTEM_INTERVAL) {
    runSystemCycle();
    lastSystemRun = now;
  }

  if (digitalRead(BTN_LIDAR) == LOW) {
    readLidar();
    updateLCD();
    delay(300);
  }

  if (digitalRead(BTN_POST) == LOW) {
    postToWebhook();
    delay(300);
  }

  if (millis() - lastTipTime > 200) {
    digitalWrite(LED_RAIN, LOW);
  }

  updateLCD();

  delay(300);
}