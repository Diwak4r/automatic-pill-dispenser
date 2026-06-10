// ============================================================
//  PILL DISPENSER — Single motor, 360° spin, dual-core
//  Hardware : ESP32 + 28BYJ-48/ULN2003 + DS3231 + buzzer+switch
//  Libraries: RTClib, UniversalTelegramBot, ArduinoJson, Preferences
// ============================================================

#include <Wire.h>
#include "RTClib.h"
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ── Config ───────────────────────────────────────────────────
const char* WIFI_SSID     = "WIFI_SSID";
const char* WIFI_PASSWORD = "hello_world2@";
const char* BOT_TOKEN     = "8637867114:AAH1rgQjveVi_TMQihyvAPXbcVp8tmRF_Jo";
const char* CHAT_ID       = "7134620341";

// ── Pins ─────────────────────────────────────────────────────
#define MOTOR_IN1  32
#define MOTOR_IN2  33
#define MOTOR_IN3  25
#define MOTOR_IN4  26
#define I2C_SDA    27
#define I2C_SCL    14
#define BUZZER_PIN 13

// ── Constants ────────────────────────────────────────────────
#define STEPS_PER_REV        4092
#define STEP_DELAY_MS        10
#define REMINDER_INTERVAL_MS 120000
#define MAX_REMINDERS        3
#define MAX_SCHEDULES        8
#define MAX_MISSED           20
#define TELEGRAM_INTERVAL_MS 3000
#define NOTE_C5 523
#define NOTE_E5 659
#define NOTE_G5 784
#define NOTE_C6 1047
#define NOTE_A4 440
#define NOTE_F5 698

// ── Reversed half-step sequence (confirmed working) ───────────
const int SEQ[8][4] = {
  {1,0,0,1},{0,0,0,1},{0,0,1,1},{0,0,1,0},
  {0,1,1,0},{0,1,0,0},{1,1,0,0},{1,0,0,0}
};

// ── Data structures ───────────────────────────────────────────
struct Schedule   { uint8_t hour, minute; bool enabled; };
struct MissedDose { uint8_t hour, minute, day, month;   };
enum   State      { IDLE, DISPENSING, WAITING_CONFIRM, DONE, ERROR_STATE };

// ── Global state ─────────────────────────────────────────────
State         currentState   = IDLE;
int           activeSlot     = -1;
int           currentStep    = 0;
int           stepsRemaining = 0;
bool          buzzerActive   = false;
bool          switchBaseline = false;
int           reminderCount  = 0;
unsigned long lastStepTime = 0, lastReminderTime = 0;
unsigned long lastTgSend   = 0, lastTgPoll       = 0;
unsigned long lastSchedChk = 0;

Schedule   schedules[MAX_SCHEDULES];
int        scheduleCount = 0;
uint8_t    lastDay[MAX_SCHEDULES];
MissedDose missedLog[MAX_MISSED];
int        missedCount = 0;

RTC_DS3231           rtc;
Preferences          prefs;
WiFiClientSecure     secClient;
UniversalTelegramBot bot(BOT_TOKEN, secClient);
SemaphoreHandle_t    mtx;

// ── Motor helpers ─────────────────────────────────────────────
void motorOff() {
  digitalWrite(MOTOR_IN1, LOW); digitalWrite(MOTOR_IN2, LOW);
  digitalWrite(MOTOR_IN3, LOW); digitalWrite(MOTOR_IN4, LOW);
}
void stepMotor() {
  int s = currentStep++ % 8;
  digitalWrite(MOTOR_IN1, SEQ[s][0]); digitalWrite(MOTOR_IN2, SEQ[s][1]);
  digitalWrite(MOTOR_IN3, SEQ[s][2]); digitalWrite(MOTOR_IN4, SEQ[s][3]);
}

// ── Buzzer helpers ────────────────────────────────────────────
void playTone(int freq, int ms) {
  ledcAttach(BUZZER_PIN, freq, 8); ledcWrite(BUZZER_PIN, 128);
  delay(ms); ledcWrite(BUZZER_PIN, 0); ledcDetach(BUZZER_PIN); delay(30);
}
void beep(int n, int ms) {
  for (int i = 0; i < n; i++) {
    ledcAttach(BUZZER_PIN, 1000, 8); ledcWrite(BUZZER_PIN, 128);
    delay(ms); ledcWrite(BUZZER_PIN, 0); ledcDetach(BUZZER_PIN);
    if (i < n-1) delay(100);
  }
}
void playReminder() { playTone(NOTE_A4, 200); playTone(NOTE_F5, 200); }

void buzzerOn_f() {
  buzzerActive = true;
  ledcAttach(BUZZER_PIN, NOTE_A4, 8); ledcWrite(BUZZER_PIN, 128);
}
void buzzerOff_f() {
  buzzerActive = false;
  ledcWrite(BUZZER_PIN, 0); ledcDetach(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);
}
bool switchFlipped() { return digitalRead(BUZZER_PIN) != switchBaseline; }

// ── NVS ───────────────────────────────────────────────────────
void saveSchedules() {
  prefs.begin("pd", false);
  prefs.putInt("cnt", scheduleCount);
  for (int i = 0; i < scheduleCount; i++) {
    prefs.putUChar(("h"+String(i)).c_str(), schedules[i].hour);
    prefs.putUChar(("m"+String(i)).c_str(), schedules[i].minute);
    prefs.putBool (("e"+String(i)).c_str(), schedules[i].enabled);
  }
  prefs.end();
}
void loadSchedules() {
  prefs.begin("pd", true);
  scheduleCount = prefs.getInt("cnt", 0);
  for (int i = 0; i < scheduleCount; i++) {
    schedules[i] = { prefs.getUChar(("h"+String(i)).c_str(), 8),
                     prefs.getUChar(("m"+String(i)).c_str(), 0),
                     prefs.getBool (("e"+String(i)).c_str(), true) };
    lastDay[i] = 0;
  }
  prefs.end();
}
void saveMissedLog() {
  prefs.begin("ms", false);
  prefs.putInt("cnt", missedCount);
  for (int i = 0; i < missedCount; i++) {
    prefs.putUChar(("a"+String(i)).c_str(), missedLog[i].hour);
    prefs.putUChar(("b"+String(i)).c_str(), missedLog[i].minute);
    prefs.putUChar(("c"+String(i)).c_str(), missedLog[i].day);
    prefs.putUChar(("d"+String(i)).c_str(), missedLog[i].month);
  }
  prefs.end();
}
void loadMissedLog() {
  prefs.begin("ms", true);
  missedCount = prefs.getInt("cnt", 0);
  for (int i = 0; i < missedCount; i++)
    missedLog[i] = { prefs.getUChar(("a"+String(i)).c_str(), 0),
                     prefs.getUChar(("b"+String(i)).c_str(), 0),
                     prefs.getUChar(("c"+String(i)).c_str(), 0),
                     prefs.getUChar(("d"+String(i)).c_str(), 0) };
  prefs.end();
}

// ── Schedule helpers ──────────────────────────────────────────
bool addSchedule(uint8_t h, uint8_t m) {
  if (scheduleCount >= MAX_SCHEDULES || h > 23 || m > 59) return false;
  schedules[scheduleCount] = { h, m, true };
  lastDay[scheduleCount++] = 0;
  saveSchedules();
  return true;
}
bool removeSchedule(int idx) {
  if (idx < 0 || idx >= scheduleCount) return false;
  for (int i = idx; i < scheduleCount-1; i++) {
    schedules[i] = schedules[i+1]; lastDay[i] = lastDay[i+1];
  }
  scheduleCount--; saveSchedules(); return true;
}
String scheduleListText() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  String s = "Schedules:\n";
  if (!scheduleCount) { xSemaphoreGive(mtx); return s + "(none)"; }
  for (int i = 0; i < scheduleCount; i++) {
    char t[6]; sprintf(t, "%02d:%02d", schedules[i].hour, schedules[i].minute);
    s += String(i) + ") " + t + (schedules[i].enabled ? "" : " [off]") + "\n";
  }
  xSemaphoreGive(mtx); return s;
}

// ── Telegram ──────────────────────────────────────────────────
void sendTelegram(String msg) {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long gap = millis() - lastTgSend;
  if (gap < TELEGRAM_INTERVAL_MS) delay(TELEGRAM_INTERVAL_MS - gap);
  bot.sendMessage(CHAT_ID, msg, "");
  lastTgSend = millis();
  Serial.println("[TG] " + msg);
}

bool parseTime(const String& s, uint8_t& h, uint8_t& m) {
  int sep = s.indexOf(':');
  if (sep <= 0) return false;
  h = s.substring(0, sep).toInt();
  m = s.substring(sep+1).toInt();
  return h <= 23 && m <= 59;
}

void handleTelegramCommands() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastTgPoll < TELEGRAM_INTERVAL_MS) return;
  lastTgPoll = millis();

  int n = bot.getUpdates(bot.last_message_received + 1);
  while (n) {
    for (int i = 0; i < n; i++) {
      if (bot.messages[i].chat_id != CHAT_ID) {
        bot.sendMessage(bot.messages[i].chat_id, "Unauthorized.", ""); continue;
      }
      String txt = bot.messages[i].text; txt.trim();

      xSemaphoreTake(mtx, portMAX_DELAY);
      bool busy = (currentState != IDLE);
      int  cnt  = scheduleCount;
      xSemaphoreGive(mtx);

      if (txt == "/start" || txt == "/help") {
        bot.sendMessage(CHAT_ID, "Commands:\n/list\n/add HH:MM\n/remove INDEX\n/reschedule INDEX HH:MM", "");
      } else if (txt == "/list") {
        bot.sendMessage(CHAT_ID, scheduleListText(), "");
      } else if (txt.startsWith("/add ")) {
        if (busy) { bot.sendMessage(CHAT_ID, "Device busy.", ""); continue; }
        uint8_t h, m;
        if (!parseTime(txt.substring(5), h, m)) {
          bot.sendMessage(CHAT_ID, "Format: /add HH:MM", "");
        } else {
          xSemaphoreTake(mtx, portMAX_DELAY);
          bool ok = addSchedule(h, m);
          xSemaphoreGive(mtx);
          bot.sendMessage(CHAT_ID, ok ? "Added.\n" + scheduleListText() : "Failed.", "");
        }
      } else if (txt.startsWith("/remove ")) {
        if (busy) { bot.sendMessage(CHAT_ID, "Device busy.", ""); continue; }
        xSemaphoreTake(mtx, portMAX_DELAY);
        bool ok = removeSchedule(txt.substring(8).toInt());
        xSemaphoreGive(mtx);
        bot.sendMessage(CHAT_ID, ok ? "Removed.\n" + scheduleListText() : "Invalid index.", "");
      } else if (txt.startsWith("/reschedule ")) {
        if (busy) { bot.sendMessage(CHAT_ID, "Device busy.", ""); continue; }
        int s1 = txt.indexOf(' '), s2 = txt.indexOf(' ', s1+1);
        if (s2 < 0) { bot.sendMessage(CHAT_ID, "Format: /reschedule INDEX HH:MM", ""); continue; }
        int idx = txt.substring(s1+1, s2).toInt();
        uint8_t h, m;
        if (!parseTime(txt.substring(s2+1), h, m) || idx < 0 || idx >= cnt) {
          bot.sendMessage(CHAT_ID, "Invalid input.", "");
        } else {
          xSemaphoreTake(mtx, portMAX_DELAY);
          schedules[idx] = { h, m, true }; lastDay[idx] = 0; saveSchedules();
          xSemaphoreGive(mtx);
          bot.sendMessage(CHAT_ID, "Rescheduled.\n" + scheduleListText(), "");
        }
      } else {
        bot.sendMessage(CHAT_ID, "Unknown. Send /help", "");
      }
    }
    n = bot.getUpdates(bot.last_message_received + 1);
  }
}

// ── Serial commands ───────────────────────────────────────────
void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n'); cmd.trim(); cmd.toLowerCase();

  if (cmd == "time") {
    DateTime now = rtc.now();
    Serial.printf("[TIME] %02d:%02d:%02d  %02d/%02d/%04d\n",
      now.hour(), now.minute(), now.second(), now.day(), now.month(), now.year());
  } else if (cmd.startsWith("set ")) {
    int h = cmd.substring(4,6).toInt(), m = cmd.substring(7,9).toInt();
    rtc.adjust(DateTime(2024, 1, 1, h, m, 0));
    Serial.printf("[RTC] Set to %02d:%02d\n", h, m);
  } else if (cmd.startsWith("add ")) {
    int h = cmd.substring(4,6).toInt(), m = cmd.substring(7,9).toInt();
    Serial.println(addSchedule(h, m) ? "[SCHED] Added." : "[SCHED] Failed.");
  } else if (cmd.startsWith("remove ")) {
    Serial.println(removeSchedule(cmd.substring(7).toInt()) ? "[SCHED] Removed." : "[SCHED] Invalid.");
  } else if (cmd == "list") {
    Serial.print(scheduleListText());
  } else if (cmd == "missed") {
    Serial.printf("\n--- Missed (%d) ---\n", missedCount);
    for (int i = 0; i < missedCount; i++)
      Serial.printf("  [%d] %02d:%02d  %02d/%02d\n", i,
        missedLog[i].hour, missedLog[i].minute, missedLog[i].day, missedLog[i].month);
  } else if (cmd == "ip") {
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[CMD] Unknown. Commands: time, set HH MM, add HH MM, remove N, list, missed, ip");
  }
}

// ── Schedule check ────────────────────────────────────────────
void checkSchedule() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (currentState != IDLE) { xSemaphoreGive(mtx); return; }
  DateTime now = rtc.now();
  for (int i = 0; i < scheduleCount; i++) {
    if (!schedules[i].enabled) continue;
    if (now.hour() == schedules[i].hour && now.minute() == schedules[i].minute
        && now.second() == 0 && lastDay[i] != now.day()) {
      lastDay[i]     = now.day();
      activeSlot     = i;
      stepsRemaining = STEPS_PER_REV;
      currentStep    = 0;
      currentState   = DISPENSING;
      Serial.printf("[SCHED] Slot %d triggered — starting dispense.\n", i);
      break;
    }
  }
  xSemaphoreGive(mtx);
}

// ── Control task (Core 1) ─────────────────────────────────────
void controlTask(void* p) {
  for (;;) {
    handleSerialCommands();
    if (millis() - lastSchedChk >= 1000) { lastSchedChk = millis(); checkSchedule(); }

    switch (currentState) {
      case IDLE: break;

      case DISPENSING:
        if (millis() - lastStepTime >= STEP_DELAY_MS) {
          lastStepTime = millis();
          if (stepsRemaining > 0) { stepMotor(); stepsRemaining--; }
          else {
            motorOff();
            buzzerOn_f();
            switchBaseline = digitalRead(BUZZER_PIN);
            DateTime now = rtc.now();
            char t[6]; sprintf(t, "%02d:%02d", now.hour(), now.minute());
            sendTelegram("Pill dispensed at " + String(t) + ". Waiting for patient.");
            lastReminderTime = millis(); reminderCount = 0;
            currentState = WAITING_CONFIRM;
          }
        }
        break;

      case WAITING_CONFIRM:
        if (switchFlipped()) {
          buzzerOff_f(); beep(2, 80);
          DateTime now = rtc.now(); char t[6]; sprintf(t, "%02d:%02d", now.hour(), now.minute());
          sendTelegram("✓ Patient took pill at " + String(t) + ".");
          currentState = DONE;
        } else if (millis() - lastReminderTime >= REMINDER_INTERVAL_MS) {
          lastReminderTime = millis();
          if (++reminderCount >= MAX_REMINDERS) {
            buzzerOff_f(); beep(5, 100);
            DateTime now = rtc.now(); char t[6]; sprintf(t, "%02d:%02d", now.hour(), now.minute());
            if (missedCount < MAX_MISSED) {
              uint8_t h = activeSlot >= 0 ? schedules[activeSlot].hour   : now.hour();
              uint8_t m = activeSlot >= 0 ? schedules[activeSlot].minute : now.minute();
              missedLog[missedCount++] = { h, m, now.day(), now.month() };
              saveMissedLog();
            }
            sendTelegram("✗ MISSED: Patient did not take pill (due " + String(t) + ").");
            currentState = DONE;
          } else {
            playReminder();
            sendTelegram("⚠ Reminder " + String(reminderCount) + "/3: Patient hasn't taken pill yet!");
          }
        }
        break;

      case DONE:
        vTaskDelay(pdMS_TO_TICKS(1000));
        activeSlot = -1; currentState = IDLE;
        Serial.println("[IDLE] Ready.");
        break;

      case ERROR_STATE:
        sendTelegram("ERROR: Motor issue. Check device.");
        currentState = IDLE;
        break;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ── Telegram task (Core 0) ────────────────────────────────────
void telegramTask(void* p) {
  for (;;) { handleTelegramCommands(); vTaskDelay(pdMS_TO_TICKS(50)); }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("=== Pill Dispenser ===");

  pinMode(MOTOR_IN1, OUTPUT); pinMode(MOTOR_IN2, OUTPUT);
  pinMode(MOTOR_IN3, OUTPUT); pinMode(MOTOR_IN4, OUTPUT);
  motorOff();
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!rtc.begin()) { Serial.println("[ERROR] RTC not found!"); while(1); }
  if (rtc.lostPower()) { rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); }

  loadSchedules(); loadMissedLog();
  if (!scheduleCount) { addSchedule(8,0); addSchedule(14,0); addSchedule(21,0); }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());
    secClient.setInsecure();
    sendTelegram("Pill dispenser online. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] Offline.");
  }

  mtx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(controlTask,  "ctrl", 8192,  NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(telegramTask, "tg",   12288, NULL, 1, NULL, 0);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
