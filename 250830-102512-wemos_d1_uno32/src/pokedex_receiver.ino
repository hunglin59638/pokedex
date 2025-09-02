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
#include <esp_system.h> // 用於系統監控
#include <esp_task_wdt.h> // 用於看門狗
// TFT and graphics
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SPI.h>
// Embedded data storage (no SD card needed)
#include <Arduino.h>
// No more SD card or JSON dependencies!

// 定義TFT引腳 (CS=GPIO5, DC=GPIO2, RST=GPIO4)
#define TFT_CS 5
#define TFT_DC 2
#define TFT_RST 4
#define TFT_LED 2 // 背光控制

// 嵌入式Pokemon資料結構
struct PokemonData {
  int id;
  const char* name_en;
  const char* name_zh;
  const char* type1;
  const char* type2; // NULL if single type
  int height;  // in decimeters
  int weight;  // in hectograms
};

// 嵌入在程式記憶體中的Pokemon資料 (PROGMEM)
const PokemonData PROGMEM pokemon_database[] = {
  {1, "Bulbasaur", "妙蛙種子", "grass", "poison", 7, 69},
  {4, "Charmander", "小火龍", "fire", NULL, 6, 85},
  {6, "Charizard", "噴火龍", "fire", "flying", 17, 905},
  {7, "Squirtle", "傑尼龜", "water", NULL, 5, 90},
  {25, "Pikachu", "皮卡丘", "electric", NULL, 4, 60},
  {94, "Gengar", "耿鬼", "ghost", "poison", 15, 405},
  {150, "Mewtwo", "超夢", "psychic", NULL, 20, 1220},
  {151, "Mew", "夢幻", "psychic", NULL, 4, 40}
};

const int POKEMON_DATABASE_SIZE = sizeof(pokemon_database) / sizeof(PokemonData);

// 簡化的動畫系統 - 使用精靈動畫替代GIF
struct PokemonSprite {
  int id;
  uint16_t color1; // 主要顏色
  uint16_t color2; // 次要顏色
  uint16_t color3; // 細節顏色
};

// 嵌入式精靈資料 (簡化的Pokemon顏色)
const PokemonSprite PROGMEM pokemon_sprites[] = {
  {1, 0x07E0, 0x8010, ILI9341_BLACK},   // Bulbasaur: Green, Purple, Black
  {4, ILI9341_RED, ILI9341_ORANGE, ILI9341_BLACK}, // Charmander: Red, Orange, Black
  {6, ILI9341_RED, ILI9341_ORANGE, ILI9341_BLUE},  // Charizard: Red, Orange, Blue
  {7, 0x047F, 0x07FF, ILI9341_BLACK},   // Squirtle: Blue, Cyan, Black  
  {25, ILI9341_YELLOW, ILI9341_RED, ILI9341_BLACK}, // Pikachu: Yellow, Red, Black
  {94, 0x4210, 0x8010, ILI9341_RED},    // Gengar: Dark Purple, Purple, Red
  {150, 0x7BEF, 0x8010, ILI9341_WHITE}, // Mewtwo: Gray, Purple, White
  {151, 0xFBDF, 0xFFE0, ILI9341_BLUE}   // Mew: Pink, Yellow, Blue
};

const int POKEMON_SPRITES_SIZE = sizeof(pokemon_sprites) / sizeof(PokemonSprite);

// 查找Pokemon資料的函數
const PokemonData* findPokemonData(int id) {
  for (int i = 0; i < POKEMON_DATABASE_SIZE; i++) {
    PokemonData data;
    memcpy_P(&data, &pokemon_database[i], sizeof(PokemonData));
    if (data.id == id) {
      return &pokemon_database[i];
    }
  }
  return NULL; // 找不到
}

// 查找精靈顏色資料
const PokemonSprite* findPokemonSprite(int id) {
  for (int i = 0; i < POKEMON_SPRITES_SIZE; i++) {
    PokemonSprite sprite;
    memcpy_P(&sprite, &pokemon_sprites[i], sizeof(PokemonSprite));
    if (sprite.id == id) {
      return &pokemon_sprites[i];
    }
  }
  return NULL;
}

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

// 系統安全和資源管理 - 極簡版本（無SD卡）
volatile bool displayBusy = false;
volatile bool pokemonDisplayRequested = false;
volatile int requestedPokemonId = 0;

// 無需SPI資源管理，只有TFT使用SPI

// 記憶體管理和系統監控
#define MIN_FREE_HEAP 50000  // 最小可用記憶體閾值
#define CRITICAL_HEAP 30000  // 危險記憶體閾值
#define WDT_TIMEOUT 30       // 看門狗超時時間(秒)

// 系統穩定性監控
volatile unsigned long lastHeartbeat = 0;
volatile bool systemHealthy = true;
volatile int consecutiveErrors = 0;
#define MAX_CONSECUTIVE_ERRORS 3

// 記憶體監控函數
bool checkMemoryAvailable(const char* operation) {
  size_t freeHeap = ESP.getFreeHeap();
  size_t minHeap = ESP.getMinFreeHeap();
  
  Serial.printf("Memory check for %s: %d bytes free (min: %d)\n", 
                operation, freeHeap, minHeap);
  
  if (freeHeap < CRITICAL_HEAP) {
    Serial.printf("CRITICAL: Memory too low for %s operation!\n", operation);
    return false;
  }
  
  if (freeHeap < MIN_FREE_HEAP) {
    Serial.printf("WARNING: Low memory for %s operation\n", operation);
    // 執行垃圾回收
    ESP.getMinFreeHeap(); // Reset min heap counter
  }
  
  return true;
}

// 系統健康檢查
void updateSystemHealth() {
  lastHeartbeat = millis();
  
  // 檢查記憶體狀態
  size_t freeHeap = ESP.getFreeHeap();
  
  if (freeHeap < CRITICAL_HEAP) {
    consecutiveErrors++;
    systemHealthy = false;
    Serial.printf("System health degraded: heap=%d, errors=%d\n", freeHeap, consecutiveErrors);
  } else {
    if (consecutiveErrors > 0) {
      consecutiveErrors--;
    }
    if (consecutiveErrors == 0) {
      systemHealthy = true;
    }
  }
  
  // 如果連續錯誤過多，嘗試系統恢復
  if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
    Serial.println("CRITICAL: Too many consecutive errors, initiating recovery");
    performSystemRecovery();
  }
}

// 系統恢復程序 - 簡化版本（無SD卡）
void performSystemRecovery() {
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

// 安全延遲函數，包含系統監控
void safeDelayWithMemCheck(unsigned long ms, const char* context = "delay") {
  unsigned long start = millis();
  while (millis() - start < ms) {
    yield();
    
    // 餵看門狗
    esp_task_wdt_reset();
    
    delay(10);
    
    // 每500ms檢查一次系統狀態
    if ((millis() - start) % 500 == 0) {
      updateSystemHealth();
      
      if (!systemHealthy) {
        Serial.printf("System unhealthy during %s\n", context);
        break;
      }
    }
  }
}

// 安全的ESP-NOW接收回調函數 - 僅設置標誌，不進行任何重操作
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len)
{
  // 在中斷上下文中只進行最基本的操作
  if (len == sizeof(struct_message) && !displayBusy) {
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
void fillEllipse(int16_t centerX, int16_t centerY, int16_t width, int16_t height, uint16_t color) {
  int16_t radiusX = width / 2;
  int16_t radiusY = height / 2;
  
  // Draw ellipse by drawing multiple circles with varying radii
  for (int16_t y = -radiusY; y <= radiusY; y++) {
    int16_t x = (int16_t)(radiusX * sqrt(1.0 - (float)(y * y) / (radiusY * radiusY)));
    if (x > 0) {
      tft.drawFastHLine(centerX - x, centerY + y, 2 * x, color);
    }
  }
}

// Helper function to draw an ellipse outline using circles
void drawEllipse(int16_t centerX, int16_t centerY, int16_t width, int16_t height, uint16_t color) {
  int16_t radiusX = width / 2;
  int16_t radiusY = height / 2;
  
  // Draw ellipse outline by drawing points at calculated positions
  for (int angle = 0; angle < 360; angle += 5) {
    float rad = angle * PI / 180.0;
    int16_t x = centerX + (int16_t)(radiusX * cos(rad));
    int16_t y = centerY + (int16_t)(radiusY * sin(rad));
    tft.drawPixel(x, y, color);
  }
}

void drawSimplePokemonSprite(int16_t x, int16_t y, int16_t size, uint16_t color1, uint16_t color2, uint16_t color3, int frame)
{
  // 簡單的精靈動畫 - 3幀動畫效果
  int16_t offset = (frame % 3 - 1) * 2; // -2, 0, +2 像素偏移
  
  // 主體 (橢圓形)
  int16_t bodyWidth = size;
  int16_t bodyHeight = size * 0.8;
  
  fillEllipse(x, y + offset, bodyWidth, bodyHeight, color1);
  
  // 陰影/邊框
  drawEllipse(x, y + offset, bodyWidth, bodyHeight, ILI9341_BLACK);
  drawEllipse(x, y + offset, bodyWidth - 1, bodyHeight - 1, color2);
  
  // 眼睛 (簡單的點)
  int16_t eyeY = y - bodyHeight / 4 + offset;
  tft.fillCircle(x - bodyWidth / 4, eyeY, 3, color3);
  tft.fillCircle(x + bodyWidth / 4, eyeY, 3, color3);
  
  // 眼睛高光
  tft.fillCircle(x - bodyWidth / 4, eyeY - 1, 1, ILI9341_WHITE);
  tft.fillCircle(x + bodyWidth / 4, eyeY - 1, 1, ILI9341_WHITE);
  
  // 嘴巴 (小弧線)
  int16_t mouthY = y + bodyHeight / 6 + offset;
  for (int i = -3; i <= 3; i++) {
    if (abs(i) <= 2) { // 創建弧形
      tft.drawPixel(x + i, mouthY + abs(i) / 2, color3);
    }
  }
}

// 播放Pokemon精靈動畫
void playPokemonSpriteAnimation(int id, int16_t areaX, int16_t areaY, int16_t areaWidth, int16_t areaHeight, int duration_ms = 3000)
{
  const PokemonSprite* sprite = findPokemonSprite(id);
  if (!sprite) {
    Serial.printf("No sprite data for Pokemon ID %d\n", id);
    return;
  }
  
  // 從PROGMEM讀取精靈資料
  PokemonSprite spriteData;
  memcpy_P(&spriteData, sprite, sizeof(PokemonSprite));
  
  Serial.printf("Playing sprite animation for Pokemon #%d\n", id);
  
  // 計算精靈大小和位置
  int16_t spriteSize = min(areaWidth, areaHeight) / 2;
  int16_t spriteX = areaX + areaWidth / 2;
  int16_t spriteY = areaY + areaHeight / 2;
  
  unsigned long startTime = millis();
  int frame = 0;
  
  while (millis() - startTime < duration_ms) {
    // 清除動畫區域
    tft.fillRect(areaX, areaY, areaWidth, areaHeight, ILI9341_BLACK);
    
    // 繪製精靈動畫幀
    drawSimplePokemonSprite(spriteX, spriteY, spriteSize, 
                           spriteData.color1, spriteData.color2, spriteData.color3, frame);
    
    // 更新幀計數器
    frame++;
    
    // 控制動畫速度 (約10 FPS)
    delay(100);
    
    // 餵看門狗
    esp_task_wdt_reset();
  }
  
  Serial.printf("Sprite animation completed for Pokemon #%d\n", id);
}

// Enhanced Pokemon animation with particle effects and sparkles
void playEnhancedPokemonAnimation(int id, int16_t areaX, int16_t areaY, int16_t areaWidth, int16_t areaHeight, int duration_ms = 3000)
{
  const PokemonSprite* sprite = findPokemonSprite(id);
  if (!sprite) {
    Serial.printf("No sprite data for Pokemon ID %d, using basic animation\n", id);
    playPokemonSpriteAnimation(id, areaX, areaY, areaWidth, areaHeight, duration_ms);
    return;
  }
  
  // 從PROGMEM讀取精靈資料
  PokemonSprite spriteData;
  memcpy_P(&spriteData, sprite, sizeof(PokemonSprite));
  
  Serial.printf("Playing enhanced animation for Pokemon #%d\n", id);
  
  // 計算精靈大小和位置
  int16_t spriteSize = min(areaWidth, areaHeight) / 2;
  int16_t spriteX = areaX + areaWidth / 2;
  int16_t spriteY = areaY + areaHeight / 2;
  
  // Particle system for sparkle effects
  struct Particle {
    int16_t x, y;
    int16_t vx, vy;
    uint16_t color;
    int life;
  };
  
  const int MAX_PARTICLES = 8;
  Particle particles[MAX_PARTICLES];
  
  // Initialize particles
  for (int i = 0; i < MAX_PARTICLES; i++) {
    particles[i].life = 0;
  }
  
  unsigned long startTime = millis();
  int frame = 0;
  
  while (millis() - startTime < duration_ms) {
    // 清除動畫區域
    tft.fillRect(areaX, areaY, areaWidth, areaHeight, ILI9341_BLACK);
    
    // Add sparkle particles periodically
    if (frame % 8 == 0) {
      for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life <= 0) {
          particles[i].x = spriteX + random(-spriteSize, spriteSize);
          particles[i].y = spriteY + random(-spriteSize, spriteSize);
          particles[i].vx = random(-3, 3);
          particles[i].vy = random(-3, 3);
          particles[i].color = (random(0, 3) == 0) ? ILI9341_YELLOW : 
                              (random(0, 2) == 0) ? ILI9341_CYAN : ILI9341_WHITE;
          particles[i].life = 20 + random(0, 20);
          break;
        }
      }
    }
    
    // Update and draw particles
    for (int i = 0; i < MAX_PARTICLES; i++) {
      if (particles[i].life > 0) {
        particles[i].x += particles[i].vx;
        particles[i].y += particles[i].vy;
        particles[i].life--;
        
        // Fade effect
        if (particles[i].life > 10) {
          tft.drawPixel(particles[i].x, particles[i].y, particles[i].color);
          tft.drawPixel(particles[i].x + 1, particles[i].y, particles[i].color);
          tft.drawPixel(particles[i].x, particles[i].y + 1, particles[i].color);
        } else if (particles[i].life > 5) {
          tft.drawPixel(particles[i].x, particles[i].y, particles[i].color);
        }
      }
    }
    
    // 繪製精靈動畫幀 with enhanced effects
    drawSimplePokemonSprite(spriteX, spriteY, spriteSize, 
                           spriteData.color1, spriteData.color2, spriteData.color3, frame);
    
    // Add glow effect around sprite every few frames
    if (frame % 15 == 0) {
      for (int r = spriteSize + 5; r < spriteSize + 15; r += 2) {
        tft.drawCircle(spriteX, spriteY, r, spriteData.color1);
        delay(30);
        tft.drawCircle(spriteX, spriteY, r, ILI9341_BLACK);
      }
    }
    
    // 更新幀計數器
    frame++;
    
    // 控制動畫速度 (約12 FPS for smoother effects)
    delay(80);
    
    // 餵看門狗
    esp_task_wdt_reset();
  }
  
  // Final sparkle burst
  for (int burst = 0; burst < 20; burst++) {
    int16_t sparkleX = spriteX + random(-spriteSize * 2, spriteSize * 2);
    int16_t sparkleY = spriteY + random(-spriteSize * 2, spriteSize * 2);
    uint16_t sparkleColor = (random(0, 3) == 0) ? ILI9341_YELLOW : 
                           (random(0, 2) == 0) ? ILI9341_CYAN : ILI9341_WHITE;
    
    tft.fillCircle(sparkleX, sparkleY, 2, sparkleColor);
    delay(50);
    tft.fillCircle(sparkleX, sparkleY, 2, ILI9341_BLACK);
  }
  
  Serial.printf("Enhanced animation completed for Pokemon #%d\n", id);
}

// 簡化的顯示器管理 - 無需SPI競爭（只有TFT）
bool acquireDisplay(const char* requester) {
  if (displayBusy) {
    Serial.printf("Display busy, waiting for %s\n", requester);
    int attempts = 0;
    while (displayBusy && attempts < 50) {
      delay(10);
      attempts++;
      esp_task_wdt_reset(); // 餵看門狗
    }
    
    if (displayBusy) {
      Serial.printf("Display timeout for %s, forcing release\n", requester);
      displayBusy = false; // 強制釋放
    }
  }
  
  displayBusy = true;
  Serial.printf("Display acquired by %s\n", requester);
  return true;
}

void releaseDisplay(const char* requester) {
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
  for (int16_t y = centerY - radius + 2; y < centerY - 2; y++) {
    int16_t halfWidth = (int16_t)sqrt(radius * radius - (y - centerY) * (y - centerY)) - 2;
    if (halfWidth > 0) {
      tft.drawFastHLine(centerX - halfWidth, y, halfWidth * 2, redColor);
    }
  }
  
  // Draw lower half (white) 
  for (int16_t y = centerY + 3; y < centerY + radius - 2; y++) {
    int16_t halfWidth = (int16_t)sqrt(radius * radius - (y - centerY) * (y - centerY)) - 2;
    if (halfWidth > 0) {
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
  for (int i = 0; i < steps; i++) {
    // Create a semi-transparent overlay effect by drawing with different patterns
    uint16_t fadeColor = ILI9341_BLACK;
    
    // Draw diagonal lines for fade effect
    for (int y = i; y < 320; y += steps) {
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
  
  for (int radius = 60; radius > 10; radius -= 5) {
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
  for (int i = 0; i < title.length(); i++) {
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
  for (int radius = 5; radius <= ballRadius; radius += 3) {
    drawPokemonBall(ballCenterX, ballCenterY, radius);
    delay(30);
    esp_task_wdt_reset();
  }
  
  // Subtitle with typewriter effect
  tft.setTextSize(1);
  int16_t subtitleY = 210;
  String subtitle = "NFC Pokemon Scanner";
  int16_t subtitleX = (tft.width() - subtitle.length() * 6) / 2;
  
  for (int i = 0; i < subtitle.length(); i++) {
    tft.setCursor(subtitleX + i * 6, subtitleY);
    tft.print(subtitle.charAt(i));
    delay(80);
  }
  
  // System info with color animation
  tft.setTextSize(1);
  tft.setCursor(10, 240);
  
  // Cycle through colors for system info
  uint16_t colors[] = {ILI9341_CYAN, ILI9341_GREEN, ILI9341_YELLOW};
  for (int c = 0; c < 3; c++) {
    tft.fillRect(10, 240, 220, 16, ILI9341_BLACK);
    tft.setTextColor(colors[c]);
    tft.setCursor(10, 240);
    tft.print("Embedded Data: ");
    tft.print(POKEMON_DATABASE_SIZE);
    tft.print(" Pokemon Ready!");
    delay(300);
  }
  
  // Final pulsing effect on Pokemon Ball
  for (int pulse = 0; pulse < 3; pulse++) {
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

// 全新的無SD卡Pokemon資訊顯示函數 - 使用嵌入式資料
bool displayPokemonInfo(int id)
{
  Serial.printf("Displaying Pokemon info for ID %d (embedded data)\n", id);
  
  if (!checkMemoryAvailable("displayPokemonInfo")) {
    Serial.println("Memory check failed for displayPokemonInfo");
    return false;
  }
  
  if (!acquireDisplay("displayPokemonInfo")) {
    Serial.println("Failed to acquire display for displayPokemonInfo");
    return false;
  }

  // 從嵌入式資料庫查找Pokemon
  const PokemonData* pokemon = findPokemonData(id);
  if (!pokemon) {
    Serial.printf("Pokemon ID %d not found in embedded database\n", id);
    releaseDisplay("displayPokemonInfo");
    return false;
  }

  // 從PROGMEM讀取資料
  PokemonData data;
  memcpy_P(&data, pokemon, sizeof(PokemonData));
  
  Serial.printf("Found Pokemon: %s (ID: %d)\n", data.name_en, data.id);

  // 無需SPI競爭 - 只有TFT使用SPI
  Serial.println("Starting TFT display (no SD conflicts!)");

  // Clear screen
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);

  // Display Pokemon ID (centered at top)
  tft.setTextSize(2);
  char idText[16];
  snprintf(idText, sizeof(idText), "#%d", data.id);
  
  int16_t idX = (tft.width() - strlen(idText) * 12) / 2;
  tft.setCursor(idX, 20);
  tft.print(idText);

  // Display Pokemon name (English)
  int16_t nameX = (tft.width() - strlen(data.name_en) * 12) / 2;
  tft.setCursor(nameX, 40);
  tft.print(data.name_en);

  // Display Chinese name (smaller)
  tft.setTextSize(1);
  int16_t nameZhX = (tft.width() - strlen(data.name_zh) * 6) / 2;
  tft.setCursor(nameZhX, 65);
  tft.print(data.name_zh);

  // Display height and weight
  tft.setTextSize(1);

  // Height (left side)
  float height = data.height / 10.0; // Convert decimeters to meters
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
  float weight = data.weight / 10.0; // Convert hectograms to kg
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

  if (data.type2) {
    // Two types
    int totalWidth = 2 * badgeWidth + badgeSpacing;
    int startX = (tft.width() - totalWidth) / 2;
    
    uint16_t type1Color = getTypeColor(data.type1);
    uint16_t type2Color = getTypeColor(data.type2);
    
    drawTypeBadge(startX, badgeY, badgeWidth, badgeHeight, data.type1, type1Color);
    drawTypeBadge(startX + badgeWidth + badgeSpacing, badgeY, badgeWidth, badgeHeight, data.type2, type2Color);
  } else {
    // Single type
    int startX = (tft.width() - badgeWidth) / 2;
    uint16_t typeColor = getTypeColor(data.type1);
    drawTypeBadge(startX, badgeY, badgeWidth, badgeHeight, data.type1, typeColor);
  }

  releaseDisplay("displayPokemonInfo");
  
  Serial.println("Pokemon info displayed successfully (no SD card needed!)");
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
  for (int radius = 10; radius < 100; radius += 15) {
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
  
  for (int i = 0; i < 3; i++) {
    tft.setCursor(textX, centerY - 10);
    tft.print(scanText);
    delay(300);
    
    // Flash effect
    tft.fillRect(textX, centerY - 10, scanText.length() * 12, 16, ILI9341_BLACK);
    delay(200);
    esp_task_wdt_reset();
  }
}

// 完整的Pokemon頁面顯示函數 - 包含精靈動畫和增強過渡效果
bool displayPokemonPage(int id)
{
  Serial.printf("Displaying Pokemon page for #%d with enhanced transitions\n", id);
  
  if (!checkMemoryAvailable("displayPokemonPage")) {
    Serial.println("Insufficient memory for Pokemon page display");
    return false;
  }

  // Step 1: Show transition from welcome screen if this is first Pokemon
  static bool isFirstPokemon = true;
  if (isFirstPokemon) {
    transitionFromWelcomeScreen();
    isFirstPokemon = false;
  } else {
    // Quick fade for subsequent Pokemon
    fadeToBlack(3);
  }

  // Step 2: Show scanning animation
  showPokemonScanAnimation();
  
  // Step 3: Brief pause for suspense
  delay(500);
  
  // Step 4: Reveal Pokemon with slide-in effect
  tft.fillScreen(ILI9341_BLACK);
  
  // Get Pokemon data for enhanced display
  const PokemonData* pokemon = findPokemonData(id);
  if (pokemon) {
    PokemonData data;
    memcpy_P(&data, pokemon, sizeof(PokemonData));
    
    // Show "Pokemon Found!" message
    tft.setTextColor(ILI9341_GREEN);
    tft.setTextSize(2);
    String foundText = "POKEMON FOUND!";
    int16_t foundX = (tft.width() - foundText.length() * 12) / 2;
    tft.setCursor(foundX, 20);
    tft.print(foundText);
    
    // Flash effect
    for (int i = 0; i < 2; i++) {
      delay(200);
      tft.fillRect(foundX, 20, foundText.length() * 12, 16, ILI9341_BLACK);
      delay(200);
      tft.setCursor(foundX, 20);
      tft.print(foundText);
    }
    
    delay(800);
  }

  // Step 5: Display Pokemon info with slide-in animation
  if (!displayPokemonInfoWithTransition(id)) {
    Serial.println("Failed to display Pokemon info");
    return false;
  }

  // Step 6: Play enhanced sprite animation with particle effects
  playEnhancedPokemonAnimation(id, 20, 90, 200, 120, 3000);

  Serial.printf("Pokemon #%d page displayed successfully with enhanced experience\n", id);
  return true;
}

// Enhanced Pokemon info display with slide-in transition
bool displayPokemonInfoWithTransition(int id)
{
  Serial.printf("Displaying Pokemon info with transition for ID %d\n", id);
  
  if (!acquireDisplay("displayPokemonInfoWithTransition")) {
    return false;
  }

  const PokemonData* pokemon = findPokemonData(id);
  if (!pokemon) {
    releaseDisplay("displayPokemonInfoWithTransition");
    return false;
  }

  PokemonData data;
  memcpy_P(&data, pokemon, sizeof(PokemonData));

  // Clear screen with gradient effect
  tft.fillScreen(ILI9341_BLACK);

  // Slide-in effect for Pokemon ID and name
  int16_t slideDistance = 240;
  int16_t slideSteps = 20;
  int16_t stepSize = slideDistance / slideSteps;
  
  for (int step = 0; step < slideSteps; step++) {
    int16_t currentX = slideDistance - (step * stepSize);
    
    // Clear previous position
    if (step > 0) {
      tft.fillRect(0, 45, 240, 30, ILI9341_BLACK);
    }
    
    // Draw Pokemon ID
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);
    char idText[16];
    snprintf(idText, sizeof(idText), "#%d", data.id);
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
  
  for (int i = 0; i < strlen(data.name_en); i++) {
    tft.setCursor(nameX + i * 12, nameY);
    tft.print(data.name_en[i]);
    delay(60);
  }
  
  delay(200);
  
  // Chinese name
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(nameX, nameY + 25);
  tft.print(data.name_zh);

  // Stats with progress bar animation
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  
  // Height
  tft.setCursor(20, 220);
  tft.print("Height: ");
  tft.print(data.height / 10.0, 1);
  tft.print(" m");
  
  // Weight  
  tft.setCursor(20, 235);
  tft.print("Weight: ");
  tft.print(data.weight / 10.0, 1);
  tft.print(" kg");

  // Type badges with slide-in effect
  int badgeY = 250;
  int badgeWidth = 60;
  int badgeHeight = 20;
  int badgeSpacing = 10;

  if (data.type2) {
    // Two types with staggered animation
    int totalWidth = 2 * badgeWidth + badgeSpacing;
    int startX = (tft.width() - totalWidth) / 2;
    
    // First type
    uint16_t type1Color = getTypeColor(data.type1);
    for (int w = 0; w <= badgeWidth; w += 5) {
      tft.fillRect(startX + badgeWidth - w, badgeY, w, badgeHeight, type1Color);
      delay(20);
    }
    drawTypeBadge(startX, badgeY, badgeWidth, badgeHeight, data.type1, type1Color);
    
    delay(200);
    
    // Second type
    uint16_t type2Color = getTypeColor(data.type2);
    int type2X = startX + badgeWidth + badgeSpacing;
    for (int w = 0; w <= badgeWidth; w += 5) {
      tft.fillRect(type2X, badgeY, w, badgeHeight, type2Color);
      delay(20);
    }
    drawTypeBadge(type2X, badgeY, badgeWidth, badgeHeight, data.type2, type2Color);
  } else {
    // Single type
    int startX = (tft.width() - badgeWidth) / 2;
    uint16_t typeColor = getTypeColor(data.type1);
    
    for (int w = 0; w <= badgeWidth; w += 5) {
      tft.fillRect(startX + badgeWidth - w, badgeY, w, badgeHeight, typeColor);
      delay(20);
    }
    drawTypeBadge(startX, badgeY, badgeWidth, badgeHeight, data.type1, typeColor);
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
  if (!systemHealthy) {
    delay(1000);
    return;
  }
  
  // 安全處理Pokemon顯示請求
  if (pokemonDisplayRequested && !displayBusy) {
    pokemonDisplayRequested = false; // 重置標誌
    newDataReceived = false; // 也重置這個標誌

    int pokemonId = requestedPokemonId;
    Serial.printf("Processing Pokemon display request for ID: %d\n", pokemonId);
    
    // 檢查記憶體狀態
    if (!checkMemoryAvailable("main_loop_display")) {
      Serial.println("Insufficient memory for display, skipping request");
      
      // 顯示錯誤訊息
      if (acquireDisplay("error_display")) {
        tft.fillRect(0, 280, 240, 40, ILI9341_BLACK);
        tft.setTextColor(ILI9341_RED);
        tft.setTextSize(1);
        tft.setCursor(10, 285);
        tft.println("Memory error!");
        tft.setCursor(10, 300);
        tft.println("Please restart device");
        
        releaseDisplay("error_display");
      }
      
      return;
    }

    // 嘗試顯示Pokemon頁面
    if (!displayPokemonPage(pokemonId)) {
      Serial.println("Failed to display Pokemon page, showing error");
      
      // 顯示失敗訊息
      if (acquireDisplay("fail_display")) {
        tft.fillRect(0, 280, 240, 40, ILI9341_BLACK);
        tft.setTextColor(ILI9341_ORANGE);
        tft.setTextSize(1);
        tft.setCursor(10, 285);
        tft.printf("Pokemon #%d not found", pokemonId);
        tft.setCursor(10, 300);
        tft.println("Check embedded data");
        
        releaseDisplay("fail_display");
      }
    } else {
      // 成功顯示後，短暫顯示狀態訊息
      safeDelayWithMemCheck(2000, "success_display");
      
      if (acquireDisplay("status_update")) {
        tft.fillRect(0, 280, 240, 40, ILI9341_BLACK);
        tft.setTextColor(ILI9341_GREEN);
        tft.setTextSize(1);
        tft.setCursor(10, 285);
        tft.println("Ready for next scan");
        tft.setCursor(10, 300);
        tft.printf("Last shown: #%d", pokemonId);
        
        releaseDisplay("status_update");
      }
    }
  }
  
  // 處理舊版本的newDataReceived標誌（fallback）
  else if (newDataReceived && !pokemonDisplayRequested) {
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
