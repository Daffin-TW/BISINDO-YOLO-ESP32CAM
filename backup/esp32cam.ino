/**
 * =============================================================================
 * ESP32-CAM BISINDO Prototype - State Machine System
 * =============================================================================
 * Hardware: AI Thinker ESP32-CAM (OV2640)
 *
 * Peripherals:
 *  - OLED: SH1106G 128x64, I2C 0x3C, SDA=GPIO15, SCL=GPIO2
 *  - Button: GPIO13, Active LOW (INPUT_PULLUP)
 *  - Battery: GPIO14 (ADC2), R1=R2=100kΩ divider
 *  - DFPlayer Mini: UART0 (TX=GPIO1, RX=GPIO3)
 *
 * Architecture: Non-blocking state machine using millis()
 * =============================================================================
 */

// ─────────────────────────────────────────────────────────────────────────────
// INCLUDES
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_camera.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>   // SH1106G driver

// ─────────────────────────────────────────────────────────────────────────────
// PIN DEFINITIONS
// ─────────────────────────────────────────────────────────────────────────────
#define BUTTON_PIN      13
#define BATTERY_PIN     14   // ADC2 – only readable when WiFi is OFF

// OLED I2C Pins (via Wire.begin)
#define OLED_SDA        15
#define OLED_SCL        2
#define OLED_I2C_ADDR   0x3C
#define OLED_WIDTH      128
#define OLED_HEIGHT     64

// ─────────────────────────────────────────────────────────────────────────────
// CAMERA PIN MAP – AI Thinker / OV2640
// ─────────────────────────────────────────────────────────────────────────────
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// ─────────────────────────────────────────────────────────────────────────────
// TIMING CONSTANTS (ms)
// ─────────────────────────────────────────────────────────────────────────────
#define DEBOUNCE_MS          50
#define DOUBLE_CLICK_MS     400   // max gap between clicks for double-click
#define BATTERY_SAMPLE_MS   200   // interval between battery samples
#define BATTERY_SAMPLE_CNT   10   // total samples
#define WIFI_TIMEOUT_MS    20000  // 20 s WiFi connect timeout
#define MSG_DISPLAY_MS      3000  // 3 s informational screens
#define DISCONNECT_MSG_MS   2000  // 2 s "Disconnected" notice
#define FAIL_MSG_MS         3000  // 3 s "Failed to Connect" notice

// ─────────────────────────────────────────────────────────────────────────────
// BATTERY CONSTANTS
// ─────────────────────────────────────────────────────────────────────────────
#define BATT_V_MIN    3.0f   // 0 %
#define BATT_V_MAX    4.2f   // 100 %
#define BATT_R1      100000.0f
#define BATT_R2      100000.0f
#define ADC_REF_V     3.3f
#define ADC_MAX      4095.0f

// ─────────────────────────────────────────────────────────────────────────────
// ACCESS POINT CREDENTIALS
// ─────────────────────────────────────────────────────────────────────────────
#define AP_SSID "BISINDO ESP"
#define AP_PORT  80

// ─────────────────────────────────────────────────────────────────────────────
// STATE MACHINE
// ─────────────────────────────────────────────────────────────────────────────
enum SystemState {
  STATE_BOOT_BATTERY,       // 0: reading battery before WiFi
  STATE_WIFI_CONFIRM,       // 1: "Connect to <ssid>?" screen
  STATE_WIFI_CONNECTING,    // 2: attempting connection
  STATE_WIFI_FAIL,          // 3: "Failed to connect" (timed)
  STATE_CHANGE_WIFI,        // 4: AP mode + web form
  STATE_WIFI_CONNECTED_INFO,// 5: "Connected!" info screen (3 s)
  STATE_STREAMING,          // 6: camera HTTP stream active
  STATE_BATTERY_CHECK,      // 7: mid-session battery read
  STATE_DISCONNECTED_NOTICE // 8: "Disconnected" notice (2 s)
};

// ─────────────────────────────────────────────────────────────────────────────
// BUTTON EVENT
// ─────────────────────────────────────────────────────────────────────────────
enum ButtonEvent {
  BTN_NONE,
  BTN_SINGLE,
  BTN_DOUBLE
};

// ─────────────────────────────────────────────────────────────────────────────
// GLOBALS
// ─────────────────────────────────────────────────────────────────────────────
Adafruit_SH1106G oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Preferences       prefs;
WebServer*        webServer = nullptr;

SystemState  currentState    = STATE_BOOT_BATTERY;
SystemState  returnState     = STATE_STREAMING; // used after battery check

// ---------- battery globals --------------------------------------------------
float   batteryVoltage       = 0.0f;
int     batteryPercent       = 0;
unsigned long battSampleTimer = 0;
int     battSampleCount      = 0;
float   battSamples[BATTERY_SAMPLE_CNT];

// ---------- WiFi globals -----------------------------------------------------
String  storedSSID           = "";
String  storedPass           = "";
unsigned long wifiTimer      = 0;

// ---------- state timer (generic) -------------------------------------------
unsigned long stateTimer     = 0;

// ---------- camera -----------------------------------------------------------
bool    cameraInitialized    = false;
bool    serverRunning        = false;

// ---------- streaming task ---------------------------------------------------
TaskHandle_t    streamTaskHandle = nullptr;
volatile bool   streamTaskStop   = false;

// ---------- button -----------------------------------------------------------
bool    lastButtonRaw        = HIGH;
bool    buttonState          = HIGH;
unsigned long debounceTimer  = 0;

int     clickCount           = 0;
unsigned long lastClickTime  = 0;
bool    waitingDoubleClick   = false;

// ─────────────────────────────────────────────────────────────────────────────
// FORWARD DECLARATIONS
// ─────────────────────────────────────────────────────────────────────────────
void handleBootBattery();
void handleWifiConfirm();
void handleWifiConnecting();
void handleWifiFail();
void handleChangeWifi();
void handleWifiConnectedInfo();
void handleStreaming();
void handleBatteryCheck();
void handleDisconnectedNotice();

void enterState(SystemState s);
void drawOledHeader();
void drawBootBatteryScreen();
void drawWifiConfirmScreen();
void drawConnectingScreen();
void drawFailScreen();
void drawChangeWifiScreen();
void drawConnectedInfoScreen();
void drawStreamingScreen();
void drawBatteryCheckScreen();
void drawDisconnectedScreen();

void startBatteryReading();
bool processBatterySamples();   // returns true when done
float mediан(float* arr, int n);
float rawToVoltage(int raw);

bool initCamera();
void startStreamServer();
void stopStreamServer();
void streamingTask(void* pvParameters);  // FreeRTOS task for MJPEG stream
void handleCamConfigPage();
void handleCamConfigSave();
void handleRoot();

void startAPMode();
void stopAPMode();
void handleAPRoot();
void handleAPSave();

ButtonEvent pollButton();
void loadCredentials();
void saveCredentials(const String& ssid, const String& pass);

// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  // --- I2C for OLED (custom pins) ---
  Wire.begin(OLED_SDA, OLED_SCL);

  // --- OLED init ---
  if (!oled.begin(OLED_I2C_ADDR, true)) {
  }
  oled.setRotation(0);
  oled.clearDisplay();
  oled.display();

  // --- Button ---
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // --- Load stored WiFi credentials ---
  loadCredentials();

  // --- Enter boot state – no WiFi yet ---
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100); // small hardware settle

  enterState(STATE_BOOT_BATTERY);
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  ButtonEvent btn = pollButton();

  switch (currentState) {
    case STATE_BOOT_BATTERY:        handleBootBattery();        break;
    case STATE_WIFI_CONFIRM:        handleWifiConfirm();        break;
    case STATE_WIFI_CONNECTING:     handleWifiConnecting();     break;
    case STATE_WIFI_FAIL:           handleWifiFail();           break;
    case STATE_CHANGE_WIFI:         handleChangeWifi();         break;
    case STATE_WIFI_CONNECTED_INFO: handleWifiConnectedInfo();  break;
    case STATE_STREAMING:           handleStreaming();          break;
    case STATE_BATTERY_CHECK:       handleBatteryCheck();       break;
    case STATE_DISCONNECTED_NOTICE: handleDisconnectedNotice(); break;
  }

  // Process button events (centralised for states that care)
  if (btn != BTN_NONE) {
    switch (currentState) {
      // ── WiFi Confirm ──────────────────────────────────────────────────────
      case STATE_WIFI_CONFIRM:
        if (btn == BTN_SINGLE) {
          enterState(STATE_WIFI_CONNECTING);
        } else if (btn == BTN_DOUBLE) {
          enterState(STATE_CHANGE_WIFI);
        }
        break;

      // ── Change WiFi (AP) – single = cancel ───────────────────────────────
      case STATE_CHANGE_WIFI:
        if (btn == BTN_SINGLE) {
          stopAPMode();
          enterState(STATE_WIFI_CONFIRM);
        }
        break;

      // ── WiFi Connected Info – single=battery, double=change wifi ─────────
      case STATE_WIFI_CONNECTED_INFO:
        if (btn == BTN_SINGLE) {
          returnState = STATE_STREAMING; // after battery, go streaming
          enterState(STATE_BATTERY_CHECK);
        } else if (btn == BTN_DOUBLE) {
          stopStreamServer();
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
          enterState(STATE_WIFI_CONFIRM);
        }
        break;

      // ── Streaming – single=battery check, double=change wifi ─────────────
      case STATE_STREAMING:
        if (btn == BTN_SINGLE) {
          stopStreamServer();
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
          delay(100);
          returnState = STATE_STREAMING;
          enterState(STATE_BATTERY_CHECK);
        } else if (btn == BTN_DOUBLE) {
          stopStreamServer();
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
          enterState(STATE_WIFI_CONFIRM);
        }
        break;

      default:
        break;
    }
  }
}

// =============================================================================
// STATE ENTRY – centralised initialisation per state
// =============================================================================
void enterState(SystemState s) {
  currentState = s;
  stateTimer   = millis();

  switch (s) {
    case STATE_BOOT_BATTERY:
      startBatteryReading();
      drawBootBatteryScreen();
      break;

    case STATE_WIFI_CONFIRM:
      drawWifiConfirmScreen();
      break;

    case STATE_WIFI_CONNECTING:
      wifiTimer = millis();
      WiFi.mode(WIFI_STA);
      WiFi.begin(storedSSID.c_str(), storedPass.c_str());
      drawConnectingScreen();
      break;

    case STATE_WIFI_FAIL:
      drawFailScreen();
      break;

    case STATE_CHANGE_WIFI:
      startAPMode();
      drawChangeWifiScreen();
      break;

    case STATE_WIFI_CONNECTED_INFO:
      drawConnectedInfoScreen();
      break;

    case STATE_STREAMING:
      if (!cameraInitialized) {
        cameraInitialized = initCamera();
      }
      if (cameraInitialized) {
        startStreamServer();
      }
      drawStreamingScreen();
      break;

    case STATE_BATTERY_CHECK:
      // WiFi must be off before reading ADC2
      stopStreamServer();
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(350); // allow radio to fully shut down
      startBatteryReading();
      drawBatteryCheckScreen();
      break;

    case STATE_DISCONNECTED_NOTICE:
      drawDisconnectedScreen();
      break;
  }
}

// =============================================================================
// STATE HANDLERS (called every loop iteration)
// =============================================================================

// ── STATE 0: Boot Battery Reading ───────────────────────────────────────────
void handleBootBattery() {
  if (processBatterySamples()) {
    // Done – proceed to WiFi confirm
    enterState(STATE_WIFI_CONFIRM);
  }
}

// ── STATE 1: WiFi Confirm ────────────────────────────────────────────────────
void handleWifiConfirm() {
  // Screen already drawn in enterState. Button handled in main loop.
}

// ── STATE 2: WiFi Connecting ─────────────────────────────────────────────────
void handleWifiConnecting() {
  if (WiFi.status() == WL_CONNECTED) {
    enterState(STATE_WIFI_CONNECTED_INFO);
    return;
  }
  if (millis() - wifiTimer > WIFI_TIMEOUT_MS) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    enterState(STATE_WIFI_FAIL);
  }
}

// ── STATE 3: WiFi Failed ─────────────────────────────────────────────────────
void handleWifiFail() {
  if (millis() - stateTimer > FAIL_MSG_MS) {
    enterState(STATE_WIFI_CONFIRM);
  }
}

// ── STATE 4: Change WiFi (AP mode) ──────────────────────────────────────────
void handleChangeWifi() {
  if (webServer) webServer->handleClient();
}

// ── STATE 5: WiFi Connected Info ─────────────────────────────────────────────
void handleWifiConnectedInfo() {
  if (millis() - stateTimer > MSG_DISPLAY_MS) {
    enterState(STATE_STREAMING);
  }
  // Buttons handled in loop
}

// ── STATE 6: Camera Streaming ────────────────────────────────────────────────
void handleStreaming() {
  if (webServer) webServer->handleClient();

  // Monitor WiFi health
  if (WiFi.status() != WL_CONNECTED) {
    stopStreamServer();
    enterState(STATE_DISCONNECTED_NOTICE);
  }
}

// ── STATE 7: Battery Check (mid-session) ─────────────────────────────────────
void handleBatteryCheck() {
  if (processBatterySamples()) {
    // Reconnect WiFi and go back to streaming (or wherever)
    WiFi.mode(WIFI_STA);
    WiFi.begin(storedSSID.c_str(), storedPass.c_str());
    wifiTimer = millis();
    enterState(STATE_WIFI_CONNECTING);
  }
}

// ── STATE 8: Disconnected Notice ──────────────────────────────────────────────
void handleDisconnectedNotice() {
  if (millis() - stateTimer > DISCONNECT_MSG_MS) {
    enterState(STATE_WIFI_CONNECTING);
  }
}

// =============================================================================
// BATTERY SUBSYSTEM
// =============================================================================
void startBatteryReading() {
  battSampleCount = 0;
  battSampleTimer = millis();
  memset(battSamples, 0, sizeof(battSamples));
}

/**
 * Call repeatedly from the state handler.
 * Takes one ADC sample every BATTERY_SAMPLE_MS ms.
 * Returns true when all samples collected and result computed.
 * WiFi MUST be OFF when this runs.
 */
bool processBatterySamples() {
  if (battSampleCount >= BATTERY_SAMPLE_CNT) return true; // already done

  if (millis() - battSampleTimer >= BATTERY_SAMPLE_MS) {
    battSampleTimer = millis();

    int raw = analogRead(BATTERY_PIN);
    battSamples[battSampleCount++] = rawToVoltage(raw);

    if (battSampleCount >= BATTERY_SAMPLE_CNT) {
      // Compute median
      batteryVoltage  = median(battSamples, BATTERY_SAMPLE_CNT);
      // Clamp and convert to percentage
      float clamped   = constrain(batteryVoltage, BATT_V_MIN, BATT_V_MAX);
      batteryPercent  = (int)(((clamped - BATT_V_MIN) / (BATT_V_MAX - BATT_V_MIN)) * 100.0f);

      drawBatteryResult(); // update display immediately
      return true;
    }
  }
  return false;
}

/** Convert raw ADC reading → real battery voltage (voltage divider) */
float rawToVoltage(int raw) {
  float vADC = (raw / ADC_MAX) * ADC_REF_V;
  // Reverse voltage divider: Vbat = vADC * (R1+R2)/R2
  return vADC * ((BATT_R1 + BATT_R2) / BATT_R2);
}

/** Simple median – sorts a copy of the array */
float median(float* arr, int n) {
  float tmp[BATTERY_SAMPLE_CNT];
  memcpy(tmp, arr, n * sizeof(float));
  // Bubble sort (small n, fine here)
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - i - 1; j++) {
      if (tmp[j] > tmp[j + 1]) {
        float t = tmp[j]; tmp[j] = tmp[j + 1]; tmp[j + 1] = t;
      }
    }
  }
  return (n % 2 == 0) ? (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0f : tmp[n / 2];
}

// =============================================================================
// OLED RENDERING
// =============================================================================

/** Small battery icon + percentage in the top-right corner of every screen */
void drawOledHeader() {
  // Battery icon (12×6 px at x=110, y=0)
  oled.drawRect(110, 1, 14, 7, SH110X_WHITE);
  oled.fillRect(124, 3, 2, 3, SH110X_WHITE); // terminal nub
  int fillW = (int)(batteryPercent / 100.0f * 12.0f);
  oled.fillRect(111, 2, fillW, 5, SH110X_WHITE);

  // Percentage text
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(82, 1);
  oled.printf("%3d%%", batteryPercent);
}

void drawBootBatteryScreen() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(20, 10);
  oled.println("BISINDO SYSTEM");
  oled.setCursor(10, 26);
  oled.println("Booting...");
  oled.setCursor(0, 40);
  oled.println("Reading battery...");
  oled.display();
}

void drawBatteryResult() {
  oled.clearDisplay();
  drawOledHeader();
  oled.setTextSize(2);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(10, 20);
  oled.printf("%d%%", batteryPercent);
  oled.setTextSize(1);
  oled.setCursor(0, 50);
  oled.printf("Volt: %.2fV", batteryVoltage);
  oled.display();
  delay(1500); // brief display of result before moving on
}

void drawWifiConfirmScreen() {
  oled.clearDisplay();
  drawOledHeader();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(0, 12);
  oled.println("Connect to:");
  oled.setCursor(0, 22);
  // Truncate SSID to fit screen
  String ssidDisp = storedSSID.isEmpty() ? "(none)" : storedSSID;
  if (ssidDisp.length() > 16) ssidDisp = ssidDisp.substring(0, 14) + "..";
  oled.println(ssidDisp);
  oled.setCursor(0, 36);
  oled.println("1x: yes");
  oled.setCursor(0, 46);
  oled.println("2x: change wifi");
  oled.display();
}

void drawConnectingScreen() {
  oled.clearDisplay();
  drawOledHeader();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(0, 12);
  oled.println("Connecting to:");
  oled.setCursor(0, 22);
  String ssidDisp = storedSSID;
  if (ssidDisp.length() > 16) ssidDisp = ssidDisp.substring(0, 14) + "..";
  oled.println(ssidDisp);
  oled.setCursor(0, 40);
  oled.println("Please wait...");
  oled.display();
}

void drawFailScreen() {
  oled.clearDisplay();
  drawOledHeader();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(10, 20);
  oled.println("Failed to");
  oled.setCursor(10, 32);
  oled.println("Connect!");
  oled.display();
}

void drawChangeWifiScreen() {
  oled.clearDisplay();
  drawOledHeader();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(0, 12);
  oled.println("Changing WiFi");
  oled.setCursor(0, 24);
  oled.println("Join: BISINDO ESP");
  oled.setCursor(0, 34);
  oled.println("IP: 192.168.1.1");
  oled.setCursor(0, 48);
  oled.println("1x: cancel");
  oled.display();
}

void drawConnectedInfoScreen() {
  oled.clearDisplay();
  drawOledHeader();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(0, 12);
  oled.println("Connected to:");
  String ssidDisp = storedSSID;
  if (ssidDisp.length() > 16) ssidDisp = ssidDisp.substring(0, 14) + "..";
  oled.setCursor(0, 22);
  oled.println(ssidDisp);
  oled.setCursor(0, 36);
  oled.println("1x: battery");
  oled.setCursor(0, 46);
  oled.println("2x: change wifi");
  oled.display();
}

void drawStreamingScreen() {
  oled.clearDisplay();
  drawOledHeader();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(0, 12);
  oled.println("Streaming ON");
  oled.setCursor(0, 24);
  oled.print("IP: ");
  oled.println(WiFi.localIP().toString());
  oled.setCursor(0, 36);
  oled.println("1x: battery");
  oled.setCursor(0, 46);
  oled.println("2x: change wifi");
  oled.display();
}

void drawBatteryCheckScreen() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(10, 20);
  oled.println("Battery Check...");
  oled.setCursor(0, 38);
  oled.printf("Sample %d/%d", battSampleCount, BATTERY_SAMPLE_CNT);
  oled.display();
}

void drawDisconnectedScreen() {
  oled.clearDisplay();
  drawOledHeader();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(15, 24);
  oled.println("WiFi");
  oled.setCursor(5, 36);
  oled.println("Disconnected");
  oled.display();
}

// =============================================================================
// CAMERA SUBSYSTEM
// =============================================================================
bool initCamera() {
  camera_config_t cfg;
  cfg.ledc_channel  = LEDC_CHANNEL_0;
  cfg.ledc_timer    = LEDC_TIMER_0;
  cfg.pin_d0        = Y2_GPIO_NUM;
  cfg.pin_d1        = Y3_GPIO_NUM;
  cfg.pin_d2        = Y4_GPIO_NUM;
  cfg.pin_d3        = Y5_GPIO_NUM;
  cfg.pin_d4        = Y6_GPIO_NUM;
  cfg.pin_d5        = Y7_GPIO_NUM;
  cfg.pin_d6        = Y8_GPIO_NUM;
  cfg.pin_d7        = Y9_GPIO_NUM;
  cfg.pin_xclk      = XCLK_GPIO_NUM;
  cfg.pin_pclk      = PCLK_GPIO_NUM;
  cfg.pin_vsync     = VSYNC_GPIO_NUM;
  cfg.pin_href      = HREF_GPIO_NUM;
  cfg.pin_sccb_sda  = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl  = SIOC_GPIO_NUM;
  cfg.pin_pwdn      = PWDN_GPIO_NUM;
  cfg.pin_reset     = RESET_GPIO_NUM;
  cfg.xclk_freq_hz  = 20000000;
  cfg.pixel_format  = PIXFORMAT_JPEG;

  // Start with lower res to save memory, configurable later
  cfg.frame_size    = FRAMESIZE_VGA;
  cfg.jpeg_quality  = 12;
  cfg.fb_count      = 2;
  cfg.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;
  cfg.fb_location   = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    return false;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP Stream Server
// ─────────────────────────────────────────────────────────────────────────────
void startStreamServer() {
  if (serverRunning) return;

  if (webServer) {
    delete webServer;
    webServer = nullptr;
  }
  webServer = new WebServer(80);

  webServer->on("/",       HTTP_GET,  handleRoot);
  webServer->on("/config", HTTP_GET,  handleCamConfigPage);
  webServer->on("/config", HTTP_POST, handleCamConfigSave);
  webServer->onNotFound([]() {
    webServer->send(404, "text/plain", "Not found");
  });

  webServer->begin();
  serverRunning = true;

  // Spawn the MJPEG streaming task on Core 0 (protocol/network core).
  // Core 1 (Arduino loop) remains free for button polling and OLED updates.
  streamTaskStop = false;
  xTaskCreatePinnedToCore(
    streamingTask,       // task function
    "StreamTask",        // name (debug)
    8192,                // stack size in bytes
    nullptr,             // task parameter
    1,                   // priority
    &streamTaskHandle,   // handle
    0                    // pin to Core 0
  );
}

void stopStreamServer() {
  // Signal the streaming task to exit, then wait briefly for it to clean up.
  streamTaskStop = true;
  if (streamTaskHandle != nullptr) {
    // Give the task up to 500 ms to exit on its own.
    for (int i = 0; i < 50 && streamTaskHandle != nullptr; i++) {
      delay(10);
    }
    // Force-delete if it hasn't exited yet.
    if (streamTaskHandle != nullptr) {
      vTaskDelete(streamTaskHandle);
      streamTaskHandle = nullptr;
    }
  }

  if (!serverRunning) return;
  if (webServer) {
    webServer->stop();
    delete webServer;
    webServer     = nullptr;
    serverRunning = false;
  }
}

// Root page – simple HTML with embedded stream (stream served on port 81)
void handleRoot() {
  String ip = WiFi.localIP().toString();
  String streamUrl = "http://" + ip + ":81";

  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>BISINDO ESP32-CAM</title>
<style>
  body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px;text-align:center;}
  h1{color:#4fc3f7;margin-bottom:4px;}
  img{width:100%;max-width:640px;border:2px solid #4fc3f7;border-radius:8px;}
  .btn{
    display:inline-block;
    margin:10px 5px;
    padding:10px 18px;
    background:#4fc3f7;
    color:#111;
    border-radius:6px;
    text-decoration:none;
    font-weight:bold;
    cursor:pointer;
  }
  .btn.stop{background:#ef5350;color:#fff;}
</style>
</head>
<body>

<h1>BISINDO ESP32-CAM</h1>
<p id="status">Stream stopped</p>

<img id="stream" src="" alt="Camera Stream">

<br>

<button class="btn" onclick="startStream()">Start Stream</button>
<button class="btn stop" onclick="stopStream()">Stop Stream</button>

<br>

<a class="btn" href="/config">Camera Config</a>

<script>
// Stream URL points to the dedicated MJPEG server on port 81.
const STREAM_URL = ")rawhtml" + streamUrl + R"rawhtml(";

let streaming = false;
const img = document.getElementById("stream");
const statusText = document.getElementById("status");

function startStream() {
  if (!streaming) {
    img.src = STREAM_URL;
    statusText.innerText = "Streaming...";
    streaming = true;
  }
}

function stopStream() {
  img.src = "";
  statusText.innerText = "Stream stopped";
  streaming = false;
}
</script>
</body>
</html>
)rawhtml";
  webServer->send(200, "text/html", html);
}

// =============================================================================
// MJPEG STREAMING TASK  (runs on Core 0, created by startStreamServer)
// =============================================================================
/**
 * Listens for incoming HTTP connections on port 81 and serves a continuous
 * MJPEG stream for each client.  Running this in a dedicated FreeRTOS task
 * means the blocking camera-capture / TCP-write loop no longer stalls the
 * main Arduino loop on Core 1, keeping button polling and OLED updates alive.
 *
 * Termination: set streamTaskStop = true before deleting the server.
 */
void streamingTask(void* pvParameters) {
  const String boundary = "frame";

  // Dedicated server on port 81 for the raw MJPEG stream.
  WiFiServer streamServer(81);
  streamServer.begin();

  while (!streamTaskStop) {
    WiFiClient client = streamServer.available();
    if (!client) {
      vTaskDelay(pdMS_TO_TICKS(5)); // yield – do not busy-spin
      continue;
    }

    // Drain the HTTP request headers (we only serve one content type).
    unsigned long headerTimeout = millis();
    while (client.connected() && !client.available()) {
      if (millis() - headerTimeout > 2000) break;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    while (client.available()) client.read(); // discard request bytes

    // Send MJPEG response headers.
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: multipart/x-mixed-replace; boundary=" + boundary);
    client.println("Cache-Control: no-cache, no-store, must-revalidate");
    client.println("Pragma: no-cache");
    client.println("Connection: close");
    client.println();

    // Stream frames until client disconnects or stop is requested.
    while (client.connected() && !streamTaskStop) {
      camera_fb_t* fb = esp_camera_fb_get();
      if (!fb) {
        vTaskDelay(pdMS_TO_TICKS(30));
        continue;
      }

      // Write MJPEG part headers + JPEG payload.
      client.print("--");
      client.println(boundary);
      client.println("Content-Type: image/jpeg");
      client.printf("Content-Length: %u\r\n\r\n", fb->len);
      client.write(fb->buf, fb->len);
      client.println();

      esp_camera_fb_return(fb);

      // Yield to the RTOS scheduler after each frame.
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    client.stop();
  }

  streamServer.stop();
  streamTaskHandle = nullptr; // signal that the task exited cleanly
  vTaskDelete(nullptr);       // delete self
}

// Camera configuration page
void handleCamConfigPage() {
  // Load saved values (or defaults)
  prefs.begin("cam_cfg", true);
  int res  = prefs.getInt("framesize",    FRAMESIZE_VGA);
  int qual = prefs.getInt("quality",      12);
  int flip = prefs.getInt("vflip",        0);
  int mir  = prefs.getInt("hmirror",      0);
  prefs.end();

  // Resolution option list
  const char* resNames[] = {
    "QQVGA(160x120)",  // 0
    "QCIF(176x144)",
    "HQVGA(240x176)",
    "240x240",
    "QVGA(320x240)",
    "CIF(400x296)",
    "HVGA(480x320)",
    "VGA(640x480)",    // 7
    "SVGA(800x600)",
    "XGA(1024x768)",
    "HD(1280x720)",
    "SXGA(1280x1024)",
    "UXGA(1600x1200)"  // 12
  };

  String opts = "";
  for (int i = 0; i <= 12; i++) {
    opts += "<option value='" + String(i) + "'";
    if (i == res) opts += " selected";
    opts += ">" + String(resNames[i]) + "</option>";
  }

  String html = R"rawhtml(
<!DOCTYPE html><html lang="en">
<head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Camera Config</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;padding:16px;}
h1{color:#4fc3f7;}
label{display:block;margin-top:10px;}
select,input{background:#222;color:#eee;border:1px solid #555;
             border-radius:4px;padding:6px;width:100%;max-width:320px;}
button{margin-top:16px;padding:10px 24px;background:#4fc3f7;color:#111;
       border:none;border-radius:6px;font-weight:bold;cursor:pointer;}
a{color:#4fc3f7;}
</style>
</head>
<body>
<h1>Camera Configuration</h1>
<form method="POST" action="/config">
  <label>Resolution:
    <select name="framesize">)rawhtml" + opts + R"rawhtml(</select>
  </label>
  <label>JPEG Quality (1-63, lower=better):
    <input type="number" name="quality" min="1" max="63" value=")rawhtml" +
    String(qual) + R"rawhtml(">
  </label>
  <label><input type="checkbox" name="vflip" value="1")rawhtml" +
    (flip ? " checked" : "") + R"rawhtml(> Vertical Flip</label>
  <label><input type="checkbox" name="hmirror" value="1")rawhtml" +
    (mir  ? " checked" : "") + R"rawhtml(> Horizontal Mirror</label>
  <button type="submit">Save & Apply</button>
</form>
<br><a href="/">&#8592; Back to Stream</a>
</body></html>
)rawhtml";

  webServer->send(200, "text/html", html);
}

// Apply and persist camera settings
void handleCamConfigSave() {
  int fs   = webServer->hasArg("framesize") ? webServer->arg("framesize").toInt() : FRAMESIZE_VGA;
  int qual = webServer->hasArg("quality")   ? webServer->arg("quality").toInt()   : 12;
  int vf   = webServer->hasArg("vflip")     ? 1 : 0;
  int hm   = webServer->hasArg("hmirror")   ? 1 : 0;

  // Clamp values
  fs   = constrain(fs,   0, 12);
  qual = constrain(qual, 1, 63);

  // Persist
  prefs.begin("cam_cfg", false);
  prefs.putInt("framesize", fs);
  prefs.putInt("quality",   qual);
  prefs.putInt("vflip",     vf);
  prefs.putInt("hmirror",   hm);
  prefs.end();

  // Apply to sensor
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s,  (framesize_t)fs);
    s->set_quality(s,    qual);
    s->set_vflip(s,      vf);
    s->set_hmirror(s,    hm);
  }

  // Redirect back to stream page
  webServer->sendHeader("Location", "/");
  webServer->send(303);
}

// =============================================================================
// ACCESS POINT + WiFi Setup Portal
// =============================================================================
void startAPMode() {
  WiFi.disconnect(true);
  delay(100);

  IPAddress local_IP(192, 168, 1, 1);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(AP_SSID);
  delay(200);

  IPAddress apIP = WiFi.softAPIP();

  if (webServer) {
    delete webServer;
    webServer = nullptr;
  }
  webServer = new WebServer(AP_PORT);
  webServer->on("/",     HTTP_GET,  handleAPRoot);
  webServer->on("/save", HTTP_POST, handleAPSave);
  webServer->begin();
}

void stopAPMode() {
  if (webServer) {
    webServer->stop();
    delete webServer;
    webServer = nullptr;
  }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

void handleAPRoot() {
  String html = R"rawhtml(
<!DOCTYPE html><html lang="en">
<head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Setup – BISINDO</title>
<style>
body{font-family:sans-serif;background:#0f1923;color:#e0e0e0;padding:24px;}
h1{color:#4fc3f7;font-size:1.4rem;}
label{display:block;margin-top:14px;font-size:.9rem;color:#aaa;}
input{background:#1e2d3d;color:#fff;border:1px solid #4fc3f7;border-radius:6px;
      padding:10px;width:100%;max-width:340px;box-sizing:border-box;font-size:1rem;}
button{margin-top:20px;padding:12px 28px;background:#4fc3f7;color:#0f1923;
       border:none;border-radius:8px;font-weight:bold;font-size:1rem;cursor:pointer;}
button:hover{background:#81d4fa;}
p.note{font-size:.8rem;color:#888;margin-top:8px;}
</style>
</head>
<body>
<h1>&#x1F4F6; WiFi Configuration</h1>
<p class="note">Connected to <b>BISINDO ESP32CAM</b> AP</p>
<form method="POST" action="/save">
  <label>Network SSID</label>
  <input type="text"     name="ssid"     placeholder="Your WiFi name"     required>
  <label>Password</label>
  <input type="password" name="pass"     placeholder="Your WiFi password">
  <button type="submit">Save &amp; Connect</button>
</form>
</body></html>
)rawhtml";
  webServer->send(200, "text/html", html);
}

void handleAPSave() {
  if (!webServer->hasArg("ssid") || webServer->arg("ssid").isEmpty()) {
    webServer->send(400, "text/plain", "SSID required");
    return;
  }

  String newSSID = webServer->arg("ssid");
  String newPass = webServer->arg("pass");

  saveCredentials(newSSID, newPass);
  storedSSID = newSSID;
  storedPass = newPass;

  webServer->send(200, "text/html",
    "<html><body style='font-family:sans-serif;background:#0f1923;color:#e0e0e0;"
    "padding:24px'><h2 style='color:#4fc3f7'>Saved! Connecting...</h2>"
    "<p>The device will now connect to <b>" + newSSID + "</b>.</p></body></html>");

  delay(1000);
  stopAPMode();
  enterState(STATE_WIFI_CONNECTING);
}

// =============================================================================
// CREDENTIALS PERSISTENCE (NVS via Preferences)
// =============================================================================
void loadCredentials() {
  prefs.begin("wifi_cred", true);
  storedSSID = prefs.getString("ssid", "");
  storedPass = prefs.getString("pass", "");
  prefs.end();
}

void saveCredentials(const String& ssid, const String& pass) {
  prefs.begin("wifi_cred", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

// =============================================================================
// BUTTON HANDLER – Non-blocking debounce + single/double click detection
// =============================================================================
/**
 * Debouncing:
 *   Track raw pin state changes with a millis debounce window.
 *
 * Click detection:
 *   - Record each confirmed press (falling edge).
 *   - After DOUBLE_CLICK_MS with no second press → emit SINGLE.
 *   - Two presses within DOUBLE_CLICK_MS → emit DOUBLE.
 */
ButtonEvent pollButton() {
  bool rawNow = digitalRead(BUTTON_PIN);

  // Debounce
  if (rawNow != lastButtonRaw) {
    debounceTimer = millis();
    lastButtonRaw = rawNow;
  }

  ButtonEvent result = BTN_NONE;

  if ((millis() - debounceTimer) >= DEBOUNCE_MS) {
    bool stableNow = rawNow;

    // Falling edge = button pressed (active LOW)
    if (stableNow == LOW && buttonState == HIGH) {
      buttonState = LOW;

      clickCount++;
      lastClickTime = millis();

      if (clickCount == 2) {
        // Immediate double-click confirm
        result     = BTN_DOUBLE;
        clickCount = 0;
        waitingDoubleClick = false;
      } else {
        waitingDoubleClick = true;
      }
    }

    // Rising edge
    if (stableNow == HIGH && buttonState == LOW) {
      buttonState = HIGH;
    }
  }

  // Timeout: single click confirm
  if (waitingDoubleClick && clickCount == 1 &&
      (millis() - lastClickTime) > DOUBLE_CLICK_MS) {
    result     = BTN_SINGLE;
    clickCount = 0;
    waitingDoubleClick = false;
  }

  return result;
}
