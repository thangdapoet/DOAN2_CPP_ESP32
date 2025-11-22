#include <Arduino.h>
#include <ESP32Servo.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ---------------- Config ----------------
const char *ssid = "THANGDAPOET";
const char *password = "15112004";
bool wifiConnected = false;

// IP of the esp32-cam (change to your cam's IP after it's connected)
const char* CAM_IP = "192.168.212.57"; // <-- set this to ESP32-CAM IP

// ---------------- Prototypes ----------------
void showMainPrompt();
void adminMenu();
void notifyCamAsync(bool allowed, const String &uid); // async notify
void wifiTask(void *pv);

// ----------------= Config pins & constants =----------------
const byte ROWS = 4, COLS = 4;
char keysArr[ROWS][COLS] = {
  {'1','2','3','A'}, {'4','5','6','B'},
  {'7','8','9','C'}, {'*','0','#','D'}
};
byte rowPins[ROWS] = {27,14,12,13};
byte colPins[COLS] = {32,33,25,26};
Keypad keypad = Keypad( makeKeymap(keysArr), rowPins, colPins, ROWS, COLS );

LiquidCrystal_I2C lcd(0x27, 20, 4);

// RFID
const int RFID_SS = 2;
const int RFID_RST = 4;
MFRC522 rfid(RFID_SS, RFID_RST);//sda/ss 5; mosi 23, miso 19, sck 18

// Servo 360° (microseconds control)
Servo doorServo;
const int SERVO_PIN = 15;
int SERVO_NEUTRAL = 1500; // try adjust if servo drifts
const int SERVO_OPEN = 1700;
const int SERVO_CLOSE = 1310;
const int SERVO_DELAY = 800;

// Buzzer - GPIO17
const int BUZZ_PIN = 17;
const int BUZZ_CH = 6;
const int BUZZ_FREQ = 2000;
const int BUZZ_RES = 8;

// Admin UID (hard-coded)
const byte ADMIN_UID[4] = {0xAC, 0x64, 0x91, 0x05};

// Preferences
Preferences prefs;
const char *PREF_NS = "rfid_store";
const int MAX_CARDS = 60;

// State
String inputBuf = "";
String storedPassword;
int wrongCount = 0;
bool alarmActive = false;
unsigned long alarmEnd = 0;
const unsigned long ALARM_MS = 15000UL;
bool showingMain = false;

// WiFi reconnect interval (not used by loop anymore)
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 5000UL; // kept for compatibility

// ---------------- Utility functions ----------------
void buzz(int duty, unsigned long ms) {
  ledcWrite(BUZZ_CH, duty);
  delay(ms);
  ledcWrite(BUZZ_CH, 0);
}

String uidToHex(const MFRC522::Uid &u) {
  String s = "";
  for (byte i=0;i<u.size;i++){
    if (u.uidByte[i] < 0x10) s += "0";
    s += String(u.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

bool isAdmin(const MFRC522::Uid &u) {
  if (u.size != 4) return false;
  for (byte i=0;i<4;i++) if (u.uidByte[i] != ADMIN_UID[i]) return false;
  return true;
}

// ---------------- Preferences helpers ----------------
String loadPassword() {
  String pw = prefs.getString("pw", "");
  if (pw == "") {
    pw = "1234";
    prefs.putString("pw", pw);
  }
  return pw;
}
void savePassword(const String &pw){ prefs.putString("pw", pw); }
int cardCount(){ return prefs.getInt("n", 0); }
String cardAt(int i){ return prefs.getString(("uid"+String(i)).c_str(), ""); }
void setCard(int i, const String &uid){ prefs.putString(("uid"+String(i)).c_str(), uid); }
void setCount(int n){ prefs.putInt("n", n); }

bool addCard(const String &uidIn){
  String uid = uidIn; uid.toUpperCase();
  int n = cardCount();
  for (int i=0;i<n;i++) if (cardAt(i) == uid) return false;
  if (n >= MAX_CARDS) return false;
  setCard(n, uid); setCount(n+1); return true;
}

bool removeCard(const String &uidIn){
  String uid = uidIn; uid.toUpperCase();
  int n = cardCount();
  int found = -1;
  for (int i=0;i<n;i++) if (cardAt(i) == uid) { found = i; break; }
  if (found == -1) return false;
  for (int i=found;i<n-1;i++) setCard(i, cardAt(i+1));
  prefs.remove(("uid"+String(n-1)).c_str());
  setCount(n-1);
  return true;
}

bool isAllowed(const String &uidIn){
  String uid = uidIn; uid.toUpperCase();
  int n = cardCount();
  for (int i=0;i<n;i++) if (cardAt(i) == uid) return true;
  return false;
}

// Wait for a single card scan (up to timeoutMs), return true & uid if found
bool waitForCard(String &outUid, unsigned long timeoutMs = 15000) {
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      outUid = uidToHex(rfid.uid);
      rfid.PICC_HaltA();
      return true;
    }
    delay(30);
  }
  return false;
}

// ---------------- WiFi helpers ----------------
void ensureWiFiConnected() {
  // kept for compatibility but NOT used in loop anymore
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      Serial.println("WiFi connected! IP: " + WiFi.localIP().toString());
    }
    return;
  }
  // attempt reconnect (blocking short)
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 1500) { // short try
    delay(200);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
  } else {
    wifiConnected = false;
    Serial.println("\nWiFi disconnected!");
  }
}

// ---------------- Async notify (FreeRTOS task) ----------------
struct NotifyParams {
  bool allowed;
  char uid[33]; // up to 32 chars + null
};

void notifyTask(void *pvParameters) {
  NotifyParams *p = (NotifyParams*)pvParameters;
  if (!p) {
    vTaskDelete(NULL);
    return;
  }

  // ensure WiFi connected (try quick reconnect if needed)
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, password);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 3000) {
      vTaskDelay(200 / portTICK_PERIOD_MS);
    }
  }

  String statusStr = p->allowed ? "ok" : "bad";
  String url = String("http://") + CAM_IP + "/notify?status=" + statusStr;
  if (p->allowed && strlen(p->uid) > 0) {
    url += "&id=" + String(p->uid);
  }

  HTTPClient http;
  http.setConnectTimeout(1500); // 1.5s
  http.begin(url);
  int code = http.GET();
  if (code > 0) {
    Serial.printf("Notify (async) %s,%s -> %d\n", statusStr.c_str(), p->uid, code);
  } else {
    Serial.printf("Notify (async) failed %s,%s err=%d\n", statusStr.c_str(), p->uid, code);
  }
  http.end();

  free(p);
  vTaskDelete(NULL);
}

void notifyCamAsync(bool allowed, const String &uid) {
  NotifyParams *p = (NotifyParams*)malloc(sizeof(NotifyParams));
  if (!p) {
    Serial.println("notifyCamAsync: malloc failed");
    return;
  }
  p->allowed = allowed;
  memset(p->uid, 0, sizeof(p->uid));
  if (uid.length() > 0) {
    String tmp = uid;
    tmp.toUpperCase();
    tmp = tmp.substring(0, 32);
    tmp.toCharArray(p->uid, sizeof(p->uid));
  }

  BaseType_t r = xTaskCreatePinnedToCore(
    notifyTask,         // task function
    "notifyTask",       // name
    4096,               // stack size
    p,                  // param
    1,                  // priority
    NULL,               // task handle
    1                   // core
  );

  if (r != pdPASS) {
    Serial.println("notifyCamAsync: xTaskCreate failed");
    free(p);
  }
}

// Only writes WiFi status line (line 4). Should be called only when showingMain==true
void showWiFiStatus() {
  lcd.setCursor(0, 3);
  if (wifiConnected) {
    lcd.print("WiFi: Connected   ");
  } else {
    lcd.print("WiFi: Disconnected");
  }
}

// ---------------- UI ----------------
void showMainPrompt() {
  showingMain = true;
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Quet the / Nhap pass");
  lcd.setCursor(0,1);
  lcd.print("# = Enter * = Del");
  lcd.setCursor(0,2);
  String disp = "";
  for (size_t i=0;i<inputBuf.length();i++) disp += '*';
  lcd.print(disp);
  showWiFiStatus();
}

void leaveMainUI() { showingMain = false; }

// ---------------- Door servo routine ----------------
void performDoorCycle() {
  leaveMainUI();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Opening door...");
  doorServo.writeMicroseconds(SERVO_OPEN);
  delay(SERVO_DELAY);
  doorServo.writeMicroseconds(SERVO_NEUTRAL);
  delay(3000);
  lcd.setCursor(0,0);
  lcd.print("Closing door...");
  doorServo.writeMicroseconds(SERVO_CLOSE);
  delay(SERVO_DELAY);
  doorServo.writeMicroseconds(SERVO_NEUTRAL);
  showMainPrompt();
}

void openDoor(const String &who, bool admin=false) {
  leaveMainUI();
  lcd.clear();
  lcd.setCursor(0,0);
  if (admin) { lcd.print("Welcome Admin!!"); buzz(200,200); }
  else { lcd.print("Welcome !!"); buzz(120,150); }
  lcd.setCursor(0,1);
  lcd.print(who);
  performDoorCycle();
}

// ---------------- Wrong / Alarm handlers ----------------
void wrongNotify() {
  leaveMainUI();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("LOCK!!");
  buzz(180,200);
  delay(150);
  buzz(180,200);
  delay(400);
  showMainPrompt();
}

void startAlarm() {
  alarmActive = true;
  alarmEnd = millis() + ALARM_MS;
}

void stopAlarm() {
  alarmActive = false;
  ledcWrite(BUZZ_CH, 0);
}

// ---------------- Admin menu ----------------
void adminMenu() {
  leaveMainUI();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("ADMIN MODE");
  lcd.setCursor(0,1);
  lcd.print("1:CHG 2:DEL 3:ADD C:Exit");
  unsigned long start = millis();
  while (millis() - start < 15000UL) {
    char k = keypad.getKey();
    if (!k) { delay(30); continue; }
    if (k == 'C' || k == 'c' || k == 'D') {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Exit Admin");
      delay(300); showMainPrompt(); return;
    }
    if (k == '1') {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("CHANGE PASS");
      lcd.setCursor(0,1); lcd.print("Enter new pass:");
      String newPw = "";
      unsigned long t0 = millis();
      while (millis() - t0 < 15000UL) {
        char kk = keypad.getKey();
        if (kk) {
          t0 = millis();
          if (kk == '#') {
            if (newPw.length() > 0) {
              savePassword(newPw);
              storedPassword = newPw;
              lcd.clear(); lcd.setCursor(0,0); lcd.print("SAVED");
              buzz(160,150);
              delay(800);
              showMainPrompt();
              return;
            }
          } else if (kk == '*') {
            if (newPw.length()) newPw.remove(newPw.length()-1);
          } else {
            if (newPw.length() < 16) newPw += kk;
          }
          lcd.setCursor(0,2);
          String ds = "";
          for (size_t i=0;i<newPw.length();i++) ds += '*';
          lcd.print("                ");
          lcd.setCursor(0,2);
          lcd.print(ds);
        }
        delay(30);
      }
      // timeout
      if (newPw.length() > 0) {
        savePassword(newPw);
        storedPassword = newPw;
        lcd.clear(); lcd.setCursor(0,0); lcd.print("SAVED");
        buzz(160,150);
        delay(800);
        showMainPrompt(); return;
      } else {
        lcd.clear(); lcd.setCursor(0,0); lcd.print("No input"); delay(600); showMainPrompt(); return;
      }
    }
    if (k == '2') {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("DEL TAG: Scan");
      String uid;
      if (waitForCard(uid, 15000UL)) {
        uid.toUpperCase();
        if (removeCard(uid)) {
          lcd.clear(); lcd.setCursor(0,0); lcd.print("Deleted:"); lcd.setCursor(0,1); lcd.print(uid); buzz(160,120);
        } else {
          lcd.clear(); lcd.setCursor(0,0); lcd.print("Not found"); buzz(60,200);
        }
        delay(900); showMainPrompt(); return;
      } else {
        lcd.clear(); lcd.setCursor(0,0); lcd.print("No card"); delay(700); showMainPrompt(); return;
      }
    }
    if (k == '3') {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("ADD TAG: Scan");
      String uid;
      if (waitForCard(uid, 15000UL)) {
        uid.toUpperCase();
        if (addCard(uid)) { lcd.clear(); lcd.setCursor(0,0); lcd.print("Added:"); lcd.setCursor(0,1); lcd.print(uid); buzz(160,120); }
        else { lcd.clear(); lcd.setCursor(0,0); lcd.print("Exists/Full"); buzz(60,200); }
        delay(900); showMainPrompt(); return;
      } else {
        lcd.clear(); lcd.setCursor(0,0); lcd.print("No card"); delay(700); showMainPrompt(); return;
      }
    }
  }
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Admin timeout"); delay(600); showMainPrompt(); return;
}

// ---------------- Setup & Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(200);

  // lcd init
  Wire.begin(21, 22); // SDA, SCL
  lcd.init();
  lcd.backlight();
  lcd.clear();

  // initial main prompt (so user sees UI ASAP)
  showMainPrompt();

  // Start WiFi task (handles reconnect non-blocking)
  xTaskCreatePinnedToCore(
    wifiTask,
    "wifiTask",
    4096,
    NULL,
    1,
    NULL,
    0 // run on core 0
  );

  // buzzer pwm setup
  ledcSetup(BUZZ_CH, BUZZ_FREQ, BUZZ_RES);
  ledcAttachPin(BUZZ_PIN, BUZZ_CH);
  ledcWrite(BUZZ_CH, 0); // ensure off

  // servo
  doorServo.attach(SERVO_PIN, 500, 2400);
  doorServo.writeMicroseconds(SERVO_NEUTRAL);
  delay(200);

  // SPI RFID init (single attempt)
  SPI.begin(18, 19, 23, RFID_SS);
  rfid.PCD_Init();
  delay(100);

  // Check RFID version at startup (log only)
  byte rfidVersion = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("RFID Version: 0x"); Serial.println(rfidVersion, HEX);
  if (rfidVersion == 0x00 || rfidVersion == 0xFF) {
    Serial.println("WARNING: RFID not detected at startup. You may reset hardware manually.");
    // Do NOT block; continue running. You will manually reset if needed.
  } else {
    Serial.println("RFID initialized successfully");
  }

  // Preferences & password
  prefs.begin(PREF_NS, false);
  storedPassword = loadPassword();

  Serial.println("=== SYSTEM READY ===");
}

void loop() {
  // Update LCD WiFi status only when the state changes and main is showing
  static bool lastWifiState = false;
  if (wifiConnected != lastWifiState) {
    if (showingMain) showWiFiStatus();
    lastWifiState = wifiConnected;
  }

  // keypad handling
  char k = keypad.getKey();
  if (k) {
    Serial.println("Key pressed: " + String(k));
    if (k == '#') {
      if (inputBuf == storedPassword) {
        wrongCount = 0;
        openDoor("", false);
      } else {
        wrongCount++;
        if (wrongCount >= 3) {
          startAlarm();
          wrongCount = 0;
        } else {
          wrongNotify();
        }
      }
      inputBuf = "";
      showMainPrompt();
    } else if (k == '*') {
      if (inputBuf.length()) inputBuf.remove(inputBuf.length()-1);
      showMainPrompt();
    } else {
      if (inputBuf.length() < 16) inputBuf += k;
      showMainPrompt();
    }
  }

  // alarm handling (pulsing sound, non-blocking)
  if (alarmActive) {
    // beep pattern: 300ms on / 300ms off
    const unsigned long PERIOD = 300;
    if (((millis() / PERIOD) % 2) == 0) ledcWrite(BUZZ_CH, 255);
    else ledcWrite(BUZZ_CH, 0);

    if (millis() >= alarmEnd) {
      stopAlarm();
      showMainPrompt();
    }
  }

  // RFID handling (single init only, assume hardware present)
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uidHex = uidToHex(rfid.uid);
    uidHex.toUpperCase();

    Serial.print("RFID detected: "); Serial.println(uidHex);

    // Determine allowed or not (admin counts as allowed)
    bool allowed = false;
    if (isAdmin(rfid.uid)) allowed = true;
    else if (isAllowed(uidHex)) allowed = true;
    else allowed = false;

    // notify cam ASYNC after determining allowed or not, include uid when available
    notifyCamAsync(allowed, uidHex);

    // if alarm and admin scanned -> stop alarm
    if (alarmActive && isAdmin(rfid.uid)) {
      stopAlarm();
      ledcWrite(BUZZ_CH, 0);
      leaveMainUI();
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Alarm stopped");
      buzz(160,120);
      delay(700);
      showMainPrompt();
      rfid.PICC_HaltA();
      delay(200);
      return;
    }

    if (isAdmin(rfid.uid)) {
      openDoor("", true);
      wrongCount = 0;
      adminMenu();
      storedPassword = loadPassword(); // reload in case changed
    } else {
      if (allowed) openDoor("Card:"+uidHex, false);
      else wrongNotify();
    }

    rfid.PICC_HaltA();
    delay(200);
  }

  delay(10);
}

void wifiTask(void *pv) {
  (void) pv;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("wifiTask: starting connect attempt");
      WiFi.begin(ssid, password);
      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 3000) {
        vTaskDelay(200 / portTICK_PERIOD_MS);
      }
      if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.println("wifiTask: connected, IP: " + WiFi.localIP().toString());
      } else {
        wifiConnected = false;
        Serial.println("wifiTask: not connected");
      }
    } else {
      // keep flag consistent
      if (!wifiConnected) {
        wifiConnected = true;
        Serial.println("wifiTask: status changed to connected");
      }
    }

    vTaskDelay(3000 / portTICK_PERIOD_MS);
  }
}
