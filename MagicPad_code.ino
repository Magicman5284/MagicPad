#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

//enter info plz
const char* WIFI_SSID = "";
const char* WIFI_PASSWORD = "";
const char* COFFEE_IP_ADDRESS = "192.168.1.100";
const char* ALARM_IP_ADDRESS  = "192.168.1.101";
//stop entering info plz
const char* TZ_INFO = "EST5EDT,M3.2.0/2,M11.1.0/2";
const char* NTP_1 = "pool.ntp.org";
const char* NTP_2 = "time.nist.gov";

static const uint32_t HTTP_TIMEOUT_MS = 2200;
static const uint32_t STATUS_REFRESH_MS = 45000;
static const uint32_t WIFI_RETRY_MS = 15000;
//pin stuff
#define TFT_CS       26
#define TFT_RST      27
#define TFT_DC       32
#define TFT_MOSI     33
#define TFT_SCK      25

#define SD_CS        14 
#define SD_MISO      13 

#define ENC_A        18 
#define ENC_B        19
#define ENC_SW       17  

#define BTN_UP       36  
#define BTN_LEFT     39
#define BTN_ENTER    34
#define BTN_RIGHT    35
#define BTN_BACK     23
#define BTN_DOWN     16
#define BTN_HOME     4


Adafruit_ILI9341 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
WiFiClient netClient;



#define C_BG       0x1082  // deep blue-gray
#define C_PANEL    0x2128
#define C_PANEL2   0x294D
#define C_LINE     0x4A55
#define C_TEXT     ILI9341_WHITE
#define C_MUTED    0xBDF7
#define C_TEAL     0x4DDA
#define C_GREEN    0x3E8D
#define C_ORANGE   0xFD20
#define C_RED      0xF986
#define C_BLUE     0x5D7F
#define C_YELLOW   0xFFE0



String coffeeIP = COFFEE_IP_ADDRESS;
String alarmIP = ALARM_IP_ADDRESS;
String coffeeStatus = "Not checked yet";
String alarmStatus = "Not checked yet";
String lastResponse = "Ready.";
String messageTitle = "Information";
String messageBody = "";
String listResponse = "";

bool coffeeOnline = false;
bool alarmOnline = false;
bool wifiWasConnected = false;
bool statusRefreshDone = false;

uint32_t lastWiFiRetry = 0;
uint32_t lastStatusRefresh = 0;
uint32_t lastClockDraw = 0;

// UI screens
enum Screen : uint8_t {
  SCREEN_HOME,
  SCREEN_COFFEE_MENU,
  SCREEN_ALARM_MENU,
  SCREEN_COFFEE_CONFIRM,
  SCREEN_DATE_EDITOR,
  SCREEN_CANCEL_EDITOR,
  SCREEN_ALARM_LIST,
  SCREEN_STATUS,
  SCREEN_MESSAGE
};

Screen screen = SCREEN_HOME;
Screen messageReturnScreen = SCREEN_HOME;
int menuIndex = 0;
int confirmChoice = 0;
int scrollOffset = 0;
bool uiDirty = true;


struct tm editDateTime = {};
int dateField = 0; 
bool editingCoffeeSchedule = true;


static const uint8_t MAX_ALARM_CHOICES = 12;
struct AlarmChoice {
  int number;
  String label;
};
AlarmChoice alarmChoices[MAX_ALARM_CHOICES];
uint8_t alarmChoiceCount = 0;
int alarmSelectedIndex = 0;

//rotary encoder matrix, no idea how this works but this is what arduino forum said so I'm using it
uint8_t lastEncoderAB = 0;
int8_t encoderDelta = 0;
const int8_t encoderTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};


enum Action : uint8_t {
  ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT,
  ACT_ENTER, ACT_BACK, ACT_HOME,
  ACT_ENC_PRESS, ACT_ENC_CW, ACT_ENC_CCW
};

struct KeyInput {
  uint8_t pin;
  Action action;
  bool externalPullup;
  int raw;
  int stable;
  uint32_t changedAt;
};

KeyInput keys[] = {
  {BTN_UP,    ACT_UP,    true,  HIGH, HIGH, 0},
  {BTN_LEFT,  ACT_LEFT,  true,  HIGH, HIGH, 0},
  {BTN_ENTER, ACT_ENTER, true,  HIGH, HIGH, 0},
  {BTN_RIGHT, ACT_RIGHT, true,  HIGH, HIGH, 0},
  {BTN_BACK,  ACT_BACK,  false, HIGH, HIGH, 0},
  {BTN_DOWN,  ACT_DOWN,  false, HIGH, HIGH, 0},
  {BTN_HOME,  ACT_HOME,  false, HIGH, HIGH, 0},
  {ENC_SW,    ACT_ENC_PRESS, false, HIGH, HIGH, 0}
};

const size_t KEY_COUNT = sizeof(keys) / sizeof(keys[0]);
const uint32_t DEBOUNCE_MS = 35;

//menu stuff
const char* coffeeMenu[] = {
  "Brew now",
  "Schedule brew",
  "Cancel scheduled brew",
  "Status"
};
const int COFFEE_MENU_COUNT = sizeof(coffeeMenu) / sizeof(coffeeMenu[0]);

const char* alarmMenu[] = {
  "Add one-time alarm",
  "View scheduled alarms",
  "Cancel an alarm",
  "Stop sounding alarm",
  "Synchronize clock",
  "Status"
};
const int ALARM_MENU_COUNT = sizeof(alarmMenu) / sizeof(alarmMenu[0]);


void setMessage(const String& title, const String& body, Screen returnTo) {
  messageTitle = title;
  messageBody = body;
  messageReturnScreen = returnTo;
  scrollOffset = 0;
  screen = SCREEN_MESSAGE;
  uiDirty = true;
}

void setStatusPage(const String& title, const String& body, Screen returnTo) {
  messageTitle = title;
  messageBody = body;
  messageReturnScreen = returnTo;
  scrollOffset = 0;
  screen = SCREEN_STATUS;
  uiDirty = true;
}

bool timeIsValid() {
  return time(nullptr) > 1700000000;
}

String currentDateTimeText() {
  if (!timeIsValid()) return "Time not synchronized";
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  char b[40];
  strftime(b, sizeof(b), "%a, %b %d  %I:%M %p", &local);
  return String(b);
}

String makeBaseUrl(const String& ip) {
  return String("http://") + ip;
}

bool runHttpGet(const String& ip, const String& path,
                String& response, int& statusCode) {
  response = "";
  statusCode = 0;
  if (WiFi.status() != WL_CONNECTED) {
    response = "Controller is not connected to Wi-Fi.";
    return false;
  }

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  String url = makeBaseUrl(ip) + path;

  Serial.print("HTTP GET ");
  Serial.println(url);

  if (!http.begin(netClient, url)) {
    response = "Could not initialize HTTP connection.";
    return false;
  }

  statusCode = http.GET();
  if (statusCode > 0) {
    response = http.getString();
  } else {
    response = String("HTTP error: ") + http.errorToString(statusCode);
  }
  http.end();

  Serial.printf("HTTP response: %d\n", statusCode);
  Serial.println(response);
  return statusCode >= 200 && statusCode < 300;
}

bool runHttpPostJson(const String& ip, const String& path,
                     const String& jsonBody, String& response,
                     int& statusCode) {
  response = "";
  statusCode = 0;
  if (WiFi.status() != WL_CONNECTED) {
    response = "Controller is not connected to Wi-Fi.";
    return false;
  }

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  String url = makeBaseUrl(ip) + path;

  Serial.print("HTTP POST ");
  Serial.println(url);
  Serial.print("JSON body: ");
  Serial.println(jsonBody);

  if (!http.begin(netClient, url)) {
    response = "Could not initialize HTTP connection.";
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  statusCode = http.POST((uint8_t*)jsonBody.c_str(), jsonBody.length());
  if (statusCode > 0) {
    response = http.getString();
  } else {
    response = String("HTTP error: ") + http.errorToString(statusCode);
  }
  http.end();

  Serial.printf("HTTP response: %d\n", statusCode);
  Serial.println(response);
  return statusCode >= 200 && statusCode < 300;
}

String twoDigits(int value) {
  return (value < 10 ? "0" : "") + String(value);
}

String makeDateTimePayload(const struct tm& input) {
  char b[11];
  snprintf(b, sizeof(b), "%02d%02d%02d%02d%02d",
           input.tm_mday, input.tm_mon + 1,
           (input.tm_year + 1900) % 100,
           input.tm_hour, input.tm_min);
  return String(b);
}

bool buildValidatedEpoch(struct tm requested, time_t& epochOut) {
  if (requested.tm_mon < 0 || requested.tm_mon > 11 ||
      requested.tm_mday < 1 || requested.tm_mday > 31 ||
      requested.tm_hour < 0 || requested.tm_hour > 23 ||
      requested.tm_min < 0 || requested.tm_min > 59) return false;

  int y = requested.tm_year;
  int m = requested.tm_mon;
  int d = requested.tm_mday;
  int h = requested.tm_hour;
  int n = requested.tm_min;
  requested.tm_sec = 0;
  requested.tm_isdst = -1;
  time_t candidate = mktime(&requested);
  if (candidate < 0) return false;

  struct tm check;
  localtime_r(&candidate, &check);
  if (check.tm_year != y || check.tm_mon != m || check.tm_mday != d ||
      check.tm_hour != h || check.tm_min != n) return false;
  epochOut = candidate;
  return true;
}

//wifi stuff

void beginWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiRetry = millis();
  Serial.print("Connecting to Wi-Fi SSID: ");
  Serial.println(WIFI_SSID);
  configTzTime(TZ_INFO, NTP_1, NTP_2);
}

void maintainWiFi() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected) {
    wifiWasConnected = true;
    statusRefreshDone = false;
    Serial.print("Wi-Fi connected. Controller IP: ");
    Serial.println(WiFi.localIP());
    configTzTime(TZ_INFO, NTP_1, NTP_2);
    uiDirty = true;
  } else if (!connected && wifiWasConnected) {
    wifiWasConnected = false;
    statusRefreshDone = false;
    coffeeOnline = false;
    alarmOnline = false;
    Serial.println("Wi-Fi disconnected.");
    uiDirty = true;
  }

  if (!connected && millis() - lastWiFiRetry >= WIFI_RETRY_MS) {
    lastWiFiRetry = millis();
    Serial.println("Retrying Wi-Fi connection...");
    WiFi.reconnect();
  }
}

//coffee maker

bool refreshCoffeeStatus(bool showResult) {
  String response;
  int code;
  bool ok = runHttpGet(coffeeIP, "/status", response, code);
  coffeeOnline = ok;
  coffeeStatus = response;
  if (showResult) setStatusPage("COFFEE MAKER STATUS", response, SCREEN_COFFEE_MENU);
  return ok;
}

void commandCoffeeBrew() {
  String response;
  int code;
  bool ok = runHttpGet(coffeeIP, "/brew", response, code);
  if (ok) {
    setMessage("BREW REQUEST ACCEPTED",
      "The Keurig returned HTTP " + String(code) + ".\n\nResponse: " + response +
      "\n\nThe command was accepted; the coffee maker does not expose a separate brew-complete endpoint.",
      SCREEN_COFFEE_MENU);
  } else {
    setMessage("BREW REQUEST FAILED",
      "HTTP " + String(code) + "\n\n" + response,
      SCREEN_COFFEE_MENU);
  }
}

void commandCoffeeCancel() {
  String response;
  int code;
  bool ok = runHttpGet(coffeeIP, "/cancel", response, code);
  if (ok) coffeeStatus = "Schedule cancelled";
  setMessage(ok ? "SCHEDULE CANCELLED" : "CANCEL FAILED",
             "HTTP " + String(code) + "\n\n" + response,
             SCREEN_COFFEE_MENU);
}

void commandCoffeeSchedule() {
  time_t scheduledEpoch;
  if (!buildValidatedEpoch(editDateTime, scheduledEpoch)) {
    setMessage("INVALID DATE OR TIME",
               "You can't do that bro",
               SCREEN_DATE_EDITOR);
    return;
  }
  if (!timeIsValid()) {
    setMessage("TIME NOT READY",
               "The conroller isn't ready yet",
               SCREEN_DATE_EDITOR);
    return;
  }
  if (scheduledEpoch <= time(nullptr)) {
    setMessage("TIME IS IN THE PAST",
               "Choose a future date and time. The Keurig also rejects past schedules.",
               SCREEN_DATE_EDITOR);
    return;
  }

  String payload = makeDateTimePayload(editDateTime);
  String json = String("{\"schedule\":\"") + payload + "\"}";
  String response;
  int code;
  bool ok = runHttpPostJson(coffeeIP, "/schedule", json, response, code);
  if (ok) coffeeStatus = response;
  setMessage(ok ? "BREW SCHEDULED" : "SCHEDULE REJECTED",
    "Payload: " + payload + "\nHTTP " + String(code) + "\n\n" + response,
    SCREEN_COFFEE_MENU);
}

// ============================================================
// DEVICE API: ALARM CLOCK
// ============================================================

bool refreshAlarmStatus(bool showResult) {
  String response;
  int code;
  bool ok = runHttpGet(alarmIP, "/command?cmd=status", response, code);
  alarmOnline = ok;
  alarmStatus = response;
  if (showResult) setStatusPage("ALARM CLOCK STATUS", response, SCREEN_ALARM_MENU);
  return ok;
}

void commandAlarmSingle() {
  time_t scheduledEpoch;
  if (!buildValidatedEpoch(editDateTime, scheduledEpoch)) {
    setMessage("INVALID DATE OR TIME", "pick a better one", SCREEN_DATE_EDITOR);
    return;
  }
  if (!timeIsValid()) {
    setMessage("TIME NOT READY", "clock aint sycronized yet", SCREEN_DATE_EDITOR);
    return;
  }
  if (scheduledEpoch <= time(nullptr)) {
    setMessage("TIME IS IN THE PAST", "do it agin in the future", SCREEN_DATE_EDITOR);
    return;
  }

  String payload = makeDateTimePayload(editDateTime);
  String path = "/command?cmd=single&payload=" + payload;
  String response;
  int code;
  bool ok = runHttpGet(alarmIP, path, response, code);
  setMessage(ok ? "ALARM CREATED" : "ALARM REJECTED",
             "Payload: " + payload + "\nHTTP " + String(code) + "\n\n" + response,
             SCREEN_ALARM_MENU);
}

void commandAlarmList() {
  String response;
  int code;
  bool ok = runHttpGet(alarmIP, "/command?cmd=cancel-list", response, code);
  alarmOnline = ok;
  listResponse = response;
  messageTitle = ok ? "SCHEDULED ALARMS" : "ALARM LIST FAILED";
  messageBody = "HTTP " + String(code) + "\n\n" + response;
  messageReturnScreen = SCREEN_ALARM_MENU;
  scrollOffset = 0;
  screen = SCREEN_ALARM_LIST;
  uiDirty = true;
}









bool parseAlarmChoiceLine(String line, int& numberOut, String& labelOut) {
  line.replace("\r", "");
  line.trim();
  if (line.length() == 0) return false;

  if (line.startsWith("Alarm ") || line.startsWith("alarm ")) line.remove(0, 6);
  if (line.startsWith("#")) line.remove(0, 1);

  int pos = 0;
  while (pos < (int)line.length() && line[pos] >= '0' && line[pos] <= '9') pos++;
  if (pos == 0) return false;

  int parsedNumber = line.substring(0, pos).toInt();
  if (parsedNumber < 1) return false;






  bool separator = false;
  if (pos < (int)line.length() &&
      (line[pos] == '.' || line[pos] == ')' || line[pos] == ':' ||
       line[pos] == '-' || line[pos] == ']')) {
    separator = true;
    pos++;
  }
  while (pos < (int)line.length() && (line[pos] == ' ' || line[pos] == '\t')) {
    separator = true;
    pos++;
  }
  if (!separator || pos >= (int)line.length()) return false;


  labelOut = line.substring(pos);
  labelOut.trim();
  if (labelOut.length() == 0) return false;
  numberOut = parsedNumber;
  return true;
}

void parseAlarmChoices(const String& response) {
  alarmChoiceCount = 0;
  alarmSelectedIndex = 0;

  int start = 0;
  while (start < (int)response.length() && alarmChoiceCount < MAX_ALARM_CHOICES) {
    int end = response.indexOf('\n', start);
    if (end < 0) end = response.length();

    String line = response.substring(start, end);
    int alarmNumber = 0;
    String alarmLabel;
    if (parseAlarmChoiceLine(line, alarmNumber, alarmLabel)) {
      alarmChoices[alarmChoiceCount].number = alarmNumber;
      alarmChoices[alarmChoiceCount].label = alarmLabel;
      alarmChoiceCount++;
    }
    start = end + 1;
  }
}

void commandAlarmCancelList() {
  String response;
  int code;
  bool ok = runHttpGet(alarmIP, "/command?cmd=cancel-list", response, code);
  alarmOnline = ok;
  listResponse = response;

  if (!ok) {
    setMessage("ALARM LIST FAILED", "HTTP " + String(code) + "\n\n" + response,
               SCREEN_ALARM_MENU);
    return;
  }

  parseAlarmChoices(response);
  screen = SCREEN_CANCEL_EDITOR;
  uiDirty = true;
}

void commandAlarmCancel() {
  if (alarmChoiceCount == 0 || alarmSelectedIndex < 0 ||
      alarmSelectedIndex >= alarmChoiceCount) return;

  int selectedNumber = alarmChoices[alarmSelectedIndex].number;
  String selectedLabel = alarmChoices[alarmSelectedIndex].label;
  String path = "/command?cmd=cancel&payload=" + String(selectedNumber);
  String response;
  int code;
  bool ok = runHttpGet(alarmIP, path, response, code);
  setMessage(ok ? "ALARM CANCELLED" : "CANCELLATION FAILED",
             selectedLabel + "\nAlarm number: " + String(selectedNumber) +
             "\nHTTP " + String(code) + "\n\n" + response,
             SCREEN_ALARM_MENU);
}

void commandAlarmSimple(const String& cmd, const String& title) {
  String response;
  int code;
  bool ok = runHttpGet(alarmIP, "/command?cmd=" + cmd, response, code);
  setMessage(ok ? title : "COMMAND FAILED",
             "HTTP " + String(code) + "\n\n" + response,
             SCREEN_ALARM_MENU);
}

void commandAlarmSyncTime() {
  if (!timeIsValid()) {
    setMessage("TIME NOT READY", "patience is a virtue", SCREEN_ALARM_MENU);//yeah but I have no more patice to write any more code
    return;
  }
  String epoch = String((unsigned long)time(nullptr));
  String response;
  int code;
  bool ok = runHttpGet(alarmIP, "/command?cmd=sync-time&payload=" + epoch, response, code);
  setMessage(ok ? "CLOCK SYNCHRONIZED" : "TIME SYNC FAILED",
             "Unix time: " + epoch + "\nHTTP " + String(code) + "\n\n" + response,
             SCREEN_ALARM_MENU);
}



void initDateEditor(bool coffee) {
  editingCoffeeSchedule = coffee;
  dateField = 0;
  time_t base = timeIsValid() ? time(nullptr) + 3600 : 0;
  if (base > 0) {
    localtime_r(&base, &editDateTime);
  } else {
    memset(&editDateTime, 0, sizeof(editDateTime));
    editDateTime.tm_year = 126;
    editDateTime.tm_mon = 0;
    editDateTime.tm_mday = 1;
    editDateTime.tm_hour = 7;
    editDateTime.tm_min = 0;
  }
  screen = SCREEN_DATE_EDITOR;
  uiDirty = true;
}

//fix later
//^thx past self, it's all fixed up now
void adjustDateField(int delta) {
  switch (dateField) {
    case 0: editDateTime.tm_mday += delta; if (editDateTime.tm_mday > 31) editDateTime.tm_mday = 1; if (editDateTime.tm_mday < 1) editDateTime.tm_mday = 31; break;
    case 1: editDateTime.tm_mon += delta; if (editDateTime.tm_mon > 11) editDateTime.tm_mon = 0; if (editDateTime.tm_mon < 0) editDateTime.tm_mon = 11; break;
    case 2: editDateTime.tm_year += delta; if (editDateTime.tm_year > 199) editDateTime.tm_year = 126; if (editDateTime.tm_year < 126) editDateTime.tm_year = 199; break;
    case 3: editDateTime.tm_hour += delta; if (editDateTime.tm_hour > 23) editDateTime.tm_hour = 0; if (editDateTime.tm_hour < 0) editDateTime.tm_hour = 23; break;
    case 4: editDateTime.tm_min += delta; if (editDateTime.tm_min > 59) editDateTime.tm_min = 0; if (editDateTime.tm_min < 0) editDateTime.tm_min = 59; break;
  }
  uiDirty = true;
}


void openCoffeeMenu() { menuIndex = 0; screen = SCREEN_COFFEE_MENU; uiDirty = true; }
void openAlarmMenu() { menuIndex = 0; screen = SCREEN_ALARM_MENU; uiDirty = true; }

void moveMenu(int delta) {
  int count = screen == SCREEN_COFFEE_MENU ? COFFEE_MENU_COUNT : ALARM_MENU_COUNT;
  menuIndex = (menuIndex + delta + count) % count;
  uiDirty = true;
}

void activateMenu() {
  if (screen == SCREEN_COFFEE_MENU) {
    switch (menuIndex) {
      case 0: confirmChoice = 0; screen = SCREEN_COFFEE_CONFIRM; uiDirty = true; break;
      case 1: initDateEditor(true); break;
      case 2: commandCoffeeCancel(); break;
      case 3: refreshCoffeeStatus(true); break;
    }
  } else if (screen == SCREEN_ALARM_MENU) {
    switch (menuIndex) {
      case 0: initDateEditor(false); break;
      case 1: commandAlarmList(); break;
      case 2: commandAlarmCancelList(); break;
      case 3: commandAlarmSimple("stop-alarm", "STOP-ALARM COMMAND SENT"); break;
      case 4: commandAlarmSyncTime(); break;
      case 5: refreshAlarmStatus(true); break;
    }
  }
}

void handleAction(Action action) {
  if (action == ACT_HOME) {
    screen = SCREEN_HOME;
    menuIndex = 0;
    uiDirty = true;
    return;
  }

  if (screen == SCREEN_MESSAGE || screen == SCREEN_STATUS || screen == SCREEN_ALARM_LIST) {
    if (action == ACT_UP || action == ACT_ENC_CCW) { if (scrollOffset > 0) scrollOffset--; uiDirty = true; }
    else if (action == ACT_DOWN || action == ACT_ENC_CW) { scrollOffset++; uiDirty = true; }
    else if (action == ACT_BACK || action == ACT_ENTER || action == ACT_ENC_PRESS) {
      screen = messageReturnScreen;
      uiDirty = true;
    }
    return;
  }

  if (screen == SCREEN_HOME) {
    if (action == ACT_UP || action == ACT_LEFT || action == ACT_ENC_CCW) {
      menuIndex = (menuIndex + 1) % 2; uiDirty = true;
    } else if (action == ACT_DOWN || action == ACT_RIGHT || action == ACT_ENC_CW) {
      menuIndex = (menuIndex + 1) % 2; uiDirty = true;
    } else if (action == ACT_ENTER || action == ACT_ENC_PRESS) {
      if (menuIndex == 0) openCoffeeMenu();
      else openAlarmMenu();
    }
    return;
  }

  if (screen == SCREEN_COFFEE_MENU || screen == SCREEN_ALARM_MENU) {
    if (action == ACT_UP || action == ACT_LEFT || action == ACT_ENC_CCW) moveMenu(-1);
    else if (action == ACT_DOWN || action == ACT_RIGHT || action == ACT_ENC_CW) moveMenu(1);
    else if (action == ACT_ENTER || action == ACT_ENC_PRESS) activateMenu();
    else if (action == ACT_BACK) { menuIndex = 0; screen = SCREEN_HOME; uiDirty = true; }
    return;
  }

  if (screen == SCREEN_COFFEE_CONFIRM) {
    if (action == ACT_LEFT || action == ACT_RIGHT || action == ACT_UP || action == ACT_DOWN || action == ACT_ENC_CW || action == ACT_ENC_CCW) {
      confirmChoice = 1 - confirmChoice; uiDirty = true;
    } else if (action == ACT_ENTER || action == ACT_ENC_PRESS) {
      if (confirmChoice == 0) commandCoffeeBrew();
      else { screen = SCREEN_COFFEE_MENU; uiDirty = true; }
    } else if (action == ACT_BACK) { screen = SCREEN_COFFEE_MENU; uiDirty = true; }
    return;
  }

  if (screen == SCREEN_DATE_EDITOR) {
    if (action == ACT_LEFT) { dateField = (dateField + 4) % 5; uiDirty = true; }
    else if (action == ACT_RIGHT) { dateField = (dateField + 1) % 5; uiDirty = true; }
    else if (action == ACT_UP || action == ACT_ENC_CW) adjustDateField(1);
    else if (action == ACT_DOWN || action == ACT_ENC_CCW) adjustDateField(-1);
    else if (action == ACT_ENTER || action == ACT_ENC_PRESS) {
      if (dateField < 4) { dateField++; uiDirty = true; }
      else if (editingCoffeeSchedule) commandCoffeeSchedule();
      else commandAlarmSingle();
    } else if (action == ACT_BACK) { screen = editingCoffeeSchedule ? SCREEN_COFFEE_MENU : SCREEN_ALARM_MENU; uiDirty = true; }
    return;
  }

  if (screen == SCREEN_CANCEL_EDITOR) {
    if (alarmChoiceCount > 0 &&
        (action == ACT_UP || action == ACT_ENC_CCW)) {
      alarmSelectedIndex = (alarmSelectedIndex + alarmChoiceCount - 1) % alarmChoiceCount;
      uiDirty = true;
    } else if (alarmChoiceCount > 0 &&
               (action == ACT_DOWN || action == ACT_ENC_CW)) {
      alarmSelectedIndex = (alarmSelectedIndex + 1) % alarmChoiceCount;
      uiDirty = true;
    } else if (action == ACT_ENTER || action == ACT_ENC_PRESS) {
      if (alarmChoiceCount > 0) commandAlarmCancel();
    } else if (action == ACT_BACK) {
      screen = SCREEN_ALARM_MENU;
      uiDirty = true;
    }
  }
}

//input

void initializeInputs() {
  for (size_t i = 0; i < KEY_COUNT; ++i) {
    if (keys[i].externalPullup) pinMode(keys[i].pin, INPUT);
    else pinMode(keys[i].pin, INPUT_PULLUP);
    keys[i].raw = digitalRead(keys[i].pin);
    keys[i].stable = keys[i].raw;
    keys[i].changedAt = millis();
  }
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  lastEncoderAB = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
}

void pollButtons() {
  uint32_t now = millis();
  for (size_t i = 0; i < KEY_COUNT; ++i) {
    int reading = digitalRead(keys[i].pin);
    if (reading != keys[i].raw) {
      keys[i].raw = reading;
      keys[i].changedAt = now;
    }
    if ((now - keys[i].changedAt) >= DEBOUNCE_MS && reading != keys[i].stable) {
      keys[i].stable = reading;
      if (reading == LOW) handleAction(keys[i].action);
    }
  }
}

void pollEncoder() {
  uint8_t ab = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  uint8_t index = (lastEncoderAB << 2) | ab;
  encoderDelta += encoderTable[index];
  lastEncoderAB = ab;
  if (encoderDelta >= 4) {
    encoderDelta = 0;
    handleAction(ACT_ENC_CW);
  } else if (encoderDelta <= -4) {
    encoderDelta = 0;
    handleAction(ACT_ENC_CCW);
  }
}

//refresher (ahhhhhhhh, refreshing)

void refreshAllStatuses(bool force) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!force && millis() - lastStatusRefresh < STATUS_REFRESH_MS) return;
  lastStatusRefresh = millis();
  refreshCoffeeStatus(false);
  refreshAlarmStatus(false);
  uiDirty = true;
}

///

void drawFitText(const String& text, int x, int y, int maxWidth,
                 uint8_t textSize, bool centered = false) {
  tft.setTextSize(textSize);
  String shown = text;
  int16_t bx, by;
  uint16_t bw, bh;
  tft.getTextBounds(shown.c_str(), 0, 0, &bx, &by, &bw, &bh);
  while (shown.length() > 0 && bw > maxWidth) {
    shown.remove(shown.length() - 1);
    tft.getTextBounds(shown.c_str(), 0, 0, &bx, &by, &bw, &bh);
  }
  int drawX = centered ? x + max(0, (maxWidth - (int)bw) / 2) : x;
  tft.setCursor(drawX, y);
  tft.print(shown);
}

void drawHeader(const String& title, const String& subtitle = "") {
  tft.fillRect(0, 0, 320, 43, C_PANEL);
  tft.setTextColor(C_TEXT);
  drawFitText(title, 10, 7, 300, 2);
  tft.setTextColor(C_MUTED);
  drawFitText(subtitle, 11, 29, 298, 1);
  tft.drawFastHLine(0, 43, 320, C_LINE);
}

void drawFooter(const String& text = "ENTER select   BACK return   HOME dashboard") {
  tft.drawFastHLine(0, 218, 320, C_LINE);
  tft.setTextColor(C_MUTED);
  drawFitText(text, 7, 226, 306, 1);
}

void drawStatusDot(int x, int y, bool online) {
  tft.fillCircle(x, y, 4, online ? C_GREEN : C_RED);
}

String coffeeSummary() {
  if (!coffeeOnline) return "Not connected";
  if (coffeeStatus.indexOf("currently scheduled to brew at") >= 0) {
    int p = coffeeStatus.indexOf("currently scheduled to brew at");
    String s = coffeeStatus.substring(p);
    int e = s.indexOf('\n');
    if (e >= 0) s = s.substring(0, e);
    return s;
  }
  if (coffeeStatus.indexOf("no brew is currently scheduled") >= 0) return "No brew scheduled";
  return "Connected";
}

String alarmSummary() {
  if (!alarmOnline) return "Not connected";
  int p = alarmStatus.indexOf("Scheduled alarms:");
  if (p >= 0) {
    String s = alarmStatus.substring(p);
    int e = s.indexOf('\n');
    if (e >= 0) s = s.substring(0, e);
    return s;
  }
  return "Connected";
}

void drawHome() {
  tft.fillScreen(C_BG);
  drawHeader("MAGICPad", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Wi-Fi disconnected");

  tft.setTextColor(C_TEXT);
  tft.setTextSize(2);
  tft.setCursor(10, 52);
  if (timeIsValid()) {
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    char clockBuf[12];
    strftime(clockBuf, sizeof(clockBuf), "%I:%M %p", &local);
    tft.print(clockBuf);
  } else {
    tft.print("--:-- --");
  }
  tft.setTextColor(C_MUTED);
  drawFitText(timeIsValid() ? currentDateTimeText().substring(0, 10) : "Waiting for NTP",
              205, 59, 105, 1);

  // Coffee card.
  tft.fillRoundRect(8, 83, 148, 91, C_PANEL, 9);
  tft.drawRoundRect(8, 83, 148, 91, 9, menuIndex == 0 ? C_TEAL : C_LINE);
  drawStatusDot(143, 96, coffeeOnline);
  tft.setTextColor(C_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(18, 94);
  tft.print("COFFEE");
  tft.setTextSize(1);
  tft.setTextColor(C_TEXT);
  tft.setCursor(18, 119);
  tft.print(coffeeOnline ? "ONLINE" : "OFFLINE");
  tft.setTextColor(C_MUTED);
  drawFitText(coffeeSummary(), 18, 137, 128, 1);
  drawFitText("IP: " + coffeeIP, 18, 155, 132, 1);


  tft.fillRoundRect(164, 83, 148, 91, C_PANEL, 9);
  tft.drawRoundRect(164, 83, 148, 91, 9, menuIndex == 1 ? C_TEAL : C_LINE);
  drawStatusDot(299, 96, alarmOnline);
  tft.setTextColor(C_BLUE);
  tft.setTextSize(2);
  tft.setCursor(174, 94);
  tft.print("ALARM");
  tft.setTextSize(1);
  tft.setTextColor(C_TEXT);
  tft.setCursor(174, 119);
  tft.print(alarmOnline ? "ONLINE" : "OFFLINE");
  tft.setTextColor(C_MUTED);
  drawFitText(alarmSummary(), 174, 137, 128, 1);
  drawFitText("IP: " + alarmIP, 174, 155, 132, 1);

  
  tft.fillRoundRect(8, 184, 148, 27, menuIndex == 0 ? C_PANEL2 : C_PANEL, 6);
  tft.fillRoundRect(164, 184, 148, 27, menuIndex == 1 ? C_PANEL2 : C_PANEL, 6);
  tft.setTextColor(C_TEXT);
  drawFitText("OPEN COFFEE MAKER", 14, 194, 136, 1, true);
  drawFitText("OPEN ALARM CLOCK", 170, 194, 136, 1, true);
  drawFooter("UP/DOWN or encoder   ENTER open   HOME dashboard");
}

void drawMenu(const String& title, const char* items[], int count) {
  tft.fillScreen(C_BG);
  drawHeader(title, "Rotate encoder or use UP/DOWN");
  const int rowH = 33;
  const int visible = 5;
  int start = 0;
  if (menuIndex >= visible) start = menuIndex - visible + 1;
  for (int row = 0; row < visible; ++row) {
    int idx = start + row;
    if (idx >= count) break;
    int y = 53 + row * rowH;
    bool selected = idx == menuIndex;
    tft.fillRoundRect(9, y, 302, 28, 6, selected ? C_PANEL2 : C_PANEL);
    if (selected) tft.drawRoundRect(9, y, 302, 28, 6, C_TEAL);
    tft.setTextColor(selected ? C_TEXT : C_MUTED);
    tft.setTextSize(2);
    tft.setCursor(19, y + 7);
    tft.print(selected ? ">" : " ");
    drawFitText(String(items[idx]), 43, y + 10, 260, 1);
  }
  drawFooter();
}

void drawWrapped(const String& text, int x, int y, int maxChars, int maxLines) {
  int logicalLine = 0;
  int visibleLine = 0;
  int start = 0;
  while (start < (int)text.length() && visibleLine < maxLines) {
    int newline = text.indexOf('\n', start);
    int end = newline >= 0 ? newline : text.length();
    if (start == end) {
      logicalLine++;
    }
    while (start < end) {
      int take = min(maxChars, end - start);
      if (take < end - start) {
        int space = text.lastIndexOf(' ', start + take);
        if (space > start) take = space - start;
      }
      if (logicalLine >= scrollOffset && visibleLine < maxLines) {
        tft.setCursor(x, y + visibleLine * 15);
        tft.print(text.substring(start, start + take));
        visibleLine++;
      }
      start += take;
      while (start < end && text[start] == ' ') start++;
      logicalLine++;
    }
    if (newline >= 0) start = newline + 1;
    else break;
  }
}

void drawMessageScreen(bool isList) {
  tft.fillScreen(C_BG);
  drawHeader(messageTitle, "Device response");
  tft.setTextColor(C_TEXT);
  tft.setTextSize(1);
  drawWrapped(messageBody, 10, 55, 50, 10);
  drawFooter("UP/DOWN scroll   ENTER or BACK return   HOME dashboard");
}

void drawCoffeeConfirm() {
  tft.fillScreen(C_BG);
  drawHeader("BREW COFFEE?", "Keurig Mini");
  tft.setTextColor(C_TEXT);
  tft.setTextSize(2);
  tft.setCursor(16, 76); tft.print("Start brewing now?");
  tft.setTextSize(1);
  tft.setTextColor(C_MUTED);
  tft.setCursor(16, 108); tft.print("This will make you less sleepy");
  tft.setCursor(16, 124); tft.print("you sure you need the caffiene?");
  tft.fillRoundRect(25, 163, 120, 39, confirmChoice == 0 ? C_GREEN : C_PANEL, 7);
  tft.fillRoundRect(175, 163, 120, 39, confirmChoice == 1 ? C_RED : C_PANEL, 7);
  tft.setTextColor(C_TEXT); tft.setTextSize(2);
  tft.setCursor(65, 175); tft.print("YES");
  tft.setCursor(215, 175); tft.print("NO");
  drawFooter("LEFT/RIGHT choose   ENTER confirm   BACK cancel");
}

void drawDateEditor() {
  tft.fillScreen(C_BG);
  drawHeader(editingCoffeeSchedule ? "SCHEDULE BREW" : "NEW ALARM", "Select date and 24-hour time");
  const int x[5] = {12, 73, 134, 195, 256};
  const int w[5] = {49, 49, 49, 49, 52};
  const char* labels[5] = {"DAY", "MONTH", "YEAR", "HOUR", "MIN"};
  int values[5] = {editDateTime.tm_mday, editDateTime.tm_mon + 1,
                   editDateTime.tm_year + 1900, editDateTime.tm_hour,
                   editDateTime.tm_min};
  for (int i = 0; i < 5; ++i) {
    tft.fillRoundRect(x[i], 91, w[i], 66, 6, C_PANEL);
    tft.drawRoundRect(x[i], 91, w[i], 66, 6, dateField == i ? C_TEAL : C_LINE);
    tft.setTextColor(C_MUTED); tft.setTextSize(1);
    tft.setCursor(x[i] + 5, 101); tft.print(labels[i]);
    tft.setTextColor(C_TEXT); tft.setTextSize(i == 2 ? 1 : 2);
    tft.setCursor(x[i] + 5, 125);
    if (i == 2) tft.print(values[i]);
    else tft.print(twoDigits(values[i]));
  }
  tft.setTextColor(C_MUTED); tft.setTextSize(1);
  tft.setCursor(12, 171);
  tft.print("Format: DD / MM / YYYY     HH : MM");



  tft.setCursor(12, 187);
  tft.print(timeIsValid() ? "Clock synchronized" : "Waiting for network time; scheduling off");
  tft.setCursor(12, 202);
  tft.print("ENTER advances; on MIN, ENTER sends.");
  drawFooter("LEFT/RIGHT field   UP/DOWN or encoder adjust   BACK cancel");
}

void drawCancelEditor() {
  tft.fillScreen(C_BG);
  drawHeader("CANCEL ALARM",
             alarmChoiceCount > 0 ? String(alarmChoiceCount) + " scheduled - select one" : "No selectable alarms found");

  if (alarmChoiceCount == 0) {
    tft.setTextColor(C_MUTED);
    tft.setTextSize(1);
    tft.setCursor(14, 91);
    tft.print("No scheduled alarms were returned.");
    tft.setCursor(14, 116);
    tft.print("Use BACK to return to the alarm menu.");
    if (listResponse.length() > 0 &&
        listResponse.indexOf("no alarm") < 0 &&
        listResponse.indexOf("No alarm") < 0) {
      tft.setCursor(14, 150);
      tft.print(listResponse.substring(0, 48));
    }
    drawFooter("BACK return   HOME dashboard");
    return;
  }

  const int rowHeight = 31;
  const int visibleRows = 5;
  int firstVisible = 0;
  if (alarmSelectedIndex >= visibleRows) firstVisible = alarmSelectedIndex - visibleRows + 1;

  for (int row = 0; row < visibleRows; ++row) {
    int idx = firstVisible + row;
    if (idx >= alarmChoiceCount) break;

    int y = 51 + row * rowHeight;
    bool selected = idx == alarmSelectedIndex;
    tft.fillRoundRect(8, y, 304, 27, 5, selected ? C_PANEL2 : C_PANEL);
    if (selected) tft.drawRoundRect(8, y, 304, 27, 5, C_TEAL);

    String rowText = String(alarmChoices[idx].number) + ". " + alarmChoices[idx].label;
    tft.setTextColor(selected ? C_TEXT : C_MUTED);
    tft.setTextSize(1);
    tft.setCursor(15, y + 9);
    


















    while (rowText.length() > 0 && rowText.length() > 47) rowText.remove(rowText.length() - 1);
    tft.print(rowText);
  }

  drawFooter("UP/DOWN or encoder select   ENTER cancel   BACK return");
}

void drawCurrentScreen() {
  switch (screen) {
    case SCREEN_HOME: drawHome(); break;
    case SCREEN_COFFEE_MENU: drawMenu("COFFEE MAKER", coffeeMenu, COFFEE_MENU_COUNT); break;
    case SCREEN_ALARM_MENU: drawMenu("ALARM CLOCK", alarmMenu, ALARM_MENU_COUNT); break;
    case SCREEN_COFFEE_CONFIRM: drawCoffeeConfirm(); break;
    case SCREEN_DATE_EDITOR: drawDateEditor(); break;
    case SCREEN_CANCEL_EDITOR: drawCancelEditor(); break;
    case SCREEN_ALARM_LIST: drawMessageScreen(true); break;
    case SCREEN_STATUS: drawMessageScreen(false); break;
    case SCREEN_MESSAGE: drawMessageScreen(false); break;
  }
  uiDirty = false;
}



void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("ESP32 Smart-Home Controller starting...");

  pinMode(TFT_CS, OUTPUT);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(SD_CS, HIGH);

  SPI.begin(TFT_SCK, SD_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(1); // Landscape: 320 x 240
  tft.fillScreen(C_BG);
  tft.setTextColor(C_TEXT);
  tft.setTextSize(2);
  tft.setCursor(18, 92);
  tft.print("MAGICPad");
  tft.setTextSize(1);
  tft.setCursor(18, 122);
  tft.setTextColor(C_MUTED);
  tft.print("Starting magicpad...");

  initializeInputs();
  beginWiFi();

  Serial.println("MagicPad initialized.");
  Serial.print("Coffee maker IP: "); Serial.println(coffeeIP);
  Serial.print("Alarm clock IP: "); Serial.println(alarmIP);


  screen = SCREEN_HOME;
  uiDirty = true;
}

void loop() {
  maintainWiFi();
  pollButtons();
  pollEncoder();

//refresher again
  if (screen == SCREEN_HOME && WiFi.status() == WL_CONNECTED &&
      (!statusRefreshDone || millis() - lastStatusRefresh >= STATUS_REFRESH_MS)) {
    statusRefreshDone = true;
    refreshAllStatuses(true);
  }

//and again...
  if (screen == SCREEN_HOME && millis() - lastClockDraw >= 1000) {
    lastClockDraw = millis();
    uiDirty = true;
  }

  if (uiDirty) drawCurrentScreen();
  delay(2);
}
//finally
//I can get some sleep
//I'm done with this
//no shot I'm adding games, that took forever
//idk, maybe I can "borrow" someone elses games of github or something like that
//if ur a reviewer reading this tha's something I'd do after half life is over and wouldn't count it as any time