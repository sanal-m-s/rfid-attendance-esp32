
#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>                // ← prevents repeated sign-up
#include <time.h>

// ─── WiFi ────────────────────────────────────────────────
#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ─── Firebase ────────────────────────────────────────────
#define API_KEY      "YOUR_FIREBASE_API_KEY"
#define DATABASE_URL "https://YOUR-PROJECT-ID-default-rtdb.firebaseio.com/"

// ─── RFID Pins ───────────────────────────────────────────
#define SS_PIN   5
#define RST_PIN  4
#define SCK_PIN  18
#define MOSI_PIN 23
#define MISO_PIN 19

// ─── Other Pins ──────────────────────────────────────────
#define BUZZER   15

// ─── Objects ─────────────────────────────────────────────
MFRC522           rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences       prefs;

FirebaseData      fbdo;
FirebaseAuth      auth;
FirebaseConfig    config;

bool firebaseReady = false;

// ─── Buzzer helpers ──────────────────────────────────────
void beepSuccess() {
  digitalWrite(BUZZER, HIGH); delay(150); digitalWrite(BUZZER, LOW);
}

void beepDouble() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER, HIGH); delay(80);
    digitalWrite(BUZZER, LOW);  delay(80);
  }
}

void beepError() {
  digitalWrite(BUZZER, HIGH); delay(500); digitalWrite(BUZZER, LOW);
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);

  // ── LCD init ─────────────────────────────────────────────
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Starting...");
  delay(500);

  // ── STEP 1: Connect WiFi ─────────────────────────────────
  lcd.clear();
  lcd.print("Connecting WiFi");
  Serial.print("Connecting to WiFi");

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
    lcd.setCursor(0, 1);
    lcd.print("Check password");
    Serial.println("\nWiFi Failed! Check SSID/Password.");
    beepError();
    while (true) delay(1000);
  }

  Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());
  lcd.clear();
  lcd.print("WiFi Connected!");
  delay(800);

  // ── STEP 2: Sync NTP time BEFORE Firebase (SSL fix) ──────
  lcd.clear();
  lcd.print("Syncing Time...");
  Serial.print("Syncing NTP time");

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  struct tm t;
  int attempts = 0;
  while (!getLocalTime(&t) && attempts < 40) {
    Serial.print(".");
    delay(500);
    attempts++;
  }

  if (getLocalTime(&t)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    Serial.println("\nTime OK: " + String(buf));
    lcd.clear();
    lcd.print("Time Synced OK");
  } else {
    Serial.println("\nTime sync FAILED — SSL may fail");
    lcd.clear();
    lcd.print("Time Sync Fail");
    lcd.setCursor(0, 1);
    lcd.print("Check internet");
    beepError();
    delay(2000);
  }
  delay(600);

  // ── STEP 3: Firebase init (sign-up only once) ────────────
  lcd.clear();
  lcd.print("Firebase init..");
  Serial.println("Initializing Firebase...");

  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;

  // Read saved sign-up flag from flash memory
  prefs.begin("fb_prefs", false);
  String savedFlag = prefs.getString("signed_up", "");

  if (savedFlag != "yes") {
    // First time — do sign-up and save flag
    Serial.println("First run — signing up anonymously...");
    if (Firebase.signUp(&config, &auth, "", "")) {
      Serial.println("Firebase anonymous sign-up OK");
      prefs.putString("signed_up", "yes");   // save so we skip next time
      firebaseReady = true;
    } else {
      Serial.printf("Sign-up failed: %s\n", config.signer.signupError.message.c_str());
      lcd.clear();
      lcd.print("Firebase");
      lcd.setCursor(0, 1);
      lcd.print("Sign-up Failed!");
      beepError();
      delay(3000);
    }
  } else {
    // Already signed up before — skip signUp entirely
    Serial.println("Already signed up before — skipping signUp");
    firebaseReady = true;
  }
  prefs.end();

  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Wait for Firebase to be ready (max 10 seconds)
  Serial.print("Waiting for Firebase");
  int fbWait = 0;
  while (!Firebase.ready() && fbWait < 20) {
    Serial.print(".");
    delay(500);
    fbWait++;
  }

  if (Firebase.ready()) {
    Serial.println(" Firebase Ready!");
    lcd.clear();
    lcd.print("Firebase OK!");
  } else {
    Serial.println(" Firebase timeout");
    lcd.clear();
    lcd.print("Firebase Timeout");
    lcd.setCursor(0, 1);
    lcd.print("Retrying later..");
    beepError();
  }
  delay(800);

  // ── STEP 4: RFID init ────────────────────────────────────
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  rfid.PCD_Init();
  delay(50);

  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.println("RFID version: 0x" + String(ver, HEX));

  if (ver == 0x00 || ver == 0xFF) {
    Serial.println("RFID not detected! Check wiring.");
    lcd.clear();
    lcd.print("RFID Error!");
    lcd.setCursor(0, 1);
    lcd.print("Check wiring");
    beepError();
    while (true) delay(1000);
  }

  lcd.clear();
  lcd.print("System Ready!");
  lcd.setCursor(0, 1);
  lcd.print("Scan RFID Card");
  Serial.println("System Ready — waiting for card.");
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop() {

  // ── Wait for card ────────────────────────────────────────
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  // ── Build UID string ─────────────────────────────────────
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  Serial.println("Scanned UID: " + uid);

  // ── Student lookup ───────────────────────────────────────
  // ↓↓↓ Add your students here ↓↓↓
  String studentName = "";
  String rollNo      = "";

  if      (uid == "625E1505") { studentName = "Arun";  rollNo = "23CS101"; }
  else if (uid == "6441D605") { studentName = "Rahul"; rollNo = "23CS102"; }
  // else if (uid == "XXXXXXXX") { studentName = "Name";  rollNo = "23CSXXX"; }
  // ↑↑↑ Add more students above this line ↑↑↑
  else {
    Serial.println("Unknown card: " + uid);
    lcd.clear();
    lcd.print("Unknown Card!");
    lcd.setCursor(0, 1);
    lcd.print(uid);
    beepError();
    delay(2000);
    lcd.clear();
    lcd.print("Scan RFID Card");
    rfid.PICC_HaltA();
    return;
  }

  // ── Get current time ─────────────────────────────────────
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Time fetch failed — re-syncing");
    configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
    delay(2000);
    if (!getLocalTime(&timeinfo)) {
      lcd.clear();
      lcd.print("Time Error!");
      lcd.setCursor(0, 1);
      lcd.print("Check internet");
      beepError();
      delay(2000);
      lcd.clear();
      lcd.print("Scan RFID Card");
      rfid.PICC_HaltA();
      return;
    }
  }

  char dateStr[12];
  char timeStr[12];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);
  strftime(timeStr, sizeof(timeStr), "%I:%M %p",  &timeinfo);

  // ── Firebase path ────────────────────────────────────────
  String basePath = "/attendance/" + String(dateStr) + "/" + uid;

  // ── Check Firebase ready ─────────────────────────────────
  if (!Firebase.ready()) {
    Serial.println("Firebase not ready — skipping");
    lcd.clear();
    lcd.print("Firebase");
    lcd.setCursor(0, 1);
    lcd.print("Not Ready!");
    beepError();
    delay(2000);
    lcd.clear();
    lcd.print("Scan RFID Card");
    rfid.PICC_HaltA();
    return;
  }

  // ── Duplicate check ──────────────────────────────────────
  if (Firebase.RTDB.getString(&fbdo, basePath + "/name")) {
    Serial.println("Already marked today: " + studentName);
    lcd.clear();
    lcd.print(studentName);
    lcd.setCursor(0, 1);
    lcd.print("Already Marked!");
    beepDouble();
    delay(2500);
    lcd.clear();
    lcd.print("Scan RFID Card");
    rfid.PICC_HaltA();
    return;
  }

  // ── Write to Firebase ────────────────────────────────────
  bool ok = Firebase.RTDB.setString(&fbdo, basePath + "/name",   studentName)
         && Firebase.RTDB.setString(&fbdo, basePath + "/rollno", rollNo)
         && Firebase.RTDB.setString(&fbdo, basePath + "/time",   String(timeStr))
         && Firebase.RTDB.setString(&fbdo, basePath + "/status", "Present");

  if (ok) {
    Serial.printf("✅ Saved: %s | %s | %s | %s\n",
                  studentName.c_str(), rollNo.c_str(), dateStr, timeStr);
    beepSuccess();
    lcd.clear();
    lcd.print(studentName);
    lcd.setCursor(0, 1);
    lcd.print(String(timeStr) + " OK");

  } else {
    Serial.println("Firebase write error: " + fbdo.errorReason());
    lcd.clear();
    lcd.print("Firebase Error!");
    lcd.setCursor(0, 1);
    lcd.print(fbdo.errorReason().substring(0, 16));
    beepError();
  }

  delay(3000);
  lcd.clear();
  lcd.print("Scan RFID Card");
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
