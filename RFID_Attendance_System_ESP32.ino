/*
  RFID Attendance System — ESP32
  Fixed version: correct SPI pins, token helper, duplicate check,
  proper anonymous auth, IST time zone
*/

#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>      
#include <addons/RTDBHelper.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

// ─── WiFi ────────────────────────────────────────────────
#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ─── Firebase ────────────────────────────────────────────
#define API_KEY      "YOUR_FIREBASE_API_KEY"
#define DATABASE_URL "https://YOUR-PROJECT-ID-default-rtdb.firebaseio.com/"

// ─── RFID Pins (safe ESP32 pins — no boot-critical GPIOs) ─
#define SS_PIN   5    // SDA/CS
#define RST_PIN  4
#define SCK_PIN  18
#define MOSI_PIN 23
#define MISO_PIN 19

// ─── Other pins ──────────────────────────────────────────
#define BUZZER   15

// ─── Objects ─────────────────────────────────────────────
MFRC522          rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

FirebaseData     fbdo;
FirebaseAuth     auth;
FirebaseConfig   config;

bool firebaseReady = false;

// ════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Connecting WiFi");

  // ─── WiFi ──────────────────────────────────────────────
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    lcd.clear();
    lcd.print("WiFi Failed!");
    Serial.println("WiFi connection failed.");
    while (true) delay(1000);   // halt — fix credentials
  }

  Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());
  lcd.clear();
  lcd.print("WiFi OK");
  delay(800);

  // ─── Firebase (anonymous sign-up once per device) ──────
  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;

  // Anonymous auth — creates one user account per device.
  // Works with "Anonymous" enabled in Firebase Auth console.
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase sign-up OK");
    firebaseReady = true;
  } else {
    Serial.printf("Sign-up failed: %s\n", config.signer.signupError.message.c_str());
  }

  config.token_status_callback = tokenStatusCallback;   // from TokenHelper.h
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // ─── RFID SPI ──────────────────────────────────────────
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  rfid.PCD_Init();
  delay(50);
  Serial.println("RFID ready, version: " + String(rfid.PCD_ReadRegister(MFRC522::VersionReg), HEX));

  // ─── NTP time (IST = UTC+5:30 = 19800 s) ──────────────
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time");
  struct tm t;
  int attempts = 0;
  while (!getLocalTime(&t) && attempts < 20) {
    Serial.print(".");
    delay(500);
    attempts++;
  }
  Serial.println(getLocalTime(&t) ? " OK" : " FAILED (check internet)");

  lcd.clear();
  lcd.print("Scan RFID Card");
  Serial.println("Ready.");
}

// ════════════════════════════════════════════════════════
void loop() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  // Build UID string
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  Serial.println("Scanned UID: " + uid);

  // ─── Student lookup ────────────────────────────────────
  // Add more entries here as needed
  String studentName = "";
  String rollNo      = "";

  if      (uid == "A4A7A800") { studentName = "Arun";  rollNo = "23CS101"; }
  else if (uid == "625E1505") { studentName = "Rahul"; rollNo = "23CS102"; }
  else {
    lcd.clear();
    lcd.print("Unknown Card");
    lcd.setCursor(0, 1);
    lcd.print(uid);
    Serial.println("Unknown UID: " + uid);
    delay(2000);
    lcd.clear();
    lcd.print("Scan RFID Card");
    rfid.PICC_HaltA();
    return;
  }

  // ─── Get current time ──────────────────────────────────
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Time fetch failed");
    lcd.clear();
    lcd.print("Time Error!");
    delay(2000);
    lcd.clear();
    lcd.print("Scan RFID Card");
    rfid.PICC_HaltA();
    return;
  }

  char dateStr[12];
  char timeStr[12];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d",   &timeinfo);
  strftime(timeStr, sizeof(timeStr), "%I:%M %p",   &timeinfo);

  // ─── Firebase path ─────────────────────────────────────
  String basePath = "/attendance/" + String(dateStr) + "/" + uid;

  // ─── Duplicate check ───────────────────────────────────
  if (Firebase.RTDB.getString(&fbdo, basePath + "/name")) {
    // Record already exists for today
    Serial.println("Already marked: " + studentName);
    lcd.clear();
    lcd.print(studentName);
    lcd.setCursor(0, 1);
    lcd.print("Already Marked!");

    // Short double-beep to signal duplicate
    for (int i = 0; i < 2; i++) {
      digitalWrite(BUZZER, HIGH); delay(80);
      digitalWrite(BUZZER, LOW);  delay(80);
    }

    delay(2500);
    lcd.clear();
    lcd.print("Scan RFID Card");
    rfid.PICC_HaltA();
    return;
  }

  // ─── Write attendance ──────────────────────────────────
  bool ok = Firebase.RTDB.setString(&fbdo, basePath + "/name",   studentName)
         && Firebase.RTDB.setString(&fbdo, basePath + "/rollno", rollNo)
         && Firebase.RTDB.setString(&fbdo, basePath + "/time",   String(timeStr))
         && Firebase.RTDB.setString(&fbdo, basePath + "/status", "Present");

  if (ok) {
    Serial.printf("Saved: %s | %s | %s | %s\n",
                  studentName.c_str(), rollNo.c_str(), dateStr, timeStr);

    // Single beep = success
    digitalWrite(BUZZER, HIGH); delay(150); digitalWrite(BUZZER, LOW);

    lcd.clear();
    lcd.print(studentName + " " + rollNo);
    lcd.setCursor(0, 1);
    lcd.print(String(timeStr) + " OK");

  } else {
    Serial.println("Firebase write error: " + fbdo.errorReason());
    lcd.clear();
    lcd.print("Firebase Error!");
    lcd.setCursor(0, 1);
    lcd.print(fbdo.errorReason().substring(0, 16));
  }

  delay(3000);
  lcd.clear();
  lcd.print("Scan RFID Card");
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}