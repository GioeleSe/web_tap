#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "OV7670.h"

// --- PIN DEFINITIONS ---
const int SIOD  = 21; // SDA
const int SIOC  = 22; // SCL
const int VSYNC = 27; // Frame Sync
const int HREF  = 14; // Line Sync
const int XCLK  = 32; // Master Clock
const int PCLK  = 25; // Pixel Clock

const int D0 = 23;
const int D1 = 13;
const int D2 = 15;
const int D3 = 4;
const int D4 = 16;
const int D5 = 17;
const int D6 = 18;
const int D7 = 19;

// --- LIGHT CONTROL PINS ---
const int LIGHT_PIN = 26;

// --- RASPBERRY PI AP CREDENTIALS ---
const char *ssid = "esp_cam_ap";
const char *password = "12345678";

// Network configuration matching NetworkManager's default 10.42.0.x subnet
IPAddress local_IP(10, 42, 0, 50);
IPAddress gateway(10, 42, 0, 1);
IPAddress subnet(255, 255, 255, 0);

AsyncWebServer server(80);
OV7670 *camera = nullptr;

const int WIDTH = 160;
const int HEIGHT = 120;
const size_t FRAME_SIZE = (size_t)WIDTH * HEIGHT * 2; // RGB565

uint8_t *frameBufA = nullptr;
uint8_t *frameBufB = nullptr;
volatile uint8_t *readyFrame = nullptr; 
volatile size_t readyFrameSize = 0;
volatile bool captureHealthy = false;

// Pre-calculated static BMP header (never changes for a fixed 160x120 resolution)
uint8_t staticBmpHeader[70];

// FreeRTOS Synchronization Mutex & Task Handle
SemaphoreHandle_t frameMutex = NULL;
TaskHandle_t cameraTaskHandle = NULL;

#define DBG(...) do { Serial.printf("[%8lu ms] ", millis()); Serial.printf(__VA_ARGS__); Serial.println(); } while (0)

// Build header once at startup
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

// --- DEDICATED CAMERA THREAD (Runs continuously on Core 1) ---
void cameraThreadFunction(void *pvParameters) {
  static bool usingBufA = true;
  
  while (true) {
    uint8_t *targetBuf = usingBufA ? frameBufA : frameBufB;

    camera->oneFrame();
    if (!camera->frame) {
      if (xSemaphoreTake(frameMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        captureHealthy = false;
        xSemaphoreGive(frameMutex);
      }
      vTaskDelay(pdMS_TO_TICKS(10)); // Brief pause only on failure
      continue;
    }

    // Safely write to the double buffer using a fast mutex lock
    if (xSemaphoreTake(frameMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      memcpy(targetBuf, camera->frame, FRAME_SIZE);
      readyFrame = targetBuf;
      readyFrameSize = FRAME_SIZE;
      captureHealthy = true;
      xSemaphoreGive(frameMutex);
      vTaskDelay(pdMS_TO_TICKS(0.1)); // Brief pause for wdt
    }

    usingBufA = !usingBufA;
    // No artificial vTaskDelay here—let it loop at maximum possible hardware speed!
  }
}

void handleCapture(AsyncWebServerRequest *request) {
  bool healthyCopy;
  const uint8_t *framePtr;
  size_t dataSizeCopy;

  // Grab pointers instantly under a tight mutex lock
  if (xSemaphoreTake(frameMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    healthyCopy = captureHealthy;
    framePtr = (const uint8_t *)readyFrame;
    dataSizeCopy = readyFrameSize;
    xSemaphoreGive(frameMutex);
  } else {
    request->send(503, "text/plain", "Server busy");
    return;
  }

  if (!healthyCopy || framePtr == nullptr) {
    request->send(503, "text/plain", "Camera error");
    return;
  }

  const size_t totalSize = 70 + dataSizeCopy;

  // Stream using pre-calculated static header for maximum speed
  AsyncWebServerResponse *response = request->beginChunkedResponse(
    "image/bmp",
    [framePtr, dataSizeCopy, totalSize](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      if (index >= totalSize) return 0;
      size_t toWrite;
      if (index < 70) {
        toWrite = min(maxLen, (size_t)70 - index);
        memcpy(buffer, staticBmpHeader + index, toWrite);
      } else {
        size_t dataIndex = index - 70;
        toWrite = min(maxLen, dataSizeCopy - dataIndex);
        memcpy(buffer, framePtr + dataIndex, toWrite);
      }
      return toWrite;
    }
  );
  request->send(response);
}

void setup() {
  pinMode(LIGHT_PIN, OUTPUT);

  Serial.begin(115200);
  delay(500);

  // Pre-calculate BMP header once at boot
  buildBmpHeader(staticBmpHeader, WIDTH, HEIGHT, FRAME_SIZE);

  frameBufA = (uint8_t *)malloc(FRAME_SIZE);
  frameBufB = (uint8_t *)malloc(FRAME_SIZE);

  camera = new OV7670(OV7670::Mode::QQVGA_RGB565, SIOD, SIOC, VSYNC, HREF, XCLK, PCLK, D0, D1, D2, D3, D4, D5, D6, D7);

  WiFi.config(local_IP, gateway, subnet);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
  }

  // CRITICAL: Disable Wi-Fi modem sleep to eliminate packet latency jitter
  WiFi.setSleep(false);

  // Create Mutex
  frameMutex = xSemaphoreCreateMutex();

  server.on("/capture", HTTP_GET, handleCapture);
  
  server.on("/light/on", HTTP_GET, [](AsyncWebServerRequest *request){
    digitalWrite(LIGHT_PIN, HIGH);
    request->send(200, "text/plain", "Light ON");
  });
  
  server.on("/light/off", HTTP_GET, [](AsyncWebServerRequest *request){
    digitalWrite(LIGHT_PIN, LOW);
    request->send(200, "text/plain", "Light OFF");
  });

  server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "Rebooting ESP32...");
    delay(100);
    ESP.restart();
  });

  server.begin();

  // Spawn camera thread on Core 1
  xTaskCreatePinnedToCore(
    cameraThreadFunction,
    "CameraTask",
    4096,
    NULL,
    2,                  // Higher priority than network background tasks
    &cameraTaskHandle,
    1                   // Core 1
  );
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}