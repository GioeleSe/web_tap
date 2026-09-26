#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "OV7670.h"
#include "esp_task_wdt.h"

// --- PIN DEFINITIONS (Explicitly mapped for verification) ---
const int SIOD  = 21; // SDA (I2C Control)
const int SIOC  = 22; // SCL (I2C Control)

const int VSYNC = 27; // Frame Sync (Must be a valid input pin)
const int HREF  = 14; // Line Sync

const int XCLK  = 32; // Master Clock (ESP32 output to camera)
const int PCLK  = 25; // Pixel Clock (Camera output to ESP32)

// Data Bus (D0 to D7) - Ensure strict sequential wiring!
const int D0 = 23;
const int D1 = 13;
const int D2 = 15;
const int D3 = 4;
const int D4 = 16;
const int D5 = 17;
const int D6 = 26; // Moved safely away from GPIO 12
const int D7 = 19;

const char *ssid = "ESP32-OV7670-Cam";
const char *password = "12345678";

AsyncWebServer server(80);
OV7670 *camera = nullptr;

const int WIDTH = 160;
const int HEIGHT = 120;
const size_t FRAME_SIZE = (size_t)WIDTH * HEIGHT * 2;
const uint32_t CAPTURE_TIMEOUT_MS = 1000;
const uint32_t CAPTURE_INTERVAL_MS = 200; 

// Double buffer configuration
uint8_t *frameBufA = nullptr;
uint8_t *frameBufB = nullptr;
volatile uint8_t *readyFrame = nullptr; 
volatile size_t readyFrameSize = 0;
volatile bool captureHealthy = true;
volatile uint32_t lastGoodCaptureMs = 0;

#define DBG(...) do { Serial.printf("[%8lu ms] ", millis()); Serial.printf(__VA_ARGS__); Serial.println(); } while (0)

const char LIVE_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 OV7670 Live</title>
  <style>
    body { font-family: Arial; text-align: center; background: #111; color: #fff; margin-top: 40px;}
    img { border: 2px solid #fff; width: 320px; height: 240px; image-rendering: pixelated; }
  </style>
</head>
<body>
  <h1>ESP32 Live Stream</h1>
  <img id="stream" src="/capture" onerror="document.getElementById('status').innerText='Image failed to load'" /><br><br>
  <div id="status">Diagnostics: Streaming active</div><br>
  <button onclick="document.getElementById('stream').src='/capture?t='+new Date().getTime()">Refresh Frame</button>
</body>
</html>
)rawliteral";

static void buildBmpHeader(uint8_t *buf, int width, int height, size_t dataSize) {
  const uint32_t headerAndMasksSize = 70;
  const uint32_t fileSize = headerAndMasksSize + dataSize;
  memset(buf, 0, 70);
  buf[0] = 'B'; buf[1] = 'M';
  buf[2]=(uint8_t)(fileSize); buf[3]=(uint8_t)(fileSize>>8); buf[4]=(uint8_t)(fileSize>>16); buf[5]=(uint8_t)(fileSize>>24);
  buf[10]=(uint8_t)(headerAndMasksSize);
  buf[14]=40;
  int32_t w = width, h = -height;
  buf[18]=(uint8_t)(w); buf[19]=(uint8_t)(w>>8); buf[20]=(uint8_t)(w>>16); buf[21]=(uint8_t)(w>>24);
  buf[22]=(uint8_t)(h); buf[23]=(uint8_t)(h>>8); buf[24]=(uint8_t)(h>>16); buf[25]=(uint8_t)(h>>24);
  buf[26]=1; buf[28]=16; buf[30]=3;
  uint32_t imgSize = dataSize;
  buf[34]=(uint8_t)(imgSize); buf[35]=(uint8_t)(imgSize>>8); buf[36]=(uint8_t)(imgSize>>16); buf[37]=(uint8_t)(imgSize>>24);
  buf[38]=19; buf[39]=11; buf[42]=19; buf[43]=11;
  uint8_t *m = buf + 54;
  m[0]=0x00;m[1]=0xF8;m[2]=0x00;m[3]=0x00;
  m[4]=0xE0;m[5]=0x07;m[6]=0x00;m[7]=0x00;
  m[8]=0x1F;m[9]=0x00;m[10]=0x00;m[11]=0x00;
  m[12]=0x00;m[13]=0x00;m[14]=0x00;m[15]=0x00;
}

void handleRoot(AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", LIVE_PAGE);
}

void handleCapture(AsyncWebServerRequest *request) {
  if (!captureHealthy || readyFrame == nullptr) {
    request->send(503, "text/plain", "Camera error: Check VSYNC/HREF or I2C wiring.");
    return;
  }

  static uint8_t header[70];
  buildBmpHeader(header, WIDTH, HEIGHT, readyFrameSize);

  const uint8_t *framePtr = (const uint8_t *)readyFrame;
  const size_t dataSize = readyFrameSize;
  const size_t totalSize = 70 + dataSize;

  AsyncWebServerResponse *response = request->beginChunkedResponse(
    "image/bmp",
    [framePtr, dataSize, totalSize](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      if (index >= totalSize) return 0;
      size_t toWrite;
      if (index < 70) {
        toWrite = min(maxLen, (size_t)70 - index);
        memcpy(buffer, header + index, toWrite);
      } else {
        size_t dataIndex = index - 70;
        toWrite = min(maxLen, dataSize - dataIndex);
        memcpy(buffer, framePtr + dataIndex, toWrite);
      }
      return toWrite;
    }
  );
  response->addHeader("Connection", "close");
  request->send(response);
}

void captureTask() {
  static bool usingBufA = true;
  uint8_t *targetBuf = usingBufA ? frameBufA : frameBufB;

  uint32_t start = millis();
  camera->oneFrame();
  uint32_t elapsed = millis() - start;

  if (!camera->frame) {
    DBG("PIN CHECK ERROR: Frame buffer is null. Possible causes:");
    DBG("  1. VSYNC (pin %d) or HREF (pin %d) is disconnected or floating.", VSYNC, HREF);
    DBG("  2. PCLK (pin %d) is not clocking data.", PCLK);
    captureHealthy = false;
    return;
  }

  if (elapsed > CAPTURE_TIMEOUT_MS) {
    DBG("Warning: Frame capture took %lu ms (sync signal might be unstable)", elapsed);
  }

  memcpy(targetBuf, camera->frame, FRAME_SIZE);
  readyFrame = targetBuf;
  readyFrameSize = FRAME_SIZE;
  captureHealthy = true;
  lastGoodCaptureMs = millis();
  usingBufA = !usingBufA;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  DBG("Boot: starting setup and pin configuration check...");

  frameBufA = (uint8_t *)malloc(FRAME_SIZE);
  frameBufB = (uint8_t *)malloc(FRAME_SIZE);
  if (!frameBufA || !frameBufB) {
    DBG("FATAL: Out of memory for frame buffers");
    while (true) delay(1000);
  }

  DBG("Initializing OV7670 and configuring registers over I2C (SDA:%d, SCL:%d)...", SIOD, SIOC);
  
  // This initializes both I2C configuration and the underlying I2S DMA framework
  camera = new OV7670(
    OV7670::Mode::QQVGA_RGB565, 
    SIOD, SIOC, 
    VSYNC, HREF, 
    XCLK, PCLK, 
    D0, D1, D2, D3, D4, D5, D6, D7
  );

  DBG("Camera constructor complete. Setting up Wi-Fi Access Point...");
  WiFi.softAP(ssid, password);
  DBG("WiFi AP IP = %s", WiFi.softAPIP().toString().c_str());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.begin();
  DBG("Web server running. Ready to stream.");
}

void loop() {
  static uint32_t lastCapture = 0;
  static uint32_t lastBeat = 0;

  if (millis() - lastCapture >= CAPTURE_INTERVAL_MS) {
    lastCapture = millis();
    captureTask();
  }

  if (millis() - lastBeat > 5000) {
    lastBeat = millis();
    DBG("Diagnostic Heartbeat: Heap free=%u | Status Healthy=%d", ESP.getFreeHeap(), captureHealthy);
  }
}