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
#include <esp_wifi.h> // 用於WiFi功率控制
// TFT and graphics
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SPI.h>
// SD and JSON (enabled only after safe checks)
#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>
// Animated GIF support
#include <AnimatedGIF.h>

// 定義TFT引腳 (CS=GPIO5, DC=GPIO2, RST=GPIO4)
#define TFT_CS 5
#define TFT_DC 2
#define TFT_RST 4
#define TFT_LED 2 // 背光控制
#define SD_CS 14  // SD CS on GPIO14 (user wired)

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// ESP-NOW 相關設定
#define WIFI_CHANNEL 1 // 與發送端保持一致

// 定義接收的資料結構 (與發送端一致)
typedef struct struct_message
{
  int pokemon_id;
} struct_message;

struct_message receivedData;
volatile bool newDataReceived = false;
bool espnowEnabled = false; // 追蹤ESP-NOW狀態

// ESP-NOW 接收回調函數
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len)
{
  memcpy(&receivedData, incomingData, sizeof(receivedData));
  newDataReceived = true;

  Serial.print("接收到寶可夢 ID: ");
  Serial.println(receivedData.pokemon_id);
}

// AnimatedGIF instance
AnimatedGIF gif;

// Centering offsets computed after open()
int16_t g_xOffset = 0;
int16_t g_yOffset = 0;

// GIF canvas dimensions for clearing
int16_t g_canvasWidth = 0;
int16_t g_canvasHeight = 0;

// Scaling factor for fullscreen display
int16_t g_scaleX = 1;
int16_t g_scaleY = 1;

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

// SD-backed file handle wrapper used by AnimatedGIF callbacks
struct SdGifHandle
{
  File f;
};

// --- AnimatedGIF file callbacks for SD ---
void *GIFOpenFile(const char *fname, int32_t *pSize)
{
  SdGifHandle *h = new SdGifHandle();
  if (!h)
    return nullptr;
  h->f = SD.open(fname);
  if (!h->f)
  {
    delete h;
    return nullptr;
  }
  if (pSize)
    *pSize = (int32_t)h->f.size();
  return (void *)h;
}

void GIFCloseFile(void *pHandle)
{
  if (!pHandle)
    return;
  SdGifHandle *h = (SdGifHandle *)pHandle;
  if (h->f)
    h->f.close();
  delete h;
}

int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
  if (!pFile || !pFile->fHandle)
    return -1;
  SdGifHandle *h = (SdGifHandle *)pFile->fHandle;
  int32_t left = (int32_t)(h->f.size() - h->f.position());
  if (left <= 0)
    return 0;
  if (iLen > left)
    iLen = left;
  int32_t r = h->f.read(pBuf, iLen);
  pFile->iPos = h->f.position();
  return r;
}

int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition)
{
  if (!pFile || !pFile->fHandle)
    return -1;
  SdGifHandle *h = (SdGifHandle *)pFile->fHandle;
  if (!h->f.seek(iPosition))
    return -1;
  pFile->iPos = h->f.position();
  return pFile->iPos;
}

// GIFDraw callback: draw one scanline with scaling to fit in designated area
void GIFDraw(GIFDRAW *pDraw)
{
  if (!pDraw)
    return;

  // Clear only the GIF canvas area before drawing the first line of each frame
  if (pDraw->y == 0)
  {
    tft.fillRect(g_xOffset, g_yOffset, g_canvasWidth, g_canvasHeight, ILI9341_BLACK);
  }

  uint16_t *palette = pDraw->pPalette; // palette is already RGB565
  uint8_t *pixels = pDraw->pPixels;
  int16_t w = pDraw->iWidth;

  // Temporary buffer for scaled line
  static uint16_t lineBuf[480]; // Enough for scaling

  // Convert and scale the line
  for (int i = 0; i < w; ++i)
  {
    uint8_t idx = pixels[i];
    uint16_t color;

    if (pDraw->ucHasTransparency && idx == pDraw->ucTransparent)
    {
      color = ILI9341_BLACK; // Replace transparent with black
    }
    else
    {
      color = palette[idx];
    }

    // Replicate pixel horizontally for scaling
    for (int sx = 0; sx < g_scaleX; ++sx)
    {
      lineBuf[i * g_scaleX + sx] = color;
    }
  }

  // Draw the scaled line multiple times for vertical scaling
  int16_t scaledY = (pDraw->iY + pDraw->y) * g_scaleY + g_yOffset;
  int16_t scaledX = pDraw->iX * g_scaleX + g_xOffset;
  int16_t scaledW = w * g_scaleX;

  tft.startWrite();
  for (int sy = 0; sy < g_scaleY; ++sy)
  {
    int16_t drawY = scaledY + sy;
    if (drawY >= 0 && drawY < tft.height())
    {
      tft.setAddrWindow(scaledX, drawY, scaledW, 1);
      tft.writePixels(lineBuf, scaledW);
    }
  }
  tft.endWrite();
} // Helper: play a pokemon gif by id in a specific area
void displayPokemonGIF(int id, int16_t areaY, int16_t areaHeight)
{
  char path[64];
  snprintf(path, sizeof(path), "/pokemon/%d.gif", id);

  gif.begin(LITTLE_ENDIAN_PIXELS);

  if (!gif.open(path, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw))
  {
    Serial.printf("gif.open failed for %s error=%d\n", path, gif.getLastError());
    return;
  }

  // Get original GIF dimensions
  int16_t origWidth = gif.getCanvasWidth();
  int16_t origHeight = gif.getCanvasHeight();

  // Calculate scaling factors to fit in the designated area
  int16_t screenWidth = tft.width();   // 240 for ILI9341
  int16_t maxWidth = screenWidth - 10; // Reduce margin from 20 to 10 for larger GIF
  int16_t maxHeight = areaHeight - 10; // Reduce margin from 20 to 10 for larger GIF

  // Calculate scale to fit within area
  int16_t scaleByWidth = maxWidth / origWidth;
  int16_t scaleByHeight = maxHeight / origHeight;
  int16_t scale = (int16_t)max(1, (int)min(scaleByWidth, scaleByHeight));

  g_scaleX = scale;
  g_scaleY = scale;

  // Calculate scaled dimensions and centering offsets within the area
  g_canvasWidth = origWidth * g_scaleX;
  g_canvasHeight = origHeight * g_scaleY;
  g_xOffset = (screenWidth - g_canvasWidth) / 2;
  g_yOffset = areaY + (areaHeight - g_canvasHeight) / 2;

  Serial.printf("GIF in area Y:%d H:%d, Original: %dx%d, Scale: %d, Scaled: %dx%d, Offset: (%d,%d)\n",
                areaY, areaHeight, origWidth, origHeight, scale,
                g_canvasWidth, g_canvasHeight, g_xOffset, g_yOffset);

  // Play frames; gif.playFrame(true,NULL) respects delays
  while (gif.playFrame(true, NULL) > 0)
  {
    yield();
  }
  gif.close();

  safeDelay(200);
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

// Modified function to display Pokemon info with layout
void displayPokemonInfo(int id)
{
  char path[64];
  snprintf(path, sizeof(path), "/pokemon/%d.json", id);

  File jsonFile = SD.open(path);
  if (!jsonFile)
  {
    Serial.printf("Failed to open %s\n", path);
    return;
  }

  // Read file content
  String jsonContent = "";
  while (jsonFile.available())
  {
    jsonContent += (char)jsonFile.read();
  }
  jsonFile.close();

  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonContent);

  if (error)
  {
    Serial.print("JSON parse failed: ");
    Serial.println(error.c_str());
    return;
  }

  // Clear screen
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);

  // Display Pokemon ID (centered at top, move down)
  tft.setTextSize(2);
  String idText = "#" + String(doc["id"].as<int>());
  int16_t idX = (tft.width() - idText.length() * 12) / 2;
  tft.setCursor(idX, 20); // Move down from 5 to 20
  tft.println(idText);

  // Display Pokemon name (centered) - use English to avoid encoding issues
  String pokeName = doc["names"]["en"].as<String>();
  int16_t nameX = (tft.width() - pokeName.length() * 12) / 2;
  tft.setCursor(nameX, 40); // Move down from 25 to 40
  tft.println(pokeName);

  // Reserve space for GIF (centered area)
  // GIF will be displayed at approximately y=65 to y=200

  // Display height and weight (below GIF area, centered)
  tft.setTextSize(1); // Change back to size 1 (smaller than name which is size 2)

  // Height (left side, centered in its half)
  float height = doc["height"].as<float>() / 10.0; // Convert decimeters to meters
  String heightText = "Height:";
  String heightValue = String(height, 1) + " m";

  // Adjust positioning for smaller text (text size 1 = 6x8 pixels per character)
  int16_t heightLabelX = (tft.width() / 2 - heightText.length() * 6) / 2;
  int16_t heightValueX = (tft.width() / 2 - heightValue.length() * 6) / 2;

  tft.setCursor(heightLabelX, 210); // Move down from 205 to 210
  tft.print(heightText);
  tft.setCursor(heightValueX, 225); // Move down from 220 to 225
  tft.print(heightValue);

  // Weight (right side, centered in its half)
  float weight = doc["weight"].as<float>() / 10.0; // Convert hectograms to kg
  String weightText = "Weight:";
  String weightValue = String(weight, 1) + " kg";

  int16_t weightLabelX = tft.width() / 2 + (tft.width() / 2 - weightText.length() * 6) / 2;
  int16_t weightValueX = tft.width() / 2 + (tft.width() / 2 - weightValue.length() * 6) / 2;

  tft.setCursor(weightLabelX, 210); // Move down from 205 to 210
  tft.print(weightText);
  tft.setCursor(weightValueX, 225); // Move down from 220 to 225
  tft.print(weightValue);

  // Display type badges
  JsonArray types = doc["types"];
  int typeCount = types.size();

  if (typeCount > 0)
  {
    int badgeWidth = 70;  // Reduce width back to original size
    int badgeHeight = 20; // Reduce height back to original size
    int badgeSpacing = 10;
    int totalWidth = typeCount * badgeWidth + (typeCount - 1) * badgeSpacing;
    int startX = (tft.width() - totalWidth) / 2;
    int badgeY = 250; // Move down from 245 to 250

    for (int i = 0; i < typeCount && i < 2; i++)
    { // Max 2 types
      String typeName = types[i].as<String>();
      uint16_t typeColor = getTypeColor(typeName.c_str());
      int badgeX = startX + i * (badgeWidth + badgeSpacing);

      drawTypeBadge(badgeX, badgeY, badgeWidth, badgeHeight, typeName.c_str(), typeColor);
    }
  }

  Serial.println("Pokemon info layout displayed successfully");
}

// Display complete Pokemon page with info and animated GIF
void displayPokemonPage(int id)
{
  // First display the static information layout
  displayPokemonInfo(id);

  // Then display the animated GIF in the designated area (larger area)
  // GIF area: Y=65 to Y=200 (height=135, increased from 130)
  displayPokemonGIF(id, 65, 135);
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

  safeDelay(500);

  // 設定 Wi-Fi 模式
  Serial.println("Step 1: 設定WiFi模式...");
  WiFi.mode(WIFI_STA);
  safeDelay(500);

  // 設定頻道
  Serial.println("Step 2: 設定WiFi頻道...");
  WiFi.channel(WIFI_CHANNEL);
  safeDelay(300);

  // 初始化 ESP-NOW
  Serial.println("Step 3: 初始化ESP-NOW...");
  esp_err_t initResult = esp_now_init();
  safeDelay(300);

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
    safeDelay(200);

    Serial.println("ESP-NOW 接收器已就緒");
  }

  return true;
}

void setup()
{
  delay(500); // 小延遲讓電源穩定
  Serial.begin(115200);

  // 初始化TFT - 逐步進行以降低電流尖峰
  tft.begin();
  safeDelay(50);
  tft.setRotation(0);
  safeDelay(50);
  tft.fillScreen(ILI9341_BLACK);
  safeDelay(50);

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

  // --- SD init: shared VSPI pins (SCK=18, MISO=19, MOSI=23), SD_CS=14 ---
  // This code assumes TFT and SD share SPI lines (you confirmed this wiring).
  safeDelay(150);

  // Basic diagnostics before SD init
  Serial.print("Digital read SD_CS before init: ");
  // ensure CS pins are in defined states
  pinMode(SD_CS, INPUT_PULLUP);
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH); // release TFT bus (CS high = inactive for many modules)
  Serial.println(digitalRead(SD_CS));

  // Turn down backlight to reduce current during SD init
  digitalWrite(TFT_LED, LOW);
  safeDelay(50);

  SPIClass spi = SPIClass(VSPI);
  spi.begin(18, 19, 23, SD_CS); // SCK, MISO, MOSI, SS

  if (!SD.begin(SD_CS, spi))
  {
    Serial.println("SD.init failed (shared SPI) - will not use SD");
    // restore backlight
    digitalWrite(TFT_LED, HIGH);
    tft.setCursor(0, 60);
    tft.setTextSize(1);
    tft.println("SD init failed");
  }
  else
  {
    Serial.println("SD initialized OK");
    digitalWrite(TFT_LED, HIGH);

    // Check if /pokemon directory exists
    if (!SD.exists("/pokemon"))
    {
      Serial.println("/pokemon directory not found");
      tft.setCursor(0, 60);
      tft.setTextSize(1);
      tft.println("/pokemon not found");
    }
    else
    {
      Serial.println("/pokemon directory exists");
      // Check if 6.json exists
      if (!SD.exists("/pokemon/6.json"))
      {
        Serial.println("6.json not found");
        tft.setCursor(0, 60);
        tft.setTextSize(1);
        tft.println("6.json not found");
      }
      else
      {
        Serial.println("6.json exists");

        // Display complete Pokemon page with info and GIF
        displayPokemonPage(6);

        // 顯示NFC感應提示
        tft.setTextColor(ILI9341_CYAN);
        tft.setTextSize(1);
        tft.setCursor(10, 285);
        tft.println("Ready for NFC scanning");
        tft.setCursor(10, 300);
        tft.println("Initializing ESP-NOW...");

        Serial.println("預設畫面顯示完成，啟用ESP-NOW...");
        safeDelay(1000); // 等待1秒讓使用者看到訊息

        // 直接啟用ESP-NOW (供電問題已解決)
        Serial.println("啟用ESP-NOW (USB供電模式)...");
        if (initESPNOW_PowerOptimized())
        {
          espnowEnabled = true;
          Serial.println("ESP-NOW已成功啟用");

          // 更新螢幕顯示就緒狀態
          tft.fillRect(0, 280, 240, 40, ILI9341_BLACK);
          tft.setTextColor(ILI9341_GREEN);
          tft.setTextSize(1);
          tft.setCursor(10, 285);
          tft.println("ESP-NOW ready!");
          tft.setCursor(10, 300);
          tft.println("NFC scanning enabled");
        }
        else
        {
          Serial.println("ESP-NOW啟用失敗");
          tft.fillRect(0, 280, 240, 40, ILI9341_BLACK);
          tft.setTextColor(ILI9341_RED);
          tft.setTextSize(1);
          tft.setCursor(10, 285);
          tft.println("ESP-NOW init failed");
          tft.setCursor(10, 300);
          tft.println("Check connections");
        }
      }
    }
  }
}

void loop()
{
  // 檢查是否接收到新的寶可夢數據
  if (newDataReceived)
  {
    newDataReceived = false; // 重置標誌

    int pokemonId = receivedData.pokemon_id;
    Serial.printf("接收到寶可夢 ID: %d\n", pokemonId);
    // 測試階段：只在螢幕下方顯示接收到的編號
    tft.fillRect(0, 280, 240, 40, ILI9341_BLACK); // 清除底部區域
    tft.setTextColor(ILI9341_YELLOW);
    tft.setTextSize(1);
    tft.setCursor(10, 285);
    tft.println("Received Pokemon ID:");
    tft.setCursor(10, 300);
    tft.print("ID: ");
    tft.print(pokemonId);
    tft.print(" (Test mode)");

    // 3秒後恢復就緒狀態
    delay(3000);
    tft.fillRect(0, 280, 240, 40, ILI9341_BLACK);
    tft.setTextColor(ILI9341_GREEN);
    tft.setTextSize(1);
    tft.setCursor(10, 285);
    tft.println("ESP-NOW ready!");
    tft.setCursor(10, 300);
    tft.println("NFC scanning enabled");
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

// ---------------- Diagnostics ----------------
void runDiagnostics()
{
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setCursor(0, 0);
  tft.println("DIAG: Display test");
  tft.setCursor(0, 40);
  tft.println("Line1: ASCII OK");
  tft.println("Line2: ASCII OK");

  Serial.println("--- DIAG START ---");
  safeDelay(200);

  // Test A: ensure SD_CS is HIGH (inactive)
  pinMode(SD_CS, INPUT_PULLUP);
  int csState = digitalRead(SD_CS);
  Serial.print("SD_CS (pullup read) = ");
  Serial.println(csState);

  // Test B: check MISO tri-state by driving CS low then high and reading MISO
  pinMode(19, INPUT); // MISO
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  safeDelay(50);
  int misoHigh = digitalRead(19);
  Serial.print("MISO when SD_CS HIGH: ");
  Serial.println(misoHigh);
  digitalWrite(SD_CS, LOW);
  safeDelay(50);
  int misoLow = digitalRead(19);
  Serial.print("MISO when SD_CS LOW: ");
  Serial.println(misoLow);
  digitalWrite(SD_CS, HIGH);

  // Test C: toggle SD_CS while writing to TFT to see corruption
  Serial.println("Toggling SD_CS while drawing text (watch TFT)");
  for (int i = 0; i < 6; ++i)
  {
    tft.fillRect(0, 80, 240, 20, ILI9341_BLACK);
    tft.setCursor(0, 80);
    tft.println("TEST LINE: ");
    safeDelay(20);
    digitalWrite(SD_CS, LOW);
    safeDelay(20);
    digitalWrite(SD_CS, HIGH);
    safeDelay(20);
  }

  Serial.println("--- DIAG END ---");
  // halt here to avoid repeating
  while (1)
    delay(1000);
}
