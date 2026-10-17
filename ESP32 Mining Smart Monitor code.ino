#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_ADXL345_U.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Firebase_ESP_Client.h>

// =====================================================
// WI-FI + FIREBASE
// =====================================================
// Wi-Fi name and password are NOT hardcoded.
// They are entered from the ESP32 setup web page.

#define FIREBASE_API_KEY     ""
#define FIREBASE_DATABASE_URL ""

#define FIREBASE_USER_EMAIL    ""
#define FIREBASE_USER_PASSWORD ""

FirebaseData fbdo;
FirebaseData fbCommand;
FirebaseAuth auth;
FirebaseConfig config;

bool firebaseStarted = false;
bool firebaseServerOnline = false;

unsigned long lastFirebaseUpload = 0;
unsigned long lastFirebaseRestart = 0;

const unsigned long FIREBASE_UPLOAD_INTERVAL = 5000;
const unsigned long FIREBASE_RESTART_INTERVAL = 30000;

int firebaseFailCount = 0;
const int FIREBASE_MAX_FAILURES = 5;
// =====================================================
// FIREBASE REMOTE COMMAND
// =====================================================

int firebaseCommand = 0;

unsigned long lastFirebaseCommandCheck = 0;

const unsigned long FIREBASE_COMMAND_INTERVAL = 5000;
// =====================================================
// WI-FI SETUP WEB PAGE
// =====================================================

WebServer webServer(80);
Preferences wifiPrefs;

bool wifiConfigured = false;
bool setupAPMode = false;

unsigned long wifiConnectStart = 0;
unsigned long lastWiFiRetry = 0;

const unsigned long WIFI_CONNECT_TIMEOUT = 20000;
const unsigned long WIFI_RETRY_INTERVAL = 10000;

const char *SETUP_AP_NAME = "MiningMonitor-Setup";
const char *SETUP_AP_PASSWORD = "12345678";



// =====================================================
// PINS
// =====================================================

#define DHT_PIN       4
#define MQ6_PIN       34
#define LED_PIN       2

#define OK_BUTTON     18
#define CHANGE_BUTTON 19

#define SDA_PIN       21
#define SCL_PIN       22

// =====================================================
// CHANGE BUTTON INTERRUPT
// =====================================================

volatile bool changeButtonPressed = false;

// =====================================================
// DHT11
// =====================================================

#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

// =====================================================
// OLED
// =====================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  -1
);

// =====================================================
// ADXL345
// =====================================================

Adafruit_ADXL345_Unified adxl =
  Adafruit_ADXL345_Unified(12345);

bool adxlOK = false;

// =====================================================
// SENSOR VALUES
// =====================================================

float temperature = 0;
float humidity = 0;

int gasValue = 0;

// MQ6 warning threshold
const int GAS_WARNING = 800;

// Current ADXL values
float accelX = 0;
float accelY = 0;
float accelZ = 0;

// Stored normal ADXL values
float baseX = 0;
float baseY = 0;
float baseZ = 0;

// ADXL warning percentage
const float ADXL_CHANGE_PERCENT = 0.10;

// =====================================================
// STATUS
// =====================================================

bool adxlBaseSet = false;

bool gasWarning = false;
bool accelWarning = false;

// MQ6 must first go below 800 before
// the warning can be triggered again.
bool gasSafe = true;

// =====================================================
// SCREEN
// =====================================================

int currentScreen = 0;

unsigned long lastScreenChange = 0;

const unsigned long SCREEN_RETURN_TIME = 60000;

// =====================================================
// SENSOR TIMING
// =====================================================

unsigned long lastSensorRead = 0;

const unsigned long SENSOR_INTERVAL = 1000;

// =====================================================
// LED
// =====================================================

bool ledState = false;
unsigned long lastBlink = 0;

const unsigned long WARNING_BLINK_TIME = 250;

// =====================================================
// OK BUTTON
// =====================================================

unsigned long okPressStart = 0;
bool okWasPressed = false;

const unsigned long LONG_PRESS = 2000;

// =====================================================
// SET MESSAGE
// =====================================================

bool setMessage = false;
unsigned long setMessageStart = 0;

const unsigned long SET_MESSAGE_TIME = 1000;
// =====================================================
// ADXL AUTO SET
// =====================================================

bool adxlRecalibrating = true;

float stableX = 0;
float stableY = 0;
float stableZ = 0;

unsigned long stableStartTime = 0;

const unsigned long ADXL_STABLE_TIME = 2000;
const float ADXL_STABLE_PERCENT = 0.10;
const float ADXL_MIN_CHANGE = 0.05;


// =====================================================
// TOTAL ACCELERATION
// =====================================================

float getTotalAcceleration() {

  return sqrt(
    accelX * accelX +
    accelY * accelY +
    accelZ * accelZ
  );
}


// =====================================================
// CHECK 10% ADXL CHANGE
// =====================================================

bool adxlChanged10Percent() {

  if (!adxlBaseSet || adxlRecalibrating) {
    return false;
  }

  // Calculate percentage change for each axis
  float xChange = abs(accelX - baseX);
  float yChange = abs(accelY - baseY);
  float zChange = abs(accelZ - baseZ);

  float xLimit = max(abs(baseX) * ADXL_CHANGE_PERCENT, 0.05f);
  float yLimit = max(abs(baseY) * ADXL_CHANGE_PERCENT, 0.05f);
  float zLimit = max(abs(baseZ) * ADXL_CHANGE_PERCENT, 0.05f);

  // Warning if ANY axis changes more than 10%
  if (xChange > xLimit ||
      yChange > yLimit ||
      zChange > zLimit) {

    return true;
  }

  return false;
}

// =====================================================
// CHECK ADXL STABILITY
// =====================================================

bool adxlValuesStable()
{
  float xLimit = max(ADXL_MIN_CHANGE,
                     abs(stableX) * ADXL_STABLE_PERCENT);

  float yLimit = max(ADXL_MIN_CHANGE,
                     abs(stableY) * ADXL_STABLE_PERCENT);

  float zLimit = max(ADXL_MIN_CHANGE,
                     abs(stableZ) * ADXL_STABLE_PERCENT);

  bool xStable = abs(accelX - stableX) <= xLimit;
  bool yStable = abs(accelY - stableY) <= yLimit;
  bool zStable = abs(accelZ - stableZ) <= zLimit;

  return xStable && yStable && zStable;
}


// =====================================================
// AUTOMATIC ADXL SET
// =====================================================

void checkAutomaticADXLSet()
{
  if (!adxlOK)
    return;

  if (adxlBaseSet && !adxlRecalibrating)
    return;

  // First value
  if (stableStartTime == 0)
  {
    stableX = accelX;
    stableY = accelY;
    stableZ = accelZ;

    stableStartTime = millis();

    Serial.println("ADXL: Waiting for stable values...");
    return;
  }

  // Movement detected
  if (!adxlValuesStable())
  {
    stableX = accelX;
    stableY = accelY;
    stableZ = accelZ;

    stableStartTime = millis();

    Serial.println("ADXL: Movement detected");
    return;
  }

  // Stable for 2 seconds
  if (millis() - stableStartTime >= ADXL_STABLE_TIME)
  {
    baseX = accelX;
    baseY = accelY;
    baseZ = accelZ;

    adxlBaseSet = true;
    adxlRecalibrating = false;

    stableStartTime = 0;

    Serial.println("ADXL BASE AUTO SET");
  }
}

// =====================================================
// WI-FI SETUP WEB PAGE
// =====================================================

String htmlPage()
{
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Mining Smart Monitor - Wi-Fi Setup</title>
  <style>
    body{font-family:Arial,sans-serif;background:#eef2f3;margin:0;padding:20px}
    .box{max-width:420px;margin:30px auto;background:white;padding:25px;
         border-radius:15px;box-shadow:0 4px 15px rgba(0,0,0,.15)}
    h2{text-align:center;margin-top:0;color:#123}
    label{display:block;margin-top:15px;font-weight:bold}
    input{width:100%;box-sizing:border-box;padding:12px;margin-top:6px;
          border:1px solid #aaa;border-radius:8px;font-size:16px}
    button{width:100%;padding:13px;margin-top:20px;border:0;border-radius:8px;
           background:#1769aa;color:white;font-size:17px}
    .info{background:#f0f7ff;padding:12px;border-radius:8px;line-height:1.5}
    .small{text-align:center;color:#666;font-size:13px;margin-top:15px}
  </style>
</head>
<body>
  <div class="box">
    <h2>Mining Smart Monitor</h2>
    <div class="info">
      <b>Wi-Fi Setup</b><br>
      Enter the Wi-Fi name (SSID) and password that the ESP32 should use.
    </div>

    <form action="/save" method="POST">
      <label>Wi-Fi Name (SSID)</label>
      <input name="ssid" type="text" maxlength="63" required>

      <label>Wi-Fi Password</label>
      <input name="password" type="password" maxlength="63">

      <button type="submit">Save & Connect</button>
    </form>

    <div class="small">
      Setup network: MiningMonitor-Setup<br>
      ESP32 setup Powered by MOIZ
    </div>
  </div>
</body>
</html>
)rawliteral";

  return page;
}

void handleWiFiRoot()
{
  webServer.send(200, "text/html", htmlPage());
}

void handleWiFiSave()
{
  if (!webServer.hasArg("ssid")) {
    webServer.send(400, "text/plain", "Wi-Fi name is required.");
    return;
  }

  String ssid = webServer.arg("ssid");
  String password = webServer.arg("password");

  ssid.trim();

  if (ssid.length() == 0) {
    webServer.send(400, "text/plain", "Wi-Fi name cannot be empty.");
    return;
  }

  // Save credentials in ESP32 non-volatile memory.
  wifiPrefs.begin("wifi", false);
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("password", password);
  wifiPrefs.end();

  wifiConfigured = true;

  String response = R"rawliteral(
<!DOCTYPE html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connecting</title></head>
<body style="font-family:Arial;text-align:center;padding:40px">
<h2>Wi-Fi credentials saved</h2>
<p>The ESP32 is now trying to connect to:</p>
<p><b>)rawliteral";
  response += ssid;
  response += R"rawliteral(</b></p>
<p>You can close this page.</p>
</body></html>
)rawliteral";

  webServer.send(200, "text/html", response);

  Serial.println("Wi-Fi credentials saved from web page.");

  // Stop setup AP and start normal Wi-Fi connection.
  delay(200);
  webServer.stop();
  WiFi.softAPdisconnect(true);
  setupAPMode = false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  wifiConnectStart = millis();
  lastWiFiRetry = millis();

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("CONNECTING WIFI");
  display.setCursor(0, 22);
  display.println("Please wait...");
  display.display();
}

void startWiFiSetupAP()
{
  setupAPMode = true;
firebaseStarted = false;
firebaseServerOnline = false;

  WiFi.disconnect(true, true);
  delay(100);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_NAME, SETUP_AP_PASSWORD);

  IPAddress apIP = WiFi.softAPIP();

  Serial.println();
  Serial.println("==============================");
  Serial.println("WI-FI SETUP MODE");
  Serial.print("Network: ");
  Serial.println(SETUP_AP_NAME);
  Serial.print("Password: ");
  Serial.println(SETUP_AP_PASSWORD);
  Serial.print("Open: http://");
  Serial.println(apIP);
  Serial.println("==============================");

  webServer.on("/", HTTP_GET, handleWiFiRoot);
  webServer.on("/save", HTTP_POST, handleWiFiSave);

  webServer.onNotFound([]() {
    webServer.send(404, "text/plain", "Open http://192.168.4.1"); 
  }); 
 
  webServer.begin(); 
 
  display.clearDisplay(); 
  display.setTextColor(SSD1306_WHITE); 
  display.setTextSize(1); 
  display.setCursor(0, 0); 
  display.println("CONNECT WIFI"); 
  display.setCursor(0, 17); 
  display.println("SSID: MiningsmartMonitor"); 
  display.setCursor(0, 34); 
  display.println("IP: 192.168.4.1"); 
  display.setCursor(0, 51); 
  display.println("Long OK = CHANGE"); 
  display.display(); 
} 
 
void loadWiFiCredentials() 
{ 
  wifiPrefs.begin("wifi", true); 
  String ssid = wifiPrefs.getString("ssid", ""); 
  String password = wifiPrefs.getString("password", ""); 
  wifiPrefs.end(); 
 
  wifiConfigured = (ssid.length() > 0); 
 
  if (!wifiConfigured) { 
    Serial.println("No Wi-Fi credentials saved."); 
    startWiFiSetupAP(); 
    return; 
  } 
 
  Serial.print("Saved Wi-Fi found: "); 
  Serial.println(ssid); 
 
  WiFi.mode(WIFI_STA); 
  WiFi.begin(ssid.c_str(), password.c_str()); 
 
  wifiConnectStart = millis(); 
  lastWiFiRetry = millis(); 
 
  Serial.println("Trying saved Wi-Fi in background..."); 
} 
 
void maintainWiFi() 
{ 
  if (setupAPMode) { 
    webServer.handleClient(); 
    return; 
  } 
 
  if (WiFi.status() == WL_CONNECTED) { 
  return; 
} 
firebaseServerOnline = false;
 
  // Retry the saved Wi-Fi without blocking the sensor/buttons. 
  if (millis() - lastWiFiRetry >= WIFI_RETRY_INTERVAL) { 
    lastWiFiRetry = millis(); 
 
    wifiPrefs.begin("wifi", true); 
    String ssid = wifiPrefs.getString("ssid", ""); 
    String password = wifiPrefs.getString("password", ""); 
    wifiPrefs.end(); 
 
    if (ssid.length() > 0) { 
      Serial.println("Retrying Wi-Fi..."); 
      WiFi.disconnect(); 
      WiFi.begin(ssid.c_str(), password.c_str()); 
      wifiConnectStart = millis(); 
    } 
  } 
 
  // If the saved Wi-Fi cannot be reached, create setup AP again. 
  if (wifiConfigured && 
      wifiConnectStart != 0 && 
      millis() - wifiConnectStart >= WIFI_CONNECT_TIMEOUT) { 
 
    Serial.println("Wi-Fi connection timeout."); 
    Serial.println("Starting Wi-Fi setup page."); 
 
    startWiFiSetupAP(); 
  } 
} 
 
void resetWiFiSettings() 
{ 
  Serial.println("Wi-Fi reset requested from OLED."); 
 
  wifiPrefs.begin("wifi", false); 
  wifiPrefs.clear(); 
  wifiPrefs.end(); 
 
  wifiConfigured = false; 
  firebaseStarted = false; 
 
  WiFi.disconnect(true, true); 
  delay(100); 
 
  startWiFiSetupAP(); 
} 
 
// ===================================================== 
// START FIREBASE 
// ===================================================== 
 
void startFirebase() { 
 
  if (WiFi.status() != WL_CONNECTED) { 
    return; 
  } 
 
  Serial.println("Starting Firebase..."); 
 
  config.api_key = FIREBASE_API_KEY; 
  config.database_url = FIREBASE_DATABASE_URL; 
 
  auth.user.email = FIREBASE_USER_EMAIL; 
  auth.user.password = FIREBASE_USER_PASSWORD; 
 
  // Recommended SSL buffers for Firebase-ESP-Client v4.4.x 
fbdo.setBSSLBufferSize(4096, 1024); 
fbCommand.setBSSLBufferSize(4096, 1024); 
 
fbdo.setResponseSize(2048); 
fbCommand.setResponseSize(2048); 
 
Firebase.begin(&config, &auth); 
Firebase.reconnectWiFi(true); 
 
firebaseFailCount = 0; 
lastFirebaseRestart = millis(); 
 
if (Firebase.ready()) 
{ 
  firebaseStarted = true; 
  Serial.println("Firebase started and ready"); 
} 
else 
{ 
  firebaseStarted = false; 
  Serial.println("Firebase started but NOT ready"); 
} 
 
  Serial.println("Firebase started"); 
} 
 
void maintainFirebase() 
{ 
  // Wi-Fi not connected 
  if (WiFi.status() != WL_CONNECTED)
{
  firebaseStarted = false;
  firebaseServerOnline = false;
  return;
} 
 
  // Firebase is running 
  if (firebaseStarted) 
  { 
    // Check whether Firebase is actually ready 
    if (!Firebase.ready()) 
    { 
      return; 
    } 
 
    return; 
  } 
 
  // Wait before trying Firebase again 
  if (millis() - lastFirebaseRestart < FIREBASE_RESTART_INTERVAL) 
  { 
    return; 
  } 
 
  Serial.println("Trying to start Firebase..."); 
 
  startFirebase(); 
} 
 
// ===================================================== 
// UPLOAD SENSOR DATA TO FIREBASE 
// ===================================================== 
 
void uploadToFirebase() { 
 
  if (WiFi.status() != WL_CONNECTED) { 
    return; 
  } 
 
  if (!firebaseStarted) { 
    return; 
  } 
 
  if (!Firebase.ready()) { 
    return; 
  } 
 
  FirebaseJson json; 
 
  json.set("temperature", temperature); 
  json.set("humidity", humidity); 
  json.set("gas", gasValue); 
  json.set("accelX", accelX); 
  json.set("accelY", accelY); 
  json.set("accelZ", accelZ); 
  json.set("gasWarning", gasWarning); 
  json.set("accelWarning", accelWarning); 
  json.set("lastUpdate/.sv", "timestamp"); 
 
  if (Firebase.RTDB.updateNode(&fbdo, "/sensors", &json)) {

  Serial.println("Firebase: sensor data uploaded");

  firebaseServerOnline = true;

  firebaseFailCount = 0;
} 
  else {

  firebaseServerOnline = false;

  firebaseFailCount++;

  Serial.print("Firebase upload failed: "); 
    Serial.println(fbdo.errorReason()); 
 
    Serial.print("Firebase failures: "); 
    Serial.println(firebaseFailCount); 
 
    // Too many consecutive failures. 
    // Allow Firebase to be restarted. 
    if (firebaseFailCount >= FIREBASE_MAX_FAILURES) { 
 
      Serial.println("Firebase connection unstable."); 
      Serial.println("Firebase will be restarted."); 
 
      firebaseStarted = false; 
      firebaseFailCount = 0; 
    } 
  } 
} 
 
// ===================================================== 
// CHECK FIREBASE REMOTE COMMAND 
// ===================================================== 
 
void checkFirebaseCommand() 
{ 
  if (WiFi.status() != WL_CONNECTED) 
    return; 
 
  if (!firebaseStarted) 
    return; 
 
  if (!Firebase.ready()) 
    return; 
 
  lastFirebaseCommandCheck = millis(); 
 
  // -------------------------------------------- 
  // READ COMMAND USING SEPARATE FirebaseData 
  // -------------------------------------------- 
 
  if (Firebase.RTDB.getInt(&fbCommand, "/sensors/command")) 
  { 
    firebaseCommand = fbCommand.intData(); 
 
    if (firebaseCommand == 2) 
    { 
      Serial.println("Firebase command 2 received"); 
 
      // ========================================== 
      // STOP MQ6 WARNING 
      // ========================================== 
 
      if (gasWarning) 
      { 
        if (gasValue < GAS_WARNING) 
        { 
          gasWarning = false; 
          gasSafe = true; 
 
          Serial.println("MQ6 WARNING STOPPED BY FIREBASE"); 
        } 
        else 
        { 
          Serial.println("MQ6 GAS STILL HIGH - WARNING ACTIVE"); 
        } 
      } 
 
      // ========================================== 
      // STOP ADXL WARNING 
      // ========================================== 
 
      if (accelWarning) 
      { 
        accelWarning = false; 
 
        adxlBaseSet = false; 
        adxlRecalibrating = true; 
 
        stableStartTime = 0; 
 
        Serial.println("LANDSLIDE WARNING STOPPED BY FIREBASE"); 
        Serial.println("ADXL WAITING FOR STABLE VALUE"); 
      } 
 
      // ========================================== 
      // RESET COMMAND TO 0 
      // ========================================== 
 
      if (Firebase.RTDB.setInt( 
            &fbCommand, 
            "/sensors/command", 
            0)) 
      { 
        Serial.println("Firebase command reset to 0"); 
      } 
      else 
      { 
        Serial.print("Command reset failed: "); 
        Serial.println(fbCommand.errorReason()); 
      } 
 
      firebaseCommand = 0; 
    } 
  } 
  else 
  { 
    Serial.print("Firebase command read failed: "); 
    Serial.println(fbCommand.errorReason()); 
  } 
} 
 
// ===================================================== 
// READ SENSORS 
// ===================================================== 
 
void readSensors() { 
 
  // =================================================== 
  // DHT11 
  // =================================================== 
 
  float t = dht.readTemperature(); 
  float h = dht.readHumidity(); 
 
  if (!isnan(t)) { 
    temperature = t; 
  } 
 
  if (!isnan(h)) { 
    humidity = h; 
  } 
 
 
  // =================================================== 
  // MQ6 
  // =================================================== 
 
  gasValue = analogRead(MQ6_PIN); 
 
 
  // =================================================== 
  // ADXL345 
  // =================================================== 
 
  if (adxlOK) { 
 
    sensors_event_t event; 
 
    adxl.getEvent(&event); 
 
    accelX = 
      event.acceleration.x / 9.80665; 
 
    accelY = 
      event.acceleration.y / 9.80665; 
 
    accelZ = 
      event.acceleration.z / 9.80665; 
 
 
   // =================================================== 
// ADXL345 LANDSLIDE WARNING 
// =================================================== 
 
if ( 
  adxlBaseSet && 
  !adxlRecalibrating && 
  adxlChanged10Percent() 
) { 
 
  if (!accelWarning) { 
    accelWarning = true; 
 
    Serial.println(); 
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!"); 
    Serial.println("!!! LANDSLIDE WARNING !!!"); 
    Serial.println("!!! ADXL345 MOVEMENT !!!"); 
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!"); 
  } 
} 
  } 
 
 
  // =================================================== 
// MQ6 WARNING 
// =================================================== 
 
// Gas has returned to safe level 
if (gasValue < GAS_WARNING) 
{ 
  gasSafe = true; 
 
  // Automatically remove warning when gas 
  // goes below 800 
  gasWarning = false; 
} 
 
// Gas is dangerous 
else if (gasValue >= GAS_WARNING) 
{ 
  // Only trigger a new warning after 
  // the gas previously became safe. 
  if (gasSafe) 
  { 
    gasWarning = true; 
    gasSafe = false; 
 
    Serial.println("!!! MQ6 GAS WARNING !!!"); 
  } 
} 
} 
 
 
// ===================================================== 
// ANY WARNING 
// ===================================================== 
 
bool anyWarning() { 
 
  return gasWarning || accelWarning; 
} 
 
 
// ===================================================== 
// SIH SCREEN 
// ===================================================== 
 
void showSIH() { 
 
  display.clearDisplay(); 
 
  display.setTextColor( 
    SSD1306_WHITE 
  ); 
 
  display.setTextSize(2); 
 
  display.setCursor(25, 5); 
 
  display.println("MINING"); 
 
  display.setTextSize(1); 
 
  display.setCursor(18, 45); 
 
  display.println( 
    "SMART MONITOR" 
  ); 
 
  display.display(); 
} 
 
 
// ===================================================== 
// DHT SCREEN 
// ===================================================== 
 
void showDHT() { 
 
  display.clearDisplay(); 
 
  display.setTextColor( 
    SSD1306_WHITE 
  ); 
 
  display.setTextSize(1); 
 
  display.setCursor(0, 0); 
 
  display.println("DHT11"); 
 
  display.drawLine( 
    0, 10, 
    127, 10, 
    SSD1306_WHITE 
  ); 
 
  display.setTextSize(2); 
 
  display.setCursor(0, 20); 
 
  display.print("T:"); 
 
  display.print( 
    temperature, 
    1 
  ); 
 
  display.print("C"); 
 
  display.setCursor(0, 45); 
 
  display.print("H:"); 
 
  display.print( 
    humidity, 
    1 
  ); 
 
  display.print("%"); 
 
  display.display(); 
} 
 
 
// ===================================================== 
// MQ6 SCREEN 
// ===================================================== 
 
void showMQ() { 
 
  display.clearDisplay(); 
 
  display.setTextColor( 
    SSD1306_WHITE 
  ); 
 
  display.setTextSize(1); 
 
  display.setCursor(0, 0); 
 
  display.println( 
    "MQ6 GAS SENSOR" 
  ); 
 
  display.drawLine( 
    0, 10, 
    127, 10, 
    SSD1306_WHITE 
  ); 
 
  display.setTextSize(2); 
 
  display.setCursor(0, 20); 
 
  display.print("GAS:"); 
 
  display.setCursor(65, 20); 
 
  display.print( 
    gasValue 
  ); 
 
  display.setTextSize(1); 
 
  display.setCursor(0, 50); 
 
  if (gasValue >= GAS_WARNING) { 
 
    display.print( 
      "WARNING" 
    ); 
 
  } else { 
 
    display.print( 
      "STATUS: OK" 
    ); 
  } 
 
  display.display(); 
} 
 
 
// ===================================================== 
// ADXL345 SCREEN 
// ===================================================== 
 
void showADXL() { 
 
  display.clearDisplay(); 
 
  display.setTextColor( 
    SSD1306_WHITE 
  ); 
 
  display.setTextSize(1); 
 
  display.setCursor(0, 0); 
 
  display.println( 
    "ADXL345" 
  ); 
 
  display.drawLine( 
    0, 10, 
    127, 10, 
    SSD1306_WHITE 
  ); 
 
 
  // Show SET message 
  if (setMessage) { 
 
    display.setTextSize(2); 
 
    display.setCursor(35, 25); 
 
    display.println( 
      "SET" 
    ); 
 
    display.setTextSize(1); 
 
    display.setCursor(22, 48); 
 
    display.println( 
      "VALUE SAVED" 
    ); 
 
    display.display(); 
 
    return; 
  } 
 
 
  // No baseline yet 
  if (!adxlBaseSet) { 
 
    display.setTextSize(1); 
 
    display.setCursor(0, 25); 
 
    display.println( 
      "VALUE NOT SET" 
    ); 
 
    display.setCursor(0, 42); 
 
    display.println( 
      "Long press OK" 
    ); 
 
    display.display(); 
 
    return; 
  } 
 
 
  // Normal ADXL values 
 
  display.setTextSize(1); 
 
  display.setCursor(0, 18); 
 
  display.print("X: "); 
 
  display.print( 
    accelX, 
    2 
  ); 
 
  display.println(" g"); 
 
 
  display.setCursor(0, 32); 
 
  display.print("Y: "); 
 
  display.print( 
    accelY, 
    2 
  ); 
 
  display.println(" g"); 
 
 
  display.setCursor(0, 46); 
 
  display.print("Z: "); 
 
  display.print( 
    accelZ, 
    2 
  ); 
 
  display.println(" g"); 
 
 
  display.display(); 
} 
 
 
// ===================================================== 
// MQ6 WARNING SCREEN 
// ===================================================== 
 
void showMQWarning() { 
 
  display.clearDisplay(); 
 
  display.setTextColor( 
    SSD1306_WHITE 
  ); 
 
  display.setTextSize(2); 
 
  display.setCursor(10, 0); 
 
  display.println( 
    "WARNING!" 
  ); 
 
  display.setTextSize(1); 
 
  display.setCursor(0, 25); 
 
  display.println( 
    "HARMFUL GAS" 
  ); 
 
  display.setCursor(0, 38); 
 
  display.println( 
    "DETECTED!" 
  ); 
 
  display.setTextSize(2); 
 
  display.setCursor(45, 48); 
 
  display.print( 
    gasValue 
  ); 
 
  display.display(); 
} 
 
 
// ===================================================== 
// LANDSLIDE WARNING SCREEN 
// ===================================================== 
 
void showADXLWarning() 
{ 
  display.clearDisplay(); 
 
  display.setTextColor(SSD1306_WHITE); 
 
  // WARNING! 
  display.setTextSize(2); 
  display.setCursor(15, 0); 
  display.println("WARNING!"); 
 
  // Separator 
  display.drawLine( 
    0, 18, 
    127, 18, 
    SSD1306_WHITE 
  ); 
 
  // LANDSLIDE 
  display.setTextSize(1); 
  display.setCursor(15, 24); 
  display.println("LANDSLIDE"); 
 
  display.setCursor(35, 36); 
  display.println("DETECTED!"); 
 
  // Current acceleration 
  display.setCursor(0, 52); 
  display.print("X:"); 
  display.print(accelX, 1); 
 
  display.print(" Y:"); 
  display.print(accelY, 1); 
 
  display.print(" Z:"); 
  display.print(accelZ, 1); 
 
  display.display(); 
} 
 
 
// ===================================================== 
// WARNING SCREEN 
// ===================================================== 
 
void showWarningScreen() { 
 
  // MQ6 warning gets priority 
  if (gasWarning) { 
 
    showMQWarning(); 
 
  } 
 
  // ADXL warning 
  else if (accelWarning) { 
 
    showADXLWarning(); 
  } 
} 
 
 
// =====================================================
// WI-FI + FIREBASE STATUS SCREEN
// =====================================================

void showWiFiStatus()
{
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ===================================================
  // SETUP AP MODE
  // ===================================================

  if (setupAPMode)
  {
    display.setTextSize(1);

    display.setCursor(0, 0);
    display.println("CONNECT WIFI");

    display.setCursor(0, 16);
    display.println("Open in Browser:");

    display.setCursor(0, 29);
    display.println("192.168.4.1");

    display.setCursor(0, 51);
    display.println("Long OK: CHANGE WIFI");

    display.display();
    return;
  }

  // ===================================================
  // NORMAL WI-FI STATUS
  // ===================================================

  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("DEVICE STATUS");

  display.drawLine(
    0, 11,
    127, 11,
    SSD1306_WHITE
  );

  // ---------------------------------------------------
  // SYSTEM / WI-FI
  // ---------------------------------------------------

  display.setCursor(0, 20);
  display.print("system:");

  if (WiFi.status() == WL_CONNECTED)
  {
    display.println("online");
  }
  else
  {
    display.println("offline");
  }

  // ---------------------------------------------------
  // FIREBASE SERVER
  // ---------------------------------------------------

  display.setCursor(0, 38);
  display.print("server:");

  if (firebaseServerOnline)
  {
    display.println("online");
  }
  else
  {
    display.println("offline");
  }

  display.display();
} 
 
 
// ===================================================== 
// NORMAL SCREEN 
// ===================================================== 
 
void showCurrentScreen() { 
 
  switch (currentScreen) { 
 
    case 0: 
      showSIH(); 
      break; 
 
    case 1: 
      showDHT(); 
      break; 
 
    case 2: 
      showMQ(); 
      break; 
 
    case 3: 
      showADXL(); 
      break; 
 
    case 4: 
      showWiFiStatus(); 
      break; 
  } 
} 
 
 
// ===================================================== 
// WARNING LED 
// ===================================================== 
 
void warningOutput() { 
 
  if (anyWarning()) { 
 
    if ( 
      millis() - lastBlink >= 
      WARNING_BLINK_TIME 
    ) { 
 
      lastBlink = millis(); 
 
      ledState = !ledState; 
 
      digitalWrite( 
        LED_PIN, 
        ledState 
      ); 
    } 
 
  } 
  else { 
 
    digitalWrite( 
      LED_PIN, 
      LOW 
    ); 
 
    ledState = false; 
  } 
} 
 
 
// ===================================================== 
// SAVE ADXL CURRENT VALUE 
// ===================================================== 
 
void setADXLBase() 
{ 
  if (!adxlOK) 
  { 
    adxlBaseSet = false; 
    return; 
  } 
 
  baseX = accelX; 
  baseY = accelY; 
  baseZ = accelZ; 
 
  adxlBaseSet = true; 
  adxlRecalibrating = false; 
 
  stableStartTime = 0; 
 
  Serial.println("ADXL VALUE SET"); 
 
  // Show SET on OLED 
  setMessage = true; 
  setMessageStart = millis(); 
 
  // GPIO2 ON for 1 second 
  digitalWrite(LED_PIN, HIGH); 
 
  delay(1000); 
 
  digitalWrite(LED_PIN, LOW); 
} 
 
 
// ===================================================== 
// OK BUTTON 
// ===================================================== 
 
void checkOKButton() 
{ 
  bool pressed = digitalRead(OK_BUTTON) == LOW; 
 
  // Button just pressed 
  if (pressed && !okWasPressed) 
  { 
    okPressStart = millis(); 
    okWasPressed = true; 
  } 
 
  // Button is being held 
  if (pressed && okWasPressed) 
  { 
    if (millis() - okPressStart >= LONG_PRESS) 
    { 
      // Prevent the action from repeating while button remains held 
      okWasPressed = false; 
 
      // ------------------------------- 
      // STOP MQ6 WARNING 
      // ------------------------------- 
      if (gasWarning) 
{ 
  // Only allow OK to stop the warning 
  // if gas is already below 800. 
  if (gasValue < GAS_WARNING) 
  { 
    gasWarning = false; 
    gasSafe = true; 
 
    Serial.println("MQ6 WARNING STOPPED - GAS SAFE"); 
  } 
  else 
  { 
    // Gas is still >= 800. 
    // Do NOT allow the warning to stay OFF. 
    Serial.println("MQ6 GAS STILL HIGH - WARNING ACTIVE"); 
  } 
} 
 
      // ------------------------------- 
      // STOP ADXL WARNING 
      // ------------------------------- 
      else if (accelWarning) 
      { 
        accelWarning = false; 
 
        // Start automatic recalibration 
        adxlBaseSet = false; 
        adxlRecalibrating = true; 
 
        stableStartTime = 0; 
 
        Serial.println("LANDSLIDE WARNING STOPPED"); 
        Serial.println("ADXL WAITING FOR STABLE VALUE"); 
      } 
 
      // ------------------------------- 
      // CHANGE WI-FI FROM WI-FI SCREEN 
      // ------------------------------- 
      else if (currentScreen == 4) 
      { 
        resetWiFiSettings(); 
      } 
 
      // ------------------------------- 
      // MANUAL ADXL SET 
      // ------------------------------- 
      else if (currentScreen == 3) 
      { 
        setADXLBase(); 
      } 
    } 
  } 
 
  // Button released 
  if (!pressed) 
  { 
    okWasPressed = false; 
  } 
} 
 
// ===================================================== 
// CHANGE BUTTON INTERRUPT 
// ===================================================== 
 
void IRAM_ATTR changeButtonISR() 
{ 
  changeButtonPressed = true; 
} 
 
// ===================================================== 
// CHANGE BUTTON 
// ===================================================== 
 
void checkChangeButton() 
{ 
  static unsigned long lastPressTime = 0; 
 
  // No button press detected 
  if (!changeButtonPressed) 
  { 
    return; 
  } 
 
  // Clear the interrupt flag 
  changeButtonPressed = false; 
 
  // Debounce 
  if (millis() - lastPressTime < 250) 
  { 
    return; 
  } 
 
  lastPressTime = millis(); 
 
  Serial.println("CHANGE BUTTON PRESSED"); 
 
  // ----------------------------------------- 
  // BLOCK SCREEN CHANGE DURING WARNING 
  // ----------------------------------------- 
 
  if (anyWarning()) 
  { 
    Serial.println("WARNING ACTIVE - SCREEN CHANGE BLOCKED"); 
    return; 
  } 
 
  // ----------------------------------------- 
  // CHANGE SCREEN 
  // ----------------------------------------- 
 
  currentScreen++; 
 
  if (currentScreen > 4) 
  { 
    currentScreen = 0; 
  } 
 
  // Reset 1-minute timer 
  lastScreenChange = millis(); 
 
  Serial.print("SCREEN = "); 
  Serial.println(currentScreen); 
 
  // Immediately show selected screen 
  showCurrentScreen(); 
} 
 
// ===================================================== 
// SETUP 
// ===================================================== 
 
void setup() { 
 
  Serial.begin(115200); 
 
 
  // =================================================== 
  // GPIO 
  // =================================================== 
 
  pinMode( 
    MQ6_PIN, 
    INPUT 
  ); 
 
  pinMode( 
    LED_PIN, 
    OUTPUT 
  ); 
 
  pinMode( 
    OK_BUTTON, 
    INPUT_PULLUP 
  ); 
 
  pinMode( 
    CHANGE_BUTTON, 
    INPUT_PULLUP 
  ); 
  // CHANGE button interrupt 
attachInterrupt( 
  digitalPinToInterrupt(CHANGE_BUTTON), 
  changeButtonISR, 
  FALLING 
); 
 
 
  digitalWrite( 
    LED_PIN, 
    LOW 
  ); 
 
 
  // =================================================== 
  // ADC 
  // =================================================== 
 
  analogReadResolution(12); 
 
 
  // =================================================== 
  // I2C 
  // =================================================== 
 
  Wire.begin( 
    SDA_PIN, 
    SCL_PIN 
  ); 
 
 
  // =================================================== 
  // OLED 
  // =================================================== 
 
  if ( 
    !display.begin( 
      SSD1306_SWITCHCAPVCC, 
      OLED_ADDRESS 
    ) 
  ) { 
 
    Serial.println( 
      "OLED ERROR" 
    ); 
 
    while (1); 
  } 
 
 
  // =================================================== 
  // START SCREEN REMOVED 
  // =================================================== 
  // Device starts directly with sensor/system operation. 
  // The SIH screen is shown at the end of setup. 
 
  // =================================================== 
  // DHT 
  // =================================================== 
 
  dht.begin(); 
 
 
  // =================================================== 
  // ADXL345 
  // =================================================== 
 
  if (adxl.begin()) { 
 
    adxlOK = true; 
 
    adxl.setRange( 
      ADXL345_RANGE_2_G 
    ); 
 
    Serial.println( 
      "ADXL345 OK" 
    ); 
 
  } 
  else { 
 
    adxlOK = false; 
 
    Serial.println( 
      "ADXL345 NOT FOUND" 
    ); 
  } 
 
 
  // =================================================== 
  // WI-FI 
  // =================================================== 
  // If no Wi-Fi is saved, ESP32 creates a setup network 
  // and hosts a configuration page at 192.168.4.1. 
  // If Wi-Fi is already saved, connection starts in 
  // the background without blocking the device. 
 
  loadWiFiCredentials(); 
 
  // =================================================== 
  // INITIAL SENSOR READ 
  // =================================================== 
 
  readSensors(); 
 
// Start automatic ADXL baseline detection 
adxlBaseSet = false; 
adxlRecalibrating = true; 
 
stableStartTime = 0; 
 
Serial.println("ADXL AUTO SET STARTED"); 
Serial.println("Waiting for 2 seconds stable..."); 
 
 
  // =================================================== 
  // START SIH SCREEN 
  // =================================================== 
 
  currentScreen = 0; 
 
  lastScreenChange = millis(); 
 
  showSIH(); 
 
 
  Serial.println( 
    "======================" 
  ); 
 
  Serial.println( 
    "SYSTEM READY" 
  ); 
 
  Serial.println( 
    "ADXL BASE: NOT SET" 
  ); 
 
  Serial.println( 
    "======================" 
  ); 
} 
 
 
// ===================================================== 
// LOOP 
// ===================================================== 
 
void loop() 
{ 
  unsigned long now = millis(); 
 
  // =================================================== 
  // WI-FI 
  // =================================================== 
  maintainWiFi(); 
  maintainFirebase(); 
 
  // =================================================== 
  // BUTTONS FIRST 
  // =================================================== 
  checkOKButton(); 
  checkChangeButton(); 
 
  // =================================================== 
  // FIREBASE REMOTE COMMAND 
  // =================================================== 
  if (now - lastFirebaseCommandCheck >= FIREBASE_COMMAND_INTERVAL) 
  { 
    checkFirebaseCommand(); 
  } 
 
  // =================================================== 
  // SENSOR READ 
  // =================================================== 
  if (now - lastSensorRead >= SENSOR_INTERVAL) 
  { 
    lastSensorRead = now; 
 
    readSensors(); 
    checkAutomaticADXLSet(); 
 
    Serial.print("TEMP: "); 
    Serial.print(temperature); 
 
    Serial.print(" C | HUM: "); 
    Serial.print(humidity); 
 
    Serial.print(" % | MQ6: "); 
    Serial.print(gasValue); 
 
    Serial.print(" | X: "); 
    Serial.print(accelX, 2); 
 
    Serial.print(" | Y: "); 
    Serial.print(accelY, 2); 
 
    Serial.print(" | Z: "); 
    Serial.println(accelZ, 2); 
  } 
 
  // =================================================== 
  // FIREBASE UPLOAD 
  // =================================================== 
  if (now - lastFirebaseUpload >= FIREBASE_UPLOAD_INTERVAL) 
  { 
    lastFirebaseUpload = now; 
    uploadToFirebase(); 
  } 
 
  // =================================================== 
  // SET MESSAGE TIMER 
  // =================================================== 
  if (setMessage && 
      now - setMessageStart >= SET_MESSAGE_TIME) 
  { 
    setMessage = false; 
  } 
 
  // =================================================== 
  // WARNING LED 
  // =================================================== 
  warningOutput(); 
 
  // =================================================== 
  // DISPLAY 
  // =================================================== 
  if (anyWarning()) 
  { 
    showWarningScreen(); 
  } 
  else 
  { 
    // ===================================================
// RETURN TO SIH AFTER 1 MINUTE
// ===================================================

unsigned long currentTime = millis();

if (currentScreen != 0 &&
    currentTime - lastScreenChange >= SCREEN_RETURN_TIME)
{
  currentScreen = 0;
  lastScreenChange = currentTime;

  Serial.println("1 MINUTE TIMEOUT -> SIH");
}
 
    showCurrentScreen(); 
  } 
 
  delay(5); 
}