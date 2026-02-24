/*
  ***************************************************************
  *  IoT Smart AC Energy Meter (ESP32 + EmonLib + LCD + Blynk)
  *  Features:
  *   ✔ Real Voltage (Vrms)
  *   ✔ Real Current (Ireal)
  *   ✔ Real Power (Watt)
  *   ✔ kWh Energy Counting (with EEPROM)
  *   ✔ Monthly Cost Calculation
  *   ✔ LCD Page Switching Button
  *   ✔ Long-Press Energy Reset
  *   ✔ Telegram Notification
  *   ✔ Blynk IoT Dashboard Update
  *
  *  Copyright:
  *  Mohit Saini
  ***************************************************************
*/

#define BLYNK_TEMPLATE_ID "Paste Blynk Template ID"
#define BLYNK_TEMPLATE_NAME "Paste Blynk Template Name"
#define BLYNK_PRINT Serial

#include "EmonLib.h"
#include <EEPROM.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------------------- LCD -------------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------------- Telegram ---------------------
const char* telegramBotToken = "Paste Telegram Bot Token";
const char* telegramChatID   = "Paste Telegram Chat ID";

// ---------------------- Calibration ------------------
const float vCalibration   = 47.50;
const float currCalibration = 7.50;

// ---------------------- WiFi & Blynk -----------------
const char auth[] = "Paste Blynk Authentication Token";
const char ssid[] = "Type WiFi SSID";
const char pass[] = "Type WiFi Password";

// ---------------------- Energy Monitor ---------------
EnergyMonitor emon;

// ---------------------- Timers -----------------------
BlynkTimer timer;

// ---------------------- Energy Variables -------------
float kWh = 0.0;
float cost = 0.0;
const float ratePerkWh = 6.5;

unsigned long lastMillis = millis();

// ---------------------- EEPROM -----------------------
const int addrKWh  = 12;
const int addrCost = 16;

// ---------------------- LCD Pages --------------------
int displayPage = 0;

// Button for changing LCD page (GPIO4)
const int displayButtonPin = 4;

// Button handling
unsigned long buttonPressTime = 0;
bool buttonPressed = false;

// ---------------------- Smoothing Filter -------------
const float SMOOTH_ALPHA = 0.15f;
float smoothV = 0.0;
float smoothI = 0.0;
float smoothP = 0.0;
bool smoothInitialized = false;


// =====================================================
//                     SETUP
// =====================================================
void setup() {

  Serial.begin(115200);

  // ----- WiFi -----
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  // ----- Blynk -----
  Blynk.begin(auth, ssid, pass);

  // ----- LCD -----
  lcd.init();
  lcd.backlight();

  // ----- EEPROM -----
  EEPROM.begin(32);
  readEnergyDataFromEEPROM();

  // ----- Button -----
  pinMode(displayButtonPin, INPUT_PULLUP);

  // ----- EmonLib Setup -----
  emon.voltage(35, vCalibration, 1.7);  // pin, calibration, phase shift
  emon.current(34, currCalibration);    // pin, calibration

  // ----- Timers -----
  timer.setInterval(1000L, sendEnergyDataToBlynk);   // Every sec
  timer.setInterval(60000L, sendBillToTelegram);     // Every minute
}


// =====================================================
//                     MAIN LOOP
// =====================================================
void loop() {
  Blynk.run();
  timer.run();

  handleButton();
}


// =====================================================
//        BUTTON HANDLING FOR PAGE CHANGE & RESET
// =====================================================
void handleButton() {
  int buttonState = digitalRead(displayButtonPin);

  if (buttonState == LOW && !buttonPressed) {
    buttonPressed = true;
    buttonPressTime = millis();
    changeDisplayPage();
  }

  // Long press → Reset energy
  if (buttonState == LOW && buttonPressed) {
    if (millis() - buttonPressTime >= 3000) {
      resetEEPROM();
      buttonPressed = false;
      delay(500);
    }
  }

  if (buttonState == HIGH && buttonPressed) {
    buttonPressed = false;
  }
}


// =====================================================
//      READ VOLTAGE / CURRENT / POWER & UPDATE UI
// =====================================================
void sendEnergyDataToBlynk() {

  emon.calcVI(20, 2000);

  float Vrms      = emon.Vrms;
  float realPower = emon.realPower;

  // -------- Ignore noise --------
  if (Vrms < 5) Vrms = 0;
  if (realPower < 0) realPower = 0;

  // -------- Real Current --------
  float Ireal = (Vrms > 0) ? realPower / Vrms : 0;

  // -------- Initialize smoothing --------
  if (!smoothInitialized) {
    smoothV = Vrms;
    smoothI = Ireal;
    smoothP = realPower;
    smoothInitialized = true;
  } else {
    smoothV = SMOOTH_ALPHA * Vrms      + (1 - SMOOTH_ALPHA) * smoothV;
    smoothI = SMOOTH_ALPHA * Ireal     + (1 - SMOOTH_ALPHA) * smoothI;
    smoothP = SMOOTH_ALPHA * realPower + (1 - SMOOTH_ALPHA) * smoothP;
  }

  // -------- Energy Calculation (kWh) --------
  unsigned long currentMillis = millis();
  kWh += smoothP * (currentMillis - lastMillis) / 3600000000.0;
  lastMillis = currentMillis;

  // -------- Billing --------
  cost = kWh * ratePerkWh;

  // Save data
  saveEnergyDataToEEPROM();

  // -------- Send to Blynk --------
  Blynk.virtualWrite(V0, smoothV);
  Blynk.virtualWrite(V1, smoothI);
  Blynk.virtualWrite(V2, smoothP);
  Blynk.virtualWrite(V3, kWh);
  Blynk.virtualWrite(V4, cost);

  // -------- Update LCD --------
  updateLCD();
}


// =====================================================
//            EEPROM SAVE / LOAD FUNCTIONS
// =====================================================
void readEnergyDataFromEEPROM() {
  EEPROM.get(addrKWh, kWh);
  EEPROM.get(addrCost, cost);

  if (isnan(kWh))  kWh = 0.0;
  if (isnan(cost)) cost = 0.0;
}

void saveEnergyDataToEEPROM() {
  EEPROM.put(addrKWh, kWh);
  EEPROM.put(addrCost, cost);
  EEPROM.commit();
}


// =====================================================
//                       LCD UPDATE
// =====================================================
void updateLCD() {
  lcd.clear();

  if (displayPage == 0) {
    lcd.setCursor(0, 0);
    lcd.printf("V:%.1f I:%.2f", smoothV, smoothI);

    lcd.setCursor(0, 1);
    lcd.printf("P: %.1f W", smoothP);
  }

  else if (displayPage == 1) {
    lcd.setCursor(0, 0);
    lcd.printf("Energy: %.2fkWh", kWh);

    lcd.setCursor(0, 1);
    lcd.printf("Cost: %.2f", cost);
  }
}


// =====================================================
//                LCD PAGE SWITCHING
// =====================================================
void changeDisplayPage() {
  displayPage = (displayPage + 1) % 2;
  updateLCD();
}


// =====================================================
//              SEND BILL TO TELEGRAM BOT
// =====================================================
void sendBillToTelegram() {

  String message = "Total Energy: " + String(kWh, 2) +
                   " kWh\nTotal Cost: ₹" + String(cost, 2);

  HTTPClient http;

  http.begin("https://api.telegram.org/bot" + 
              String(telegramBotToken) + "/sendMessage");

  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument jsonDoc(256);
  jsonDoc["chat_id"] = telegramChatID;
  jsonDoc["text"] = message;

  String jsonString;
  serializeJson(jsonDoc, jsonString);

  http.POST(jsonString);
  http.end();
}


// =====================================================
//              RESET ENERGY (EEPROM CLEAR)
// =====================================================
void resetEEPROM() {

  kWh  = 0.0;
  cost = 0.0;

  saveEnergyDataToEEPROM();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Energy Reset!");
  delay(1500);

  updateLCD();
}

/*  
  -------------------------------------------------------
  © Circuit Diagrams (www.circuitdiagrams.in)
  All Rights Reserved.
  -------------------------------------------------------
*/
