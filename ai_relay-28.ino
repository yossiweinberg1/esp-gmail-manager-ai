#include "secrets.h"
#include "ca_cert.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h> 
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Preferences.h>
#include <FS.h>
#include <SD.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <time.h>

// Pocket-Dongle-S3-0.96 (AliExpress) — NOT the LilyGO T-Dongle-S3.
// Pinout: https://github.com/ronenkr/Pocket-Dongle-S3
#define PROBE_SD_AT_BOOT 1

// --- HARDWARE PINOUTS ---
#define BUTTON_PIN 0  // BOOT button on ESP32-S3

// SD card uses hardware SPI (separate from the bit-banged TFT bus)
#define SD_SCK   17
#define SD_MISO  16
#define SD_MOSI  18
#define SD_CS    47

// Display pins (Pocket-Dongle-S3-0.96 ST7735, software SPI)
#define TFT_SCLK 10
#define TFT_MOSI 11
#define TFT_CS   12
#define TFT_DC   13
#define TFT_RST  14

Adafruit_ST7735 display = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// --- DYNAMIC CONFIGURATION (SD or Secrets Fallback) ---
String wifi_ssid       = SECRET_WIFI_SSID;
String wifi_password   = SECRET_WIFI_PASS;
String gemini_api_key  = SECRET_GEMINI_KEY;
String gmail_user      = SECRET_GMAIL_USER;
String gmail_pass      = SECRET_GMAIL_PASS;
String admin_phone     = SECRET_ADMIN_PHONE;
String sms_gateway     = "";  // e.g. "vtext.com" for Verizon, "tmomail.net" for T-Mobile
String default_persona = DEFAULT_PERSONA;

void *allocPsram(size_t bytes) {
  if (bytes == 0) return nullptr;
  if (psramFound()) {
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) return ptr;
  }
  return malloc(bytes);
}

void *reallocPsram(void *ptr, size_t bytes) {
  if (bytes == 0) {
    free(ptr);
    return nullptr;
  }
  if (psramFound()) {
    return heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  return realloc(ptr, bytes);
}

void freePsram(void *ptr) {
  if (ptr) free(ptr);
}

struct PsramText {
  char *data;
  size_t capacity;
  size_t len;

  PsramText() : data(nullptr), capacity(0), len(0) {}
  PsramText(const PsramText &other) : data(nullptr), capacity(0), len(0) { assign(other.c_str()); }
  ~PsramText() { freeBuffer(); }

  PsramText &operator=(const PsramText &other) {
    if (this != &other) assign(other.c_str());
    return *this;
  }
  PsramText &operator=(const String &value) { assign(value.c_str()); return *this; }
  PsramText &operator=(const char *value) { assign(value ? value : ""); return *this; }
  PsramText &operator+=(const String &value) { append(value.c_str()); return *this; }
  PsramText &operator+=(const char *value) { append(value ? value : ""); return *this; }
  PsramText &operator+=(char value) { append(value); return *this; }
  operator String() const { return String(c_str()); }

  const char *c_str() const { return data ? data : ""; }
  size_t length() const { return len; }
  bool isEmpty() const { return len == 0; }

  void clear() {
    if (data) data[0] = '\0';
    len = 0;
  }

  void freeBuffer() {
    if (data) freePsram(data);
    data = nullptr;
    capacity = 0;
    len = 0;
  }

  void assign(const char *value) {
    size_t valueLen = value ? strlen(value) : 0;
    if (!ensureCapacity(valueLen + 1)) return;
    if (valueLen > 0) memcpy(data, value, valueLen);
    data[valueLen] = '\0';
    len = valueLen;
  }

  void append(const char *value) {
    size_t valueLen = value ? strlen(value) : 0;
    if (valueLen == 0) return;
    if (!ensureCapacity(len + valueLen + 1)) return;
    memcpy(data + len, value, valueLen);
    len += valueLen;
    data[len] = '\0';
  }

  void append(char value) {
    if (!ensureCapacity(len + 2)) return;
    data[len++] = value;
    data[len] = '\0';
  }

  bool ensureCapacity(size_t desired) {
    if (desired <= capacity && data) return true;
    size_t newCapacity = capacity == 0 ? 128 : capacity;
    while (newCapacity < desired) newCapacity *= 2;
    void *newData = reallocPsram(data, newCapacity);
    if (!newData) return false;
    data = static_cast<char *>(newData);
    capacity = newCapacity;
    if (len == 0) data[0] = '\0';
    return true;
  }

  int indexOf(const String &needle, int from = 0) const { return String(c_str()).indexOf(needle, from); }
  int indexOf(const char *needle, int from = 0) const { return String(c_str()).indexOf(needle, from); }
  String substring(int start, int end = -1) const { return String(c_str()).substring(start, end); }
  bool startsWith(const char *prefix) const { return String(c_str()).startsWith(prefix); }
  void trim() { String tmp = String(c_str()); tmp.trim(); assign(tmp.c_str()); }
};

#define MAX_GEMINI_KEYS 5
struct GeminiKeyState {
  String key;
  int requestsToday;
  unsigned long cooldownUntilMs;
  int failures;
};

GeminiKeyState geminiKeys[MAX_GEMINI_KEYS];
int geminiKeyCount = 0;
int geminiUsageDay = 0;
int nextGeminiKeyCursor = 0;
String lastGeminiModel = "unknown";
String lastGeminiKeyLabel = "primary";

bool sdAvailable = false;
bool ntpReady = false;
unsigned long lastClockSyncAttemptMs = 0;
const unsigned long CLOCK_SYNC_INTERVAL_MS = 3600000UL;
String lastResetReason = "Unknown";

WiFiClientSecure imapClient;
bool imapConnected = false;
unsigned long imapLastActivityMs = 0;
unsigned long imapLastNoopMs = 0;
const unsigned long IMAP_KEEPALIVE_INTERVAL_MS = 30000UL;
const unsigned long IMAP_IDLE_TTL_MS = 300000UL;

unsigned long consecutiveImapErrors = 0;
unsigned long consecutiveGeminiEmpty = 0;
unsigned long lastHealthResetMs = 0;
const unsigned long HEALTH_REBOOT_THRESHOLD = 5;
const unsigned long GEMINI_EMPTY_REBOOT_THRESHOLD = 3;

struct AdminPendingAction {
  String action;
  String args;
  String code;
  unsigned long expiresAt;
  bool active;
};
AdminPendingAction pendingAdminAction = {"", "", "", 0, false};

unsigned long adminCommandWindowStart = 0;
int adminCommandCount = 0;
const int MAX_ADMIN_CMDS_PER_WINDOW = 8;
const unsigned long ADMIN_CMD_WINDOW_MS = 60000UL;
const unsigned long IMAP_BACKOFF_BASE_MS = 15000UL;
const unsigned long IMAP_BACKOFF_MAX_MS = 300000UL;
const unsigned long IMAP_BACKOFF_JITTER_MS = 2000UL;
// Timezone used for configTzTime. Can be overridden by /config.json on SD.
String timezoneStr = "EST5EDT";

struct AnalyticsStats {
  unsigned long messageCount = 0;
  unsigned long totalGeminiMs = 0;
  unsigned long totalSmtpMs = 0;
  unsigned long totalResponseChars = 0;
  unsigned long maxGeminiMs = 0;
  unsigned long maxSmtpMs = 0;
  double totalEnergyWh = 0.0;
  int lastRssi = 0;
  String lastModel = "unknown";
  String lastStatus = "idle";
};

AnalyticsStats analyticsStats;

struct SharedUiState {
  String title = "AI Relay";
  String body = "Booting up...";
  String lastError = "None";
  bool paused = false;
  bool screenDirty = true;
  bool autoDimmed = false;
  uint8_t pausedPage = 0;
  unsigned long lastUiActivityMs = 0;
  unsigned long lastButtonEventMs = 0;
  unsigned long lastNetworkActivityMs = 0;
};

SharedUiState uiState;
SemaphoreHandle_t uiStateMutex = nullptr;
TaskHandle_t networkTaskHandle = nullptr;
TaskHandle_t uiTaskHandle = nullptr;

#define GEMINI_CACHE_SIZE 24
struct GeminiCacheEntry {
  uint64_t keyHash;
  PsramText reply;
  unsigned long storedAtMs;
  unsigned long lastUsedMs;
  bool valid;
};

GeminiCacheEntry geminiResponseCache[GEMINI_CACHE_SIZE];
unsigned long geminiCacheHits = 0;
unsigned long geminiCacheMisses = 0;
unsigned long geminiCacheStores = 0;
unsigned long geminiCacheEvictions = 0;

// --- DYNAMIC POLLING INTERVALS ---
const unsigned long IDLE_POLL_INTERVAL_MS   = 15000; 
const unsigned long ACTIVE_POLL_INTERVAL_MS = 5000;  

// --- NVS STORAGE ---
Preferences prefs;

// --- DYNAMIC WHITELIST (Up to 20 Users) ---
#define MAX_ALLOWED_USERS 20

struct AllowedUser {
  String phoneDigits;
  String systemPrompt;
  bool muted;
};

AllowedUser dynamicAllowedUsers[MAX_ALLOWED_USERS];
int dynamicUserCount = 0;

void loadAllowedUsers();
void saveAllowedUsers();
bool handleAdminCommand(String senderDigits, String textBody, String replyAddress);
void setPausedState(bool paused);
bool getPausedState();
void markUiActivity();
void setLastErrorMessage(const String &message);
void cyclePausedStatusPage();
void refreshCurrentScreen();
void renderScreenPage();
void networkTask(void *parameter);
void uiTask(void *parameter);
void runNetworkIteration();
void runUiIteration();
String normalizeForFingerprint(String text);
String getConversationPromptHistory(int convoIdx, bool fullHistory);
uint64_t buildGeminiCacheKey(int convoIdx, const String &userMessage, const String &promptHistory, const String &personaPrompt);
bool lookupGeminiCache(uint64_t keyHash, String &replyOut);
void storeGeminiCache(uint64_t keyHash, const String &replyText);
String getGeminiCacheStatsString();
bool tryLocalIntentReply(const String &textBody, String &replyOut);

// --- PAUSE & POWER CONTROL ---
volatile bool isPaused = false;
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 50;

const float IDLE_CURRENT_MA = 80.0;
const float ACTIVE_CURRENT_MA = 180.0;
const float SUPPLY_VOLTAGE = 5.0;
unsigned long powerLastMillis = 0;
double totalEnergy_mWh = 0.0;
bool relayActive = false;
String lastScreenTitle = "AI Relay";
String lastScreenBody = "Booting up...";
int consecutiveLoginFailures = 0;

const int SCREEN_BL_PIN = 15;
const int BACKLIGHT_CHANNEL = 0;
const int BACKLIGHT_FREQUENCY = 5000;
const int BACKLIGHT_RESOLUTION = 8;
const int DEFAULT_SCREEN_BRIGHTNESS = 255;
const int DIM_SCREEN_BRIGHTNESS = 64;
int screenBacklightPin = SCREEN_BL_PIN;
int screenBrightness = DEFAULT_SCREEN_BRIGHTNESS;
bool screenEnabled = true;
bool screenBacklightActiveLow = false;

const int ACTIVE_CPU_MHZ = 240;
const int IDLE_CPU_MHZ = 80;
int currentCpuMHz = ACTIVE_CPU_MHZ;

String overridePhoneDigits = "";
unsigned long overrideExpiresAtMs = 0;
const unsigned long OVERRIDE_DURATION_MS = 3600000UL;

void trackPower();
String getPowerStatusString();
void checkButton();
void responsiveDelay(unsigned long ms);
bool saveWiFiConfig();
void initDisplayBacklight();
void setDisplayBrightness(int level);
void setDisplayEnabled(bool enabled);
void updateCpuFrequency(bool active);
bool isOverrideActive(String senderDigits);

// --- PER-SENDER CONVERSATION & PERSONA MEMORY ---
#define MAX_CONVERSATIONS 10
#define MAX_HISTORY_CHARS 6000

struct Conversation {
  String phoneDigits;
  String personaPrompt;
  PsramText history;
  bool used;
  unsigned long rateWindowStart;
  int rateCount;
};

Conversation conversations[MAX_CONVERSATIONS];

// --- SHARED-QUOTA FAIR USE LIMITS ---
const unsigned long RATE_WINDOW_MS = 3600000UL;      
const int MAX_MSGS_PER_SENDER_PER_HOUR = 10;
const unsigned long GLOBAL_WINDOW_MS = 86400000UL;   
const int MAX_GLOBAL_MSGS_PER_DAY = 18;              
unsigned long globalRateWindowStart = 0;
int globalRateCount = 0;

bool checkRateLimit(int convoIdx, String &reasonOut);
void recordRateLimitUsage(int convoIdx);

int findOrCreateConversation(String phoneDigits, String personaPrompt);
void appendToHistory(int idx, String userMsg, String aiMsg);
String getPersonaForSender(String senderDigits);
void loadConversations();
void saveConversations();

// --- SD CARD HELPERS ---
bool ensureDir(const String &path);
bool appendTextToFile(const String &path, const String &data);
bool appendTextToFileWithSizeLimit(const String &path, const String &data, size_t maxBytes);
bool writeFileAtomic(const String &path, const String &data);
bool syncTimeFromNTP();
String getWallClockTimestamp();
String getDateTag();
String getTimeTag();
void updateAnalytics(String modelName, unsigned long geminiMs, unsigned long smtpMs, int responseLength, int rssi, String status);
void writeAnalyticsReport();
void rotateBackupFiles();
void writeBackupArchive();
String loadRelevantKnowledge(String prompt);
void recordMemoryFact(String phoneDigits, String factText);
bool initSDCard();
void loadSDConfig();
void loadGeminiUsage();
bool saveGeminiUsage();
void logToSD(String logLine);
void logTrafficCSV(String sender, String text, unsigned long geminiMs, unsigned long smtpMs, int responseLen);
void queueOfflineMessage(String recipient, String aiResponse);
void processOfflineQueue();
void logConversationToSD(String phoneDigits, String userMsg, String aiMsg);
String readFullHistoryFromSD(String phoneDigits);
void loadGlobalRateLimit();
void saveGlobalRateLimit();
String searchConversationArchive(String phoneDigits, String keyword, String datePrefix);
String getResetReasonString();

// Forward declarations
String sendGeminiApiRequest(String modelName, String requestBody);
String sendGeminiApiRequestWithKey(String modelName, String requestBody, String apiKey);
String queryGemini(String prompt, String history, String personaPrompt);
void rebuildGeminiKeyPool();
void resetGeminiKeyUsageIfNeeded();
void markGeminiKeyFailure(int index, unsigned long cooldownMs);
void markGeminiKeySuccess(int index);
int pickGeminiKey();
bool sendEmailReply(String recipient, String aiResponse);
bool forwardEmailToAdmin(const String &fromHeader, const String &subject, const String &body);
bool isSenderAllowed(String sender, String subject, String &matchedDigitsOut);
String extractDigits(String input);
void updateScreen(String title, String bodyText);
String cleanBody(String raw);
String extractPlainTextPart(String raw);
String decodeQuotedPrintable(String input);

// Raw IMAP helpers
bool imapRawConnect(WiFiClientSecure &client);
String imapReadUntilTagged(WiFiClientSecure &client, const String &tag, unsigned long timeoutMs = 15000);
String imapSearchUnseenUIDs(WiFiClientSecure &client);
bool imapFetchHeader(WiFiClientSecure &client, const String &uid, String &fromOut, String &subjectOut);
String imapFetchBodyText(WiFiClientSecure &client, const String &uid);
void imapMarkSeen(WiFiClientSecure &client, const String &uid);
void imapLogout(WiFiClientSecure &client);
String extractLiteral(const String &resp, int searchFromIdx);

bool ensureImapConnection();
bool imapKeepAlive();
void imapDisconnect();
void healthCheck();
void logAndReboot(const String &reason);
bool enforceAdminRateLimit(const String &replyAddress);
String generateAdminConfirmationCode();

// Raw SMTP base64
// Sizes the output buffer dynamically and checks mbedtls's return code so a
// long password/email never silently truncates (the old fixed 256-byte
// buffer had no bounds check at all).
String base64Encode(const String &input) {
  size_t requiredLen = 0;
  mbedtls_base64_encode(nullptr, 0, &requiredLen, (const unsigned char*)input.c_str(), input.length());
  if (requiredLen == 0) return "";

  unsigned char *out = (unsigned char*)malloc(requiredLen + 1);
  if (!out) return "";

  size_t outLen = 0;
  int ret = mbedtls_base64_encode(out, requiredLen, &outLen, (const unsigned char*)input.c_str(), input.length());
  if (ret != 0) {
    Serial.println("base64Encode failed, ret=" + String(ret));
    free(out);
    return "";
  }
  out[outLen] = '\0';

  String result((char*)out);
  free(out);
  return result;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n--- AI Relay boot ---");

  if (uiStateMutex == nullptr) {
    uiStateMutex = xSemaphoreCreateMutex();
  }

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 60000,
    .idle_core_mask = 1,
    .trigger_panic = true
  };
  esp_err_t wdtErr = esp_task_wdt_init(&wdt_config);
  if (wdtErr == ESP_ERR_INVALID_STATE) {
    Serial.println("WDT already initialized, continuing.");
  } else if (wdtErr != ESP_OK) {
    Serial.printf("WDT init failed: %d\n", wdtErr);
  }
  esp_err_t wdtAddErr = esp_task_wdt_add(NULL);
  if (wdtAddErr == ESP_ERR_INVALID_STATE) {
    Serial.println("WDT task already registered, continuing.");
  } else if (wdtAddErr != ESP_OK) {
    Serial.printf("WDT add failed: %d\n", wdtAddErr);
  }
  randomSeed((uint32_t)esp_random());

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  display.initR(INITR_MINI160x80);
  display.setRotation(3);
  updateScreen("AI Relay", "Booting up...");

  // NVS first — fast, and must not depend on SD
  Serial.println("Loading NVS...");
  updateScreen("Boot", "Loading config...");
  loadAllowedUsers();
  loadConversations();
  rebuildGeminiKeyPool();

  if (PROBE_SD_AT_BOOT) {
    Serial.println("Probing SD card...");
    updateScreen("Boot", "Checking SD...");
    sdAvailable = initSDCard();
    if (sdAvailable) {
      // Load config from SD first so timezone (if provided) is applied
      // before attempting the initial NTP sync.
      loadSDConfig();
      lastResetReason = getResetReasonString();
      ntpReady = syncClockFromNTP(true);
      if (ntpReady) {
        Serial.println("NTP time synced: " + getWallClockTimestamp());
      }
      loadGlobalRateLimit();
      loadGeminiUsage();
    } else {
      Serial.println("SD unavailable — continuing without it.");
    }
  } else {
    Serial.println("SD boot probe disabled — skipping.");
    sdAvailable = false;
  }

  Serial.println("Connecting Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
  updateScreen("Wi-Fi", "Connecting...");

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
    esp_task_wdt_reset();
    checkButton();
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi connect timed out after 30s");
    updateScreen("Wi-Fi Error", "Connect failed");
    setLastErrorMessage("Wi-Fi connect timed out");
    setPausedState(true);
    return;
  }

  WiFi.setSleep(false); // Disable modem sleep latency

  Serial.println();
  Serial.println("Wi-Fi connected, IP: " + WiFi.localIP().toString());
  ntpReady = syncClockFromNTP(false);
  if (ntpReady) {
    Serial.println("Clock synced after Wi-Fi connect: " + getWallClockTimestamp());
  }

  initDisplayBacklight();
  setDisplayEnabled(true);
  updateCpuFrequency(false);

  // Check for queued emails if SD is active
  if (sdAvailable) {
    processOfflineQueue();
  }

  setPausedState(false);
  updateScreen("AI Relay", "Running...");

  if (uiTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(uiTask, "uiTask", 8192, nullptr, 2, &uiTaskHandle, 1);
  }
  if (networkTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(networkTask, "networkTask", 12288, nullptr, 3, &networkTaskHandle, 0);
  }
}

void runNetworkIteration() {
  esp_task_wdt_reset();

  if (WiFi.status() == WL_CONNECTED && (millis() - lastClockSyncAttemptMs > CLOCK_SYNC_INTERVAL_MS || !ntpReady)) {
    lastClockSyncAttemptMs = millis();
    syncClockFromNTP(false);
  }

  if (getPausedState()) {
    delay(100);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, attempting reconnect...");
    updateScreen("WiFi Lost", "Reconnecting...");
    WiFi.disconnect();
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());

    unsigned long reconnectStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - reconnectStart < 15000) {
      esp_task_wdt_reset();
      delay(250);
    }

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi reconnect attempt failed");
      responsiveDelay(IDLE_POLL_INTERVAL_MS);
      return;
    }
    
    WiFi.setSleep(false);
    Serial.println("WiFi reconnected: " + WiFi.localIP().toString());
    updateScreen(lastScreenTitle, lastScreenBody);
    
    if (sdAvailable) processOfflineQueue();
  }

  relayActive = true;
  bool processedMessageThisLoop = false;

  if (!ensureImapConnection()) {
    consecutiveImapErrors++;
    consecutiveLoginFailures++;
    unsigned long backoff = min(IMAP_BACKOFF_MAX_MS, IMAP_BACKOFF_BASE_MS << min(consecutiveImapErrors, 10UL));
    backoff += random(IMAP_BACKOFF_JITTER_MS);
    Serial.printf("IMAP connect/login failed (%d consecutive), backoff=%lums\n", consecutiveImapErrors, backoff);
    logToSD("IMAP Connect Failure: " + String(consecutiveImapErrors));
    relayActive = false;
    updateScreen("IMAP Error", "Connect/login failed");
    setLastErrorMessage("IMAP connect/login failed");
    healthCheck();
    responsiveDelay(backoff);
    return;
  }

  consecutiveImapErrors = 0;
  consecutiveLoginFailures = 0;

  String uidLine = imapSearchUnseenUIDs(imapClient);

  if (uidLine.length() == 0) {
    imapLastActivityMs = millis();
    relayActive = false;
    updateScreen(lastScreenTitle, lastScreenBody);
    responsiveDelay(IDLE_POLL_INTERVAL_MS);
    return;
  }

  int start = 0;
  while (start < (int)uidLine.length()) {
    if (getPausedState()) break;

    int spaceIdx = uidLine.indexOf(' ', start);
    String uid = (spaceIdx == -1) ? uidLine.substring(start) : uidLine.substring(start, spaceIdx);
    uid.trim();

    if (uid.length() > 0) {
      String fromHeader, subjectHeader;
      imapFetchHeader(imapClient, uid, fromHeader, subjectHeader);

      String replyAddress = fromHeader;
      int lt = fromHeader.indexOf('<');
      int gt = fromHeader.indexOf('>');
      if (lt != -1 && gt != -1 && gt > lt) {
        replyAddress = fromHeader.substring(lt + 1, gt);
      }
      replyAddress.trim();

      if (replyAddress.indexOf('@') == -1) {
        imapMarkSeen(imapClient, uid);
        if (spaceIdx == -1) break;
        start = spaceIdx + 1;
        continue;
      }

      bool isGoogleVoice = subjectHeader.indexOf("New text message from") != -1;
      String matchedDigits = "";
      bool allowed = isSenderAllowed(fromHeader, subjectHeader, matchedDigits);

      if (!isGoogleVoice) {
        // Forward every non-Google-Voice email directly to the owner's inbox
        processedMessageThisLoop = true;
        String rawBody = imapFetchBodyText(imapClient, uid);
        String emailBody = cleanBody(rawBody);
        forwardEmailToAdmin(fromHeader, subjectHeader, emailBody);
        updateScreen("Email Fwd", subjectHeader.substring(0, 20));
        logToSD("Forwarded email: From=" + fromHeader + " Subject=" + subjectHeader);
      } else if (isGoogleVoice && allowed) {
        processedMessageThisLoop = true;
        String rawBody = imapFetchBodyText(imapClient, uid);
        String textBody = cleanBody(rawBody);

        if (!handleAdminCommand(matchedDigits, textBody, replyAddress)) {
                bool isMuted = false;
          for (int i = 0; i < dynamicUserCount; i++) {
            if (dynamicAllowedUsers[i].phoneDigits == matchedDigits) {
              isMuted = dynamicAllowedUsers[i].muted;
              break;
            }
          }

          if (isMuted) {
            sendEmailReply(replyAddress, "AI replies are currently paused for you by the admin.");
            updateScreen("Muted", "Ignored msg from " + matchedDigits);
          } else {
            updateScreen("Text Received!", textBody);
            String personaPrompt = getPersonaForSender(matchedDigits);
            int convoIdx = findOrCreateConversation(matchedDigits, personaPrompt);
            bool senderIsAdmin = normalizeDigits(admin_phone).length() == 0 || matchedDigits == normalizeDigits(admin_phone);
            String promptHistory = getConversationPromptHistory(convoIdx, senderIsAdmin);

            String rateLimitReason;
            if (!checkRateLimit(convoIdx, matchedDigits, rateLimitReason)) {
              sendEmailReply(replyAddress, rateLimitReason);
              updateScreen("Rate Limited", rateLimitReason);
            } else {
              String aiResponse = "";
              String responseSource = "gemini";
              uint64_t cacheKey = buildGeminiCacheKey(convoIdx, textBody, promptHistory, conversations[convoIdx].personaPrompt);
              unsigned long geminiDuration = 0;

              String cannedReply;
              if (tryLocalIntentReply(textBody, cannedReply)) {
                aiResponse = cannedReply;
                responseSource = "local";
              } else if (lookupGeminiCache(cacheKey, aiResponse)) {
                responseSource = "cache";
                logToSD("Gemini cache hit: " + matchedDigits);
                Serial.println("Gemini cache hit for " + matchedDigits);
              } else {
                unsigned long tGeminiStart = millis();
                aiResponse = queryGemini(textBody, promptHistory, conversations[convoIdx].personaPrompt);
                geminiDuration = millis() - tGeminiStart;
                if (aiResponse.length() > 0) {
                  storeGeminiCache(cacheKey, aiResponse);
                }
              }

              if (aiResponse.length() > 0) {
                String statusLabel = (responseSource == "cache") ? "Cache Hit" : (responseSource == "local") ? "Local Reply" : "AI Response:";
                updateScreen(statusLabel, aiResponse);
                
                unsigned long tSmtpStart = millis();
                bool sendSuccess = sendEmailReply(replyAddress, aiResponse);
                unsigned long smtpDuration = millis() - tSmtpStart;

                if (!sendSuccess && sdAvailable) {
                  queueOfflineMessage(replyAddress, aiResponse);
                }

                appendToHistory(convoIdx, textBody, aiResponse);
                saveConversations();
                recordRateLimitUsage(convoIdx);
                saveGlobalRateLimit();

                if (sdAvailable) {
                  String modelTag = (responseSource == "cache") ? "cache" : (responseSource == "local") ? "local" : lastGeminiModel;
                  String statusTag = (responseSource == "cache") ? "cache-hit" : (responseSource == "local") ? "local-intent" : (sendSuccess ? "sent" : "queued");
                  lastGeminiModel = modelTag;
                  lastGeminiKeyLabel = responseSource;
                  updateAnalytics(modelTag, geminiDuration, smtpDuration, aiResponse.length(), WiFi.RSSI(), statusTag);
                  logTrafficCSV(matchedDigits, textBody, geminiDuration, smtpDuration, aiResponse.length());
                  logConversationToSD(matchedDigits, textBody, aiResponse);
                  writeAnalyticsReport();
                }
              } else {
                consecutiveGeminiEmpty++;
                setLastErrorMessage("Gemini returned empty response");
                healthCheck();
              }
            }
          }
        }
      }
      imapMarkSeen(imapClient, uid);
    }
    if (spaceIdx == -1) break;
    start = spaceIdx + 1;
  }

  // Keep the connection warm for the next poll unless we are pausing
  // or the idle TTL has expired (ensureImapConnection will handle that).
  if (getPausedState()) {
    imapDisconnect();
  } else {
    imapLastActivityMs = millis();   // refresh activity so TTL doesn't trip
  }
  relayActive = false;

  if (!getPausedState()) {
    updateScreen(lastScreenTitle, lastScreenBody);
  }

  unsigned long delayTime = processedMessageThisLoop ? ACTIVE_POLL_INTERVAL_MS : IDLE_POLL_INTERVAL_MS;
  responsiveDelay(delayTime);
}

void loop() {
  esp_task_wdt_reset();
  delay(250);
}

// --- SD CARD FUNCTIONS ---
bool ensureDir(const String &path) {
  if (path.length() == 0) return false;
  if (SD.exists(path)) return true;
  return SD.mkdir(path.c_str());
}

bool appendTextToFile(const String &path, const String &data) {
  if (!sdAvailable) return false;

  int lastSlash = path.lastIndexOf('/');
  if (lastSlash > 0) {
    String dirPath = path.substring(0, lastSlash);
    if (!SD.exists(dirPath) && !ensureDir(dirPath)) {
      Serial.println("Failed to create SD directory: " + dirPath);
      return false;
    }
  }

  File f = SD.open(path.c_str(), FILE_APPEND);
  if (!f) return false;
  f.print(data);
  f.close();
  return true;
}

bool appendTextToFileWithSizeLimit(const String &path, const String &data, size_t maxBytes) {
  if (!sdAvailable) return false;

  if (SD.exists(path)) {
    File existing = SD.open(path.c_str(), FILE_READ);
    if (existing) {
      size_t currentSize = existing.size();
      existing.close();
      if (currentSize + data.length() > maxBytes) {
        String rotatedPath = path + ".bak";
        if (SD.exists(rotatedPath)) SD.remove(rotatedPath.c_str());
        SD.rename(path.c_str(), rotatedPath.c_str());
      }
    }
  }

  return appendTextToFile(path, data);
}

bool writeFileAtomic(const String &path, const String &data) {
  if (!sdAvailable) return false;

  int lastSlash = path.lastIndexOf('/');
  if (lastSlash > 0) {
    String dirPath = path.substring(0, lastSlash);
    if (!SD.exists(dirPath) && !ensureDir(dirPath)) {
      Serial.println("Failed to create SD directory: " + dirPath);
      return false;
    }
  }

  String tmpPath = path + ".tmp";
  if (SD.exists(tmpPath)) SD.remove(tmpPath.c_str());
  File tmp = SD.open(tmpPath.c_str(), FILE_WRITE);
  if (!tmp) return false;
  tmp.print(data);
  tmp.close();

  if (SD.exists(path)) SD.remove(path.c_str());
  return SD.rename(tmpPath.c_str(), path.c_str());
}

bool imapKeepAlive() {
  if (!imapClient.connected()) return false;
  imapClient.print("a0 NOOP\r\n");
  String resp = imapReadUntilTagged(imapClient, "a0", 10000);
  if (resp.indexOf("a0 OK") == -1) return false;
  imapLastNoopMs = millis();
  imapLastActivityMs = millis();
  return true;
}

void imapDisconnect() {
  if (imapClient.connected()) {
    imapLogout(imapClient);
    imapClient.stop();
  }
  imapConnected = false;
}

bool ensureImapConnection() {
  if (!WiFi.isConnected()) {
    imapDisconnect();
    return false;
  }

  if (imapConnected && imapClient.connected()) {
    if (millis() - imapLastNoopMs > IMAP_KEEPALIVE_INTERVAL_MS) {
      if (!imapKeepAlive()) {
        imapDisconnect();
        return false;
      }
    }
    if (millis() - imapLastActivityMs > IMAP_IDLE_TTL_MS) {
      imapDisconnect();
      return false;
    }
    return true;
  }

  imapDisconnect();
  if (!imapRawConnect(imapClient)) {
    imapConnected = false;
    return false;
  }
  imapConnected = true;
  imapLastActivityMs = millis();
  imapLastNoopMs = millis();
  consecutiveImapErrors = 0;
  return true;
}

String generateAdminConfirmationCode() {
  int code = random(1000, 9999);
  return String(code);
}

bool enforceAdminRateLimit(const String &replyAddress) {
  unsigned long now = millis();
  if (now - adminCommandWindowStart > ADMIN_CMD_WINDOW_MS) {
    adminCommandWindowStart = now;
    adminCommandCount = 0;
  }
  if (adminCommandCount >= MAX_ADMIN_CMDS_PER_WINDOW) {
    sendEmailReply(replyAddress, "Admin command rate limit exceeded. Try again later.");
    return false;
  }
  adminCommandCount++;
  return true;
}

void clearPendingAdminAction() {
  pendingAdminAction.action = "";
  pendingAdminAction.args = "";
  pendingAdminAction.code = "";
  pendingAdminAction.expiresAt = 0;
  pendingAdminAction.active = false;
}

void logAndReboot(const String &reason) {
  Serial.println("Health reboot: " + reason);
  setLastErrorMessage(reason);
  if (sdAvailable) {
    logToSD("Health reboot: " + reason);
    writeFileAtomic("/logs/reboot-reason.txt", reason + "\n");
  }
  delay(500);
  esp_restart();
}

void healthCheck() {
  if (consecutiveImapErrors >= HEALTH_REBOOT_THRESHOLD) {
    setLastErrorMessage("IMAP repeated failures");
    logAndReboot("IMAP repeated failures");
  }
  if (consecutiveGeminiEmpty >= GEMINI_EMPTY_REBOOT_THRESHOLD) {
    setLastErrorMessage("Gemini empty response streak");
    logAndReboot("Gemini empty response streak");
  }
}

bool syncClockFromNTP(bool force) {
  if (!WiFi.isConnected()) return false;

  if (!force) {
    time_t now = time(nullptr);
    if (now > 100000) {
      prefs.begin("clock", true);
      time_t lastSyncEpoch = (time_t)prefs.getLong("last_sync_epoch", 0);
      prefs.end();
      if (lastSyncEpoch > 100000 && (now - lastSyncEpoch) < 3600) {
        ntpReady = true;
        return true;
      }
    }
  }

  configTzTime(timezoneStr.c_str(), "pool.ntp.org", "time.nist.gov");

  unsigned long start = millis();
  while (millis() - start < 15000) {
    esp_task_wdt_reset();
    time_t now = time(nullptr);
    if (now > 100000) {
      prefs.begin("clock", false);
      prefs.putLong("last_sync_epoch", (long)now);
      prefs.end();
      ntpReady = true;
      lastClockSyncAttemptMs = millis();
      return true;
    }
    delay(200);
  }

  return false;
}

String getWallClockTimestamp() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

String getDateTag() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[16];
  strftime(buf, sizeof(buf), "%Y%m%d", &timeinfo);
  return String(buf);
}

String getTimeTag() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[16];
  strftime(buf, sizeof(buf), "%H%M%S", &timeinfo);
  return String(buf);
}

void updateAnalytics(String modelName, unsigned long geminiMs, unsigned long smtpMs, int responseLength, int rssi, String status) {
  analyticsStats.messageCount++;
  analyticsStats.totalGeminiMs += geminiMs;
  analyticsStats.totalSmtpMs += smtpMs;
  analyticsStats.totalResponseChars += responseLength;
  if (geminiMs > analyticsStats.maxGeminiMs) analyticsStats.maxGeminiMs = geminiMs;
  if (smtpMs > analyticsStats.maxSmtpMs) analyticsStats.maxSmtpMs = smtpMs;
  analyticsStats.totalEnergyWh = totalEnergy_mWh / 1000.0;
  analyticsStats.lastRssi = rssi;
  analyticsStats.lastModel = modelName;
  analyticsStats.lastStatus = status;
}

void writeAnalyticsReport() {
  if (!sdAvailable) return;

  ensureDir("/logs/reports");
  String path = "/logs/reports/" + getDateTag() + ".json";
  DynamicJsonDocument doc(2048);
  doc["timestamp"] = getWallClockTimestamp();
  doc["messages"] = analyticsStats.messageCount;
  doc["avgGeminiMs"] = analyticsStats.messageCount > 0 ? (double)analyticsStats.totalGeminiMs / analyticsStats.messageCount : 0.0;
  doc["avgSmtpMs"] = analyticsStats.messageCount > 0 ? (double)analyticsStats.totalSmtpMs / analyticsStats.messageCount : 0.0;
  doc["avgResponseChars"] = analyticsStats.messageCount > 0 ? (double)analyticsStats.totalResponseChars / analyticsStats.messageCount : 0.0;
  doc["maxGeminiMs"] = analyticsStats.maxGeminiMs;
  doc["maxSmtpMs"] = analyticsStats.maxSmtpMs;
  doc["energyWh"] = analyticsStats.totalEnergyWh;
  doc["rssi"] = analyticsStats.lastRssi;
  doc["lastModel"] = analyticsStats.lastModel;
  doc["lastStatus"] = analyticsStats.lastStatus;
  doc["lastApiKey"] = lastGeminiKeyLabel;
  doc["resetReason"] = lastResetReason;

  File f = SD.open(path.c_str(), FILE_WRITE);
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void rotateBackupFiles() {
  if (!sdAvailable) return;
  ensureDir("/backups");

  File root = SD.open("/backups");
  if (!root) return;

  String backupFiles[16];
  int backupCount = 0;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    if (name.startsWith("/backups/backup-") && name.endsWith(".json")) {
      if (backupCount < 16) backupFiles[backupCount++] = name;
    }
    entry.close();
  }
  root.close();

  if (backupCount <= 5) return;
  for (int i = 0; i < backupCount - 5; i++) {
    SD.remove(backupFiles[i].c_str());
  }
}

void writeBackupArchive() {
  if (!sdAvailable) return;

  ensureDir("/backups");
  String backupPath = "/backups/backup-" + getDateTag() + "-" + getTimeTag() + ".json";

  DynamicJsonDocument doc(16384);
  doc["timestamp"] = getWallClockTimestamp();
  doc["resetReason"] = lastResetReason;

  JsonArray users = doc.createNestedArray("users");
  for (int i = 0; i < dynamicUserCount; i++) {
    JsonObject u = users.createNestedObject();
    u["phone"] = dynamicAllowedUsers[i].phoneDigits;
    u["persona"] = dynamicAllowedUsers[i].systemPrompt;
    u["muted"] = dynamicAllowedUsers[i].muted;
  }

  JsonArray convos = doc.createNestedArray("conversations");
  for (int i = 0; i < MAX_CONVERSATIONS; i++) {
    if (conversations[i].used) {
      JsonObject c = convos.createNestedObject();
      c["phone"] = conversations[i].phoneDigits;
      c["history"] = conversations[i].history.c_str();
    }
  }

  File f = SD.open(backupPath.c_str(), FILE_WRITE);
  if (!f) return;
  serializeJson(doc, f);
  f.close();
  rotateBackupFiles();
}

String loadRelevantKnowledge(String prompt) {
  if (!sdAvailable) return "";

  String lowerPrompt = prompt;
  lowerPrompt.toLowerCase();
  PsramText context;

  String searchDirs[3] = {"/knowledge", "/memory", "/personas"};
  for (int i = 0; i < 3; i++) {
    esp_task_wdt_reset();
    if (!SD.exists(searchDirs[i])) continue;

    File root = SD.open(searchDirs[i].c_str());
    if (!root) continue;
    while (true) {
      esp_task_wdt_reset();
      File entry = root.openNextFile();
      if (!entry) break;
      String name = entry.name();
      bool isTextLike = name.endsWith(".txt") || name.endsWith(".json") || name.endsWith(".md");
      if (isTextLike && !entry.isDirectory()) {
        PsramText content;
        while (entry.available()) {
          esp_task_wdt_reset();
          content += (char)entry.read();
        }
        entry.close();
        String lowerContent = String(content.c_str());
        lowerContent.toLowerCase();
        if (lowerContent.indexOf(lowerPrompt) != -1 || lowerContent.indexOf(lowerPrompt.substring(0, min(8u, lowerPrompt.length()))) != -1) {
          context += "[Knowledge] " + name + "\n" + String(content.c_str()) + "\n";
        }
      } else {
        entry.close();
      }
    }
    root.close();
  }

  return String(context.c_str());
}

String loadMatchingMemoryFacts(String prompt) {
  if (!sdAvailable) return "";

  String lowerPrompt = prompt;
  lowerPrompt.toLowerCase();
  PsramText facts;

  if (!SD.exists("/memory")) return "";

  File root = SD.open("/memory");
  if (!root) return "";

  while (true) {
    esp_task_wdt_reset();
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    if (entry.isDirectory() || !name.endsWith(".txt")) {
      entry.close();
      continue;
    }

    PsramText content;
    while (entry.available()) {
      esp_task_wdt_reset();
      content += (char)entry.read();
    }
    entry.close();

    String lowerContent = String(content.c_str());
    lowerContent.toLowerCase();
    if (lowerContent.indexOf(lowerPrompt) != -1 || lowerContent.indexOf(lowerPrompt.substring(0, min(8u, lowerPrompt.length()))) != -1) {
      facts += "[Memory] " + name + "\n" + String(content.c_str()) + "\n";
    }
  }
  root.close();

  return String(facts.c_str());
}

void recordMemoryFact(String phoneDigits, String factText) {
  if (!sdAvailable) return;
  ensureDir("/memory");
  String path = "/memory/" + phoneDigits + ".txt";
  String line = "[" + getWallClockTimestamp() + "] " + factText + "\n";
  appendTextToFile(path, line);
}

bool initSDCard() {
  Serial.printf("Initializing SD (SCK=%d MISO=%d MOSI=%d CS=%d)...\n",
                SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(10);

  // TFT uses bit-banged SPI on GPIO 10/11; SD uses its own hardware SPI bus.
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, SPI, 10000000)) {
    Serial.println("SD card not detected or unsupported format.");
    Serial.println("Tip: cards >32GB must be formatted as FAT32 (not exFAT).");
    return false;
  }

  ensureDir("/logs");
  ensureDir("/conversations");
  ensureDir("/memory");
  ensureDir("/backups");
  ensureDir("/knowledge");
  ensureDir("/personas");

  Serial.printf("SD card ready: type=%d total=%llu bytes used=%llu bytes\n",
                SD.cardType(),
                (unsigned long long)SD.totalBytes(),
                (unsigned long long)SD.usedBytes());
  return true;
}

void loadSDConfig() {
  if (!SD.exists("/config.json")) {
    Serial.println("No /config.json found on SD. Using secrets.h defaults.");
    return;
  }

  File configFile = SD.open("/config.json", FILE_READ);
  if (!configFile) return;

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, configFile);
  configFile.close();

  if (err) {
    Serial.println("Failed to parse /config.json. Using defaults.");
    return;
  }

  if (doc.containsKey("wifi_ssid"))     wifi_ssid = doc["wifi_ssid"].as<String>();
  if (doc.containsKey("wifi_pass"))     wifi_password = doc["wifi_pass"].as<String>();
  if (doc.containsKey("gemini_key"))    gemini_api_key = doc["gemini_key"].as<String>();
  if (doc.containsKey("gmail_user"))    gmail_user = doc["gmail_user"].as<String>();
  if (doc.containsKey("gmail_pass"))    gmail_pass = doc["gmail_pass"].as<String>();
  if (doc.containsKey("admin_phone"))   admin_phone = doc["admin_phone"].as<String>();
  if (doc.containsKey("sms_gateway"))   sms_gateway = doc["sms_gateway"].as<String>();
  if (doc.containsKey("default_persona")) default_persona = doc["default_persona"].as<String>();
  if (doc.containsKey("timezone")) {
    timezoneStr = doc["timezone"].as<String>();
    Serial.println("Timezone set from SD: " + timezoneStr);
  }

  if (doc.containsKey("gemini_keys") && doc["gemini_keys"].is<JsonArray>()) {
    JsonArray keyArray = doc["gemini_keys"].as<JsonArray>();
    geminiKeyCount = 0;
    for (JsonVariant v : keyArray) {
      if (geminiKeyCount >= MAX_GEMINI_KEYS) break;
      String key = v.as<String>();
      if (key.length() == 0) continue;
      geminiKeys[geminiKeyCount].key = key;
      geminiKeys[geminiKeyCount].requestsToday = 0;
      geminiKeys[geminiKeyCount].cooldownUntilMs = 0;
      geminiKeys[geminiKeyCount].failures = 0;
      geminiKeyCount++;
    }
    if (geminiKeyCount > 0) {
      gemini_api_key = geminiKeys[0].key;
      lastGeminiKeyLabel = "primary";
    }
  }

  Serial.println("Loaded config from SD Card successfully!");
}

bool saveWiFiConfig() {
  if (!sdAvailable) return false;

  DynamicJsonDocument doc(1024);
  if (SD.exists("/config.json")) {
    File current = SD.open("/config.json", FILE_READ);
    if (current) {
      DynamicJsonDocument existing(4096);
      DeserializationError err = deserializeJson(existing, current);
      if (!err) {
        if (existing.containsKey("timezone")) doc["timezone"] = existing["timezone"].as<String>();
        if (existing.containsKey("gemini_key")) doc["gemini_key"] = existing["gemini_key"].as<String>();
        if (existing.containsKey("admin_phone")) doc["admin_phone"] = existing["admin_phone"].as<String>();
        if (existing.containsKey("sms_gateway")) doc["sms_gateway"] = existing["sms_gateway"].as<String>();
        if (existing.containsKey("default_persona")) doc["default_persona"] = existing["default_persona"].as<String>();
        if (existing.containsKey("gemini_keys") && existing["gemini_keys"].is<JsonArray>()) {
          JsonArray sourceKeys = existing["gemini_keys"].as<JsonArray>();
          JsonArray targetKeys = doc.createNestedArray("gemini_keys");
          for (JsonVariant v : sourceKeys) {
            targetKeys.add(v.as<String>());
          }
        }
      }
      current.close();
    }
  }

  doc["wifi_ssid"] = wifi_ssid;
  doc["wifi_pass"] = wifi_password;
  doc["admin_phone"] = admin_phone;
  if (sms_gateway.length() > 0) doc["sms_gateway"] = sms_gateway;
  doc["default_persona"] = default_persona;

  String out;
  serializeJson(doc, out);
  return writeFileAtomic("/config.json", out);
}

void logToSD(String logLine) {
  if (!sdAvailable) return;
  String line = "[" + getWallClockTimestamp() + "] " + logLine + "\n";
  appendTextToFileWithSizeLimit("/logs/system-" + getDateTag() + ".log", line, 512 * 1024);
}

void logTrafficCSV(String sender, String text, unsigned long geminiMs, unsigned long smtpMs, int responseLen) {
  if (!sdAvailable) return;
  String csvPath = "/logs/traffic-" + getDateTag() + ".csv";
  bool isNew = !SD.exists(csvPath);
  if (isNew) {
    appendTextToFile(csvPath, "Timestamp,Sender,Message,GeminiMs,SmtpMs,EnergyWh,ResponseLen,Model,ApiKey,Rssi,Status\n");
  }

  double wh = totalEnergy_mWh / 1000.0;
  text.replace(",", " "); // strip commas for clean CSV
  String csvLine = getWallClockTimestamp() + "," + sender + ",\"" + text + "\"," + String(geminiMs) + "," + String(smtpMs) + "," + String(wh, 4) + "," + String(responseLen) + "," + lastGeminiModel + "," + lastGeminiKeyLabel + "," + String(WiFi.RSSI()) + "," + analyticsStats.lastStatus + "\n";
  appendTextToFileWithSizeLimit(csvPath, csvLine, 1024 * 1024);
}

String makeOfflineQueueEntry(const String &recipient, const String &message, int attempts, unsigned long expiresAt) {
  DynamicJsonDocument doc(1536);
  doc["recipient"] = recipient;
  doc["message"] = message;
  doc["attempts"] = attempts;
  doc["expiresAt"] = expiresAt;

  String line;
  serializeJson(doc, line);
  return line + "\n";
}

void queueOfflineMessage(String recipient, String aiResponse) {
  unsigned long expiresAt = millis() / 1000 + 86400UL;
  String entry = makeOfflineQueueEntry(recipient, aiResponse, 0, expiresAt);
  if (appendTextToFileWithSizeLimit("/logs/queue.txt", entry, 256 * 1024)) {
    Serial.println("Message queued to SD card.");
    updateScreen("Queue Saved", "Saved unsent reply to SD");
  }
}

void processOfflineQueue() {
  if (!SD.exists("/logs/queue.txt")) return;

  File qFile = SD.open("/logs/queue.txt", FILE_READ);
  if (!qFile) return;

  Serial.println("Processing queued offline messages from SD...");
  updateScreen("Queue Flush", "Sending queued replies...");

  String remainingQueue = "";

  while (qFile.available()) {
    esp_task_wdt_reset();
    String line = qFile.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    DynamicJsonDocument doc(3072);
    DeserializationError err = deserializeJson(doc, line);
    if (err) {
      Serial.println("Skipping malformed queued message line.");
      continue;
    }

    String recipient = doc["recipient"].as<String>();
    String msg = doc["message"].as<String>();
    int attempts = doc["attempts"].as<int>();
    unsigned long expiresAt = doc["expiresAt"].as<unsigned long>();
    unsigned long nowSec = millis() / 1000;

    if (expiresAt > 0 && nowSec > expiresAt) {
      continue;
    }

    if (!sendEmailReply(recipient, msg)) {
      if (attempts < 3) {
        remainingQueue += makeOfflineQueueEntry(recipient, msg, attempts + 1, expiresAt);
      }
    }
    esp_task_wdt_reset();
    delay(500);
  }
  qFile.close();

  SD.remove("/logs/queue.txt");
  if (remainingQueue.length() > 0) {
    writeFileAtomic("/logs/queue.txt", remainingQueue);
  }
}

// Full, unbounded per-sender conversation transcript. The in-memory/NVS
// history stays trimmed to MAX_HISTORY_CHARS for the Gemini prompt, but SD
// has orders of magnitude more room, so this keeps the complete record --
// useful for !VIEWHISTORY and just having a real archive.
void logConversationToSD(String phoneDigits, String userMsg, String aiMsg) {
  if (!sdAvailable) return;
  String path = "/conversations/" + phoneDigits + ".log";
  String ts = getWallClockTimestamp();
  String entry = "[" + ts + "] User: " + userMsg + "\n[" + ts + "] AI: " + aiMsg + "\n";
  appendTextToFileWithSizeLimit(path, entry, 1024 * 1024);

  String indexPath = "/conversations/" + phoneDigits + ".index.json";
  DynamicJsonDocument doc(4096);
  if (SD.exists(indexPath)) {
    File idxFile = SD.open(indexPath, FILE_READ);
    if (idxFile) {
      DeserializationError err = deserializeJson(doc, idxFile);
      if (!err) {
        idxFile.close();
      } else {
        idxFile.close();
        doc.clear();
      }
    }
  }

  JsonArray entries = doc.containsKey("entries") ? doc["entries"].as<JsonArray>() : doc.createNestedArray("entries");
  JsonObject entryObj = entries.createNestedObject();
  entryObj["timestamp"] = ts;
  entryObj["user"] = userMsg;
  entryObj["ai"] = aiMsg;
  entryObj["model"] = lastGeminiModel;
  while (entries.size() > 100) entries.remove(0);
  doc["phone"] = phoneDigits;
  doc["entryCount"] = entries.size();
  doc["lastUpdated"] = ts;

  File idxWrite = SD.open(indexPath.c_str(), FILE_WRITE);
  if (idxWrite) {
    serializeJson(doc, idxWrite);
    idxWrite.close();
  }
}

String readFullHistoryFromSD(String phoneDigits) {
  if (!sdAvailable) return "";
  String path = "/conversations/" + phoneDigits + ".log";
  if (!SD.exists(path)) return "";

  File f = SD.open(path, FILE_READ);
  if (!f) return "";

  PsramText content;
  while (f.available()) {
    content += (char)f.read();
  }
  f.close();
  return String(content.c_str());
}

// Persists the global daily rate-limit count so a power cycle can't be used
// to dodge the shared-quota cap. millis()-based windowStart can't survive a
// reboot meaningfully (it resets to ~0), so on load the window restarts from
// boot time, but the accumulated count carries over -- someone would still
// need to wait out the real ~24h window, not just power-cycle the board.
void loadGlobalRateLimit() {
  if (!sdAvailable || !SD.exists("/logs/ratelimit.dat")) return;
  File f = SD.open("/logs/ratelimit.dat", FILE_READ);
  if (!f) return;
  String line = f.readStringUntil('\n');
  f.close();

  if (line.length() == 0) return;
  int commaIdx = line.indexOf(',');
  if (commaIdx != -1) {
    globalRateCount = line.substring(0, commaIdx).toInt();
  } else {
    globalRateCount = line.toInt();
  }
  globalRateWindowStart = millis();
}

void saveGlobalRateLimit() {
  if (!sdAvailable) return;
  String line = String(globalRateCount) + "\n";
  writeFileAtomic("/logs/ratelimit.dat", line);
}

void loadGeminiUsage() {
  if (!sdAvailable || !SD.exists("/logs/gemini-usage.json")) return;
  File f = SD.open("/logs/gemini-usage.json", FILE_READ);
  if (!f) return;

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  if (doc.containsKey("day")) {
    geminiUsageDay = doc["day"].as<int>();
  }
  if (doc.containsKey("keys") && doc["keys"].is<JsonArray>()) {
    JsonArray keys = doc["keys"].as<JsonArray>();
    for (int i = 0; i < geminiKeyCount && i < (int)keys.size(); i++) {
      JsonObject keyObj = keys[i].as<JsonObject>();
      geminiKeys[i].requestsToday = keyObj["requestsToday"].as<int>();
      geminiKeys[i].failures = keyObj["failures"].as<int>();
    }
  }
}

bool saveGeminiUsage() {
  if (!sdAvailable) return false;

  DynamicJsonDocument doc(2048);
  doc["day"] = geminiUsageDay;
  JsonArray keys = doc.createNestedArray("keys");
  for (int i = 0; i < geminiKeyCount; i++) {
    JsonObject keyObj = keys.createNestedObject();
    keyObj["requestsToday"] = geminiKeys[i].requestsToday;
    keyObj["failures"] = geminiKeys[i].failures;
  }

  String out;
  serializeJson(doc, out);
  return writeFileAtomic("/logs/gemini-usage.json", out);
}

String searchConversationArchive(String phoneDigits, String keyword, String datePrefix) {
  if (!sdAvailable) return "";
  String path = "/conversations/" + phoneDigits + ".log";
  if (!SD.exists(path)) return "";

  File f = SD.open(path, FILE_READ);
  if (!f) return "";

  String results = "";
  String lowerKeyword = keyword;
  lowerKeyword.toLowerCase();
  int lineNum = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    lineNum++;
    if (line.length() == 0) continue;
    String lowerLine = line;
    lowerLine.toLowerCase();
    bool matchesKeyword = lowerKeyword.length() == 0 || lowerLine.indexOf(lowerKeyword) != -1;
    bool matchesDate = datePrefix.length() == 0 || line.indexOf(datePrefix) != -1;
    if (matchesKeyword && matchesDate) {
      results += String(lineNum) + ": " + line + "\n";
    }
  }
  f.close();
  return results;
}

String getResetReasonString() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power-on";
    case ESP_RST_EXT:       return "External reset";
    case ESP_RST_SW:        return "Software reset";
    case ESP_RST_PANIC:     return "Panic/exception";
    case ESP_RST_INT_WDT:   return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "Task watchdog";
    case ESP_RST_WDT:       return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_BROWNOUT:  return "Brownout";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "Unknown";
  }
}

// --- DYNAMIC WHITELIST NVS MANAGEMENT ---
void loadAllowedUsers() {
  prefs.begin("whitelist", true);
  dynamicUserCount = prefs.getInt("count", -1);

  if (dynamicUserCount < 0) {
    dynamicUserCount = NUM_ALLOWED_USERS;
    for (int i = 0; i < NUM_ALLOWED_USERS && i < MAX_ALLOWED_USERS; i++) {
      dynamicAllowedUsers[i].phoneDigits = normalizeDigits(ALLOWED_USERS[i].phoneDigits);
      dynamicAllowedUsers[i].systemPrompt = String(ALLOWED_USERS[i].systemPrompt);
      dynamicAllowedUsers[i].muted = false;
    }
  } else {
    for (int i = 0; i < dynamicUserCount && i < MAX_ALLOWED_USERS; i++) {
      String kPhone = "p" + String(i);
      String kPrompt = "s" + String(i);
      String kMuted = "m" + String(i);
      dynamicAllowedUsers[i].phoneDigits = prefs.getString(kPhone.c_str(), "");
      dynamicAllowedUsers[i].systemPrompt = prefs.getString(kPrompt.c_str(), default_persona);
      dynamicAllowedUsers[i].muted = prefs.getBool(kMuted.c_str(), false);
    }
  }
  prefs.end();
}

void saveAllowedUsers() {
  prefs.begin("whitelist", false);
  prefs.putInt("count", dynamicUserCount);
  for (int i = 0; i < dynamicUserCount; i++) {
    String kPhone = "p" + String(i);
    String kPrompt = "s" + String(i);
    String kMuted = "m" + String(i);
    prefs.putString(kPhone.c_str(), dynamicAllowedUsers[i].phoneDigits);
    prefs.putString(kPrompt.c_str(), dynamicAllowedUsers[i].systemPrompt);
    prefs.putBool(kMuted.c_str(), dynamicAllowedUsers[i].muted);
  }
  prefs.end();
}

void loadConversations() {
  prefs.begin("convos", true);
  for (int i = 0; i < MAX_CONVERSATIONS; i++) {
    String kUsed = "u" + String(i);
    String kPhone = "cp" + String(i);
    String kHist = "ch" + String(i);
    conversations[i].used = prefs.getBool(kUsed.c_str(), false);
    conversations[i].phoneDigits = prefs.getString(kPhone.c_str(), "");
    conversations[i].history = prefs.getString(kHist.c_str(), "");
    conversations[i].personaPrompt = "";
  }
  prefs.end();
}

void saveConversations() {
  prefs.begin("convos", false);
  for (int i = 0; i < MAX_CONVERSATIONS; i++) {
    String kUsed = "u" + String(i);
    String kPhone = "cp" + String(i);
    String kHist = "ch" + String(i);
    prefs.putBool(kUsed.c_str(), conversations[i].used);
    prefs.putString(kPhone.c_str(), conversations[i].phoneDigits);
    prefs.putString(kHist.c_str(), conversations[i].history.c_str());
  }
  prefs.end();
}

bool handleAdminCommand(String senderDigits, String textBody, String replyAddress) {
  String trimmed = textBody;
  trimmed.trim();

  if (!trimmed.startsWith("!")) return false;
  trimmed = trimmed.substring(1);
  trimmed.trim();

  String upperCmd = trimmed;
  upperCmd.toUpperCase();

  if (upperCmd == "CLEAR") {
    for (int i = 0; i < MAX_CONVERSATIONS; i++) {
      if (conversations[i].used && conversations[i].phoneDigits == senderDigits) {
        conversations[i].history = "";
        break;
      }
    }
    saveConversations();
    sendEmailReply(replyAddress, "Your conversation history has been cleared.");
    updateScreen("User Cmd", "History Cleared");
    return true;
  }

  if (upperCmd == "STATUS") {
    unsigned long uptimeSec = millis() / 1000;
    unsigned long days = uptimeSec / 86400;
    unsigned long hours = (uptimeSec % 86400) / 3600;
    unsigned long mins = (uptimeSec % 3600) / 60;
    String statusMsg = "Up " + String(days) + "d " + String(hours) + "h " + String(mins) + "m | " +
                       getPowerStatusString() + " | Heap=" + String(ESP.getFreeHeap()) +
                       " | PSRAM=" + String(psramFound() ? ESP.getFreePsram() : 0) +
                       " | " + getGeminiCacheStatsString() +
                       " | SD: " + String(sdAvailable ? "OK" : "NO") +
                       (getPausedState() ? " | PAUSED" : "");
    sendEmailReply(replyAddress, statusMsg);
    updateScreen("User Cmd", "Sent Status");
    return true;
  }

  if (upperCmd == "HELP") {
    String adminCheckTarget = normalizeDigits(admin_phone);
    bool senderIsAdmin = adminCheckTarget.length() == 0 || senderDigits == adminCheckTarget;
    String helpMsg = "Commands: !status !clear !help !explain";
    if (senderIsAdmin) helpMsg += " | Admin: !ADD !DEL !LIST !RESET !MUTE !UNMUTE !VIEWHISTORY !USAGE !BACKUP !RESTORE !REPORT !MEMORY !KNOWLEDGE !SEARCH !KEYS !SCREENDIM !SCREENBRIGHT !SCREENOFF !SCREENON !SENDQUEUE !PAUSE !WIFI !OVERRIDE !CANCELOVERRIDE !OVERRIDESTATUS";
    sendEmailReply(replyAddress, helpMsg);
    updateScreen("User Cmd", "Sent Help");
    return true;
  }

  if (upperCmd == "EXPLAIN") {
    String adminCheckTarget = normalizeDigits(admin_phone);
    bool senderIsAdmin = adminCheckTarget.length() == 0 || senderDigits == adminCheckTarget;
    String lines[32];
    int lineCount = 0;
    lines[lineCount++] = "User commands:";
    lines[lineCount++] = "!status - show uptime, power, and SD status";
    lines[lineCount++] = "!clear - clear your current conversation history";
    lines[lineCount++] = "!help - list available commands";
    lines[lineCount++] = "!explain - show detailed help for commands you can use";
    if (senderIsAdmin) {
      lines[lineCount++] = "";
      lines[lineCount++] = "Admin commands:";
      lines[lineCount++] = "!ADD <phone> [persona] - add or update an allowed sender";
      lines[lineCount++] = "!DEL <phone> - remove an allowed sender";
      lines[lineCount++] = "!LIST - list allowed users";
      lines[lineCount++] = "!RESET - clear whitelist and restore defaults";
      lines[lineCount++] = "!MUTE <phone> - silence a user";
      lines[lineCount++] = "!UNMUTE <phone> - restore a user";
      lines[lineCount++] = "!VIEWHISTORY <phone> - view conversation history for a sender";
      lines[lineCount++] = "!USAGE - show current rate limit usage";
      lines[lineCount++] = "!BACKUP - save admin state to SD";
      lines[lineCount++] = "!RESTORE - restore admin state from SD";
      lines[lineCount++] = "!REPORT - write analytics report to SD";
      lines[lineCount++] = "!SENDQUEUE - resend any queued replies from SD";
      lines[lineCount++] = "!MEMORY <fact> - store an admin memory fact";
      lines[lineCount++] = "!KNOWLEDGE <keyword> - search SD knowledge files";
      lines[lineCount++] = "!SEARCH <phone> <keyword> [date] - search archived conversations";
      lines[lineCount++] = "!KEYS - show Gemini API key usage status";
      lines[lineCount++] = "!PAUSE - request pause; reply with confirmation code";
      lines[lineCount++] = "!SCREENDIM - dim the display";
      lines[lineCount++] = "!SCREENBRIGHT - set full display brightness";
      lines[lineCount++] = "!SCREENOFF - turn the display off";
      lines[lineCount++] = "!SCREENON - turn the display on";
      lines[lineCount++] = "!SCREENINVERT - toggle active-high/active-low backlight";
      lines[lineCount++] = "!PAUSE - request pause; reply with 1234 to confirm";
      lines[lineCount++] = "!WIFI <ssid> <password> - update Wi-Fi credentials";
      lines[lineCount++] = "!OVERRIDE <phone> <minutes> - temporarily allow a sender";
      lines[lineCount++] = "!CANCELOVERRIDE - cancel any active override";
      lines[lineCount++] = "!OVERRIDESTATUS - show override status";
    }
    String chunk = "";
    for (int i = 0; i < lineCount; i++) {
      if (chunk.length() + lines[i].length() + 2 > 220) {
        sendEmailReply(replyAddress, chunk);
        chunk = "";
      }
      if (chunk.length() > 0) chunk += "\n";
      chunk += lines[i];
    }
    if (chunk.length() > 0) {
      sendEmailReply(replyAddress, chunk);
    }
    updateScreen("User Cmd", "Sent Explain");
    return true;
  }

  String adminTarget = normalizeDigits(admin_phone);
  bool isAdmin = adminTarget.length() == 0 || senderDigits == adminTarget;
  if (!isAdmin) {
    sendEmailReply(replyAddress, "Not authorized for that command.");
    updateScreen("User Cmd", "Unauthorized attempt");
    return true;
  }

  if (!enforceAdminRateLimit(replyAddress)) {
    updateScreen("Admin Cmd", "Rate limited");
    return true;
  }

  if (upperCmd.startsWith("PAUSE")) {
    String args = trimmed.substring(5);
    args.trim();

    if (args.length() == 0) {
      if (pendingAdminAction.active && pendingAdminAction.action == "PAUSE" && millis() < pendingAdminAction.expiresAt) {
        sendEmailReply(replyAddress, "Confirm pause by replying: !PAUSE " + pendingAdminAction.code);
        updateScreen("Admin Cmd", "Pause pending");
      } else {
        pendingAdminAction.action = "PAUSE";
        pendingAdminAction.args = "";
        pendingAdminAction.code = generateAdminConfirmationCode();
        pendingAdminAction.expiresAt = millis() + 300000UL;
        pendingAdminAction.active = true;
        sendEmailReply(replyAddress, "Reply with !PAUSE " + pendingAdminAction.code + " within 5 minutes to pause the system.");
        updateScreen("Admin Cmd", "Pause requested");
      }
      return true;
    }

    if (pendingAdminAction.active && pendingAdminAction.action == "PAUSE" && args == pendingAdminAction.code && millis() < pendingAdminAction.expiresAt) {
      setPausedState(true);
      clearPendingAdminAction();
      sendEmailReply(replyAddress, "System paused. Press BOOT button to resume.");
      updateScreen("Admin Cmd", "System Paused");
    } else {
      sendEmailReply(replyAddress, "Invalid pause confirmation code. Send !PAUSE to request a new code.");
      updateScreen("Admin Cmd", "Pause failed");
    }
    return true;
  }

  if (upperCmd.startsWith("ADD ")) {
    String rest = trimmed.substring(4);
    rest.trim();
    int spaceIdx = rest.indexOf(' ');
    String phonePart = (spaceIdx == -1) ? rest : rest.substring(0, spaceIdx);
    String personaPart = (spaceIdx == -1) ? default_persona : rest.substring(spaceIdx + 1);
    personaPart.trim();
    if (personaPart.length() == 0) personaPart = default_persona;
    String newDigits = normalizeDigits(phonePart);
    if (newDigits.length() >= 7) {
      bool updated = false;
      for (int i = 0; i < dynamicUserCount; i++) {
        if (dynamicAllowedUsers[i].phoneDigits == newDigits) {
          dynamicAllowedUsers[i].systemPrompt = personaPart;
          updated = true;
          break;
        }
      }
      if (!updated && dynamicUserCount < MAX_ALLOWED_USERS) {
        dynamicAllowedUsers[dynamicUserCount].phoneDigits = newDigits;
        dynamicAllowedUsers[dynamicUserCount].systemPrompt = personaPart;
        dynamicAllowedUsers[dynamicUserCount].muted = false;
        dynamicUserCount++;
      }
      saveAllowedUsers();
      String respMsg = updated ? ("Updated user " + newDigits) : ("Added user " + newDigits);
      sendEmailReply(replyAddress, respMsg);
      updateScreen("Admin Cmd", respMsg);
      return true;
    } else {
      sendEmailReply(replyAddress, "Err: Invalid phone number.");
      return true;
    }
  }
  else if (upperCmd.startsWith("DEL ") || upperCmd.startsWith("REMOVE ")) {
    int spaceIdx = trimmed.indexOf(' ');
    String phonePart = trimmed.substring(spaceIdx + 1);
    phonePart.trim();
    String targetDigits = normalizeDigits(phonePart);
    bool found = false;
    for (int i = 0; i < dynamicUserCount; i++) {
      if (dynamicAllowedUsers[i].phoneDigits == targetDigits) {
        for (int j = i; j < dynamicUserCount - 1; j++) {
          dynamicAllowedUsers[j] = dynamicAllowedUsers[j + 1];
        }
        dynamicUserCount--;
        found = true;
        break;
      }
    }
    if (found) {
      saveAllowedUsers();
      sendEmailReply(replyAddress, "Removed " + targetDigits);
      updateScreen("Admin Cmd", "Removed " + targetDigits);
    } else {
      sendEmailReply(replyAddress, "User " + targetDigits + " not found.");
    }
    return true;
  }
  else if (upperCmd == "LIST") {
    String listMsg = "Users (" + String(dynamicUserCount) + "): ";
    for (int i = 0; i < dynamicUserCount; i++) {
      listMsg += dynamicAllowedUsers[i].phoneDigits;
      if (dynamicAllowedUsers[i].muted) listMsg += "(muted)";
      listMsg += " ";
    }
    sendEmailReply(replyAddress, listMsg);
    updateScreen("Admin Cmd", "Sent User List");
    return true;
  }
  else if (upperCmd.startsWith("MUTE ")) {
    String targetDigits = normalizeDigits(trimmed.substring(5));
    bool found = false;
    for (int i = 0; i < dynamicUserCount; i++) {
      if (dynamicAllowedUsers[i].phoneDigits == targetDigits) {
        dynamicAllowedUsers[i].muted = true;
        found = true;
        break;
      }
    }
    if (found) {
      saveAllowedUsers();
      sendEmailReply(replyAddress, "Muted " + targetDigits);
      updateScreen("Admin Cmd", "Muted " + targetDigits);
    } else {
      sendEmailReply(replyAddress, "User " + targetDigits + " not found.");
    }
    return true;
  }
  else if (upperCmd.startsWith("UNMUTE ")) {
    String targetDigits = normalizeDigits(trimmed.substring(7));
    bool found = false;
    for (int i = 0; i < dynamicUserCount; i++) {
      if (dynamicAllowedUsers[i].phoneDigits == targetDigits) {
        dynamicAllowedUsers[i].muted = false;
        found = true;
        break;
      }
    }
    if (found) {
      saveAllowedUsers();
      sendEmailReply(replyAddress, "Unmuted " + targetDigits);
      updateScreen("Admin Cmd", "Unmuted " + targetDigits);
    } else {
      sendEmailReply(replyAddress, "User " + targetDigits + " not found.");
    }
    return true;
  }
  else if (upperCmd == "VIEWHISTORY") {
    String listMsg = "";
    bool any = false;
    for (int i = 0; i < MAX_CONVERSATIONS; i++) {
      if (conversations[i].used) {
        listMsg += conversations[i].phoneDigits + "(" + String(conversations[i].history.length()) + "c) ";
        any = true;
      }
    }
    sendEmailReply(replyAddress, any ? ("Active convos: " + listMsg) : "No active conversations.");
    updateScreen("Admin Cmd", "Sent Convo List");
    return true;
  }
  else if (upperCmd.startsWith("VIEWHISTORY ")) {
    String targetDigits = normalizeDigits(trimmed.substring(12));

    // Prefer the full, unbounded SD transcript over the NVS-trimmed copy
    String fullHist = readFullHistoryFromSD(targetDigits);
    String hist = fullHist.length() > 0 ? fullHist : "";

    if (hist.length() == 0) {
      for (int i = 0; i < MAX_CONVERSATIONS; i++) {
        if (conversations[i].used && conversations[i].phoneDigits == targetDigits) {
          hist = conversations[i].history;
          break;
        }
      }
    }

    if (hist.length() == 0) {
      sendEmailReply(replyAddress, "No conversation found for " + targetDigits);
    } else {
      const int chunkSize = 280;
      int totalLen = hist.length();
      int numChunks = (totalLen + chunkSize - 1) / chunkSize;
      for (int c = 0; c < numChunks; c++) {
        int chunkStart = c * chunkSize;
        int chunkEnd = chunkStart + chunkSize;
        if (chunkEnd > totalLen) chunkEnd = totalLen;
        String label = "[" + String(c + 1) + "/" + String(numChunks) + "] ";
        sendEmailReply(replyAddress, label + hist.substring(chunkStart, chunkEnd));
      }
    }
    updateScreen("Admin Cmd", "Sent History: " + targetDigits);
    return true;
  }
  else if (upperCmd == "USAGE") {
    unsigned long now = millis();
    unsigned long globalElapsed = now - globalRateWindowStart;
    unsigned long globalRemainingHrs = (globalElapsed < GLOBAL_WINDOW_MS) ? ((GLOBAL_WINDOW_MS - globalElapsed) / 3600000UL) : 0;

    String usageMsg = "Global: " + String(globalRateCount) + "/" + String(MAX_GLOBAL_MSGS_PER_DAY) +
                       " (resets ~" + String(globalRemainingHrs) + "h) | ";
    bool any = false;
    for (int i = 0; i < MAX_CONVERSATIONS; i++) {
      if (conversations[i].used) {
        usageMsg += conversations[i].phoneDigits + ":" + String(conversations[i].rateCount) + "/" + String(MAX_MSGS_PER_SENDER_PER_HOUR) + " ";
        any = true;
      }
    }
    if (!any) usageMsg += "(no active senders)";

    sendEmailReply(replyAddress, usageMsg);
    updateScreen("Admin Cmd", "Sent Usage Stats");
    return true;
  }
  else if (upperCmd.startsWith("RESET")) {
    String args = trimmed.substring(5);
    args.trim();
    if (args.length() == 0) {
      if (pendingAdminAction.active && pendingAdminAction.action == "RESET" && millis() < pendingAdminAction.expiresAt) {
        sendEmailReply(replyAddress, "Confirm whitelist reset by replying: !RESET " + pendingAdminAction.code);
        updateScreen("Admin Cmd", "Reset pending");
      } else {
        pendingAdminAction.action = "RESET";
        pendingAdminAction.args = "";
        pendingAdminAction.code = generateAdminConfirmationCode();
        pendingAdminAction.expiresAt = millis() + 300000UL;
        pendingAdminAction.active = true;
        sendEmailReply(replyAddress, "Reply with !RESET " + pendingAdminAction.code + " within 5 minutes to reset the whitelist.");
        updateScreen("Admin Cmd", "Reset requested");
      }
      return true;
    }

    if (pendingAdminAction.active && pendingAdminAction.action == "RESET" && args == pendingAdminAction.code && millis() < pendingAdminAction.expiresAt) {
      prefs.begin("whitelist", false);
      prefs.clear();
      prefs.end();
      loadAllowedUsers();
      clearPendingAdminAction();
      sendEmailReply(replyAddress, "Whitelist reset to defaults. (Tip: !BACKUP before a RESET next time to make this reversible.)");
      updateScreen("Admin Cmd", "Whitelist Reset");
    } else {
      sendEmailReply(replyAddress, "Invalid reset confirmation code. Send !RESET to request a new code.");
      updateScreen("Admin Cmd", "Reset failed");
    }
    return true;
  }
  else if (upperCmd.startsWith("REPORT")) {
    writeAnalyticsReport();
    String reportPath = "/logs/reports/" + getDateTag() + ".json";
    if (SD.exists(reportPath)) {
      sendEmailReply(replyAddress, "Report written to " + reportPath);
    } else {
      sendEmailReply(replyAddress, "Report generation failed.");
    }
    updateScreen("Admin Cmd", "Report Saved");
    return true;
  }
  else if (upperCmd == "MEMORY") {
    String rest = trimmed.substring(7);
    rest.trim();
    if (rest.length() == 0) {
      sendEmailReply(replyAddress, "Usage: !MEMORY <fact>");
    } else {
      recordMemoryFact(senderDigits, rest);
      sendEmailReply(replyAddress, "Stored memory fact.");
    }
    updateScreen("Admin Cmd", "Memory Saved");
    return true;
  }
  else if (upperCmd == "KNOWLEDGE") {
    String rest = trimmed.substring(9);
    rest.trim();
    if (rest.length() == 0) {
      sendEmailReply(replyAddress, "Usage: !KNOWLEDGE <keyword>");
    } else {
      String knowledge = loadRelevantKnowledge(rest);
      if (knowledge.length() > 0) {
        sendEmailReply(replyAddress, knowledge.substring(0, min(280u, knowledge.length())));
      } else {
        sendEmailReply(replyAddress, "No relevant SD knowledge found.");
      }
    }
    updateScreen("Admin Cmd", "Knowledge Lookup");
    return true;
  }
  else if (upperCmd.startsWith("SEARCH ")) {
    String rest = trimmed.substring(7);
    rest.trim();
    int firstSpace = rest.indexOf(' ');
    String targetPhone = firstSpace == -1 ? "" : rest.substring(0, firstSpace);
    String keyword = firstSpace == -1 ? "" : rest.substring(firstSpace + 1);
    keyword.trim();
    String datePrefix = "";
    int secondSpace = keyword.indexOf(' ');
    if (secondSpace != -1) {
      datePrefix = keyword.substring(secondSpace + 1);
      keyword = keyword.substring(0, secondSpace);
    }
    String results = searchConversationArchive(targetPhone, keyword, datePrefix);
    if (results.length() == 0) {
      sendEmailReply(replyAddress, "No archive matches found.");
    } else {
      sendEmailReply(replyAddress, results.substring(0, min(280u, results.length())));
    }
    updateScreen("Admin Cmd", "Archive Search");
    return true;
  }
  else if (upperCmd == "KEYS") {
    String keyStatus = "Active key: " + lastGeminiKeyLabel + " | ";
    for (int i = 0; i < geminiKeyCount; i++) {
      unsigned long now = millis();
      unsigned long remainingCooldownMs = (geminiKeys[i].cooldownUntilMs > now) ? (geminiKeys[i].cooldownUntilMs - now) : 0;
      keyStatus += String(i + 1) + ":" + String(geminiKeys[i].requestsToday) + "/day";
      if (remainingCooldownMs > 0) {
        keyStatus += " cool=" + String(remainingCooldownMs / 1000) + "s";
      }
      keyStatus += " ";
    }
    sendEmailReply(replyAddress, keyStatus);
    updateScreen("Admin Cmd", "Sent Key Status");
    return true;
  }
  else if (upperCmd == "WIFI" || upperCmd.startsWith("WIFI ")) {
    String rest = trimmed.substring(4);
    rest.trim();
    if (rest.length() == 0) {
      String status = "SSID=" + wifi_ssid + " | Connected=" + String(WiFi.status() == WL_CONNECTED ? "yes" : "no");
      sendEmailReply(replyAddress, status);
      updateScreen("Admin Cmd", "Wi-Fi Status");
      return true;
    }

    int firstSpaceIdx = rest.indexOf(' ');
    if (firstSpaceIdx == -1) {
      sendEmailReply(replyAddress, "Usage: !WIFI <ssid> <password>");
      return true;
    }

    String newSsid = rest.substring(0, firstSpaceIdx);
    String newPass = rest.substring(firstSpaceIdx + 1);
    newSsid.trim();
    newPass.trim();
    if (newSsid.length() == 0 || newPass.length() == 0) {
      sendEmailReply(replyAddress, "Usage: !WIFI <ssid> <password>");
      return true;
    }

    wifi_ssid = newSsid;
    wifi_password = newPass;
    bool saved = saveWiFiConfig();
    String resp = "Wi-Fi updated";
    if (saved) resp += " and saved to SD.";
    else resp += " but config not saved (SD unavailable).";
    sendEmailReply(replyAddress, resp);
    updateScreen("Admin Cmd", resp);

    WiFi.disconnect();
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
    unsigned long retryStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - retryStart < 20000) {
      delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setSleep(false);
      sendEmailReply(replyAddress, "Reconnected to Wi-Fi.");
      updateScreen("Admin Cmd", "Wi-Fi Reconnected");
    } else {
      sendEmailReply(replyAddress, "Failed to reconnect after Wi-Fi update.");
      updateScreen("Admin Cmd", "Wi-Fi Update Failed");
    }
    return true;
  }
  else if (upperCmd == "SCREENDIM") {
    setDisplayBrightness(DIM_SCREEN_BRIGHTNESS);
    sendEmailReply(replyAddress, "Screen dimmed.");
    updateScreen("Admin Cmd", "Screen Dimmed");
    return true;
  }
  else if (upperCmd == "SCREENBRIGHT") {
    setDisplayBrightness(DEFAULT_SCREEN_BRIGHTNESS);
    sendEmailReply(replyAddress, "Screen brightness set to maximum.");
    updateScreen("Admin Cmd", "Screen Bright");
    return true;
  }
  else if (upperCmd == "SCREENOFF") {
    setDisplayEnabled(false);
    sendEmailReply(replyAddress, "Screen turned off.");
    updateScreen("Admin Cmd", "Screen Off");
    return true;
  }
  else if (upperCmd == "SCREENON") {
    setDisplayEnabled(true);
    sendEmailReply(replyAddress, "Screen turned on.");
    updateScreen("Admin Cmd", "Screen On");
    return true;
  }
  else if (upperCmd.startsWith("SCREENPIN ")) {
    String pinStr = trimmed.substring(10);
    pinStr.trim();
    int newPin = pinStr.toInt();
    if (newPin <= 0) {
      sendEmailReply(replyAddress, "Usage: !SCREENPIN <gpio>");
      return true;
    }
    screenBacklightPin = newPin;
    pinMode(screenBacklightPin, OUTPUT);
    setDisplayBrightness(screenBrightness);
    sendEmailReply(replyAddress, "Backlight pin set to GPIO " + String(screenBacklightPin));
    updateScreen("Admin Cmd", "Backlight pin " + String(screenBacklightPin));
    return true;
  }
  else if (upperCmd == "SCREENSTATUS") {
    String status = "Backlight pin=" + String(screenBacklightPin)
      + " enabled=" + String(screenEnabled ? "yes" : "no")
      + " brightness=" + String(screenBrightness)
      + " activeLow=" + String(screenBacklightActiveLow ? "yes" : "no")
      + " effectiveDuty=" + String(getBacklightOutputValue(screenEnabled ? screenBrightness : 0));
    sendEmailReply(replyAddress, status);
    updateScreen("Admin Cmd", "Backlight status");
    return true;
  }
  else if (upperCmd == "SCREENINVERT") {
    screenBacklightActiveLow = !screenBacklightActiveLow;
    setDisplayBrightness(screenBrightness);
    String polarity = screenBacklightActiveLow ? "active-low" : "active-high";
    sendEmailReply(replyAddress, "Backlight polarity set to " + polarity + ".");
    updateScreen("Admin Cmd", "Backlight " + polarity);
    return true;
  }
  else if (upperCmd == "SENDQUEUE") {
    if (!sdAvailable) {
      sendEmailReply(replyAddress, "SD card unavailable; cannot process queued messages.");
      updateScreen("Admin Cmd", "SendQueue failed");
      return true;
    }
    if (!SD.exists("/logs/queue.txt")) {
      sendEmailReply(replyAddress, "No queued messages found on SD.");
      updateScreen("Admin Cmd", "SendQueue empty");
      return true;
    }
    processOfflineQueue();
    sendEmailReply(replyAddress, "Queued messages processed from SD.");
    updateScreen("Admin Cmd", "SendQueue processed");
    return true;
  }
  else if (upperCmd.startsWith("OVERRIDE ")) {
    String rest = trimmed.substring(9);
    rest.trim();
    int spaceIdx = rest.indexOf(' ');
    String phonePart = (spaceIdx == -1) ? rest : rest.substring(0, spaceIdx);
    String minutesPart = (spaceIdx == -1) ? "" : rest.substring(spaceIdx + 1);
    phonePart.trim();
    minutesPart.trim();
    String targetDigits = normalizeDigits(phonePart);
    unsigned long durationMs = OVERRIDE_DURATION_MS;
    if (minutesPart.length() > 0) {
      int mins = minutesPart.toInt();
      if (mins > 0) durationMs = (unsigned long)mins * 60000UL;
    }
    if (targetDigits.length() < 7) {
      sendEmailReply(replyAddress, "Err: Invalid phone number.");
      return true;
    }
    overridePhoneDigits = targetDigits;
    overrideExpiresAtMs = millis() + durationMs;
    sendEmailReply(replyAddress, "Override enabled for " + targetDigits + " for " + String(durationMs / 60000UL) + " minutes.");
    updateScreen("Admin Cmd", "Override " + targetDigits);
    return true;
  }
  else if (upperCmd == "CANCELOVERRIDE") {
    overridePhoneDigits = "";
    overrideExpiresAtMs = 0;
    sendEmailReply(replyAddress, "Override canceled.");
    updateScreen("Admin Cmd", "Override Canceled");
    return true;
  }
  else if (upperCmd == "OVERRIDESTATUS") {
    if (isOverrideActive(overridePhoneDigits)) {
      unsigned long remaining = (overrideExpiresAtMs > millis()) ? ((overrideExpiresAtMs - millis()) / 60000UL) : 0;
      sendEmailReply(replyAddress, "Override active for " + overridePhoneDigits + " (" + String(remaining) + " min left).");
    } else {
      sendEmailReply(replyAddress, "No active override.");
    }
    updateScreen("Admin Cmd", "Override Status");
    return true;
  }
  else if (upperCmd == "BACKUP") {
    if (!sdAvailable) {
      sendEmailReply(replyAddress, "No SD card available for backup.");
      return true;
    }

    DynamicJsonDocument doc(8192);
    JsonArray users = doc.createNestedArray("users");
    for (int i = 0; i < dynamicUserCount; i++) {
      JsonObject u = users.createNestedObject();
      u["phone"] = dynamicAllowedUsers[i].phoneDigits;
      u["persona"] = dynamicAllowedUsers[i].systemPrompt;
      u["muted"] = dynamicAllowedUsers[i].muted;
    }

    JsonArray convos = doc.createNestedArray("conversations");
    for (int i = 0; i < MAX_CONVERSATIONS; i++) {
      if (conversations[i].used) {
        JsonObject c = convos.createNestedObject();
        c["phone"] = conversations[i].phoneDigits;
        c["history"] = conversations[i].history;
      }
    }

    SD.remove("/backup.json");
    File f = SD.open("/backup.json", FILE_WRITE);
    if (f) {
      serializeJson(doc, f);
      f.close();
      writeBackupArchive();
      sendEmailReply(replyAddress, "Backup saved to /backup.json and /backups/ (" + String(dynamicUserCount) + " users).");
      updateScreen("Admin Cmd", "Backup Saved");
    } else {
      sendEmailReply(replyAddress, "Failed to write backup file.");
    }
    return true;
  }
  else if (upperCmd.startsWith("RESTORE")) {
    String args = trimmed.substring(7);
    args.trim();
    if (args.length() == 0) {
      if (pendingAdminAction.active && pendingAdminAction.action == "RESTORE" && millis() < pendingAdminAction.expiresAt) {
        sendEmailReply(replyAddress, "Confirm restore by replying: !RESTORE " + pendingAdminAction.code);
        updateScreen("Admin Cmd", "Restore pending");
      } else {
        pendingAdminAction.action = "RESTORE";
        pendingAdminAction.args = "";
        pendingAdminAction.code = generateAdminConfirmationCode();
        pendingAdminAction.expiresAt = millis() + 300000UL;
        pendingAdminAction.active = true;
        sendEmailReply(replyAddress, "Reply with !RESTORE " + pendingAdminAction.code + " within 5 minutes to restore from SD backup.");
        updateScreen("Admin Cmd", "Restore requested");
      }
      return true;
    }

    if (!(pendingAdminAction.active && pendingAdminAction.action == "RESTORE" && args == pendingAdminAction.code && millis() < pendingAdminAction.expiresAt)) {
      sendEmailReply(replyAddress, "Invalid restore confirmation code. Send !RESTORE to request a new code.");
      updateScreen("Admin Cmd", "Restore failed");
      return true;
    }

    if (!sdAvailable || !SD.exists("/backup.json")) {
      clearPendingAdminAction();
      sendEmailReply(replyAddress, "No backup file found on SD.");
      return true;
    }

    File f = SD.open("/backup.json", FILE_READ);
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
      clearPendingAdminAction();
      sendEmailReply(replyAddress, "Backup file is corrupted, restore failed.");
      return true;
    }

    dynamicUserCount = 0;
    for (JsonObject u : doc["users"].as<JsonArray>()) {
      if (dynamicUserCount >= MAX_ALLOWED_USERS) break;
      dynamicAllowedUsers[dynamicUserCount].phoneDigits = u["phone"].as<String>();
      dynamicAllowedUsers[dynamicUserCount].systemPrompt = u["persona"].as<String>();
      dynamicAllowedUsers[dynamicUserCount].muted = u["muted"].as<bool>();
      dynamicUserCount++;
    }
    saveAllowedUsers();

    for (int i = 0; i < MAX_CONVERSATIONS; i++) conversations[i].used = false;
    int idx = 0;
    for (JsonObject c : doc["conversations"].as<JsonArray>()) {
      if (idx >= MAX_CONVERSATIONS) break;
      conversations[idx].used = true;
      conversations[idx].phoneDigits = c["phone"].as<String>();
      conversations[idx].history = c["history"].as<String>();
      conversations[idx].personaPrompt = "";
      idx++;
    }
    saveConversations();
    clearPendingAdminAction();

    sendEmailReply(replyAddress, "Restored " + String(dynamicUserCount) + " users, " + String(idx) + " conversations.");
    updateScreen("Admin Cmd", "Backup Restored");
    return true;
  }
  return false;
}

// --- BUTTON DEBOUNCE & PAUSE LOGIC ---
void checkButton() {
  static int stableState = HIGH;
  static int lastReading = HIGH;
  static unsigned long lastChangeMs = 0;
  static unsigned long pressStartMs = 0;

  int reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) {
    lastChangeMs = millis();
    lastReading = reading;
  }

  if ((millis() - lastChangeMs) <= DEBOUNCE_DELAY) {
    return;
  }

  if (reading == stableState) {
    return;
  }

  stableState = reading;
  if (stableState == LOW) {
    pressStartMs = millis();
    markUiActivity();
    return;
  }

  unsigned long heldMs = millis() - pressStartMs;
  if (getPausedState()) {
    if (heldMs >= 700UL) {
      setPausedState(false);
      Serial.println(">> SYSTEM RESUMED VIA BUTTON <<");
      updateScreen("AI Relay", "Resumed & Listening...");
      refreshCurrentScreen();
    } else {
      cyclePausedStatusPage();
      refreshCurrentScreen();
    }
  } else {
    setPausedState(true);
    Serial.println(">> SYSTEM PAUSED VIA BUTTON <<");
    updateScreen("PAUSED", "Press BOOT button to resume");
    refreshCurrentScreen();
  }
}

void responsiveDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    delay(50);
  }
}

void runUiIteration() {
  esp_task_wdt_reset();
  checkButton();
  trackPower();

  unsigned long now = millis();
  bool paused = getPausedState();
  if (uiStateMutex && xSemaphoreTake(uiStateMutex, portMAX_DELAY) == pdTRUE) {
    if (!paused && !uiState.autoDimmed && now - uiState.lastUiActivityMs > 180000UL) {
      uiState.autoDimmed = true;
      setDisplayBrightness(DIM_SCREEN_BRIGHTNESS);
      uiState.screenDirty = true;
    }
    if (paused && uiState.screenDirty) {
      uiState.screenDirty = true;
    }
    xSemaphoreGive(uiStateMutex);
  }

  if (uiStateMutex && xSemaphoreTake(uiStateMutex, portMAX_DELAY) == pdTRUE) {
    bool dirty = uiState.screenDirty;
    xSemaphoreGive(uiStateMutex);
    if (dirty) {
      renderScreenPage();
    }
  }
}

void uiTask(void *parameter) {
  (void)parameter;
  esp_task_wdt_add(NULL);
  for (;;) {
    esp_task_wdt_reset();
    runUiIteration();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void runNetworkIteration();

void networkTask(void *parameter) {
  (void)parameter;
  esp_task_wdt_add(NULL);
  for (;;) {
    esp_task_wdt_reset();
    runNetworkIteration();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// --- PERSONA & WHITELIST HELPERS ---
bool isSenderAllowed(String sender, String subject, String &matchedDigitsOut) {
  if (dynamicUserCount == 0) return false;
  // Google Voice notification subjects look like "New text message from
  // <number>" -- that's where the real phone number lives. Checking sender
  // and subject SEPARATELY (rather than concatenating them into one digit
  // blob) and requiring an EXACT normalized match avoids the old substring
  // bug, where a short/overlapping allowed number could match digits that
  // happened to appear anywhere in the combined string (e.g. in a message
  // ID inside the From header).
  String subjectDigits = normalizeDigits(subject);
  String senderDigits = normalizeDigits(sender);
  for (int i = 0; i < dynamicUserCount; i++) {
    String allowedTarget = dynamicAllowedUsers[i].phoneDigits;
    if (allowedTarget.length() == 0) continue;
    if (subjectDigits == allowedTarget || senderDigits == allowedTarget) {
      matchedDigitsOut = allowedTarget;
      return true;
    }
  }
  return false;
}

String getPersonaForSender(String senderDigits) {
  for (int i = 0; i < dynamicUserCount; i++) {
    if (dynamicAllowedUsers[i].phoneDigits == senderDigits) {
      return dynamicAllowedUsers[i].systemPrompt;
    }
  }
  return default_persona;
}

int findOrCreateConversation(String phoneDigits, String personaPrompt) {
  for (int i = 0; i < MAX_CONVERSATIONS; i++) {
    if (conversations[i].used && conversations[i].phoneDigits == phoneDigits) {
      conversations[i].personaPrompt = personaPrompt;
      return i;
    }
  }
  for (int i = 0; i < MAX_CONVERSATIONS; i++) {
    if (!conversations[i].used) {
      conversations[i].used = true;
      conversations[i].phoneDigits = phoneDigits;
      conversations[i].personaPrompt = personaPrompt;
      conversations[i].history.clear();
      return i;
    }
  }
  conversations[0].used = true;
  conversations[0].phoneDigits = phoneDigits;
  conversations[0].personaPrompt = personaPrompt;
  conversations[0].history.clear();
  return 0;
}

void appendToHistory(int idx, String userMsg, String aiMsg) {
  conversations[idx].history += "User: " + userMsg + "\nAssistant: " + aiMsg + "\n";
  while (conversations[idx].history.length() > MAX_HISTORY_CHARS) {
    int nextUserIdx = conversations[idx].history.indexOf("User: ", 6);
    if (nextUserIdx == -1) {
      conversations[idx].history.clear();
      break;
    }
    conversations[idx].history = conversations[idx].history.substring(nextUserIdx);
  }
}

bool checkRateLimit(int convoIdx, String senderDigits, String &reasonOut) {
  unsigned long now = millis();
  if (now - globalRateWindowStart > GLOBAL_WINDOW_MS) {
    globalRateWindowStart = now;
    globalRateCount = 0;
  }
  if (globalRateCount >= MAX_GLOBAL_MSGS_PER_DAY) {
    reasonOut = "Daily AI quota reached for all users. Try again later.";
    return false;
  }
  if (isOverrideActive(senderDigits)) {
    return true;
  }
  if (now - conversations[convoIdx].rateWindowStart > RATE_WINDOW_MS) {
    conversations[convoIdx].rateWindowStart = now;
    conversations[convoIdx].rateCount = 0;
  }
  if (conversations[convoIdx].rateCount >= MAX_MSGS_PER_SENDER_PER_HOUR) {
    reasonOut = "You've hit the hourly message limit. Try again in a bit.";
    return false;
  }
  return true;
}

void recordRateLimitUsage(int convoIdx) {
  globalRateCount++;
  conversations[convoIdx].rateCount++;
}

String normalizeForFingerprint(String text) {
  text.toLowerCase();
  String out = "";
  bool lastWasSpace = true;
  for (size_t i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if (isalnum((unsigned char)c)) {
      out += c;
      lastWasSpace = false;
    } else if (isspace((unsigned char)c)) {
      if (!lastWasSpace) {
        out += ' ';
        lastWasSpace = true;
      }
    }
  }
  out.trim();
  return out;
}

String getConversationPromptHistory(int convoIdx, bool fullHistory) {
  String history = conversations[convoIdx].history.c_str();
  if (fullHistory || history.length() == 0) return history;

  const int recentTurnCount = 8;
  int searchEnd = history.length();
  int startIdx = 0;

  for (int turn = 0; turn < recentTurnCount; turn++) {
    int markerIdx = history.lastIndexOf("User: ", searchEnd - 1);
    if (markerIdx < 0) break;
    startIdx = markerIdx;
    if (markerIdx == 0) break;
    searchEnd = markerIdx;
  }

  return history.substring(startIdx);
}

uint64_t fnv1a64Update(uint64_t hash, const String &value) {
  for (size_t i = 0; i < value.length(); i++) {
    hash ^= (uint8_t)value.charAt(i);
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint64_t buildGeminiCacheKey(int convoIdx, const String &userMessage, const String &promptHistory, const String &personaPrompt) {
  (void)convoIdx;
  uint64_t hash = 1469598103934665603ULL;
  hash = fnv1a64Update(hash, normalizeForFingerprint(userMessage));
  hash = fnv1a64Update(hash, String("|") + normalizeForFingerprint(promptHistory));
  hash = fnv1a64Update(hash, String("|") + normalizeForFingerprint(personaPrompt));
  return hash;
}

int findGeminiCacheSlot(uint64_t keyHash) {
  int freeSlot = -1;
  int lruSlot = 0;
  unsigned long oldestUse = 0;

  for (int i = 0; i < GEMINI_CACHE_SIZE; i++) {
    if (geminiResponseCache[i].valid) {
      if (geminiResponseCache[i].keyHash == keyHash) {
        return i;
      }
      if (geminiResponseCache[i].lastUsedMs <= oldestUse) {
        oldestUse = geminiResponseCache[i].lastUsedMs;
        lruSlot = i;
      }
    } else if (freeSlot == -1) {
      freeSlot = i;
    }
  }

  if (freeSlot != -1) return freeSlot;
  geminiCacheEvictions++;
  return lruSlot;
}

bool lookupGeminiCache(uint64_t keyHash, String &replyOut) {
  for (int i = 0; i < GEMINI_CACHE_SIZE; i++) {
    if (geminiResponseCache[i].valid && geminiResponseCache[i].keyHash == keyHash) {
      geminiResponseCache[i].lastUsedMs = millis();
      replyOut = String(geminiResponseCache[i].reply.c_str());
      geminiCacheHits++;
      return true;
    }
  }
  geminiCacheMisses++;
  return false;
}

void storeGeminiCache(uint64_t keyHash, const String &replyText) {
  int slot = findGeminiCacheSlot(keyHash);
  geminiResponseCache[slot].keyHash = keyHash;
  geminiResponseCache[slot].reply = replyText;
  geminiResponseCache[slot].storedAtMs = millis();
  geminiResponseCache[slot].lastUsedMs = geminiResponseCache[slot].storedAtMs;
  geminiResponseCache[slot].valid = true;
  geminiCacheStores++;
}

String getGeminiCacheStatsString() {
  int validCount = 0;
  for (int i = 0; i < GEMINI_CACHE_SIZE; i++) {
    if (geminiResponseCache[i].valid) validCount++;
  }
  return "Cache " + String(geminiCacheHits) + "H/" + String(geminiCacheMisses) + "M | " +
         String(validCount) + "/" + String(GEMINI_CACHE_SIZE) + " used";
}

bool tryLocalIntentReply(const String &textBody, String &replyOut) {
  String normalized = normalizeForFingerprint(textBody);
  if (normalized.length() == 0) return false;

  if (normalized == "ok" || normalized == "okay" || normalized == "k" || normalized == "got it" ||
      normalized == "thanks" || normalized == "thank you" || normalized == "thx" || normalized == "yep" ||
      normalized == "yes" || normalized == "no" || normalized == "cool") {
    replyOut = (normalized == "no") ? "No problem." : "Got it.";
    return true;
  }

  if (normalized.length() <= 4 && (normalized == "y" || normalized == "n")) {
    replyOut = (normalized == "y") ? "Yes." : "No.";
    return true;
  }

  return false;
}

// --- GEMINI API HELPERS ---
void rebuildGeminiKeyPool() {
  geminiKeyCount = 0;
  nextGeminiKeyCursor = 0;

  String primary = String(SECRET_GEMINI_KEY);
  if (primary.length() > 0) {
    geminiKeys[0].key = primary;
    geminiKeys[0].requestsToday = 0;
    geminiKeys[0].cooldownUntilMs = 0;
    geminiKeys[0].failures = 0;
    geminiKeyCount = 1;
    gemini_api_key = primary;
    lastGeminiKeyLabel = "primary";
  }

#ifdef SECRET_GEMINI_KEYS
  String extraKeys = String(SECRET_GEMINI_KEYS);
  int start = 0;
  while (start < (int)extraKeys.length() && geminiKeyCount < MAX_GEMINI_KEYS) {
    int comma = extraKeys.indexOf(',', start);
    String token = (comma == -1) ? extraKeys.substring(start) : extraKeys.substring(start, comma);
    token.trim();
    if (token.length() > 0 && token != primary) {
      geminiKeys[geminiKeyCount].key = token;
      geminiKeys[geminiKeyCount].requestsToday = 0;
      geminiKeys[geminiKeyCount].cooldownUntilMs = 0;
      geminiKeys[geminiKeyCount].failures = 0;
      geminiKeyCount++;
    }
    if (comma == -1) break;
    start = comma + 1;
  }
#endif

  if (geminiKeyCount == 0) {
    geminiKeys[0].key = gemini_api_key;
    geminiKeys[0].requestsToday = 0;
    geminiKeys[0].cooldownUntilMs = 0;
    geminiKeys[0].failures = 0;
    geminiKeyCount = 1;
  }
}

void resetGeminiKeyUsageIfNeeded() {
  String todayTag = getDateTag();
  int todayInt = todayTag.toInt();
  if (geminiUsageDay != todayInt) {
    geminiUsageDay = todayInt;
    for (int i = 0; i < geminiKeyCount; i++) {
      geminiKeys[i].requestsToday = 0;
      geminiKeys[i].cooldownUntilMs = 0;
      geminiKeys[i].failures = 0;
    }
    saveGeminiUsage();
  }
}

void markGeminiKeyFailure(int index, unsigned long cooldownMs) {
  if (index < 0 || index >= geminiKeyCount) return;
  geminiKeys[index].failures++;
  geminiKeys[index].cooldownUntilMs = millis() + cooldownMs;
  saveGeminiUsage();
}

void markGeminiKeySuccess(int index) {
  if (index < 0 || index >= geminiKeyCount) return;
  geminiKeys[index].requestsToday++;
  geminiKeys[index].cooldownUntilMs = 0;
  geminiKeys[index].failures = 0;
  saveGeminiUsage();
}

int pickGeminiKey() {
  resetGeminiKeyUsageIfNeeded();
  unsigned long now = millis();
  const int maxPerDay = 12;
  for (int attempt = 0; attempt < geminiKeyCount; attempt++) {
    int idx = (nextGeminiKeyCursor + attempt) % geminiKeyCount;
    if (geminiKeys[idx].cooldownUntilMs > now) continue;
    if (geminiKeys[idx].requestsToday >= maxPerDay) continue;
    nextGeminiKeyCursor = (idx + 1) % geminiKeyCount;
    return idx;
  }

  for (int attempt = 0; attempt < geminiKeyCount; attempt++) {
    int idx = (nextGeminiKeyCursor + attempt) % geminiKeyCount;
    if (geminiKeys[idx].cooldownUntilMs <= now) {
      nextGeminiKeyCursor = (idx + 1) % geminiKeyCount;
      return idx;
    }
  }
  return 0;
}

String sendGeminiApiRequestWithKey(String modelName, String requestBody, String apiKey) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setCACert(GOOGLE_ROOT_CA);

  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + modelName + ":generateContent?key=" + apiKey;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000); 

  esp_task_wdt_delete(NULL);
  int httpCode = http.POST(requestBody);
  esp_task_wdt_add(NULL);
  String result = "";

  if (httpCode == 200) {
    esp_task_wdt_reset();
    String payload = http.getString();
    DynamicJsonDocument responseDoc(4096);
    DeserializationError parseErr = deserializeJson(responseDoc, payload);

    if (parseErr) {
      Serial.println("Gemini JSON parse failed: " + String(parseErr.c_str()));
      http.end();
      return "";
    }
    if (!responseDoc.containsKey("candidates") || responseDoc["candidates"].size() == 0) {
      // A 200 with no candidates usually means a safety block or malformed
      // request -- log the reason instead of silently returning "".
      String blockReason = responseDoc["promptFeedback"]["blockReason"].as<String>();
      String errMsg = responseDoc["error"]["message"].as<String>();
      Serial.println("Gemini returned no candidates. blockReason=" + blockReason + " error=" + errMsg);
      http.end();
      return "";
    }

    result = responseDoc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
    result.replace("*", "");
    result.replace("#", "");
    result.replace("\n", " ");
    result.trim();
    http.end();
    return result;
  } else if (httpCode == 429) {
    http.end();
    return "HTTP_429";
  } else if (httpCode < 0) {
    http.end();
    return "HTTP_NETERR";
  } else {
    Serial.println("Gemini HTTP error " + String(httpCode) + ": " + http.getString());
    http.end();
    return "";
  }
}

String sendGeminiApiRequest(String modelName, String requestBody) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setCACert(GOOGLE_ROOT_CA);

  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + modelName + ":generateContent?key=" + gemini_api_key;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000); 

  int httpCode = http.POST(requestBody);
  String result = "";

  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument responseDoc(4096);
    DeserializationError parseErr = deserializeJson(responseDoc, payload);
    if (parseErr) {
      Serial.println("Gemini JSON parse failed: " + String(parseErr.c_str()));
      http.end();
      return "";
    }

    if (!responseDoc.containsKey("candidates") || responseDoc["candidates"].size() == 0) {
      String blockReason = responseDoc["promptFeedback"]["blockReason"].as<String>();
      String errMsg = responseDoc["error"]["message"].as<String>();
      Serial.println("Gemini returned no candidates. blockReason=" + blockReason + " error=" + errMsg);
      http.end();
      return "";
    }

    result = responseDoc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
    result.replace("*", "");
    result.replace("#", "");
    result.replace("\n", " ");
    result.trim();
    http.end();
    return result;
  } else if (httpCode == 429) {
    http.end();
    return "HTTP_429";
  } else if (httpCode < 0) {
    http.end();
    return "HTTP_NETERR";
  } else {
    http.end();
    return "";
  }
}

String queryGemini(String prompt, String history, String personaPrompt) {
  if (getPausedState()) return "";

  String knowledgeContext = loadRelevantKnowledge(prompt);
  String memoryFacts = loadMatchingMemoryFacts(prompt);
  String formattedPrompt = "SYSTEM INSTRUCTION:\n" + personaPrompt + "\n\n"
                           "FORMAT RULES:\n"
                           "- Reply via SMS text.\n"
                           "- Keep responses strictly under 280 characters.\n"
                           "- Use plain text only (no markdown).\n\n";

  if (knowledgeContext.length() > 0) {
    formattedPrompt += "SD KNOWLEDGE:\n" + knowledgeContext + "\n";
  }

  if (memoryFacts.length() > 0) {
    formattedPrompt += "RELEVANT MEMORY:\n" + memoryFacts + "\n";
  }

  if (history.length() > 0) {
    formattedPrompt += "CONVERSATION HISTORY:\n" + history + "\n";
  }

  formattedPrompt += "USER MESSAGE: " + prompt;

  DynamicJsonDocument doc(16384);
  doc["contents"][0]["parts"][0]["text"] = formattedPrompt;

  String requestBody;
  serializeJson(doc, requestBody);

  const String models[2] = {"gemini-3.6-flash", "gemini-3.5-flash-lite"};
  for (int m = 0; m < 2; m++) {
    esp_task_wdt_reset();
    lastGeminiModel = models[m];
    for (int attempt = 0; attempt < geminiKeyCount; attempt++) {
      esp_task_wdt_reset();
      int idx = pickGeminiKey();
      if (idx < 0 || idx >= geminiKeyCount) break;
      String selectedKey = geminiKeys[idx].key;
      gemini_api_key = selectedKey;
      lastGeminiKeyLabel = (idx == 0) ? "primary" : String("key") + String(idx + 1);
      Serial.println("Querying " + lastGeminiModel + " with " + lastGeminiKeyLabel + "...");
      String response = sendGeminiApiRequestWithKey(lastGeminiModel, requestBody, selectedKey);
      if (response.length() > 0 && response != "HTTP_429" && response != "HTTP_NETERR") {
        consecutiveGeminiEmpty = 0;
        markGeminiKeySuccess(idx);
        return response;
      }
      if (response.length() == 0 || response == "HTTP_429" || response == "HTTP_NETERR") {
        consecutiveGeminiEmpty++;
        healthCheck();
      }
      markGeminiKeyFailure(idx, response == "HTTP_429" ? 60000UL : 15000UL);
      if (response.length() == 0) {
        break;
      }
      responsiveDelay(2000);
    }
    if (m == 0) {
      Serial.println("Falling back to Flash Lite...");
      responsiveDelay(10000);
    }
  }

  return "";
}

// --- RAW IMAP HELPERS ---
bool imapRawConnect(WiFiClientSecure &client) {
  client.setCACert(GOOGLE_ROOT_CA);
  if (!client.connect("imap.gmail.com", 993)) {
    Serial.println("Raw IMAP connect failed!");
    return false;
  }
  
  unsigned long start = millis();
  while (!client.available() && millis() - start < 5000) {
    esp_task_wdt_reset();
    delay(10);
  }
  delay(200);
  while (client.available()) client.read();
  
  client.print("a1 LOGIN " + gmail_user + " " + gmail_pass + "\r\n");
  String resp = imapReadUntilTagged(client, "a1");
  Serial.println("IMAP LOGIN response: " + resp);
  if (resp.indexOf("a1 OK") == -1) {
    Serial.println("IMAP LOGIN failed.");
    return false;
  }

  client.print("a2 SELECT INBOX\r\n");
  resp = imapReadUntilTagged(client, "a2");
  Serial.println("IMAP SELECT response: " + resp);
  bool ok = resp.indexOf("a2 OK") != -1;
  if (!ok) Serial.println("IMAP SELECT failed.");
  return ok;
}

String imapReadUntilTagged(WiFiClientSecure &client, const String &tag, unsigned long timeoutMs) {
  String response = "";
  unsigned long start = millis();
  String tagOK = tag + " OK";
  String tagNO = tag + " NO";
  String tagBAD = tag + " BAD";

  while (millis() - start < timeoutMs) {
    esp_task_wdt_reset();
    while (client.available()) {
      response += (char)client.read();
      start = millis();
    }
    if (response.indexOf(tagOK) != -1 || response.indexOf(tagNO) != -1 || response.indexOf(tagBAD) != -1) break;
    delay(5);
  }
  return response;
}

String imapSearchUnseenUIDs(WiFiClientSecure &client) {
  client.print("a3 UID SEARCH UNSEEN\r\n");
  String resp = imapReadUntilTagged(client, "a3");
  int idx = resp.indexOf("* SEARCH");
  if (idx == -1) return "";
  int endLine = resp.indexOf("\r\n", idx);
  if (endLine == -1) return "";
  String uidLine = resp.substring(idx + 8, endLine);
  uidLine.trim();
  return uidLine;
}

String extractLiteral(const String &resp, int searchFromIdx) {
  int braceStart = resp.indexOf('{', searchFromIdx);
  if (braceStart == -1) return "";
  int braceEnd = resp.indexOf('}', braceStart);
  if (braceEnd == -1) return "";
  int len = resp.substring(braceStart + 1, braceEnd).toInt();
  int contentStart = resp.indexOf("\r\n", braceEnd);
  if (contentStart == -1) return "";
  contentStart += 2;
  if (contentStart + len > (int)resp.length()) len = resp.length() - contentStart;
  if (len < 0) return "";
  return resp.substring(contentStart, contentStart + len);
}

String extractFoldedHeaderValue(const String &headerBlock, int fieldStartIdx, int labelLength) {
  int pos = fieldStartIdx + labelLength;
  String value = "";
  while (true) {
    int lineEnd = headerBlock.indexOf("\r\n", pos);
    if (lineEnd == -1) {
      value += headerBlock.substring(pos);
      break;
    }
    value += headerBlock.substring(pos, lineEnd);
    int nextPos = lineEnd + 2;
    bool isContinuation = nextPos < (int)headerBlock.length() &&
                          (headerBlock.charAt(nextPos) == ' ' || headerBlock.charAt(nextPos) == '\t');
    if (isContinuation) {
      value += " ";
      pos = nextPos;
    } else {
      break;
    }
  }
  value.trim();
  return value;
}

bool imapFetchHeader(WiFiClientSecure &client, const String &uid, String &fromOut, String &subjectOut) {
  String tag = "h" + uid;
  client.print(tag + " UID FETCH " + uid + " (BODY.PEEK[HEADER.FIELDS (FROM SUBJECT)])\r\n");
  String resp = imapReadUntilTagged(client, tag);
  String headerBlock = extractLiteral(resp, 0);
  fromOut = "";
  subjectOut = "";
  int fromIdx = headerBlock.indexOf("From:");
  if (fromIdx != -1) fromOut = extractFoldedHeaderValue(headerBlock, fromIdx, 5);
  int subjIdx = headerBlock.indexOf("Subject:");
  if (subjIdx != -1) subjectOut = extractFoldedHeaderValue(headerBlock, subjIdx, 8);
  return fromOut.length() > 0 || subjectOut.length() > 0;
}

String imapFetchBodyText(WiFiClientSecure &client, const String &uid) {
  String tag = "t" + uid;
  client.print(tag + " UID FETCH " + uid + " (BODY.PEEK[TEXT])\r\n");
  String resp = imapReadUntilTagged(client, tag);
  return extractLiteral(resp, 0);
}

void imapMarkSeen(WiFiClientSecure &client, const String &uid) {
  String tag = "s" + uid;
  client.print(tag + " UID STORE " + uid + " +FLAGS (\\Seen)\r\n");
  imapReadUntilTagged(client, tag);
}

void imapLogout(WiFiClientSecure &client) {
  client.print("z1 LOGOUT\r\n");
  delay(200);
  client.stop();
}

// --- DISPLAY & POWER HELPERS ---
bool getPausedState() {
  if (!uiStateMutex) return isPaused;
  if (xSemaphoreTake(uiStateMutex, portMAX_DELAY) == pdTRUE) {
    bool paused = uiState.paused;
    xSemaphoreGive(uiStateMutex);
    return paused;
  }
  return isPaused;
}

void setPausedState(bool paused) {
  if (!uiStateMutex) {
    isPaused = paused;
    return;
  }
  if (xSemaphoreTake(uiStateMutex, portMAX_DELAY) == pdTRUE) {
    uiState.paused = paused;
    isPaused = paused;
    if (paused) {
      uiState.pausedPage = 0;
    }
    uiState.screenDirty = true;
    xSemaphoreGive(uiStateMutex);
  }
}

void markUiActivity() {
  unsigned long now = millis();
  if (!uiStateMutex) return;
  if (xSemaphoreTake(uiStateMutex, portMAX_DELAY) == pdTRUE) {
    uiState.lastUiActivityMs = now;
    uiState.lastButtonEventMs = now;
    if (uiState.autoDimmed) {
      uiState.autoDimmed = false;
      setDisplayBrightness(DEFAULT_SCREEN_BRIGHTNESS);
    }
    xSemaphoreGive(uiStateMutex);
  }
}

void setLastErrorMessage(const String &message) {
  if (!uiStateMutex) return;
  if (xSemaphoreTake(uiStateMutex, portMAX_DELAY) == pdTRUE) {
    uiState.lastError = message;
    uiState.screenDirty = true;
    xSemaphoreGive(uiStateMutex);
  }
}

void updateScreen(String title, String bodyText) {
  lastScreenTitle = title;
  lastScreenBody = bodyText;
  if (uiStateMutex && xSemaphoreTake(uiStateMutex, portMAX_DELAY) == pdTRUE) {
    String previousTitle = uiState.title;
    String previousBody = uiState.body;
    uiState.title = title;
    uiState.body = bodyText;
    String loweredTitle = title;
    loweredTitle.toLowerCase();
    String loweredBody = bodyText;
    loweredBody.toLowerCase();
    if (loweredTitle.indexOf("error") != -1 || loweredTitle.indexOf("failed") != -1 || loweredBody.indexOf("error") != -1 || loweredBody.indexOf("failed") != -1) {
      uiState.lastError = title + ": " + bodyText;
    }
    uiState.screenDirty = true;
    bool changed = previousTitle != title || previousBody != bodyText;
    if (changed) {
      uiState.lastUiActivityMs = millis();
      if (uiState.autoDimmed) {
        uiState.autoDimmed = false;
        setDisplayBrightness(DEFAULT_SCREEN_BRIGHTNESS);
      }
    }
    xSemaphoreGive(uiStateMutex);
  }
}

void cyclePausedStatusPage() {
  if (!uiStateMutex) return;
  if (xSemaphoreTake(uiStateMutex, portMAX_DELAY) == pdTRUE) {
    uiState.pausedPage = (uiState.pausedPage + 1) % 4;
    uiState.screenDirty = true;
    xSemaphoreGive(uiStateMutex);
  }
}

String buildPausedPageBody(uint8_t page, const String &snapshotBody, const String &snapshotError) {
  switch (page % 4) {
    case 0:
      return snapshotBody.length() > 0 ? snapshotBody : "Press BOOT to resume";
    case 1:
      return "Wi-Fi: " + String(WiFi.status() == WL_CONNECTED ? "connected" : "down") +
             " | RSSI=" + String(WiFi.isConnected() ? WiFi.RSSI() : 0) +
             " | Heap=" + String(ESP.getFreeHeap()) +
             " | PSRAM=" + String(psramFound() ? ESP.getFreePsram() : 0) +
             " | Cache=" + getGeminiCacheStatsString() +
             " | SD=" + String(sdAvailable ? "OK" : "NO");
    case 2: {
      String keyLine = "Keys: ";
      for (int i = 0; i < geminiKeyCount; i++) {
        keyLine += String(i + 1) + ":" + String(geminiKeys[i].requestsToday) + " ";
      }
      if (keyLine.length() == 6) keyLine += "none";
      return keyLine;
    }
    default:
      return "Last error: " + (snapshotError.length() > 0 ? snapshotError : String("None"));
  }
}

void renderScreenPage() {
  String title;
  String body;
  bool paused;
  uint8_t pausedPage;
  String lastError;
  if (uiStateMutex && xSemaphoreTake(uiStateMutex, portMAX_DELAY) == pdTRUE) {
    title = uiState.title;
    body = uiState.body;
    paused = uiState.paused;
    pausedPage = uiState.pausedPage;
    lastError = uiState.lastError;
    uiState.screenDirty = false;
    xSemaphoreGive(uiStateMutex);
  } else {
    title = lastScreenTitle;
    body = lastScreenBody;
    paused = getPausedState();
    pausedPage = 0;
    lastError = String("None");
  }

  String displayTitle = title;
  String displayBody = body;
  if (paused) {
    displayTitle = "PAUSED";
    displayBody = "Page " + String((pausedPage % 4) + 1) + "/4 | " + buildPausedPageBody(pausedPage, body, lastError);
  }

  lastScreenTitle = displayTitle;
  lastScreenBody = displayBody;

  if (!screenEnabled) return;
  display.fillScreen(ST77XX_BLACK);
  display.fillRect(0, 0, 160, 16, ST77XX_BLUE);
  display.setTextColor(ST77XX_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 4);
  display.println(displayTitle);
  display.setTextColor(ST77XX_GREEN);
  display.setCursor(4, 22);
  String clippedBody = displayBody;
  if (clippedBody.length() > 90) clippedBody = clippedBody.substring(0, 87) + "...";
  display.println(clippedBody);
  display.setTextColor(ST77XX_YELLOW);
  display.setCursor(4, 68);
  display.println(getPowerStatusString());
}

void refreshCurrentScreen() {
  if (!uiStateMutex) return;
  if (xSemaphoreTake(uiStateMutex, portMAX_DELAY) == pdTRUE) {
    uiState.screenDirty = true;
    xSemaphoreGive(uiStateMutex);
  }
}

int getBacklightOutputValue(int level) {
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    return screenBacklightActiveLow ? (255 - level) : level;
}

void initDisplayBacklight() {
    pinMode(screenBacklightPin, OUTPUT);
    digitalWrite(screenBacklightPin, screenBacklightActiveLow ? HIGH : LOW);
    display.enableSleep(false);
    display.enableDisplay(true);
    setDisplayBrightness(DEFAULT_SCREEN_BRIGHTNESS);
}

void setDisplayBrightness(int level) {
    screenBrightness = (level < 0) ? 0 : ((level > 255) ? 255 : level);
    if (screenEnabled) {
        analogWrite(screenBacklightPin, getBacklightOutputValue(screenBrightness));
    }
}

void setDisplayEnabled(bool enabled) {
  screenEnabled = enabled;
  if (!screenEnabled) {
    display.enableDisplay(false);
    display.enableSleep(true);
    analogWrite(screenBacklightPin, getBacklightOutputValue(0));
    digitalWrite(screenBacklightPin, screenBacklightActiveLow ? HIGH : LOW);
  } else {
    display.enableSleep(false);
    display.enableDisplay(true);
    analogWrite(screenBacklightPin, getBacklightOutputValue(screenBrightness));
    updateScreen(lastScreenTitle, lastScreenBody);
  }
}

void updateCpuFrequency(bool active) {
  int targetMHz = active ? ACTIVE_CPU_MHZ : IDLE_CPU_MHZ;
  if (currentCpuMHz != targetMHz) {
    currentCpuMHz = targetMHz;
    setCpuFrequencyMhz(currentCpuMHz);
  }
}

bool isOverrideActive(String senderDigits) {
  if (overridePhoneDigits.length() == 0) return false;
  if (millis() > overrideExpiresAtMs) {
    overridePhoneDigits = "";
    overrideExpiresAtMs = 0;
    return false;
  }
  return senderDigits == overridePhoneDigits;
}

void trackPower() {
  updateCpuFrequency(relayActive);
  unsigned long now = millis();
  if (powerLastMillis == 0) {
    powerLastMillis = now;
    return;
  }
  unsigned long elapsedMs = now - powerLastMillis;
  powerLastMillis = now;
  float currentMa = relayActive ? ACTIVE_CURRENT_MA : IDLE_CURRENT_MA;
  double hours = elapsedMs / 3600000.0;
  totalEnergy_mWh += currentMa * SUPPLY_VOLTAGE * hours;
}

String getPowerStatusString() {
  float currentMa = relayActive ? ACTIVE_CURRENT_MA : IDLE_CURRENT_MA;
  float watts = (currentMa * SUPPLY_VOLTAGE) / 1000.0;
  double wh = totalEnergy_mWh / 1000.0;
  char buf[40];
  snprintf(buf, sizeof(buf), "~%.2fW est | %.3f Wh", watts, wh);
  return String(buf);
}

String cleanBody(String raw) {
  raw = extractPlainTextPart(raw);
  int accIdx = raw.indexOf("YOUR ACCOUNT");
  if (accIdx != -1) raw = raw.substring(0, accIdx);
  
  String result = "";
  int start = 0;
  while (start < (int)raw.length()) {
    int nl = raw.indexOf('\n', start);
    String line = (nl == -1) ? raw.substring(start) : raw.substring(start, nl);
    line.trim();
    bool isBoilerplate =
      line.length() == 0 ||
      line.startsWith("<https://voice.google.com>") ||
      line.startsWith("New text message") ||
      line.startsWith("New message") ||
      line == "To reply to this message, reply to this email.";
    if (!isBoilerplate) {
      if (result.length() > 0) result += " ";
      result += line;
    }
    if (nl == -1) break;
    start = nl + 1;
  }
  result.trim();
  return result;
}

String extractPlainTextPart(String raw) {
  int ctIdx = raw.indexOf("Content-Type: text/plain");
  if (ctIdx == -1) return raw;
  int headerEnd = raw.indexOf("\r\n\r\n", ctIdx);
  if (headerEnd == -1) return raw;
  bool isQuotedPrintable = raw.substring(ctIdx, headerEnd).indexOf("quoted-printable") != -1;
  int contentStart = headerEnd + 4;
  int boundaryIdx = raw.indexOf("\r\n--", contentStart);
  String content = (boundaryIdx == -1) ? raw.substring(contentStart) : raw.substring(contentStart, boundaryIdx);
  if (isQuotedPrintable) content = decodeQuotedPrintable(content);
  return content;
}

String decodeQuotedPrintable(String input) {
  String output = "";
  for (size_t i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    if (c == '=' && i + 1 < input.length()) {
      char next1 = input.charAt(i + 1);
      if (next1 == '\r' || next1 == '\n') {
        if (next1 == '\r' && i + 2 < input.length() && input.charAt(i + 2) == '\n') i += 2;
        else i += 1;
        continue;
      } else if (i + 2 < input.length() && isxdigit(next1) && isxdigit(input.charAt(i + 2))) {
        String hex = input.substring(i + 1, i + 3);
        char decoded = (char) strtol(hex.c_str(), NULL, 16);
        output += decoded;
        i += 2;
        continue;
      }
    }
    output += c;
  }
  return output;
}

String extractDigits(String input) {
  String digits = "";
  for (size_t i = 0; i < input.length(); i++) {
    if (isDigit(input.charAt(i))) digits += input.charAt(i);
  }
  return digits;
}

// Reduces a phone number to a canonical form for EXACT comparison: digits
// only, with a leading US country code '1' stripped when present. Used
// instead of substring matching (see isSenderAllowed / admin check below) --
// substring matching on raw digit strings meant a short or overlapping
// number could match a sender it was never meant to match, including the
// admin number.
String normalizeDigits(String input) {
  String digits = extractDigits(input);
  if (digits.length() == 11 && digits.charAt(0) == '1') {
    digits = digits.substring(1);
  }
  return digits;
}

// --- FORWARD EMAIL TO ADMIN ---
bool forwardEmailToAdmin(const String &fromHeader, const String &subject, const String &body) {
  String adminDigits = normalizeDigits(admin_phone);
  if (adminDigits.length() == 0) {
    Serial.println("forwardEmailToAdmin: admin_phone not set, cannot forward");
    return false;
  }
  String adminSmsEmail = adminDigits + "@" + (sms_gateway.length() > 0 ? sms_gateway : "txt.voice.google.com");
  String fwdSubject = subject.length() > 0 ? "Fwd: " + subject : "Forwarded Email";
  String fwdBody = "From: " + fromHeader + "\r\n\r\n" + body;
  WiFiClientSecure client;
  client.setCACert(GOOGLE_ROOT_CA);
  client.setTimeout(3000);
  if (!client.connect("smtp.gmail.com", 465)) {
    Serial.println("forwardEmailToAdmin: SMTP connect failed");
    return false;
  }
  auto getResponse = [&client]() -> String {
    String resp = "";
    unsigned long start = millis();
    while (millis() - start < 3000) {
      while (client.available()) {
        resp += (char)client.read();
        start = millis();
      }
      if (resp.endsWith("\r\n")) {
        int lineStart = resp.lastIndexOf('\n', resp.length() - 3);
        if (lineStart == -1) lineStart = 0;
        else lineStart += 1;
        if (resp.length() >= lineStart + 4) {
          String code = resp.substring(lineStart, lineStart + 3);
          char sep = resp.charAt(lineStart + 3);
          if (isDigit(code.charAt(0)) && isDigit(code.charAt(1)) && isDigit(code.charAt(2)) && sep == ' ') {
            break;
          }
        }
      }
      delay(5);
    }
    return resp;
  };
  getResponse(); // 220 banner
  client.print("EHLO ESP32\r\n"); getResponse();
  client.print("AUTH LOGIN\r\n"); getResponse();
  client.print(base64Encode(gmail_user) + "\r\n"); getResponse();
  client.print(base64Encode(gmail_pass) + "\r\n");
  String authResp = getResponse();
  if (authResp.indexOf("235") == -1) {
    Serial.println("forwardEmailToAdmin: SMTP auth failed");
    client.stop();
    return false;
  }
  client.print("MAIL FROM:<" + gmail_user + ">\r\n"); getResponse();
  client.print("RCPT TO:<" + adminSmsEmail + ">\r\n"); getResponse();
  client.print("DATA\r\n"); getResponse();
  client.print("From: AI Relay <" + gmail_user + ">\r\n");
  client.print("To: " + adminSmsEmail + "\r\n");
  client.print("Subject: " + fwdSubject + "\r\n");
  client.print("Content-Type: text/plain; charset=UTF-8\r\n\r\n");
  String stuffedBody = fwdBody;
  stuffedBody.replace("\r\n", "\n");
  stuffedBody.replace("\n", "\r\n");
  stuffedBody.replace("\r\n.", "\r\n..");
  if (stuffedBody.startsWith(".")) stuffedBody = "." + stuffedBody;
  client.print(stuffedBody + "\r\n.\r\n"); getResponse();
  client.print("QUIT\r\n"); getResponse();
  client.stop();
  Serial.println("forwardEmailToAdmin: forwarded to " + adminSmsEmail);
  return true;
}

// --- RAW SMTP SEND FUNCTION ---
bool sendEmailReply(String recipient, String aiResponse) {
  unsigned long t0 = millis();

  String cleanRecipient = recipient;
  int startBracket = cleanRecipient.indexOf('<');
  int endBracket = cleanRecipient.indexOf('>');
  if (startBracket != -1 && endBracket != -1) {
    cleanRecipient = cleanRecipient.substring(startBracket + 1, endBracket);
  }
  cleanRecipient.trim();

  WiFiClientSecure client;
  client.setCACert(GOOGLE_ROOT_CA);
  client.setTimeout(3000);

  Serial.println("Connecting to raw SMTP...");
  if (!client.connect("smtp.gmail.com", 465)) {
    Serial.println("Raw SMTP connect failed!");
    return false;
  }

  auto getResponse = [&client]() -> String {
    String resp = "";
    unsigned long start = millis();
    while (millis() - start < 3000) {
      while (client.available()) {
        resp += (char)client.read();
        start = millis();
      }
      if (resp.endsWith("\r\n")) {
        int lineStart = resp.lastIndexOf('\n', resp.length() - 3);
        if (lineStart == -1) lineStart = 0;
        else lineStart += 1;

        if (resp.length() >= lineStart + 4) {
          String code = resp.substring(lineStart, lineStart + 3);
          char sep = resp.charAt(lineStart + 3);
          if (isDigit(code.charAt(0)) && isDigit(code.charAt(1)) && isDigit(code.charAt(2)) && sep == ' ') {
            break;
          }
        }
      }
      delay(5);
    }
    return resp;
  };

  getResponse(); // 220 banner

  client.print("EHLO ESP32\r\n");
  esp_task_wdt_reset();
  getResponse();

  client.print("AUTH LOGIN\r\n");
  esp_task_wdt_reset();
  getResponse();

  client.print(base64Encode(gmail_user) + "\r\n");
  esp_task_wdt_reset();
  getResponse();

  client.print(base64Encode(gmail_pass) + "\r\n");
  esp_task_wdt_reset();
  String authResp = getResponse();

  if (authResp.indexOf("235") == -1) {
    Serial.println("SMTP Auth Failed: " + authResp);
    client.stop();
    return false;
  }

  client.print("MAIL FROM:<" + gmail_user + ">\r\n");
  getResponse();

  client.print("RCPT TO:<" + cleanRecipient + ">\r\n");
  getResponse();

  client.print("DATA\r\n");
  getResponse();

  client.print("From: AI Relay <" + gmail_user + ">\r\n");
  client.print("To: " + cleanRecipient + "\r\n");
  client.print("Subject: Re: Google Voice Text\r\n");
  client.print("Content-Type: text/plain; charset=UTF-8\r\n\r\n");

  // RFC 5321 dot-stuffing: any body line starting with '.' must become '..'
  // or the SMTP server reads it as the end-of-DATA marker and truncates the
  // message right there. AI replies are flattened to one line elsewhere,
  // but admin command output (e.g. !SEARCH results) isn't, so this matters.
  String body = aiResponse;
  body.replace("\r\n", "\n");
  body.replace("\n", "\r\n");
  String stuffedBody = body;
  stuffedBody.replace("\r\n.", "\r\n..");
  if (stuffedBody.startsWith(".")) stuffedBody = "." + stuffedBody;

  client.print(stuffedBody + "\r\n.\r\n");
  getResponse();

  client.print("QUIT\r\n");
  getResponse();
  client.stop();

  Serial.printf("Timing: Raw sendEmailReply total = %lums\n", millis() - t0);
  return true;
}
