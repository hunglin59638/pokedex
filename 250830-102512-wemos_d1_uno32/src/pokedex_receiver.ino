/*
 * 專案：寶可夢NFC圖鑑 - 圖鑑接收端
 * 開發板：Wemos D1 R32
 * 功能：接收從寶貝球傳來的寶可夢編號，顯示對應的寶可夢資訊和動畫
 * 同時，它也會在啟動時顯示自己的 MAC 位址，以便設定發送端。
 * 版本：v5 (USB供電模式 - 測試階段)
 */

// ESP-NOW and WiFi includes
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>     // 用於WiFi功率控制
#include <esp_system.h>   // 用於系統監控
#include <esp_task_wdt.h> // 用於看門狗
// TFT and graphics
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SPI.h>
// SD card and JSON support (re-enabled for dynamic loading)
#include <SD.h>
#include <ArduinoJson.h>
// Embedded data storage (no SD card needed)
#include <Arduino.h>
// Embedded GIF animation support
#include <AnimatedGIF.h>
// No more SD card or JSON dependencies!

// 定義TFT引腳 (CS=GPIO5, DC=GPIO2, RST=GPIO4)
#define TFT_CS 5
#define TFT_DC 2
#define TFT_RST 4
#define TFT_LED 2 // 背光控制

// 定義SD卡引腳 (重新啟用)
#define SD_CS 14 // SD卡片選引腳

// 定義GIF顯示區域參數
#define GIF_AREA_SIZE 144
#define GIF_AREA_X ((240 - GIF_AREA_SIZE) / 2)
#define GIF_AREA_Y 65

// Embedded Pokemon data removed - now using SD card JSON files only
// (Previous embedded data was for testing only)

// Test sprite data removed - using GIF-only mode

// 動態Pokemon資料結構 (從SD卡載入)
struct DynamicPokemonData
{
  int id;
  String name_en;
  String name_zh;
  String type1;
  String type2;
  int height;
  int weight;
  bool loaded;
};

// GIF記憶體緩衝系統 (單一Pokemon的暫存)
struct GIFBuffer
{
  int pokemon_id;
  uint8_t *gif_data;
  size_t gif_size;
  size_t capacity;
  bool loaded;

  GIFBuffer() : pokemon_id(-1), gif_data(nullptr), gif_size(0), capacity(0), loaded(false) {}

  ~GIFBuffer()
  {
    if (gif_data)
    {
      free(gif_data);
      gif_data = nullptr;
    }
  }

  bool allocate(size_t size)
  {
    if (gif_data)
    {
      free(gif_data);
    }
    gif_data = (uint8_t *)malloc(size);
    if (gif_data)
    {
      capacity = size;
      return true;
    }
    capacity = 0;
    return false;
  }

  void clear()
  {
    if (gif_data)
    {
      free(gif_data);
      gif_data = nullptr;
    }
    pokemon_id = -1;
    gif_size = 0;
    capacity = 0;
    loaded = false;
  }
};

// 全域變數：暫存當前Pokemon的資料
DynamicPokemonData currentPokemon;
GIFBuffer currentGIF;
AnimatedGIF gif;

// GIF canvas dimensions for proper frame clearing (fixes 殘影 issue)
int16_t g_xOffset = 0;
int16_t g_yOffset = 0;
int16_t g_canvasWidth = 0;
int16_t g_canvasHeight = 0;
// Add globals for scaling
int16_t g_origWidth = 0;
int16_t g_origHeight = 0;

// Pokemon data lookup removed - now using currentPokemon loaded from SD card JSON
// All Pokemon data access goes through loadAndDisplayPokemon() -> currentPokemon

// findPokemonSprite function removed - using GIF-only mode

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// ESP-NOW 相關設定
#define WIFI_CHANNEL 1 // 與發送端保持一致

// 系統狀態機定義
enum SystemState
{
  LISTENING,     // ESP-NOW開啟，等待接收Pokemon ID
  SD_LOADING,    // ESP-NOW關閉，正在從SD卡載入資料
  DISPLAYING,    // 正在顯示Pokemon資訊和動畫
  ERROR_RECOVERY // 錯誤恢復狀態
};

volatile SystemState currentState = LISTENING;
unsigned long stateChangeTime = 0; // 記錄狀態改變時間，用於超時檢查

// 定義接收的資料結構 (與發送端一致)
typedef struct struct_message
{
  int pokemon_id;
} struct_message;

struct_message receivedData;
volatile bool newDataReceived = false;
bool espnowEnabled = false; // 追蹤ESP-NOW狀態

// 系統安全和資源管理 - 極簡版本（無SD卡）
volatile bool displayBusy = false;
volatile bool pokemonDisplayRequested = false;
volatile int requestedPokemonId = 0;

// 無需SPI資源管理，只有TFT使用SPI

// 記憶體管理和系統監控
#define MIN_FREE_HEAP 50000 // 最小可用記憶體閾值
#define CRITICAL_HEAP 30000 // 危險記憶體閾值
#define WDT_TIMEOUT 30      // 看門狗超時時間(秒)

// 系統穩定性監控
volatile unsigned long lastHeartbeat = 0;
volatile bool systemHealthy = true;
volatile int consecutiveErrors = 0;
#define MAX_CONSECUTIVE_ERRORS 3

// 狀態轉換和安全檢查函數
bool changeSystemState(SystemState newState, const char *reason = "")
{
  if (newState == currentState)
  {
    return true; // 已經在目標狀態
  }

  Serial.printf("State transition: %d -> %d (%s)\n", currentState, newState, reason);

  // 檢查狀態轉換的有效性
  switch (currentState)
  {
  case LISTENING:
    if (newState != SD_LOADING && newState != ERROR_RECOVERY)
    {
      Serial.println("Invalid state transition from LISTENING");
      return false;
    }
    break;
  case SD_LOADING:
    if (newState != DISPLAYING && newState != ERROR_RECOVERY && newState != LISTENING)
    {
      Serial.println("Invalid state transition from SD_LOADING");
      return false;
    }
    break;
  case DISPLAYING:
    if (newState != LISTENING && newState != ERROR_RECOVERY)
    {
      Serial.println("Invalid state transition from DISPLAYING");
      return false;
    }
    break;
  case ERROR_RECOVERY:
    if (newState != LISTENING)
    {
      Serial.println("Invalid state transition from ERROR_RECOVERY");
      return false;
    }
    break;
  }

  currentState = newState;
  stateChangeTime = millis();
  return true;
}

// 檢查狀態超時
void checkStateTimeout()
{
  unsigned long timeInState = millis() - stateChangeTime;

  // SD_LOADING狀態不應該超過10秒
  if (currentState == SD_LOADING && timeInState > 10000)
  {
    Serial.println("SD_LOADING timeout - forcing recovery");
    changeSystemState(ERROR_RECOVERY, "SD loading timeout");
  }

  // ERROR_RECOVERY狀態不應該超過5秒
  if (currentState == ERROR_RECOVERY && timeInState > 5000)
  {
    Serial.println("ERROR_RECOVERY timeout - returning to LISTENING");
    changeSystemState(LISTENING, "Recovery timeout");
  }
}

// 記憶體監控函數
bool checkMemoryAvailable(const char *operation)
{
  size_t freeHeap = ESP.getFreeHeap();
  size_t minHeap = ESP.getMinFreeHeap();

  Serial.printf("Memory check for %s: %d bytes free (min: %d)\n",
                operation, freeHeap, minHeap);

  if (freeHeap < CRITICAL_HEAP)
  {
    Serial.printf("CRITICAL: Memory too low for %s operation!\n", operation);
    return false;
  }

  if (freeHeap < MIN_FREE_HEAP)
  {
    Serial.printf("WARNING: Low memory for %s operation\n", operation);
    // 執行垃圾回收
    ESP.getMinFreeHeap(); // Reset min heap counter
  }

  return true;
}

// 系統健康檢查
void updateSystemHealth()
{
  lastHeartbeat = millis();

  // 檢查記憶體狀態
  size_t freeHeap = ESP.getFreeHeap();

  if (freeHeap < CRITICAL_HEAP)
  {
    consecutiveErrors++;
    systemHealthy = false;
    Serial.printf("System health degraded: heap=%d, errors=%d\n", freeHeap, consecutiveErrors);
  }
  else
  {
    if (consecutiveErrors > 0)
    {
      consecutiveErrors--;
    }
    if (consecutiveErrors == 0)
    {
      systemHealthy = true;
    }
  }

  // 如果連續錯誤過多，嘗試系統恢復
  if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS)
  {
    Serial.println("CRITICAL: Too many consecutive errors, initiating recovery");
    performSystemRecovery();
  }
}

// 系統恢復程序 - 簡化版本（無SD卡）
void performSystemRecovery()
{
  Serial.println("Performing system recovery...");

  // 停止所有活動
  displayBusy = true;
  pokemonDisplayRequested = false;
  newDataReceived = false;

  // 重置TFT狀態
  digitalWrite(TFT_CS, HIGH);

  // 清理記憶體
  ESP.getMinFreeHeap();

  // 重新初始化基本功能
  delay(1000);

  // 重置錯誤計數器
  consecutiveErrors = 0;
  displayBusy = false;
  systemHealthy = true;

  Serial.println("System recovery completed");
}

// ESP-NOW安全控制函數
bool safelyDisableESPNOW()
{
  Serial.println("Safely disabling ESP-NOW for SD access...");

  try
  {
    // 停止ESP-NOW接收
    esp_now_deinit();

    // 關閉WiFi模式
    WiFi.mode(WIFI_OFF);

    // 等待確保完全關閉
    delay(100);

    Serial.println("ESP-NOW disabled successfully");
    return true;
  }
  catch (...)
  {
    Serial.println("Error disabling ESP-NOW");
    return false;
  }
}

bool safelyEnableESPNOW()
{
  Serial.println("Re-enabling ESP-NOW...");

  try
  {
    // 重新啟動WiFi
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // 等待WiFi穩定
    delay(100);

    // 重新初始化ESP-NOW
    if (esp_now_init() != ESP_OK)
    {
      Serial.println("Error re-initializing ESP-NOW");
      return false;
    }

    // 重新註冊回調函數
    esp_now_register_recv_cb(OnDataRecv);

    Serial.println("ESP-NOW re-enabled successfully");
    return true;
  }
  catch (...)
  {
    Serial.println("Error re-enabling ESP-NOW");
    return false;
  }
}

// SD卡安全載入函數 (ESP-NOW已關閉狀態)
bool loadPokemonFromSD(int pokemon_id)
{
  Serial.printf("Loading Pokemon #%d from SD card (ESP-NOW disabled)\n", pokemon_id);

  // 確保TFT_CS為高電平，避免SPI衝突
  digitalWrite(TFT_CS, HIGH);
  delay(10);

  // 初始化SD卡
  if (!SD.begin(SD_CS))
  {
    Serial.println("SD card initialization failed");
    return false;
  }

  // 載入JSON資料
  if (!loadPokemonJSON(pokemon_id))
  {
    SD.end();
    return false;
  }

  // 載入GIF資料
  if (!loadPokemonGIF(pokemon_id))
  {
    SD.end();
    return false;
  }

  // 安全關閉SD卡
  SD.end();
  digitalWrite(TFT_CS, LOW); // 恢復TFT控制

  Serial.printf("Pokemon #%d loaded successfully from SD\n", pokemon_id);
  return true;
}

// 載入Pokemon JSON資料
bool loadPokemonJSON(int pokemon_id)
{
  String jsonPath = "/pokemon/" + String(pokemon_id) + ".json";

  File jsonFile = SD.open(jsonPath);
  if (!jsonFile)
  {
    Serial.printf("Failed to open JSON file: %s\n", jsonPath.c_str());
    return false;
  }

  // 讀取JSON內容
  String jsonContent = "";
  while (jsonFile.available())
  {
    jsonContent += (char)jsonFile.read();
  }
  jsonFile.close();

  Serial.printf("DEBUG: Raw JSON content for Pokemon #%d:\n", pokemon_id);
  Serial.println(jsonContent);
  Serial.println("DEBUG: End of JSON content");

  // 解析JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonContent);

  if (error)
  {
    Serial.printf("JSON parsing failed: %s\n", error.c_str());
    return false;
  }

  // 提取Pokemon資料 - 匹配實際JSON結構
  currentPokemon.id = pokemon_id;
  currentPokemon.name_en = doc["names"]["en"].as<String>();
  // Chinese name removed - not needed

  JsonArray types = doc["types"];
  if (types.size() > 0)
  {
    currentPokemon.type1 = types[0].as<String>();
  }
  if (types.size() > 1)
  {
    currentPokemon.type2 = types[1].as<String>();
  }
  else
  {
    currentPokemon.type2 = "";
  }

  currentPokemon.height = doc["height"].as<int>();
  currentPokemon.weight = doc["weight"].as<int>();
  currentPokemon.loaded = true;

  Serial.printf("DEBUG: Pokemon JSON loaded successfully!\n");
  Serial.printf("  ID: %d\n", currentPokemon.id);
  Serial.printf("  Name EN: '%s'\n", currentPokemon.name_en.c_str());
  // Chinese name debug removed
  Serial.printf("  Type1: '%s'\n", currentPokemon.type1.c_str());
  Serial.printf("  Type2: '%s'\n", currentPokemon.type2.c_str());
  Serial.printf("  Height: %d, Weight: %d\n", currentPokemon.height, currentPokemon.weight);
  Serial.printf("  Loaded: %s\n", currentPokemon.loaded ? "true" : "false");

  return true;
}

// 載入Pokemon GIF資料到記憶體緩衝區
bool loadPokemonGIF(int pokemon_id)
{
  String gifPath = "/pokemon/" + String(pokemon_id) + ".gif";

  File gifFile = SD.open(gifPath);
  if (!gifFile)
  {
    Serial.printf("Failed to open GIF file: %s\n", gifPath.c_str());
    return false;
  }

  size_t gifSize = gifFile.size();
  Serial.printf("GIF file size: %d bytes\n", gifSize);

  // 檢查記憶體是否足夠
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < gifSize + 50000) // 保留50KB緩衝
  {
    Serial.printf("Insufficient memory for GIF (need %d, have %d)\n", gifSize, freeHeap);
    gifFile.close();
    return false;
  }

  // 分配記憶體緩衝區
  if (!currentGIF.allocate(gifSize))
  {
    Serial.println("Failed to allocate GIF buffer");
    gifFile.close();
    return false;
  }

  // 讀取GIF資料到記憶體
  size_t bytesRead = 0;
  while (gifFile.available() && bytesRead < gifSize)
  {
    int bytesToRead = min(1024, (int)(gifSize - bytesRead));
    int actualRead = gifFile.read(currentGIF.gif_data + bytesRead, bytesToRead);
    if (actualRead <= 0)
    {
      break;
    }
    bytesRead += actualRead;
  }

  gifFile.close();

  if (bytesRead != gifSize)
  {
    Serial.printf("GIF read error: expected %d bytes, got %d\n", gifSize, bytesRead);
    currentGIF.clear();
    return false;
  }

  currentGIF.pokemon_id = pokemon_id;
  currentGIF.gif_size = gifSize;
  currentGIF.loaded = true;

  Serial.printf("Loaded GIF to memory: %d bytes\n", gifSize);
  return true;
}

// 安全延遲函數，包含系統監控
void safeDelayWithMemCheck(unsigned long ms, const char *context = "delay")
{
  unsigned long start = millis();
  while (millis() - start < ms)
  {
    yield();

    // 餵看門狗
    esp_task_wdt_reset();

    delay(10);

    // 每500ms檢查一次系統狀態
    if ((millis() - start) % 500 == 0)
    {
      updateSystemHealth();

      if (!systemHealthy)
      {
        Serial.printf("System unhealthy during %s\n", context);
        break;
      }
    }
  }
}

// 安全的ESP-NOW接收回調函數 - 狀態機版本
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len)
{
  // 只在LISTENING狀態才處理新請求
  if (currentState != LISTENING)
  {
    // 系統正忙，忽略請求 (不在中斷中印出訊息)
    return;
  }

  // 檢查資料長度和系統狀態
  if (len == sizeof(struct_message) && !displayBusy)
  {
    memcpy(&receivedData, incomingData, sizeof(receivedData));
    requestedPokemonId = receivedData.pokemon_id;
    pokemonDisplayRequested = true;
    newDataReceived = true;
  }

  // 避免在中斷中進行Serial輸出
}

// GIF support removed - using embedded data only

// Pokemon type colors (RGB565 format)
struct TypeColor
{
  const char *name;
  uint16_t color;
};

const TypeColor typeColors[] = {
    {"normal", 0xA534},   // Light brown
    {"fire", 0xF800},     // Red
    {"water", 0x047F},    // Blue
    {"electric", 0xFFE0}, // Yellow
    {"grass", 0x07E0},    // Green
    {"ice", 0xB7FF},      // Light blue
    {"fighting", 0xC000}, // Dark red
    {"poison", 0x8010},   // Purple
    {"ground", 0xFBE0},   // Brown/yellow
    {"flying", 0x867F},   // Light purple
    {"psychic", 0xF81F},  // Magenta
    {"bug", 0x8400},      // Green/brown
    {"rock", 0x7800},     // Brown
    {"ghost", 0x4210},    // Dark purple
    {"dragon", 0x7817},   // Blue/purple
    {"dark", 0x2104},     // Dark brown
    {"steel", 0x8410},    // Silver/gray
    {"fairy", 0xFBDF}     // Pink
};

// Pokemon精靈動畫系統 (替代GIF)
// Helper function to draw a filled ellipse using circles
void fillEllipse(int16_t centerX, int16_t centerY, int16_t width, int16_t height, uint16_t color)
{
  int16_t radiusX = width / 2;
  int16_t radiusY = height / 2;

  // Draw ellipse by drawing multiple circles with varying radii
  for (int16_t y = -radiusY; y <= radiusY; y++)
  {
    int16_t x = (int16_t)(radiusX * sqrt(1.0 - (float)(y * y) / (radiusY * radiusY)));
    if (x > 0)
    {
      tft.drawFastHLine(centerX - x, centerY + y, 2 * x, color);
    }
  }
}

// Helper function to draw an ellipse outline using circles
void drawEllipse(int16_t centerX, int16_t centerY, int16_t width, int16_t height, uint16_t color)
{
  int16_t radiusX = width / 2;
  int16_t radiusY = height / 2;

  // Draw ellipse outline by drawing points at calculated positions
  for (int angle = 0; angle < 360; angle += 5)
  {
    float rad = angle * PI / 180.0;
    int16_t x = centerX + (int16_t)(radiusX * cos(rad));
    int16_t y = centerY + (int16_t)(radiusY * sin(rad));
    tft.drawPixel(x, y, color);
  }
}

// drawSimplePokemonSprite function removed - using GIF animations only

// playPokemonSpriteAnimation function removed - using GIF animations only

// Enhanced Pokemon animation with particle effects and sparkles
void playEnhancedPokemonAnimation(int id, int16_t areaX, int16_t areaY, int16_t areaWidth, int16_t areaHeight, int duration_ms = 3000)
{
  Serial.printf("GIF-only mode: No programmatic animation for Pokemon #%d\n", id);

  // Just show the border area for debugging in GIF-only mode
  tft.drawRect(areaX, areaY, areaWidth, areaHeight, ILI9341_RED);
  tft.drawRect(areaX + 1, areaY + 1, areaWidth - 2, areaHeight - 2, ILI9341_YELLOW);

  Serial.printf("Showing 150x150 border area at X=%d, Y=%d for GIF placeholder\n", areaX, areaY);
}

// 簡化的顯示器管理 - 無需SPI競爭（只有TFT）
bool acquireDisplay(const char *requester)
{
  if (displayBusy)
  {
    Serial.printf("Display busy, waiting for %s\n", requester);
    int attempts = 0;
    while (displayBusy && attempts < 50)
    {
      delay(10);
      attempts++;
      esp_task_wdt_reset(); // 餵看門狗
    }

    if (displayBusy)
    {
      Serial.printf("Display timeout for %s, forcing release\n", requester);
      displayBusy = false; // 強制釋放
    }
  }

  displayBusy = true;
  Serial.printf("Display acquired by %s\n", requester);
  return true;
}

void releaseDisplay(const char *requester)
{
  displayBusy = false;
  Serial.printf("Display released by %s\n", requester);
}

void safeDelay(unsigned long ms)
{
  unsigned long start = millis();
  while (millis() - start < ms)
  {
    yield();
    delay(10);
  }
}

// Helper function to get type color
uint16_t getTypeColor(const char *typeName)
{
  for (int i = 0; i < sizeof(typeColors) / sizeof(typeColors[0]); i++)
  {
    if (strcmp(typeColors[i].name, typeName) == 0)
    {
      return typeColors[i].color;
    }
  }
  return ILI9341_WHITE; // Default color if type not found
}

// Helper function to draw a type badge
void drawTypeBadge(int16_t x, int16_t y, int16_t w, int16_t h, const char *typeName, uint16_t bgColor)
{
  // Draw rounded rectangle background
  tft.fillRoundRect(x, y, w, h, 4, bgColor);
  tft.drawRoundRect(x, y, w, h, 4, ILI9341_WHITE);

  // Draw text centered in badge with smaller font
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1); // Change back to size 1 (smaller than name which is size 2)

  // Calculate text position to center it (text size 1 = 6x8 pixels per character)
  int16_t textX = x + (w - strlen(typeName) * 6) / 2;
  int16_t textY = y + (h - 8) / 2;

  tft.setCursor(textX, textY);
  tft.print(typeName);
}

// Draw Pokemon Ball graphics
void drawPokemonBall(int16_t centerX, int16_t centerY, int16_t radius)
{
  // Main ball colors
  uint16_t redColor = ILI9341_RED;
  uint16_t whiteColor = ILI9341_WHITE;
  uint16_t blackColor = ILI9341_BLACK;
  uint16_t grayColor = 0x7BEF; // Light gray

  // Draw outer circle (black border)
  tft.drawCircle(centerX, centerY, radius, blackColor);
  tft.drawCircle(centerX, centerY, radius - 1, blackColor);

  // Draw upper half (red)
  for (int16_t y = centerY - radius + 2; y < centerY - 2; y++)
  {
    int16_t halfWidth = (int16_t)sqrt(radius * radius - (y - centerY) * (y - centerY)) - 2;
    if (halfWidth > 0)
    {
      tft.drawFastHLine(centerX - halfWidth, y, halfWidth * 2, redColor);
    }
  }

  // Draw lower half (white)
  for (int16_t y = centerY + 3; y < centerY + radius - 2; y++)
  {
    int16_t halfWidth = (int16_t)sqrt(radius * radius - (y - centerY) * (y - centerY)) - 2;
    if (halfWidth > 0)
    {
      tft.drawFastHLine(centerX - halfWidth, y, halfWidth * 2, whiteColor);
    }
  }

  // Draw middle band (black)
  tft.drawFastHLine(centerX - radius + 2, centerY - 2, (radius - 2) * 2, blackColor);
  tft.drawFastHLine(centerX - radius + 2, centerY - 1, (radius - 2) * 2, blackColor);
  tft.drawFastHLine(centerX - radius + 2, centerY, (radius - 2) * 2, blackColor);
  tft.drawFastHLine(centerX - radius + 2, centerY + 1, (radius - 2) * 2, blackColor);
  tft.drawFastHLine(centerX - radius + 2, centerY + 2, (radius - 2) * 2, blackColor);

  // Draw center button (white circle with black border)
  int16_t buttonRadius = radius / 4;
  tft.fillCircle(centerX, centerY, buttonRadius + 2, blackColor);
  tft.fillCircle(centerX, centerY, buttonRadius, whiteColor);

  // Add inner button detail
  tft.drawCircle(centerX, centerY, buttonRadius - 2, grayColor);
}

// Smooth screen transition effect
void fadeToBlack(int steps = 10)
{
  // Simple fade-to-black effect by drawing increasingly dark rectangles
  for (int i = 0; i < steps; i++)
  {
    // Create a semi-transparent overlay effect by drawing with different patterns
    uint16_t fadeColor = ILI9341_BLACK;

    // Draw diagonal lines for fade effect
    for (int y = i; y < 320; y += steps)
    {
      tft.drawFastHLine(0, y, 240, fadeColor);
    }
    delay(30);
  }
  tft.fillScreen(ILI9341_BLACK);
}

// Smooth transition from Pokemon Ball to Pokemon display
void transitionFromWelcomeScreen()
{
  Serial.println("Starting smooth transition from welcome screen");

  // Animate Pokemon Ball shrinking
  int16_t ballCenterX = tft.width() / 2;
  int16_t ballCenterY = 120;

  for (int radius = 60; radius > 10; radius -= 5)
  {
    // Clear area around ball
    tft.fillCircle(ballCenterX, ballCenterY, radius + 10, ILI9341_BLACK);

    // Draw smaller ball
    drawPokemonBall(ballCenterX, ballCenterY, radius);

    delay(50);
    esp_task_wdt_reset();
  }

  // Final fade
  fadeToBlack(5);
  Serial.println("Transition completed");
}

// Enhanced Pokemon Ball welcome screen with pulsing animation
void displayPokemonBallWelcome()
{
  Serial.println("Displaying Pokemon Ball welcome screen");

  // Clear screen with black background
  tft.fillScreen(ILI9341_BLACK);

  // Title text with fade-in effect
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  int16_t titleY = 30;
  String title = "POKEDEX";
  int16_t titleX = (tft.width() - title.length() * 12) / 2;

  // Fade in title character by character
  for (int i = 0; i < title.length(); i++)
  {
    tft.setCursor(titleX + i * 12, titleY);
    tft.print(title.charAt(i));
    delay(100);
  }

  delay(300);

  // Draw Pokemon Ball with animation
  int16_t ballCenterX = tft.width() / 2;
  int16_t ballCenterY = 120;
  int16_t ballRadius = 60;

  // Animate ball growing from center
  for (int radius = 5; radius <= ballRadius; radius += 3)
  {
    drawPokemonBall(ballCenterX, ballCenterY, radius);
    delay(30);
    esp_task_wdt_reset();
  }

  // Subtitle with typewriter effect
  tft.setTextSize(1);
  int16_t subtitleY = 210;
  String subtitle = "NFC Pokemon Scanner";
  int16_t subtitleX = (tft.width() - subtitle.length() * 6) / 2;

  for (int i = 0; i < subtitle.length(); i++)
  {
    tft.setCursor(subtitleX + i * 6, subtitleY);
    tft.print(subtitle.charAt(i));
    delay(80);
  }

  // System info with color animation
  tft.setTextSize(1);
  tft.setCursor(10, 240);

  // Cycle through colors for system info
  uint16_t colors[] = {ILI9341_CYAN, ILI9341_GREEN, ILI9341_YELLOW};
  for (int c = 0; c < 3; c++)
  {
    tft.fillRect(10, 240, 220, 16, ILI9341_BLACK);
    tft.setTextColor(colors[c]);
    tft.setCursor(10, 240);
    tft.print("SD Card Data Ready!");
    delay(300);
  }

  // Final pulsing effect on Pokemon Ball
  for (int pulse = 0; pulse < 3; pulse++)
  {
    // Brighten
    tft.drawCircle(ballCenterX, ballCenterY, ballRadius + 2, ILI9341_YELLOW);
    delay(200);

    // Return to normal
    tft.drawCircle(ballCenterX, ballCenterY, ballRadius + 2, ILI9341_BLACK);
    delay(200);
    esp_task_wdt_reset();
  }

  Serial.println("Enhanced Pokemon Ball welcome screen displayed");
}

// Pokemon資訊顯示函數 - 使用SD卡載入的currentPokemon資料
bool displayPokemonInfo(int id)
{
  Serial.printf("Displaying Pokemon info for ID %d (from SD card data)\n", id);

  if (!checkMemoryAvailable("displayPokemonInfo"))
  {
    Serial.println("Memory check failed for displayPokemonInfo");
    return false;
  }

  if (!acquireDisplay("displayPokemonInfo"))
  {
    Serial.println("Failed to acquire display for displayPokemonInfo");
    return false;
  }

  // 檢查currentPokemon是否已載入且ID匹配
  if (!currentPokemon.loaded || currentPokemon.id != id)
  {
    Serial.printf("Pokemon ID %d not loaded from SD card (currentPokemon.loaded=%s, currentPokemon.id=%d)\n",
                  id, currentPokemon.loaded ? "true" : "false", currentPokemon.id);
    releaseDisplay("displayPokemonInfo");
    return false;
  }

  Serial.printf("Found Pokemon: %s (ID: %d)\n", currentPokemon.name_en.c_str(), currentPokemon.id);

  // 無需SPI競爭 - 只有TFT使用SPI
  Serial.println("Starting TFT display (no SD conflicts!)");

  // Clear screen
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);

  // Display Pokemon ID (centered at top)
  tft.setTextSize(2);
  char idText[16];
  snprintf(idText, sizeof(idText), "#%d", currentPokemon.id);

  int16_t idX = (tft.width() - strlen(idText) * 12) / 2;
  tft.setCursor(idX, 20);
  tft.print(idText);

  // Display Pokemon name (English)
  int16_t nameX = (tft.width() - currentPokemon.name_en.length() * 12) / 2;
  tft.setCursor(nameX, 40);
  tft.print(currentPokemon.name_en);

  // Chinese name display removed

  // Display height and weight
  tft.setTextSize(1);

  // Height (left side)
  float height = currentPokemon.height / 10.0; // Convert decimeters to meters
  char heightLabel[] = "Height:";
  char heightValue[16];
  snprintf(heightValue, sizeof(heightValue), "%.1f m", height);

  int16_t heightLabelX = (tft.width() / 2 - strlen(heightLabel) * 6) / 2;
  int16_t heightValueX = (tft.width() / 2 - strlen(heightValue) * 6) / 2;

  tft.setCursor(heightLabelX, 210);
  tft.print(heightLabel);
  tft.setCursor(heightValueX, 225);
  tft.print(heightValue);

  // Weight (right side)
  float weight = currentPokemon.weight / 10.0; // Convert hectograms to kg
  char weightLabel[] = "Weight:";
  char weightValue[16];
  snprintf(weightValue, sizeof(weightValue), "%.1f kg", weight);

  int16_t weightLabelX = tft.width() / 2 + (tft.width() / 2 - strlen(weightLabel) * 6) / 2;
  int16_t weightValueX = tft.width() / 2 + (tft.width() / 2 - strlen(weightValue) * 6) / 2;

  tft.setCursor(weightLabelX, 210);
  tft.print(weightLabel);
  tft.setCursor(weightValueX, 225);
  tft.print(weightValue);

  // Display type badges
  int badgeY = 250;
  int badgeWidth = 70;
  int badgeHeight = 20;
  int badgeSpacing = 10;

  if (currentPokemon.type2.length() > 0)
  {
    // Two types
    int totalWidth = 2 * badgeWidth + badgeSpacing;
    int startX = (tft.width() - totalWidth) / 2;

    uint16_t type1Color = getTypeColor(currentPokemon.type1.c_str());
    uint16_t type2Color = getTypeColor(currentPokemon.type2.c_str());

    drawTypeBadge(startX, badgeY, badgeWidth, badgeHeight, currentPokemon.type1.c_str(), type1Color);
    drawTypeBadge(startX + badgeWidth + badgeSpacing, badgeY, badgeWidth, badgeHeight, currentPokemon.type2.c_str(), type2Color);
  }
  else
  {
    // Single type
    int startX = (tft.width() - badgeWidth) / 2;
    uint16_t typeColor = getTypeColor(currentPokemon.type1.c_str());
    drawTypeBadge(startX, badgeY, badgeWidth, badgeHeight, currentPokemon.type1.c_str(), typeColor);
  }

  releaseDisplay("displayPokemonInfo");

  Serial.println("Pokemon info displayed successfully (from SD card data)");
  return true;
}

// Enhanced Pokemon scanning notification with animation
void showPokemonScanAnimation()
{
  Serial.println("Showing Pokemon scan animation");

  // Show scanning effect
  int16_t centerX = tft.width() / 2;
  int16_t centerY = 160;

  // Expanding circles effect
  for (int radius = 10; radius < 100; radius += 15)
  {
    tft.drawCircle(centerX, centerY, radius, ILI9341_CYAN);
    delay(100);
    tft.drawCircle(centerX, centerY, radius, ILI9341_BLACK); // Erase
    esp_task_wdt_reset();
  }

  // Scanning text effect
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(2);
  String scanText = "SCANNING...";
  int16_t textX = (tft.width() - scanText.length() * 12) / 2;

  for (int i = 0; i < 3; i++)
  {
    tft.setCursor(textX, centerY - 10);
    tft.print(scanText);
    delay(300);

    // Flash effect
    tft.fillRect(textX, centerY - 10, scanText.length() * 12, 16, ILI9341_BLACK);
    delay(200);
    esp_task_wdt_reset();
  }
}

// 載入畫面顯示函數
void showLoadingScreen(int pokemon_id)
{
  Serial.printf("Showing loading screen for Pokemon #%d\n", pokemon_id);

  // 清除螢幕
  tft.fillScreen(ILI9341_BLACK);

  // 顯示Pokemon ID
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(3);
  String idText = "Pokemon #" + String(pokemon_id);
  int16_t idX = (tft.width() - idText.length() * 18) / 2;
  tft.setCursor(idX, 60);
  tft.print(idText);

  // 顯示載入文字
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(2);
  String loadingText = "Loading from SD...";
  int16_t loadingX = (tft.width() - loadingText.length() * 12) / 2;
  tft.setCursor(loadingX, 120);
  tft.print(loadingText);

  // 載入進度條動畫
  int16_t barWidth = 200;
  int16_t barHeight = 10;
  int16_t barX = (tft.width() - barWidth) / 2;
  int16_t barY = 160;

  // 進度條外框
  tft.drawRect(barX - 2, barY - 2, barWidth + 4, barHeight + 4, ILI9341_WHITE);

  // 動畫載入條
  for (int progress = 0; progress <= 100; progress += 10)
  {
    int16_t fillWidth = (barWidth * progress) / 100;
    tft.fillRect(barX, barY, fillWidth, barHeight, ILI9341_GREEN);

    // 顯示百分比
    tft.fillRect(barX, barY + 20, 60, 16, ILI9341_BLACK);
    tft.setTextSize(1);
    tft.setCursor(barX, barY + 20);
    tft.printf("%d%%", progress);

    delay(50); // 載入動畫速度
  }

  // 載入完成提示
  tft.setTextColor(ILI9341_GREEN);
  tft.setTextSize(1);
  String completeText = "Loading Complete!";
  int16_t completeX = (tft.width() - completeText.length() * 6) / 2;
  tft.setCursor(completeX, 200);
  tft.print(completeText);

  delay(500); // 短暫停留顯示完成訊息
}

// 主要的Pokemon載入和顯示流程 (動態ESP-NOW切換版本)
bool loadAndDisplayPokemon(int pokemon_id)
{
  Serial.printf("Starting dynamic load process for Pokemon #%d\n", pokemon_id);

  // 1. 切換到載入狀態
  if (!changeSystemState(SD_LOADING, "Pokemon requested"))
  {
    return false;
  }

  // 2. 顯示載入畫面
  showLoadingScreen(pokemon_id);

  // 3. 安全關閉ESP-NOW
  if (!safelyDisableESPNOW())
  {
    Serial.println("Failed to disable ESP-NOW");
    changeSystemState(ERROR_RECOVERY, "ESP-NOW disable failed");
    return false;
  }

  // 4. 從SD卡載入Pokemon資料 (此時無SPI衝突!)
  bool loadSuccess = loadPokemonFromSD(pokemon_id);

  // 5. 重新啟動ESP-NOW
  if (!safelyEnableESPNOW())
  {
    Serial.println("Failed to re-enable ESP-NOW");
    changeSystemState(ERROR_RECOVERY, "ESP-NOW re-enable failed");
    return false;
  }

  // 6. 檢查載入結果並顯示
  if (loadSuccess)
  {
    // 狀態：正在顯示靜態資訊
    changeSystemState(DISPLAYING, "Data loaded, showing info");

    // 顯示Pokemon資訊和動畫。
    // displayDynamicPokemonData() 將會進入一個持續的GIF循環，
    // 直到新的NFC掃描發生。
    Serial.printf("DEBUG: About to call displayDynamicPokemonData. currentPokemon.loaded=%s, currentPokemon.id=%d\n",
                  currentPokemon.loaded ? "true" : "false", currentPokemon.id);

    // *** 核心變更 ***
    // 在進入GIF循環之前，將狀態切換回LISTENING，以便OnDataRecv回調可以接收下一個寶可夢ID。
    changeSystemState(LISTENING, "Displaying GIF, ready for next scan");
    displayDynamicPokemonData();

    // 當displayDynamicPokemonData()返回時，代表已經收到了新的寶可夢ID。
    // 我們不需要再做任何事，直接返回true，主循環會處理新的請求。
    return true;
  }
  else
  {
    Serial.printf("Failed to load Pokemon #%d data\n", pokemon_id);
    changeSystemState(ERROR_RECOVERY, "Data loading failed");
    return false;
  }
}

// 顯示動態載入的Pokemon資料
void displayDynamicPokemonData()
{
  if (!currentPokemon.loaded)
  {
    Serial.println("No Pokemon data loaded");
    return;
  }

  Serial.printf("Displaying dynamic Pokemon: %s\n", currentPokemon.name_en.c_str());

  // 清除螢幕
  tft.fillScreen(ILI9341_BLACK);

  // 顯示Pokemon ID
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  String idText = "#" + String(currentPokemon.id);
  int16_t idX = (tft.width() - idText.length() * 12) / 2;
  tft.setCursor(idX, 20);
  tft.print(idText);

  // 顯示英文名稱
  int16_t nameX = (tft.width() - currentPokemon.name_en.length() * 12) / 2;
  tft.setCursor(nameX, 45);
  tft.print(currentPokemon.name_en);

  // Chinese name display removed

  // 顯示屬性標籤 (調整為適當間距)
  int badgeY = 245; // 精靈結束於Y=220，留25px間距
  int badgeWidth = 70;
  int badgeHeight = 20;

  if (currentPokemon.type2.length() > 0)
  {
    // 雙屬性
    drawTypeBadge(40, badgeY, badgeWidth, badgeHeight,
                  currentPokemon.type1.c_str(), getTypeColor(currentPokemon.type1.c_str()));
    drawTypeBadge(130, badgeY, badgeWidth, badgeHeight,
                  currentPokemon.type2.c_str(), getTypeColor(currentPokemon.type2.c_str()));
  }
  else
  {
    // 單屬性
    int singleBadgeX = (tft.width() - badgeWidth) / 2;
    drawTypeBadge(singleBadgeX, badgeY, badgeWidth, badgeHeight,
                  currentPokemon.type1.c_str(), getTypeColor(currentPokemon.type1.c_str()));
  }

  // 顯示身高體重 (同一行)
  tft.setTextSize(1);
  float height = currentPokemon.height / 10.0;
  float weight = currentPokemon.weight / 10.0;

  // 格式化為單行顯示: "Height: XX.X m     Weight: XX.X kg"
  char statsText[64];
  snprintf(statsText, sizeof(statsText), "Height: %.1fm     Weight: %.1fkg", height, weight);

  tft.setCursor(20, 275); // 精靈結束於Y=220，屬性在Y=245，身高體重在Y=275
  tft.print(statsText);

  // 播放GIF動畫 (GIF-only mode)
  if (currentGIF.loaded)
  {
    playGIFFromMemory();
  }
  else
  {
    Serial.printf("No GIF available for Pokemon #%d - showing border area only\n", currentPokemon.id);
    // 顯示邊框區域以便調試
    tft.drawRect(GIF_AREA_X, GIF_AREA_Y, GIF_AREA_SIZE, GIF_AREA_SIZE, ILI9341_RED);
    tft.drawRect(GIF_AREA_X + 1, GIF_AREA_Y + 1, GIF_AREA_SIZE - 2, GIF_AREA_SIZE - 2, ILI9341_YELLOW);
  }
}

// GIF回調函數 - 在TFT上繪製GIF幀
void GIFDraw(GIFDRAW *pDraw) {
    if (!pDraw) return;

    uint8_t *pPixels = pDraw->pPixels;
    uint16_t *pPalette = pDraw->pPalette;

    // A more reliable check for the start of a new frame to clear the background.
    // This is crucial for GIFs that use transparency and different disposal methods.
    if (pDraw->y == 0 && pDraw->iX == 0 && pDraw->iY == 0) {
        tft.fillRect(g_xOffset, g_yOffset, g_canvasWidth, g_canvasHeight, ILI9341_BLACK);
    }

    // Scaling factors
    float scaleX = (float)g_canvasWidth / g_origWidth;
    float scaleY = (float)g_canvasHeight / g_origHeight;

    // Calculate destination drawing parameters based on the partial update's position and size
    int16_t destX = g_xOffset + (int16_t)(pDraw->iX * scaleX);
    int16_t destY_start = g_yOffset + (int16_t)((pDraw->iY + pDraw->y) * scaleY);
    int16_t destY_end = g_yOffset + (int16_t)((pDraw->iY + pDraw->y + 1) * scaleY);
    int16_t destW = (int16_t)(pDraw->iWidth * scaleX);

    if (destW <= 0) return; // Nothing to draw

    // Create a buffer for the scaled partial line
    uint16_t scaledLineBuffer[destW];

    // Generate the scaled line of pixels for the partial width
    for (int x = 0; x < destW; x++) {
        int src_x = (int)(x / scaleX);
        if (src_x >= pDraw->iWidth) src_x = pDraw->iWidth - 1;

        uint8_t idx = pPixels[src_x];
        uint16_t color;

        if (pDraw->ucHasTransparency && idx == pDraw->ucTransparent) {
            color = ILI9341_BLACK; // Render transparent pixels as black
        } else {
            color = pPalette[idx];
        }
        scaledLineBuffer[x] = color;
    }

    // Draw the scaled partial line buffer multiple times for vertical scaling
    tft.startWrite();
    for (int y = destY_start; y < destY_end; y++) {
        if (y < (g_yOffset + g_canvasHeight)) { // Ensure we don't draw outside the canvas
            tft.setAddrWindow(destX, y, destW, 1);
            tft.writePixels(scaledLineBuffer, destW);
        }
    }
    tft.endWrite();
}

// 從記憶體播放GIF動畫
void playGIFFromMemory()
{
  if (!currentGIF.loaded || !currentGIF.gif_data)
  {
    Serial.println("No GIF data available");
    return;
  }

  Serial.printf("Playing GIF from memory: %d bytes\n", currentGIF.gif_size);

  // 打開記憶體中的GIF
  if (gif.open(currentGIF.gif_data, currentGIF.gif_size, GIFDraw))
  {
    // Get GIF dimensions and calculate canvas area for frame clearing
    g_origWidth = gif.getCanvasWidth();
    g_origHeight = gif.getCanvasHeight();

    Serial.printf("GIF opened successfully: %dx%d, scaling to %dx%d\n", g_origWidth, g_origHeight, GIF_AREA_SIZE, GIF_AREA_SIZE);

    // Set the global canvas size to the target rendering size
    g_canvasWidth = GIF_AREA_SIZE;
    g_canvasHeight = GIF_AREA_SIZE;

    // Use the predefined square area
    int16_t squareX = GIF_AREA_X;
    int16_t squareY = GIF_AREA_Y;

    // The scaled GIF will start at the top-left of the defined area
    g_xOffset = squareX;
    g_yOffset = squareY;

    // 詳細調試資訊
    Serial.printf("=== GIF POSITIONING DEBUG ===\n");
    Serial.printf("Square area: X=%d, Y=%d, Size=%dx%d\n", squareX, squareY, GIF_AREA_SIZE, GIF_AREA_SIZE);
    Serial.printf("GIF dimensions: %dx%d\n", g_origWidth, g_origHeight);
    Serial.printf("Calculated GIF position: X=%d, Y=%d\n", g_xOffset, g_yOffset);
    Serial.printf("GIF will be centered in square: %s\n",
                  (g_xOffset >= squareX && g_yOffset >= squareY) ? "YES" : "NO");

    // 檢查GIF是否超出邊界
    int16_t gifRight = g_xOffset + g_origWidth;
    int16_t gifBottom = g_yOffset + g_origHeight;
    int16_t squareRight = squareX + GIF_AREA_SIZE;
    int16_t squareBottom = squareY + GIF_AREA_SIZE;

    Serial.printf("GIF bounds: Left=%d, Right=%d, Top=%d, Bottom=%d\n",
                  g_xOffset, gifRight, g_yOffset, gifBottom);
    Serial.printf("Square bounds: Left=%d, Right=%d, Top=%d, Bottom=%d\n",
                  squareX, squareRight, squareY, squareBottom);

    if (gifRight > squareRight || gifBottom > squareBottom || g_xOffset < squareX || g_yOffset < squareY)
    {
      Serial.printf("WARNING: GIF extends beyond square boundaries!\n");
    }

    // 添加邊框以便調試 - 顯示GIF_AREA_SIZE正方形區域
    tft.drawRect(squareX, squareY, GIF_AREA_SIZE, GIF_AREA_SIZE, ILI9341_RED);                    // 外邊框 (紅色)
    tft.drawRect(squareX + 1, squareY + 1, GIF_AREA_SIZE - 2, GIF_AREA_SIZE - 2, ILI9341_YELLOW); // 內邊框 (黃色)

    // 添加GIF實際邊界框 (藍色)
    tft.drawRect(g_xOffset, g_yOffset, g_origWidth, g_origHeight, ILI9341_BLUE);

    Serial.printf("DEBUG: Red/Yellow borders show intended square area\n");
    Serial.printf("DEBUG: Blue border shows actual GIF area\n");

    // 持續播放動畫，直到收到新的Pokemon ID
    Serial.println("Entering continuous GIF playback loop...");
    while (!newDataReceived) // Loop until a new Pokemon is scanned
    {
      if (!gif.playFrame(true, NULL))
      {
        gif.reset(); // Loop the GIF
      }
      esp_task_wdt_reset(); // Feed the watchdog
      delay(50);            // Approx 20 FPS, and allows other tasks to run
    }

    gif.close();
    Serial.println("New data received, exiting GIF playback.");
  }
  else
  {
    Serial.println("Failed to open GIF from memory");
  }
}

// playProgrammaticAnimation function removed - using GIF-only mode

// 完整的Pokemon頁面顯示函數 - 包含精靈動畫和增強過渡效果
bool displayPokemonPage(int id)
{
  Serial.printf("Displaying Pokemon page for #%d with enhanced transitions\n", id);

  if (!checkMemoryAvailable("displayPokemonPage"))
  {
    Serial.println("Insufficient memory for Pokemon page display");
    return false;
  }

  // Step 1: Show transition from welcome screen if this is first Pokemon
  static bool isFirstPokemon = true;
  if (isFirstPokemon)
  {
    transitionFromWelcomeScreen();
    isFirstPokemon = false;
  }
  else
  {
    // Quick fade for subsequent Pokemon
    fadeToBlack(3);
  }

  // Step 2: Show scanning animation
  showPokemonScanAnimation();

  // Step 3: Brief pause for suspense
  delay(500);

  // Step 4: Reveal Pokemon with slide-in effect
  tft.fillScreen(ILI9341_BLACK);

  // Check if Pokemon data is loaded from SD card
  if (currentPokemon.loaded && currentPokemon.id == id)
  {

    // Show "Pokemon Found!" message
    tft.setTextColor(ILI9341_GREEN);
    tft.setTextSize(2);
    String foundText = "POKEMON FOUND!";
    int16_t foundX = (tft.width() - foundText.length() * 12) / 2;
    tft.setCursor(foundX, 20);
    tft.print(foundText);

    // Flash effect
    for (int i = 0; i < 2; i++)
    {
      delay(200);
      tft.fillRect(foundX, 20, foundText.length() * 12, 16, ILI9341_BLACK);
      delay(200);
      tft.setCursor(foundX, 20);
      tft.print(foundText);
    }

    delay(800);
  }

  // Step 5: Display Pokemon info with slide-in animation
  if (!displayPokemonInfoWithTransition(id))
  {
    Serial.println("Failed to display Pokemon info");
    return false;
  }

  // Step 6: Play enhanced sprite animation with particle effects - 使用150x150正方形
  playEnhancedPokemonAnimation(id, 45, 70, 150, 150, 3000);

  Serial.printf("Pokemon #%d page displayed successfully with enhanced experience\n", id);
  return true;
}

// Enhanced Pokemon info display with slide-in transition
bool displayPokemonInfoWithTransition(int id)
{
  Serial.printf("Displaying Pokemon info with transition for ID %d\n", id);

  if (!acquireDisplay("displayPokemonInfoWithTransition"))
  {
    return false;
  }

  // 檢查currentPokemon是否已載入且ID匹配
  if (!currentPokemon.loaded || currentPokemon.id != id)
  {
    Serial.printf("Pokemon ID %d not loaded from SD card (currentPokemon.loaded=%s, currentPokemon.id=%d)\n",
                  id, currentPokemon.loaded ? "true" : "false", currentPokemon.id);
    releaseDisplay("displayPokemonInfoWithTransition");
    return false;
  }

  // Clear screen with gradient effect
  tft.fillScreen(ILI9341_BLACK);

  // Slide-in effect for Pokemon ID and name
  int16_t slideDistance = 240;
  int16_t slideSteps = 20;
  int16_t stepSize = slideDistance / slideSteps;

  for (int step = 0; step < slideSteps; step++)
  {
    int16_t currentX = slideDistance - (step * stepSize);

    // Clear previous position
    if (step > 0)
    {
      tft.fillRect(0, 45, 240, 30, ILI9341_BLACK);
    }

    // Draw Pokemon ID
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);
    char idText[16];
    snprintf(idText, sizeof(idText), "#%d", currentPokemon.id);
    tft.setCursor(currentX, 50);
    tft.print(idText);

    delay(20);
    esp_task_wdt_reset();
  }

  // English name with typewriter effect
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_CYAN);
  int16_t nameX = 20;
  int16_t nameY = 80;

  for (int i = 0; i < currentPokemon.name_en.length(); i++)
  {
    tft.setCursor(nameX + i * 12, nameY);
    tft.print(currentPokemon.name_en.charAt(i));
    delay(60);
  }

  delay(200);

  // Chinese name display removed

  // Stats display (height and weight on same line)
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);

  // 格式化為單行顯示: "Height: XX.X m     Weight: XX.X kg"
  char statsLine[64];
  snprintf(statsLine, sizeof(statsLine), "Height: %.1fm     Weight: %.1fkg",
           currentPokemon.height / 10.0, currentPokemon.weight / 10.0);

  tft.setCursor(20, 250); // 精靈結束於Y=220，身高體重在Y=250 (30px間距)
  tft.print(statsLine);

  // Type badges with slide-in effect (調整間距)
  int badgeY = 275; // 身高體重在Y=250，屬性在Y=275 (25px間距)
  int badgeWidth = 60;
  int badgeHeight = 20;
  int badgeSpacing = 10;

  if (currentPokemon.type2.length() > 0)
  {
    // Two types with staggered animation
    int totalWidth = 2 * badgeWidth + badgeSpacing;
    int startX = (tft.width() - totalWidth) / 2;

    // First type
    uint16_t type1Color = getTypeColor(currentPokemon.type1.c_str());
    for (int w = 0; w <= badgeWidth; w += 5)
    {
      tft.fillRect(startX + badgeWidth - w, badgeY, w, badgeHeight, type1Color);
      delay(20);
    }
    drawTypeBadge(startX, badgeY, badgeWidth, badgeHeight, currentPokemon.type1.c_str(), type1Color);

    delay(200);

    // Second type
    uint16_t type2Color = getTypeColor(currentPokemon.type2.c_str());
    int type2X = startX + badgeWidth + badgeSpacing;
    for (int w = 0; w <= badgeWidth; w += 5)
    {
      tft.fillRect(type2X, badgeY, w, badgeHeight, type2Color);
      delay(20);
    }
    drawTypeBadge(type2X, badgeY, badgeWidth, badgeHeight, currentPokemon.type2.c_str(), type2Color);
  }
  else
  {
    // Single type
    int startX = (tft.width() - badgeWidth) / 2;
    uint16_t typeColor = getTypeColor(currentPokemon.type1.c_str());

    for (int w = 0; w <= badgeWidth; w += 5)
    {
      tft.fillRect(startX + badgeWidth - w, badgeY, w, badgeHeight, typeColor);
      delay(20);
    }
    drawTypeBadge(startX, badgeY, badgeWidth, badgeHeight, currentPokemon.type1.c_str(), typeColor);
  }

  releaseDisplay("displayPokemonInfoWithTransition");

  Serial.println("Pokemon info displayed with enhanced transitions");
  return true;
}

// 簡化的ESP-NOW初始化函數
bool initESPNOW_PowerOptimized()
{
  Serial.println("正在初始化ESP-NOW...");

  // 在螢幕上顯示初始化狀態
  tft.fillRect(0, 280, 240, 40, ILI9341_BLACK);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(1);
  tft.setCursor(10, 285);
  tft.println("Initializing ESP-NOW...");
  tft.setCursor(10, 300);
  tft.println("Please wait...");

  safeDelay(100);

  // 設定 Wi-Fi 模式
  Serial.println("Step 1: 設定WiFi模式...");
  WiFi.mode(WIFI_STA);
  safeDelay(100);

  // 設定頻道
  Serial.println("Step 2: 設定WiFi頻道...");
  WiFi.channel(WIFI_CHANNEL);
  safeDelay(100);

  // 初始化 ESP-NOW
  Serial.println("Step 3: 初始化ESP-NOW...");
  esp_err_t initResult = esp_now_init();
  safeDelay(100);

  if (initResult != ESP_OK)
  {
    Serial.println("ESP-NOW 初始化失敗");
    Serial.printf("錯誤代碼: 0x%x\n", initResult);

    // 顯示失敗狀態
    tft.fillRect(0, 280, 240, 40, ILI9341_BLACK);
    tft.setTextColor(ILI9341_RED);
    tft.setTextSize(1);
    tft.setCursor(10, 285);
    tft.println("ESP-NOW init FAILED");
    tft.setCursor(10, 300);
    tft.print("Error: 0x");
    tft.println(initResult, HEX);

    return false;
  }
  else
  {
    Serial.println("ESP-NOW 初始化成功！");

    // 註冊回調函數
    Serial.println("Step 4: 註冊回調函數...");
    esp_now_register_recv_cb(OnDataRecv);
    safeDelay(100);

    Serial.println("ESP-NOW 接收器已就緒");
  }

  return true;
}

void setup()
{
  delay(500); // 小延遲讓電源穩定
  Serial.begin(115200);

  Serial.println("=== 寶可夢圖鑑啟動 ===");

  // 初始化簡單的資源管理標誌
  displayBusy = false;
  pokemonDisplayRequested = false;

  Serial.println("Resource management initialized (SD-free)");

  // 初始化看門狗
  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
  Serial.println("Watchdog initialized");

  // 初始化TFT - 逐步進行以降低電流尖峰
  tft.begin();
  safeDelayWithMemCheck(50, "tft_init");
  tft.setRotation(0);
  safeDelayWithMemCheck(50, "tft_rotation");
  tft.fillScreen(ILI9341_BLACK);
  safeDelayWithMemCheck(50, "tft_clear");

  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH); // 開背光

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(0, 0);
  tft.println("MAC Address:");

  // 直接從芯片讀取 MAC，不啟動 Wi-Fi
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  tft.println(macStr);
  Serial.print("MAC: ");
  Serial.println(macStr);

  Serial.println("TFT initialized - MAC displayed");

  // Diagnostic mode: run isolation tests to identify SPI/CS issues
#define DIAG_MODE 0
#if DIAG_MODE
  // run diagnostics and stop
  runDiagnostics();
#endif

  // 無需SD卡初始化！使用嵌入式資料
  Serial.println("Using embedded Pokemon data - no SD card needed!");

  // 恢復背光
  digitalWrite(TFT_LED, HIGH);

  // 只設置TFT CS腳位，無需SD卡腳位
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);

  // 顯示Pokemon Ball歡迎畫面
  Serial.println("Displaying Pokemon Ball welcome screen...");
  displayPokemonBallWelcome();

  // 等一下再更新狀態，先讓用戶看到歡迎畫面
  safeDelayWithMemCheck(1000, "welcome_display");

  Serial.println("Embedded data test complete, enabling ESP-NOW...");
  safeDelayWithMemCheck(1000, "display_delay");

  // 啟用ESP-NOW (無SD卡衝突)
  Serial.println("啟用ESP-NOW (無SD卡衝突模式)...");
  if (initESPNOW_PowerOptimized())
  {
    espnowEnabled = true;
    Serial.println("ESP-NOW已成功啟用");

    // 更新螢幕狀態 - 在Pokemon Ball下方顯示就緒狀態
    tft.fillRect(0, 260, 240, 60, ILI9341_BLACK);
    tft.setTextColor(ILI9341_GREEN);
    tft.setTextSize(1);

    // Center the text
    String readyText = "ESP-NOW Ready!";
    int16_t readyX = (tft.width() - readyText.length() * 6) / 2;
    tft.setCursor(readyX, 270);
    tft.print(readyText);

    String scanText = "Scan NFC Pokemon Card";
    int16_t scanX = (tft.width() - scanText.length() * 6) / 2;
    tft.setCursor(scanX, 290);
    tft.print(scanText);
  }
  else
  {
    Serial.println("ESP-NOW啟用失敗");
    tft.fillRect(0, 260, 240, 60, ILI9341_BLACK);
    tft.setTextColor(ILI9341_RED);
    tft.setTextSize(1);

    String errorText = "ESP-NOW Init Failed";
    int16_t errorX = (tft.width() - errorText.length() * 6) / 2;
    tft.setCursor(errorX, 270);
    tft.print(errorText);

    String checkText = "Check Connections";
    int16_t checkX = (tft.width() - checkText.length() * 6) / 2;
    tft.setCursor(checkX, 290);
    tft.print(checkText);
  }
}

void loop()
{
  // 餵看門狗和系統健康檢查
  esp_task_wdt_reset();
  updateSystemHealth();

  // 如果系統不健康，跳過主要處理
  if (!systemHealthy)
  {
    delay(1000);
    return;
  }

  // 安全處理Pokemon顯示請求 - 新版動態載入系統
  if (currentState == LISTENING && pokemonDisplayRequested && !displayBusy)
  {
    pokemonDisplayRequested = false; // 重置標誌
    newDataReceived = false;         // 也重置這個標誌

    int pokemonId = requestedPokemonId;
    Serial.printf("Processing Pokemon display request for ID: %d (Dynamic Loading)\n", pokemonId);

    // 檢查記憶體狀態
    if (!checkMemoryAvailable("dynamic_load"))
    {
      Serial.println("Insufficient memory for dynamic loading");
      changeSystemState(ERROR_RECOVERY, "Insufficient memory");
      return;
    }

    // 使用新的動態載入系統
    if (!loadAndDisplayPokemon(pokemonId))
    {
      Serial.printf("Failed to load Pokemon #%d dynamically\n", pokemonId);
      changeSystemState(ERROR_RECOVERY, "Dynamic loading failed");
      return;
    }
  }

  // 處理錯誤恢復狀態
  else if (currentState == ERROR_RECOVERY)
  {
    performSystemRecovery();
    changeSystemState(LISTENING, "Recovery completed");
    displayPokemonBallWelcome();
    return;
  }

  // 處理舊版本的newDataReceived標誌（fallback）
  else if (newDataReceived && !pokemonDisplayRequested)
  {
    newDataReceived = false;
    Serial.println("Received data without display request flag - possible race condition");
  }

  // 簡單的狀態檢查命令 (除錯用)
  if (Serial.available() > 0)
  {
    String command = Serial.readString();
    command.trim();

    if (command == "status")
    {
      Serial.println("=== 系統狀態 ===");
      Serial.printf("ESP-NOW狀態: %s\n", espnowEnabled ? "已啟用" : "未啟用");
      Serial.printf("可用堆內存: %d bytes\n", ESP.getFreeHeap());
      if (espnowEnabled)
      {
        int8_t power;
        esp_wifi_get_max_tx_power(&power);
        Serial.printf("WiFi發送功率: %d (1/4 dBm)\n", power);
      }
    }
  }

  delay(100); // 短暫延遲，減少CPU使用率
}

// ---------------- Simplified Diagnostics (SD-Free) ----------------
void runDiagnostics()
{
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setCursor(0, 0);
  tft.println("DIAG: TFT Test");
  tft.setCursor(0, 40);
  tft.println("Line1: ASCII OK");
  tft.println("Line2: ASCII OK");
  tft.setCursor(0, 100);
  tft.println("SD Card: DISABLED");
  tft.setCursor(0, 140);
  tft.println("Using embedded data");

  Serial.println("--- SIMPLIFIED DIAG START ---");
  Serial.println("TFT display test completed");
  Serial.println("SD card diagnostics skipped (using embedded data)");
  Serial.println("System is using embedded Pokemon database");
  Serial.println("--- SIMPLIFIED DIAG END ---");

  // halt here to avoid repeating
  while (1)
    delay(1000);
}