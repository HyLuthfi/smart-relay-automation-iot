#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <WebSocketsServer.h>
#include <ESP8266HTTPUpdateServer.h>

#define FIRMWARE_VERSION "2.2.0"
#define BUILD_DATE __DATE__
#define BUILD_TIME __TIME__

// --- KONFIGURASI PENGGUNA ---
char ap_ssid[33] = "ESP8266_Relay";
char ap_password[33] = "12345678";
char hostname[33] = "ESP-Relay";
const char* ADMIN_PASSWORD = "admin123";
int relayPins[4] = {D5, D6, D7, D8};
// ----------------------------

ESP8266WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
ESP8266HTTPUpdateServer httpUpdater;

bool relayStatus[4] = {false, false, false, false};
int relayMode[4] = {0, 0, 0, 0};

unsigned long timerDuration[4] = {0, 0, 0, 0};
unsigned long timerStart[4] = {0, 0, 0, 0};

char relayNames[4][17] = {"Relay 1", "Relay 2", "Relay 3", "Relay 4"};

unsigned int cycleOnDuration[4] = {0, 0, 0, 0};
unsigned int cycleOffDuration[4] = {0, 0, 0, 0};
bool cycleState[4] = {false, false, false, false};
unsigned long lastCycleSwitch[4] = {0, 0, 0, 0};

byte relayEnabledMask = 0x0F;

#define MAX_SCHEDULES 3

struct Schedule {
  int onHour;
  int onMin;
  int offHour;
  int offMin;
  byte dayMask;
  bool enabled;
};

Schedule schedules[4][MAX_SCHEDULES];

int currentHour = 0;
int currentMinute = 0;
int currentDayOfWeek = 0;
unsigned long lastTimeSync = 0;

#define EEPROM_SIZE 1024
#define EEPROM_INIT_FLAG 0
#define EEPROM_MODE_START 1
#define EEPROM_NAMES_START 5
#define EEPROM_TIME_START 73
#define EEPROM_RELAY_STATUS 76
#define EEPROM_TIMER_START 77
#define EEPROM_SCHEDULES_START 85
#define EEPROM_EMERGENCY 170
#define EEPROM_WIFI_START 175
#define EEPROM_PASS_START 207
#define EEPROM_HOST_START 239
#define EEPROM_PINS_START 271
#define EEPROM_RTC_FLAG 275
#define EEPROM_CYCLE_START 276
#define EEPROM_RELAY_ENABLE 292
#define EEPROM_INIT_FLAG_VAL 128

unsigned long lastTimeSave = 0;
bool useRTC = false;

unsigned long lastRTCSyncMillis = 0;
unsigned long lastRTCTimestamp = 0;
unsigned long rtcSyncInterval = 5000;
int rtcReadSuccess = 0;
int rtcReadFailed = 0;
bool rtcSetPending = false;
int rtcSetHour = 0, rtcSetMin = 0, rtcSetSec = 0, rtcSetDow = 0;
unsigned long lastRTCSetAttempt = 0;
int rtcSetRetryCount = 0;
int rtcLastHour = -1, rtcLastMin = -1, rtcLastSec = -1, rtcLastDow = -1;

bool emergencyMode = false;

void broadcastStatus();
String getStatusJSON();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);
void webLog(String msg);
void webLogf(const char* format, ...);

void saveConfig() {
  EEPROM.write(EEPROM_INIT_FLAG, EEPROM_INIT_FLAG_VAL);
  broadcastStatus();

  for(int i = 0; i < 4; i++) {
    EEPROM.write(EEPROM_MODE_START + i, relayMode[i]);
  }

  int addr = EEPROM_NAMES_START;
  for(int i = 0; i < 4; i++) {
    for(int j = 0; j < 17; j++) {
      EEPROM.write(addr++, relayNames[i][j]);
    }
  }

  addr = EEPROM_SCHEDULES_START;
  for(int i = 0; i < 4; i++) {
    for(int j = 0; j < MAX_SCHEDULES; j++) {
      EEPROM.write(addr++, schedules[i][j].onHour);
      EEPROM.write(addr++, schedules[i][j].onMin);
      EEPROM.write(addr++, schedules[i][j].offHour);
      EEPROM.write(addr++, schedules[i][j].offMin);
      EEPROM.write(addr++, schedules[i][j].dayMask);
      EEPROM.write(addr++, schedules[i][j].enabled ? 1 : 0);
    }
  }

  if(EEPROM.commit()) {
    Serial.println("✓ Config saved to EEPROM");
  } else {
    Serial.println("✗ Failed to save config");
  }
}

void saveTime() {
  EEPROM.write(EEPROM_TIME_START, currentHour);
  EEPROM.write(EEPROM_TIME_START + 1, currentMinute);
  EEPROM.write(EEPROM_TIME_START + 2, currentDayOfWeek);
  if(EEPROM.commit()) {
    webLogf("✓ Time saved: %02d:%02d Day:%d", currentHour, currentMinute, currentDayOfWeek);
  }
}

void saveRelayStatus() {
  byte statusByte = 0;
  for(int i = 0; i < 4; i++) {
    if(relayStatus[i]) {
      statusByte |= (1 << i);
    }
  }
  EEPROM.write(EEPROM_RELAY_STATUS, statusByte);
  if(EEPROM.commit()) {
    webLogf("✓ Relay status saved: 0x%02X", statusByte);
  }
  broadcastStatus();
}

void saveTimerData() {
  int addr = EEPROM_TIMER_START;
  for(int i = 0; i < 4; i++) {
    if(relayMode[i] == 2 && timerDuration[i] > 0) {
      unsigned long elapsed = millis() - timerStart[i];
      unsigned long remaining = 0;
      if(timerDuration[i] > elapsed) {
        remaining = (timerDuration[i] - elapsed) / 1000;
      }
      EEPROM.write(addr, remaining >> 8);
      EEPROM.write(addr + 1, remaining & 0xFF);
    } else {
      EEPROM.write(addr, 0);
      EEPROM.write(addr + 1, 0);
    }
    addr += 2;
  }
  broadcastStatus();
  EEPROM.commit();
}

void saveCycleData(int relay) {
  int addr = EEPROM_CYCLE_START + (relay * 4);
  EEPROM.write(addr, cycleOnDuration[relay] >> 8);
  EEPROM.write(addr+1, cycleOnDuration[relay] & 0xFF);
  EEPROM.write(addr+2, cycleOffDuration[relay] >> 8);
  EEPROM.write(addr+3, cycleOffDuration[relay] & 0xFF);
  EEPROM.commit();
}

void saveEmergency() {
  EEPROM.write(EEPROM_EMERGENCY, emergencyMode ? 1 : 0);
  if(EEPROM.commit()) {
    webLogf("✓ Emergency mode saved: %s", emergencyMode ? "ACTIVE" : "OFF");
  }
}

void loadEmergency() {
  emergencyMode = EEPROM.read(EEPROM_EMERGENCY) == 1;
  webLogf("Emergency mode: %s", emergencyMode ? "ACTIVE" : "OFF");

  if(emergencyMode) {

    for(int i = 0; i < 4; i++) {
      digitalWrite(relayPins[i], HIGH);
      relayStatus[i] = false;
    }
    webLog("!!! ALL RELAYS OFF - EMERGENCY MODE !!!");
  }
}

byte bcdToDec(byte val) { return ( (val/16*10) + (val%16) ); }
byte decToBcd(byte val) { return ( (val/10*16) + (val%10) ); }

bool syncWithRTC() {
  if(!useRTC) return false;

  Wire.beginTransmission(0x68);
  Wire.write(0x00);
  byte error = Wire.endTransmission();

  if (error != 0) {
    rtcReadFailed++;
    return false;
  }

  Wire.requestFrom((int)0x68, 7);
  if (Wire.available() < 7) {
    rtcReadFailed++;
    webLog("RTC: Not enough data");
    return false;
  }

  byte second = bcdToDec(Wire.read() & 0x7F);
  byte minute = bcdToDec(Wire.read() & 0x7F);
  byte hour = bcdToDec(Wire.read() & 0x3F);
  byte dow = bcdToDec(Wire.read() & 0x07);

  Wire.read(); Wire.read(); Wire.read();

  if (second > 59 || minute > 59 || hour > 23) {
    rtcReadFailed++;
    webLogf("RTC invalid: %02d:%02d:%02d", hour, minute, second);
    return false;
  }

  currentHour = hour;
  currentMinute = minute;
  currentDayOfWeek = (dow > 0) ? dow - 1 : 6;

  lastRTCTimestamp = (unsigned long)hour * 3600 +
                     (unsigned long)minute * 60 +
                     (unsigned long)second;
  lastRTCSyncMillis = millis();

  rtcLastHour = hour;
  rtcLastMin = minute;
  rtcLastSec = second;
  rtcLastDow = currentDayOfWeek;

  rtcReadSuccess++;
  webLogf("RTC sync OK: %02d:%02d:%02d (OK:%d ERR:%d)", hour, minute, second, rtcReadSuccess, rtcReadFailed);
  return true;
}

void startRTCSet(int hour, int minute, int second, int dow) {
  if(!useRTC) return;

  rtcSetHour = hour;
  rtcSetMin = minute;
  rtcSetSec = second;
  rtcSetDow = dow;
  rtcSetPending = true;
  rtcSetRetryCount = 0;
  lastRTCSetAttempt = 0;
  webLogf("RTC set queued: %02d:%02d:%02d dow:%d", hour, minute, second, dow);
}

void processRTCSet() {
  if(!rtcSetPending || !useRTC) return;

  if(millis() - lastRTCSetAttempt < 500) return;
  lastRTCSetAttempt = millis();

  rtcSetRetryCount++;
  webLogf("RTC set attempt %d...", rtcSetRetryCount);

  Wire.beginTransmission(0x68);
  Wire.write(0x00);
  Wire.write(decToBcd(rtcSetSec));
  Wire.write(decToBcd(rtcSetMin));
  Wire.write(decToBcd(rtcSetHour));
  Wire.write(decToBcd(rtcSetDow + 1));
  byte error = Wire.endTransmission();

  if(error == 0) {
    delay(50);

    if(syncWithRTC()) {

      if(currentHour == rtcSetHour && currentMinute == rtcSetMin) {
        webLogf("RTC set VERIFIED after %d attempts", rtcSetRetryCount);
        rtcSetPending = false;
        return;
      }
    }
  }

  if(rtcSetRetryCount >= 20) {
    webLog("RTC set FAILED after max retries, giving up");
    rtcSetPending = false;
  }
}

void saveToRTC() {

  startRTCSet(currentHour, currentMinute, 0, currentDayOfWeek);
}

void loadRTCConfig() {
  useRTC = EEPROM.read(EEPROM_RTC_FLAG) == 1;
  webLogf("RTC Feature: %s", useRTC ? "ENABLED (DS3231)" : "DISABLED");

  if(useRTC) {

    Wire.begin(D2, D1);
    Wire.setClock(50000);
    delay(100);

    if(syncWithRTC()) {
      webLog("Boot: Time synced from RTC");
    } else {

      lastRTCTimestamp = (unsigned long)currentHour * 3600 +
                         (unsigned long)currentMinute * 60;
      lastRTCSyncMillis = millis();
      webLog("Boot: Using EEPROM time, RTC will sync later");
    }
  }
}

void loadConfig() {
  byte initFlag = EEPROM.read(EEPROM_INIT_FLAG);

  webLog("EEPROM Init Flag: " + String(initFlag));

  if(initFlag != EEPROM_INIT_FLAG_VAL) {
    webLog("Initial setup: Writing defaults to EEPROM...");

    for(int i = 0; i < 4; i++) {
      EEPROM.write(EEPROM_MODE_START + i, 0);
      relayMode[i] = 0;
    }

    int addr = EEPROM_NAMES_START;
    for(int i = 0; i < 4; i++) {
      for(int j = 0; j < 17; j++) {
        EEPROM.write(addr + j, relayNames[i][j]);
      }
      addr += 17;
    }

    for(int i=0; i<32; i++) {
        EEPROM.write(EEPROM_WIFI_START + i, ap_ssid[i]);
        EEPROM.write(EEPROM_PASS_START + i, ap_password[i]);
        EEPROM.write(EEPROM_HOST_START + i, hostname[i]);
    }
    for(int i=0; i<4; i++) EEPROM.write(EEPROM_PINS_START + i, relayPins[i]);

    EEPROM.write(EEPROM_RELAY_STATUS, 0);
    EEPROM.write(EEPROM_EMERGENCY, 0);
    EEPROM.write(EEPROM_RTC_FLAG, 0);

    for(int i=0; i<4*3*7; i++) EEPROM.write(EEPROM_SCHEDULES_START + i, 0);

    for(int i=0; i<16; i++) EEPROM.write(EEPROM_CYCLE_START + i, 0);

    EEPROM.write(EEPROM_RELAY_ENABLE, 0x0F);

    EEPROM.write(EEPROM_INIT_FLAG, EEPROM_INIT_FLAG_VAL);
    EEPROM.commit();
    return;
  }

  for(int i = 0; i < 4; i++) {
    relayMode[i] = EEPROM.read(EEPROM_MODE_START + i);
    if(relayMode[i] > 3) relayMode[i] = 0;
  }

  int addr = EEPROM_NAMES_START;
  for(int i = 0; i < 4; i++) {
    for(int j = 0; j < 17; j++) {
      relayNames[i][j] = EEPROM.read(addr++);
    }
    relayNames[i][16] = '\0';
  }

  addr = EEPROM_SCHEDULES_START;
  for(int i = 0; i < 4; i++) {
    for(int j = 0; j < MAX_SCHEDULES; j++) {
      schedules[i][j].onHour = EEPROM.read(addr++);
      schedules[i][j].onMin = EEPROM.read(addr++);
      schedules[i][j].offHour = EEPROM.read(addr++);
      schedules[i][j].offMin = EEPROM.read(addr++);
      schedules[i][j].dayMask = EEPROM.read(addr++);
      schedules[i][j].enabled = EEPROM.read(addr++) == 1;

      if(schedules[i][j].onHour > 23) schedules[i][j].onHour = 0;
      if(schedules[i][j].onMin > 59) schedules[i][j].onMin = 0;
      if(schedules[i][j].offHour > 23) schedules[i][j].offHour = 0;
      if(schedules[i][j].offMin > 59) schedules[i][j].offMin = 0;
    }
  }

  for(int i=0; i<32; i++) {
      ap_ssid[i] = EEPROM.read(EEPROM_WIFI_START + i);
      ap_password[i] = EEPROM.read(EEPROM_PASS_START + i);
      hostname[i] = EEPROM.read(EEPROM_HOST_START + i);
  }

  ap_ssid[32] = 0; ap_password[32] = 0; hostname[32] = 0;

  for(int i=0; i<4; i++) relayPins[i] = EEPROM.read(EEPROM_PINS_START + i);
  relayEnabledMask = EEPROM.read(EEPROM_RELAY_ENABLE);

  addr = EEPROM_CYCLE_START;
  for(int i=0; i<4; i++) {
    cycleOnDuration[i] = (EEPROM.read(addr) << 8) | EEPROM.read(addr+1); addr+=2;
    cycleOffDuration[i] = (EEPROM.read(addr) << 8) | EEPROM.read(addr+1); addr+=2;
  }

  relayEnabledMask = EEPROM.read(EEPROM_RELAY_ENABLE);

  currentHour = EEPROM.read(EEPROM_TIME_START);
  currentMinute = EEPROM.read(EEPROM_TIME_START + 1);
  currentDayOfWeek = EEPROM.read(EEPROM_TIME_START + 2);

  if(currentHour > 23) currentHour = 0;
  if(currentMinute > 59) currentMinute = 0;
  if(currentDayOfWeek > 6) currentDayOfWeek = 0;

  lastTimeSync = millis();

  byte statusByte = EEPROM.read(EEPROM_RELAY_STATUS);
  for(int i = 0; i < 4; i++) {
    relayStatus[i] = (statusByte & (1 << i)) != 0;
    digitalWrite(relayPins[i], relayStatus[i] ? LOW : HIGH);
  }

  addr = EEPROM_TIMER_START;
  for(int i = 0; i < 4; i++) {
    unsigned int remaining = (EEPROM.read(addr) << 8) | EEPROM.read(addr + 1);
    addr += 2;

    if(remaining > 0 && relayMode[i] == 2) {
      timerDuration[i] = remaining * 1000UL;
      timerStart[i] = millis();
      digitalWrite(relayPins[i], LOW);
      relayStatus[i] = true;
    }
  }

  webLog("=== Config loaded successfully ===");
}

const char MAIN_page[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Kontrol 4 Relay</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background: #09090b; min-height: 100vh; padding: 20px; color: #f4f4f5; line-height: 1.5; }
.container { max-width: 900px; margin: 0 auto; }
h1 { text-align: center; margin-bottom: 25px; font-size: 24px; font-weight: 600; color: #ffffff; letter-spacing: -0.5px; }
.time-sync { background: #18181b; padding: 15px 20px; border-radius: 12px; border: 1px solid #27272a; display: flex; justify-content: space-between; align-items: center; margin-bottom: 25px; }
.time-info { display: flex; gap: 20px; align-items: center; }
.time-display { font-size: 24px; font-weight: 600; font-variant-numeric: tabular-nums; }
.day-display { font-size: 14px; color: #a1a1aa; }
.relay-card, .card { background: #18181b; border-radius: 12px; padding: 20px; margin-bottom: 20px; border: 1px solid #27272a; transition: border-color 0.2s ease; }
.relay-card:hover, .card:hover { border-color: #3f3f46; }
.card h3 { margin-bottom: 15px; font-size: 16px; font-weight: 600; border-bottom: 1px solid #27272a; padding-bottom: 8px; }
.relay-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px; flex-wrap: wrap; gap: 10px; }
.relay-name-container { display: flex; align-items: center; gap: 8px; flex: 1; }
.relay-name-display { font-size: 16px; font-weight: 500; color: #e4e4e7; }
.relay-name-input { font-size: 14px; background: #09090b; border: 1px solid #3f3f46; color: #f4f4f5; padding: 6px 10px; border-radius: 6px; max-width: 180px; outline: none; }
.relay-name-input:focus { border-color: #3b82f6; }
.btn-edit, .btn-clear { background: transparent; border: 1px solid #27272a; color: #a1a1aa; padding: 5px 10px; border-radius: 6px; cursor: pointer; font-size: 12px; transition: all 0.2s; }
.btn-edit:hover, .btn-clear:hover { background: #27272a; color: #f4f4f5; }
.btn-save { background: #10b981; border: none; color: #fff; padding: 5px 10px; border-radius: 6px; cursor: pointer; font-size: 12px; font-weight: 500; }
.btn-cancel { background: #27272a; border: none; color: #e4e4e7; padding: 5px 10px; border-radius: 6px; cursor: pointer; font-size: 12px; font-weight: 500; }
.status { padding: 4px 12px; border-radius: 20px; font-size: 12px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; display: inline-flex; align-items: center; gap: 6px; }
.status::before { content: ''; width: 6px; height: 6px; border-radius: 50%; background: currentColor; }
.status.on { background: rgba(16, 185, 129, 0.1); color: #10b981; border: 1px solid rgba(16, 185, 129, 0.2); }
.status.off { background: rgba(113, 113, 122, 0.1); color: #71717a; border: 1px solid rgba(113, 113, 122, 0.2); }
.mode-selector, .pattern-select { margin-bottom: 15px; }
.mode-selector select, .pattern-select { width: 100%; padding: 10px; border: 1px solid #27272a; border-radius: 8px; font-size: 14px; background: #09090b; color: #e4e4e7; cursor: pointer; outline: none; transition: border-color 0.2s; }
.mode-selector select:focus, .pattern-select:focus { border-color: #3b82f6; }
.control-panel { padding: 15px; background: #09090b; border-radius: 8px; border: 1px solid #27272a; }
.btn { padding: 8px 16px; border: none; border-radius: 6px; cursor: pointer; font-size: 13px; font-weight: 500; margin: 4px; transition: all 0.2s; }
.btn:hover { opacity: 0.9; }
.btn-on { background: #10b981; color: #fff; }
.btn-off, .btn-danger, .btn-reset { background: #ef4444; color: #fff; }
.btn-set { background: #3b82f6; color: #fff; }
.btn-sync { background: #27272a; color: #e4e4e7; border: 1px solid #3f3f46; }
.btn-sync:hover { background: #3f3f46; }
.btn-add { background: transparent; color: #a1a1aa; border: 1px dashed #3f3f46; width: 100%; margin-top: 10px; }
.btn-add:hover { border-color: #71717a; color: #e4e4e7; }
.btn-del { background: #ef4444; color: #fff; padding: 4px 8px; font-size: 11px; }
.input-group { margin: 10px 0; }
.input-group label { display: block; margin-bottom: 6px; font-size: 12px; color: #a1a1aa; }
.input-group input, input[type="text"] { width: 100%; padding: 10px; margin-bottom:10px; border: 1px solid #27272a; border-radius: 6px; background: #09090b; color: #f4f4f5; font-size: 14px; outline: none; }
.input-group input:focus, input[type="text"]:focus { border-color: #3b82f6; }
.time-input { display: flex; gap: 10px; }
.time-input input { flex: 1; text-align: center; font-variant-numeric: tabular-nums; }
.schedule-item { background: #18181b; padding: 15px; border-radius: 8px; margin-bottom: 10px; border: 1px solid #27272a; }
.schedule-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; }
.day-selector { display: flex; flex-wrap: wrap; gap: 6px; margin: 10px 0; }
.day-btn { padding: 6px 12px; border-radius: 4px; border: 1px solid #27272a; background: #09090b; color: #a1a1aa; cursor: pointer; font-size: 12px; transition: all 0.2s; }
.day-btn.active { background: rgba(59, 130, 246, 0.1); color: #3b82f6; border-color: rgba(59, 130, 246, 0.3); }
.timer-container { display: flex; flex-direction: column; align-items: center; padding: 20px; }
.timer-display { font-size: 32px; font-weight: 600; text-align: center; padding: 20px; font-variant-numeric: tabular-nums; color: #10b981; }
.timer-inactive { color: #52525b; }
.schedule-status { font-size: 12px; color: #a1a1aa; margin-top: 5px; }
.schedule-times { display: flex; gap: 15px; margin-top: 10px; }
.time-picker-group { flex: 1; }
.time-picker-group label { display: block; font-size: 11px; color: #a1a1aa; margin-bottom: 6px; text-transform: uppercase; letter-spacing: 0.5px; }
.time-picker-group input[type="time"] { width: 100%; padding: 10px; border: 1px solid #27272a; border-radius: 6px; background: #09090b; color: #f4f4f5; font-size: 14px; font-weight: 500; cursor: pointer; }
.time-picker-group input[type="time"]::-webkit-calendar-picker-indicator { filter: invert(1); cursor: pointer; opacity: 0.5; }
.btn-emergency { background: #dc2626; color: #fff; font-weight: 600; }
.btn-emergency.active { background: #10b981; }
.emergency-banner { background: rgba(220, 38, 38, 0.1); border: 1px solid rgba(220, 38, 38, 0.2); color: #ef4444; padding: 12px 20px; border-radius: 8px; margin-bottom: 20px; text-align: center; display: none; font-size: 14px; }
.emergency-banner.show { display: block; }
.emergency-overlay { display: none; position: fixed; inset: 0; background: rgba(9, 9, 11, 0.8); backdrop-filter: blur(4px); z-index: 100; justify-content: center; align-items: center; }
.emergency-overlay.show { display: flex; }
.emergency-modal { background: #18181b; padding: 30px; border-radius: 12px; text-align: center; border: 1px solid #27272a; max-width: 400px; box-shadow: 0 20px 40px rgba(0,0,0,0.4); }
.emergency-modal h2 { color: #ef4444; margin-bottom: 12px; font-size: 18px; }
.emergency-modal p { margin-bottom: 20px; color: #a1a1aa; font-size: 14px; }
.connection-status { position: fixed; top: 15px; right: 20px; padding: 6px 12px; border-radius: 20px; font-size: 11px; font-weight: 500; background: #18181b; display: flex; align-items: center; gap: 8px; z-index: 9999; border: 1px solid #27272a; }
.connection-status.connected { color: #10b981; }
.connection-status.disconnected { color: #ef4444; }
.status-dot { width: 6px; height: 6px; border-radius: 50%; background: currentColor; }

/* === Toast Notifications === */
.toast-container { position: fixed; bottom: 20px; right: 20px; z-index: 10000; display: flex; flex-direction: column; gap: 10px; }
.toast { padding: 12px 16px; border-radius: 8px; background: #18181b; color: #f4f4f5; box-shadow: 0 10px 30px rgba(0,0,0,0.5); transform: translateX(120%); opacity: 0; transition: all 0.3s ease; display: flex; align-items: center; gap: 10px; font-size: 13px; max-width: 300px; border: 1px solid #27272a; }
.toast.show { transform: translateX(0); opacity: 1; }
.toast.success { border-left: 3px solid #10b981; }
.toast.error { border-left: 3px solid #ef4444; }
.toast.warning { border-left: 3px solid #f59e0b; }
.toast.info { border-left: 3px solid #3b82f6; }
.toast-icon { font-size: 16px; }
.toast-close { margin-left: auto; background: none; border: none; color: #71717a; cursor: pointer; font-size: 14px; }
.toast-close:hover { color: #e4e4e7; }

/* === Modal Dialogs === */
.modal-overlay { display: none; position: fixed; inset: 0; background: rgba(9, 9, 11, 0.8); backdrop-filter: blur(4px); z-index: 10001; justify-content: center; align-items: center; opacity: 0; transition: opacity 0.2s ease; }
.modal-overlay.show { display: flex; opacity: 1; }
.modal-content { background: #18181b; padding: 24px; border-radius: 12px; text-align: center; border: 1px solid #27272a; max-width: 360px; width: 90%; box-shadow: 0 20px 40px rgba(0,0,0,0.4); transform: scale(0.95); transition: transform 0.2s ease; }
.modal-overlay.show .modal-content { transform: scale(1); }
.modal-icon { font-size: 32px; margin-bottom: 12px; }
.modal-content h2 { font-size: 18px; margin-bottom: 8px; font-weight: 600; color: #f4f4f5; }
.modal-content p { font-size: 13px; color: #a1a1aa; margin-bottom: 24px; line-height: 1.5; }
.modal-actions { display: flex; gap: 10px; justify-content: center; }

/* === Loading Overlay === */
.loading-overlay { display: none; position: fixed; inset: 0; background: rgba(9, 9, 11, 0.8); backdrop-filter: blur(4px); z-index: 9998; justify-content: center; align-items: center; }
.loading-overlay.show { display: flex; }
.loading-spinner { width: 40px; height: 40px; border: 2px solid #27272a; border-radius: 50%; border-top-color: #3b82f6; animation: btn-spin 0.8s linear infinite; }
.loading-text { margin-top: 12px; font-size: 13px; color: #a1a1aa; }
@keyframes btn-spin { to { transform: rotate(360deg); } }

/* === Admin Specific === */
.info-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #27272a; }
.info-row:last-child { border-bottom: none; }
.info-label { color: #a1a1aa; font-size: 13px; }
.info-val { font-weight: 500; font-size: 13px; color: #f4f4f5; }
.pins-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 15px; }
.pins-grid select { padding: 6px; background: #09090b; border: 1px solid #3f3f46; color: #fff; border-radius: 4px; width: 100%; }
#terminal-container { background: #09090b; border-radius: 8px; border: 1px solid #27272a; overflow: hidden; margin-bottom:15px; }
.terminal-header { display: flex; justify-content: space-between; align-items: center; padding: 8px 12px; background: #27272a; font-size: 12px; font-weight: 600; }
#serial-terminal { height: 200px; overflow-y: auto; padding: 12px; font-family: monospace; font-size: 12px; color: #10b981; line-height: 1.4; }
.log-time { color: #71717a; margin-right: 8px; }
.back-link { display: inline-block; margin-top: 10px; color: #3b82f6; text-decoration: none; font-size: 14px; font-weight: 500; }
.back-link:hover { text-decoration: underline; }
.live-preview { display: flex; justify-content: center; gap: 15px; margin: 20px 0; padding: 15px; background: #09090b; border-radius: 8px; border: 1px solid #27272a; }
.preview-relay { width: 40px; height: 40px; border-radius: 50%; background: #27272a; color: #71717a; display: flex; align-items: center; justify-content: center; font-weight: 600; border: 2px solid #3f3f46; transition: all 0.2s; }
.preview-relay.on { background: rgba(16, 185, 129, 0.2); color: #10b981; border-color: #10b981; box-shadow: 0 0 15px rgba(16, 185, 129, 0.4); }

/* === Mobile Responsiveness === */
@media (max-width: 768px) {
  body { padding: 12px; }
  h1 { font-size: 20px; margin-bottom: 16px; }
  .time-sync { flex-direction: column; gap: 12px; padding: 16px; text-align: center; }
  .relay-card, .card { padding: 16px; }
  .relay-header { flex-direction: column; align-items: stretch; gap: 12px; }
  .pins-grid { grid-template-columns: 1fr; }
}
</style>
</head>
<body>
<!-- Global SVG Defs for gradients -->
<svg style="width:0;height:0;position:absolute">
  <defs>
    <linearGradient id="timerGradient" x1="0%" y1="0%" x2="100%" y2="0%">
      <stop offset="0%" stop-color="#11998e"/>
      <stop offset="100%" stop-color="#38ef7d"/>
    </linearGradient>
  </defs>
</svg>
<div id="connStatus" class="connection-status disconnected">
  <div class="status-dot"></div>
  <span>Connecting...</span>
</div>
<div class="container">
<h1>🔌 Kontrol 4 Relay ESP8266</h1>
<div class="emergency-banner" id="emergencyBanner">
  ⚠️ <strong>EMERGENCY STOP AKTIF</strong> - Semua kontrol dinonaktifkan
</div>
<div class="time-sync">
  <div class="time-info">
    <div>
      <div class="time-display" id="espTime">--:--</div>
      <div class="day-display" id="espDay">---</div>
    </div>
  </div>
  <div style="display:flex;gap:10px;">
    <button class="btn btn-sync" onclick="syncTime()">⏰ Sync</button>
    <button class="btn btn-emergency" id="emergencyBtn" onclick="toggleEmergency()">🚨 STOP</button>
  </div>
</div>

<div class="header-actions" style="display:flex;gap:10px;justify-content:center;margin-bottom:20px;">
  <button onclick="toggleAll(1, this)" class="btn btn-on" style="flex:1;padding:12px;font-size:16px;">⚡ ALL ON</button>
  <button onclick="toggleAll(0, this)" class="btn btn-off" style="flex:1;padding:12px;font-size:16px;">🌑 ALL OFF</button>
</div>

<div id="relays"></div>

<!-- Smart Summary Section -->
<div class="smart-summary" id="smartSummary">
  <h3>📋 Ringkasan Aktivitas</h3>
  <div id="summaryList">Loading...</div>
</div>

</div>
<div class="emergency-overlay" id="emergencyOverlay">
  <div class="emergency-modal">
    <h2>🚨 EMERGENCY STOP</h2>
    <p>Semua relay telah dimatikan.<br>Klik tombol di bawah untuk menonaktifkan mode darurat.</p>
    <button class="btn btn-emergency active" onclick="toggleEmergency()">✅ Nonaktifkan Emergency</button>
</div>

<!-- Toast Container -->
<div id="toastContainer" class="toast-container"></div>

<!-- Loading Overlay -->
<div id="loadingOverlay" class="loading-overlay">
  <div style="text-align:center">
    <div class="loading-spinner"></div>
    <div class="loading-text" id="loadingText">Loading...</div>
  </div>
</div>

<!-- Confirm Modal -->
<div id="confirmModal" class="modal-overlay">
  <div class="modal-content">
    <div class="modal-icon" id="confirmIcon">⚠️</div>
    <h2 id="confirmTitle">Konfirmasi</h2>
    <p id="confirmMessage">Apakah Anda yakin?</p>
    <div class="modal-actions">
      <button class="btn btn-sync" id="confirmCancel">Batal</button>
      <button class="btn btn-off" id="confirmOk">Ya, Lanjutkan</button>
    </div>
  </div>
</div>

<script>
const DAYS = ['Minggu', 'Senin', 'Selasa', 'Rabu', 'Kamis', 'Jumat', 'Sabtu'];
const DAYS_SHORT = ['Min', 'Sen', 'Sel', 'Rab', 'Kam', 'Jum', 'Sab'];
let timerRemaining = [0, 0, 0, 0];
let timerMaxDuration = [3600, 3600, 3600, 3600];
let lastData = null;
let needsFullRender = true;
let selectOpen = false;
let confirmResolver = null;

function showToast(message, type = 'success', duration = 3000) {
  const container = document.getElementById('toastContainer');
  const toast = document.createElement('div');
  toast.className = `toast ${type}`;

  const icons = { success: '✓', error: '✕', warning: '⚠', info: 'ℹ' };
  toast.innerHTML = `
    <span class="toast-icon">${icons[type] || '•'}</span>
    <span>${message}</span>
    <button class="toast-close" onclick="this.parentElement.remove()">×</button>
  `;

  container.appendChild(toast);
  requestAnimationFrame(() => toast.classList.add('show'));

  setTimeout(() => {
    toast.classList.remove('show');
    setTimeout(() => toast.remove(), 400);
  }, duration);
}

function showLoading(text = 'Loading...') {
  document.getElementById('loadingText').textContent = text;
  document.getElementById('loadingOverlay').classList.add('show');
}

function hideLoading() {
  document.getElementById('loadingOverlay').classList.remove('show');
}

function setBtnLoading(btn, loading) {
  if (typeof btn === 'string') btn = document.querySelector(btn);
  if (!btn) return;
  if (loading) {
    btn.classList.add('loading');
    btn.disabled = true;
  } else {
    btn.classList.remove('loading');
    btn.disabled = false;
  }
}

function showConfirm(title, message, icon = '⚠️') {
  return new Promise((resolve) => {
    document.getElementById('confirmTitle').textContent = title;
    document.getElementById('confirmMessage').innerHTML = message;
    document.getElementById('confirmIcon').textContent = icon;
    document.getElementById('confirmModal').classList.add('show');
    confirmResolver = resolve;
  });
}

document.addEventListener('DOMContentLoaded', function() {
  var okBtn = document.getElementById('confirmOk');
  var cancelBtn = document.getElementById('confirmCancel');
  if(okBtn) {
    okBtn.onclick = function() {
      document.getElementById('confirmModal').classList.remove('show');
      if (confirmResolver) confirmResolver(true);
    };
  }
  if(cancelBtn) {
    cancelBtn.onclick = function() {
      document.getElementById('confirmModal').classList.remove('show');
      if (confirmResolver) confirmResolver(false);
    };
  }
});

async function apiCall(url, options = {}) {
  const { showLoadingOverlay = false, loadingText = 'Processing...', successMsg, errorMsg, btn } = options;

  if (btn) setBtnLoading(btn, true);
  if (showLoadingOverlay) showLoading(loadingText);

  try {
    const res = await fetch(url);
    if (res.ok) {
      if (successMsg) showToast(successMsg, 'success');
      return { success: true, data: res };
    } else {
      showToast(errorMsg || 'Request gagal!', 'error');
      return { success: false };
    }
  } catch (e) {
    showToast('Koneksi error!', 'error');
    return { success: false, error: e };
  } finally {
    if (btn) setBtnLoading(btn, false);
    if (showLoadingOverlay) hideLoading();
  }
}

document.addEventListener('mousedown', (e) => {
  if(e.target.tagName === 'SELECT') {
    selectOpen = true;
  }
});
document.addEventListener('change', (e) => {
  if(e.target.tagName === 'SELECT') {
    selectOpen = false;
  }
});
document.addEventListener('click', (e) => {
  if(e.target.tagName !== 'SELECT' && e.target.tagName !== 'OPTION') {
    selectOpen = false;
  }
});

setInterval(() => {
  for(let i = 0; i < 4; i++) {
    if(timerRemaining[i] > 0) {
      timerRemaining[i] -= 100;
      if(timerRemaining[i] < 0) timerRemaining[i] = 0;
      updateTimerDisplay(i);
    }
  }
}, 100);

function updateTimerDisplay(i) {
  let el = document.getElementById('timer-countdown-' + i);
  if(el) {
    let totalSecs = Math.ceil(timerRemaining[i] / 1000);
    let hours = Math.floor(totalSecs / 3600);
    let mins = Math.floor((totalSecs % 3600) / 60);
    let secs = totalSecs % 60;
    el.textContent = String(hours).padStart(2,'0') + ':' + String(mins).padStart(2,'0') + ':' + String(secs).padStart(2,'0');
    el.classList.toggle('timer-inactive', totalSecs <= 0);

    let maxDuration = timerMaxDuration[i] || 3600;
    let progress = maxDuration > 0 ? (timerRemaining[i] / 1000) / maxDuration : 0;
    let circumference = 2 * Math.PI * 80;
    let dashOffset = circumference - (circumference * progress);

    let progressEl = document.getElementById('timer-progress-' + i);
    let glowEl = document.getElementById('timer-glow-' + i);
    if(progressEl) progressEl.style.strokeDashoffset = dashOffset;
    if(glowEl) glowEl.style.strokeDashoffset = dashOffset;

    let labelEl = el.nextElementSibling;
    if(labelEl && labelEl.classList.contains('timer-label')) {
      labelEl.textContent = totalSecs > 0 ? 'Remaining' : 'Stopped';
    }
  }
}

function isUserEditing() {
  let activeEl = document.activeElement;

  return activeEl && activeEl.tagName === 'INPUT';
}

function updateStatus() {
  fetch('/status').then(r => r.json()).then(data => {

    document.getElementById('espTime').textContent =
      String(data.hour).padStart(2,'0') + ':' + String(data.minute).padStart(2,'0');
    document.getElementById('espDay').textContent = DAYS[data.dayOfWeek] || '---';

    let banner = document.getElementById('emergencyBanner');
    let overlay = document.getElementById('emergencyOverlay');
    let btn = document.getElementById('emergencyBtn');
    if(data.emergency) {
      banner.classList.add('show');
      overlay.classList.add('show');
      btn.textContent = '✅ Nonaktifkan Emergency';
      btn.classList.add('active');
    } else {
      banner.classList.remove('show');
      overlay.classList.remove('show');
      btn.textContent = '🚨 EMERGENCY STOP';
      btn.classList.remove('active');
    }

    for(let i = 0; i < 4; i++) {
      if(data.timer[i] > 0) {
        timerRemaining[i] = data.timer[i] * 1000;

        if(data.timerMax && data.timerMax[i]) {
          timerMaxDuration[i] = data.timerMax[i];
        } else if(!timerMaxDuration[i] || timerMaxDuration[i] < data.timer[i]) {
          timerMaxDuration[i] = data.timer[i];
        }
      } else {
        timerRemaining[i] = 0;
      }
    }

    let structureChanged = false;
    if(lastData) {
      for(let i = 0; i < 4; i++) {
        if(data.mode[i] !== lastData.mode[i]) structureChanged = true;

        if(data.mask !== lastData.mask) structureChanged = true;

        let oldEnabled = lastData.schedules[i].filter(s => s.enabled).length;
        let newEnabled = data.schedules[i].filter(s => s.enabled).length;
        if(oldEnabled !== newEnabled) structureChanged = true;
      }
    }

    if(isUserEditing()) {
      smartUpdate(data);
    } else if(needsFullRender || structureChanged) {
      fullRender(data);
      needsFullRender = false;
    } else {

      smartUpdate(data);
    }

    lastData = JSON.parse(JSON.stringify(data));
    setConnStatus(true);
  }).catch(err => {
    console.log('Status fetch error:', err);
    setConnStatus(false);
  });
}

function setConnStatus(connected, tag = '') {
    const el = document.getElementById('connStatus');
    if(!el) return;
    if(connected) {
        el.className = 'connection-status connected';
        el.querySelector('span').textContent = 'Connected';
    } else {
        el.className = 'connection-status disconnected';
        el.querySelector('span').textContent = tag || 'Disconnected';
    }
}

function smartUpdate(data) {
  for(let i = 0; i < 4; i++) {

    let cardEl = document.getElementById(`relay-card-${i}`);
    if(cardEl) {
      cardEl.classList.toggle('on-state', data.status[i]);
    }

    let statusEl = document.querySelector(`#relay-card-${i} .status`);
    if(statusEl) {
      statusEl.className = 'status ' + (data.status[i] ? 'on' : 'off');
      statusEl.textContent = data.status[i] ? 'ON' : 'OFF';
    }

    if(!isUserEditing()) {
      for(let j = 0; j < 3; j++) {
        if(data.schedules[i][j] && data.schedules[i][j].enabled) {
          for(let d = 0; d < 7; d++) {
            let dayBtn = document.getElementById(`day-${i}-${j}-${d}`);
            if(dayBtn) {
              let active = (data.schedules[i][j].dayMask & (1 << d)) != 0;
              dayBtn.classList.toggle('active', active);
            }
          }
        }
      }
    }
  }
}

const SUMMARY_DAYS = ['Min', 'Sen', 'Sel', 'Rab', 'Kam', 'Jum', 'Sab'];

function padNum(n) { return n.toString().padStart(2, '0'); }

function generateScheduleText(name, schedules) {
  if(!schedules || schedules.length === 0) {
    return `<span class="summary-name">"${name}"</span> tidak ada jadwal`;
  }

  const active = schedules.find(s => s.enabled);
  if(!active) {
    return `<span class="summary-name">"${name}"</span> mode jadwal (tidak ada jadwal aktif)`;
  }

  let days = [];
  for(let d = 0; d < 7; d++) {
    if(active.days & (1 << d)) days.push(SUMMARY_DAYS[d]);
  }

  let daysStr = days.length === 7 ? 'setiap hari' : days.join(', ');
  let onTime = padNum(active.onH) + ':' + padNum(active.onM);
  let offTime = padNum(active.offH) + ':' + padNum(active.offM);

  return `<span class="summary-name">"${name}"</span> aktif jam <span class="summary-time">${onTime}</span> hingga <span class="summary-time">${offTime}</span> ${daysStr}`;
}

function generateTimerText(name, remaining, isOn) {
  if(!remaining || remaining <= 0) {
    return `<span class="summary-name">"${name}"</span> timer selesai / tidak aktif`;
  }

  let mins = Math.floor(remaining / 60);
  let hours = Math.floor(mins / 60);
  let m = mins % 60;

  let duration = hours > 0 ? `${hours} jam ${m} menit` : `${mins} menit`;

  let now = new Date();
  now.setSeconds(now.getSeconds() + remaining);
  let endTime = padNum(now.getHours()) + ':' + padNum(now.getMinutes());

  if(isOn) {
    return `<span class="summary-name">"${name}"</span> <span class="summary-active">AKTIF</span> selama ${duration}, akan mati pukul <span class="summary-time">${endTime}</span>`;
  } else {
    return `<span class="summary-name">"${name}"</span> timer ${duration} (belum dimulai)`;
  }
}

function generateCycleText(name, cycle, isOn) {
  if(!cycle || (!cycle.onDur && !cycle.offDur)) {
    return `<span class="summary-name">"${name}"</span> cycle tidak dikonfigurasi`;
  }
  let onMin = Math.floor(cycle.onDur / 60000);
  let offMin = Math.floor(cycle.offDur / 60000);
  let status = isOn ? '<span class="summary-active">ON</span>' : '<span class="summary-inactive">OFF</span>';
  return `<span class="summary-name">"${name}"</span> ${status} berulang: ON ${onMin} menit / OFF ${offMin} menit`;
}

function generateSummary(data) {
  try {
    if(!data) {
      console.error('generateSummary: no data');
      document.getElementById('summaryList').innerHTML = '<div style="opacity:0.5;text-align:center">Data tidak tersedia</div>';
      return;
    }

    let html = '';

    for(let i = 0; i < 4; i++) {

      if(data.mask !== undefined && !((data.mask >> i) & 1)) continue;

      let name = (data.names && data.names[i]) ? data.names[i] : 'Relay ' + (i+1);
      let mode = data.mode ? data.mode[i] : 0;
      let isOn = data.status ? data.status[i] : false;

      let icon, text;

      switch(mode) {
        case 0:
          icon = '✋';
          text = `<span class="summary-name">"${name}"</span> sedang <span class="${isOn ? 'summary-active' : 'summary-inactive'}">${isOn ? 'AKTIF' : 'MATI'}</span> (kontrol manual)`;
          break;

        case 1:
          icon = '📅';
          text = generateScheduleText(name, data.schedules ? data.schedules[i] : []);
          break;

        case 2:
          icon = '⏱️';
          text = generateTimerText(name, data.timer ? data.timer[i] : 0, isOn);
          break;

        case 3:
          icon = '🔄';
          text = generateCycleText(name, data.cycle ? data.cycle[i] : null, isOn);
          break;

        default:
          icon = '❓';
          text = `<span class="summary-name">"${name}"</span> mode tidak dikenal`;
      }

      html += `<div class="summary-item">
        <span class="summary-icon">${icon}</span>
        <span class="summary-text">${text}</span>
      </div>`;
    }

    if(html === '') {
      html = '<div style="opacity:0.5;text-align:center">Tidak ada relay aktif</div>';
    }

    document.getElementById('summaryList').innerHTML = html;
  } catch(e) {
    console.error('generateSummary error:', e);
    document.getElementById('summaryList').innerHTML = '<div style="opacity:0.5;color:#f55">Error: ' + e.message + '</div>';
  }
}

function fullRender(data) {
  let html = '';
  for(let i = 0; i < 4; i++) {

    if(data.mask !== undefined && !((data.mask >> i) & 1)) continue;

    html += `
      <div class="relay-card ${data.status[i] ? 'on-state' : ''}" id="relay-card-${i}">
        <div class="relay-header">
          <div class="relay-name-container" id="name-container-${i}">
            <span class="relay-name-display" id="name-display-${i}">${escapeHtml(data.names[i])}</span>
            <button class="btn-edit" onclick="startEditName(${i})">✏️</button>
          </div>
          <div class="status ${data.status[i] ? 'on' : 'off'}">
            ${data.status[i] ? 'ON' : 'OFF'}
          </div>
        </div>

        <div class="mode-selector">
          <select id="mode${i}" onchange="changeMode(${i})">
            <option value="0" ${data.mode[i]==0?'selected':''}>⚡ Manual</option>
            <option value="1" ${data.mode[i]==1?'selected':''}>📅 Penjadwalan</option>
            <option value="2" ${data.mode[i]==2?'selected':''}>⏱️ Timer</option>
            <option value="3" ${data.mode[i]==3?'selected':''}>🔄 Cycle Timer</option>
          </select>
        </div>

        <div class="control-panel" id="control${i}">
          ${getControlPanel(i, data)}
        </div>
      </div>
    `;
  }
  document.getElementById('relays').innerHTML = html;

  generateSummary(data);
}

function escapeHtml(str) {
  return str.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function getControlPanel(i, data) {
  if(data.mode[i] == 0) {
    return `
      <button class="btn btn-on" onclick="setRelay(${i}, 1, this)">💡 Nyalakan</button>
      <button class="btn btn-off" onclick="setRelay(${i}, 0, this)">🔌 Matikan</button>
    `;
  } else if(data.mode[i] == 1) {
    let schedHtml = '';
    for(let j = 0; j < data.schedules[i].length; j++) {
      let s = data.schedules[i][j];
      if(s.enabled) {
        schedHtml += getScheduleItemHtml(i, j, s);
      }
    }

    let enabledCount = data.schedules[i].filter(s => s.enabled).length;
    let canAdd = enabledCount < 3;

    return `
      ${schedHtml}
      ${canAdd ? `<button class="btn btn-add" onclick="addSchedule(${i})">+ Tambah Jadwal</button>` : ''}
      ${enabledCount == 0 ? '<p style="opacity:0.6;text-align:center;padding:10px;">Belum ada jadwal. Klik tombol di atas untuk menambah.</p>' : ''}
      ${enabledCount == 0 ? '<p style="opacity:0.6;text-align:center;padding:10px;">Belum ada jadwal. Klik tombol di atas untuk menambah.</p>' : ''}
    `;
  } else if(data.mode[i] == 3) {
    let on = data.cycleOn ? data.cycleOn[i] : 0;
    let off = data.cycleOff ? data.cycleOff[i] : 0;
    return `
       <div class="input-group">
         <label>Konfigurasi Loop (Menit)</label>
         <div class="time-input">
           <input type="number" id="cycleOn${i}" placeholder="ON" value="${on}">
           <input type="number" id="cycleOff${i}" placeholder="OFF" value="${off}">
         </div>
       </div>
       <button class="btn btn-set" onclick="setCycle(${i}, this)" style="width:100%">💾 Set Cycle</button>
       <div style="font-size:12px;opacity:0.6;margin-top:5px;text-align:center">
          Relay akan ON selama ${on}m & OFF selama ${off}m berulang.
       </div>
    `;
  } else {
    let totalSecs = Math.ceil(timerRemaining[i] / 1000);
    let hours = Math.floor(totalSecs / 3600);
    let mins = Math.floor((totalSecs % 3600) / 60);
    let secs = totalSecs % 60;
    let maxDuration = data.timerMax ? data.timerMax[i] : 3600;
    let progress = maxDuration > 0 ? (timerRemaining[i] / 1000) / maxDuration : 0;
    let dashOffset = 502 - (502 * progress);
    return `
      <div class="timer-container">
        <div class="timer-ring-wrapper">
          <svg class="timer-ring" viewBox="0 0 200 200">
            <circle class="timer-ring-bg" cx="100" cy="100" r="80"></circle>
            <circle class="timer-ring-glow" id="timer-glow-${i}" cx="100" cy="100" r="80"
                    style="stroke-dashoffset: ${dashOffset}"></circle>
            <circle class="timer-ring-progress" id="timer-progress-${i}" cx="100" cy="100" r="80"
                    style="stroke-dashoffset: ${dashOffset}"></circle>
          </svg>
          <div class="timer-center">
            <div class="timer-display ${totalSecs <= 0 ? 'timer-inactive' : ''}" id="timer-countdown-${i}">
              ${String(hours).padStart(2,'0')}:${String(mins).padStart(2,'0')}:${String(secs).padStart(2,'0')}
            </div>
            <div class="timer-label">${totalSecs > 0 ? 'Remaining' : 'Stopped'}</div>
          </div>
        </div>
        <div class="input-group" style="width:100%;max-width:280px">
          <label>Durasi Timer:</label>
          <div class="time-input">
            <input type="number" id="timerHour${i}" placeholder="Jam" min="0" max="23" value="0">
            <input type="number" id="timerMin${i}" placeholder="Menit" min="0" max="59" value="30">
          </div>
        </div>
        <div class="timer-controls">
          <button class="btn btn-on" onclick="startTimer(${i}, this)">▶️ Start</button>
          <button class="btn btn-off" onclick="stopTimer(${i}, this)">⏹️ Stop</button>
        </div>
      </div>
    `;
  }
}

function getScheduleItemHtml(relay, slot, s) {
  let dayBtns = '';
  for(let d = 0; d < 7; d++) {
    let active = (s.dayMask & (1 << d)) != 0;
    dayBtns += `<button class="day-btn ${active?'active':''}" id="day-${relay}-${slot}-${d}" onclick="toggleDay(${relay},${slot},${d})">${DAYS_SHORT[d]}</button>`;
  }
  let onTime = String(s.onH).padStart(2,'0') + ':' + String(s.onM).padStart(2,'0');
  let offTime = String(s.offH).padStart(2,'0') + ':' + String(s.offM).padStart(2,'0');
  return `
    <div class="schedule-item" id="sched-${relay}-${slot}">
      <div class="schedule-header">
        <strong>Jadwal ${slot+1}</strong>
        <button class="btn btn-del" onclick="deleteSchedule(${relay},${slot})">🗑️ Hapus</button>
      </div>
      <div class="day-selector">${dayBtns}</div>
      <div style="display:flex;gap:10px;margin-top:5px">
        <div style="flex:1">
          <label style="font-size:11px;display:block;opacity:0.7;margin-bottom:2px">🌅 Nyala</label>
          <input type="time" id="onTime${relay}_${slot}" value="${onTime}" onchange="updateScheduleTime(${relay},${slot})"
                 style="width:100%;padding:6px;border:1px solid #444;border-radius:4px;background:#222;color:#fff;font-family:inherit">
        </div>
        <div style="flex:1">
          <label style="font-size:11px;display:block;opacity:0.7;margin-bottom:2px">🌙 Mati</label>
          <input type="time" id="offTime${relay}_${slot}" value="${offTime}" onchange="updateScheduleTime(${relay},${slot})"
                 style="width:100%;padding:6px;border:1px solid #444;border-radius:4px;background:#222;color:#fff;font-family:inherit">
        </div>
      </div>
    </div>
  `;

}

function setRelay(relay, state, btn) {
  if (btn) setBtnLoading(btn, true);
  fetch(`/relay?r=${relay}&s=${state}`)
    .then(() => {
      showToast(state ? 'Relay dinyalakan!' : 'Relay dimatikan!', state ? 'success' : 'info');
      updateStatus();
    })
    .catch(() => showToast('Gagal mengubah relay!', 'error'))
    .finally(() => { if (btn) setBtnLoading(btn, false); });
}

function changeMode(relay) {
  let modeEl = document.getElementById('mode'+relay);
  let mode = modeEl.value;
  modeEl.blur();
  let modeNames = ['Manual', 'Penjadwalan', 'Timer', 'Cycle Timer'];
  needsFullRender = true;
  fetch(`/mode?r=${relay}&m=${mode}`).then(() => {
    showToast(`Mode diubah ke ${modeNames[mode]}`, 'info');
    setTimeout(() => updateStatus(), 300);
  }).catch(() => showToast('Gagal mengubah mode!', 'error'));
}

function startEditName(relay) {
  let container = document.getElementById('name-container-' + relay);
  let currentName = document.getElementById('name-display-' + relay).textContent;
  container.innerHTML = `
    <input type="text" class="relay-name-input" id="name-input-${relay}" value="${currentName}" maxlength="16"
           onkeypress="if(event.key==='Enter')saveEditName(${relay})">
    <button class="btn-save" onclick="saveEditName(${relay})">💾</button>
    <button class="btn-cancel" onclick="cancelEditName(${relay})">✕</button>
  `;
  document.getElementById('name-input-' + relay).focus();
  document.getElementById('name-input-' + relay).select();
}

function saveEditName(relay) {
  let name = document.getElementById('name-input-' + relay).value;
  fetch(`/setname?r=${relay}&name=${encodeURIComponent(name)}`).then(() => {
    showToast('Nama relay tersimpan!', 'success');
    needsFullRender = true;
    updateStatus();
  }).catch(() => showToast('Gagal menyimpan nama!', 'error'));
}

function cancelEditName(relay) {
  needsFullRender = true;
  updateStatus();
}

function addSchedule(relay) {
  needsFullRender = true;
  fetch(`/addschedule?r=${relay}`).then(() => {
    showToast('Jadwal baru ditambahkan!', 'success');
    updateStatus();
  }).catch(() => showToast('Gagal menambah jadwal!', 'error'));
}

async function deleteSchedule(relay, slot) {
  const confirmed = await showConfirm(
    'Hapus Jadwal?',
    'Jadwal ini akan dihapus permanen.',
    '🗑️'
  );
  if (!confirmed) return;

  needsFullRender = true;
  fetch(`/delschedule?r=${relay}&slot=${slot}`).then(() => {
    showToast('Jadwal dihapus!', 'info');
    updateStatus();
  }).catch(() => showToast('Gagal menghapus jadwal!', 'error'));
}

function updateSchedule(relay, slot) {
  let onH = document.getElementById(`onH${relay}_${slot}`).value || 0;
  let onM = document.getElementById(`onM${relay}_${slot}`).value || 0;
  let offH = document.getElementById(`offH${relay}_${slot}`).value || 0;
  let offM = document.getElementById(`offM${relay}_${slot}`).value || 0;
  fetch(`/setschedule?r=${relay}&slot=${slot}&onH=${onH}&onM=${onM}&offH=${offH}&offM=${offM}`);
}

function updateScheduleTime(relay, slot) {
  let onTime = document.getElementById(`onTime${relay}_${slot}`).value || '00:00';
  let offTime = document.getElementById(`offTime${relay}_${slot}`).value || '00:00';
  let [onH, onM] = onTime.split(':').map(Number);
  let [offH, offM] = offTime.split(':').map(Number);
  fetch(`/setschedule?r=${relay}&slot=${slot}&onH=${onH}&onM=${onM}&offH=${offH}&offM=${offM}`)
    .then(() => showToast('Waktu jadwal disimpan!', 'success'))
    .catch(() => showToast('Gagal menyimpan jadwal!', 'error'));
}

function toggleDay(relay, slot, day) {
  fetch(`/toggleday?r=${relay}&slot=${slot}&day=${day}`)
    .then(() => updateStatus())
    .catch(() => showToast('Gagal mengubah hari!', 'error'));
}

function startTimer(relay, btn) {
  let hours = parseInt(document.getElementById('timerHour'+relay).value) || 0;
  let mins = parseInt(document.getElementById('timerMin'+relay).value) || 0;
  let duration = hours * 3600 + mins * 60;
  if(duration > 0) {
    timerMaxDuration[relay] = duration;
    if (btn) setBtnLoading(btn, true);
    fetch(`/timer?r=${relay}&d=${duration}`)
      .then(() => {
        showToast(`▶️ Timer dimulai: ${hours}j ${mins}m`, 'success');
        updateStatus();
      })
      .catch(() => showToast('Gagal memulai timer!', 'error'))
      .finally(() => { if (btn) setBtnLoading(btn, false); });
  } else {
    showToast('Durasi timer harus > 0!', 'warning');
  }
}

function stopTimer(relay, btn) {
  if (btn) setBtnLoading(btn, true);
  fetch(`/timer?r=${relay}&d=0`)
    .then(() => {
      showToast('⏹️ Timer dihentikan', 'info');
      updateStatus();
    })
    .catch(() => showToast('Gagal menghentikan timer!', 'error'))
    .finally(() => { if (btn) setBtnLoading(btn, false); });
}

function syncTime() {
  console.log('syncTime called');
  var now = new Date();
  var h = now.getHours();
  var m = now.getMinutes();
  var d = now.getDay();
  var url = '/settime?h=' + h + '&m=' + m + '&d=' + d;
  console.log('Fetching: ' + url);
  fetch(url)
    .then(function(r) { return r.text(); })
    .then(function() {
      console.log('syncTime success');
      showToast('Waktu tersinkronisasi!', 'success');
      updateStatus();
    })
    .catch(function(e) {
      console.error('syncTime error', e);
      showToast('Gagal sinkronisasi waktu!', 'error');
    });
}

function toggleAll(s, btn) {
  if (btn) setBtnLoading(btn, true);
  fetch('/all?s='+s).then(() => {
    showToast(s ? '⚡ Semua relay ON!' : '🌑 Semua relay OFF!', s ? 'success' : 'info');
    updateStatus();
  })
  .catch(() => showToast('Gagal!', 'error'))
  .finally(() => { if (btn) setBtnLoading(btn, false); });
}

function setCycle(r, btn) {
  let on = document.getElementById('cycleOn'+r).value;
  let off = document.getElementById('cycleOff'+r).value;
  if (btn) setBtnLoading(btn, true);
  fetch(`/setcycle?r=${r}&on=${on}&off=${off}`).then(() => {
    showToast('💾 Cycle Mode tersimpan!', 'success');
    updateStatus();
  })
  .catch(() => showToast('Gagal menyimpan cycle!', 'error'))
  .finally(() => { if (btn) setBtnLoading(btn, false); });
}

function toggleEmergency() {
  var btn = document.getElementById('emergencyBtn');

  if(!btn.classList.contains('active')) {
    if(confirm('PERINGATAN!\n\nMengaktifkan Emergency Stop akan:\n- Mematikan SEMUA relay\n- Memblokir semua kontrol\n- ESP akan restart\n\nLanjutkan?')) {
      fetch('/emergency?action=on').then(function() {
        showToast('Emergency Stop AKTIF! Restarting...', 'warning');
        setTimeout(function() { location.reload(); }, 2000);
      }).catch(function() {
        showToast('Gagal mengaktifkan emergency!', 'error');
      });
    }
  } else {
    fetch('/emergency?action=off').then(function() {
      showToast('Emergency Stop dinonaktifkan', 'success');
      updateStatus();
    }).catch(function() { showToast('Gagal menonaktifkan emergency!', 'error'); });
  }
}

let lastWSMsg = Date.now();
function connectWS() {
  const ws = new WebSocket('ws://' + location.hostname + ':81/');

  ws.onopen = function() {
    console.log('WS Connected');
    lastWSMsg = Date.now();
    setConnStatus(true);
  };

  ws.onmessage = function(e) {
    lastWSMsg = Date.now();
    setConnStatus(true);
    try {
      let d = JSON.parse(e.data);
      if(isUserEditing()) {
        smartUpdate(d);
      } else {
        fullRender(d);
      }
      lastData = JSON.parse(JSON.stringify(d));
    } catch(e) { console.error(e); }
  };

  ws.onclose = function() {
    console.log('WS Disconnected');
    setConnStatus(false);
    setTimeout(connectWS, 3000);
  };

  ws.onerror = function(e) {
    console.error('WS Error', e);
    setConnStatus(false);
  };
}

setInterval(() => {
    if (Date.now() - lastWSMsg > 12000) {
        setConnStatus(false, 'Status: Offline');
    }
}, 2000);

document.addEventListener('DOMContentLoaded', function() {
  connectWS();
  setInterval(updateStatus, 10000);
  updateStatus();
  syncTime();
});
</script>
</body>
</html>
)=====";

const char ADMIN_page[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Admin Panel - ESP8266</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background: #09090b; min-height: 100vh; padding: 20px; color: #f4f4f5; line-height: 1.5; }
.container { max-width: 900px; margin: 0 auto; }
h1 { text-align: center; margin-bottom: 25px; font-size: 24px; font-weight: 600; color: #ffffff; letter-spacing: -0.5px; }
.time-sync { background: #18181b; padding: 15px 20px; border-radius: 12px; border: 1px solid #27272a; display: flex; justify-content: space-between; align-items: center; margin-bottom: 25px; }
.time-info { display: flex; gap: 20px; align-items: center; }
.time-display { font-size: 24px; font-weight: 600; font-variant-numeric: tabular-nums; }
.day-display { font-size: 14px; color: #a1a1aa; }
.relay-card, .card { background: #18181b; border-radius: 12px; padding: 20px; margin-bottom: 20px; border: 1px solid #27272a; transition: border-color 0.2s ease; }
.relay-card:hover, .card:hover { border-color: #3f3f46; }
.card h3 { margin-bottom: 15px; font-size: 16px; font-weight: 600; border-bottom: 1px solid #27272a; padding-bottom: 8px; }
.relay-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px; flex-wrap: wrap; gap: 10px; }
.relay-name-container { display: flex; align-items: center; gap: 8px; flex: 1; }
.relay-name-display { font-size: 16px; font-weight: 500; color: #e4e4e7; }
.relay-name-input { font-size: 14px; background: #09090b; border: 1px solid #3f3f46; color: #f4f4f5; padding: 6px 10px; border-radius: 6px; max-width: 180px; outline: none; }
.relay-name-input:focus { border-color: #3b82f6; }
.btn-edit, .btn-clear { background: transparent; border: 1px solid #27272a; color: #a1a1aa; padding: 5px 10px; border-radius: 6px; cursor: pointer; font-size: 12px; transition: all 0.2s; }
.btn-edit:hover, .btn-clear:hover { background: #27272a; color: #f4f4f5; }
.btn-save { background: #10b981; border: none; color: #fff; padding: 5px 10px; border-radius: 6px; cursor: pointer; font-size: 12px; font-weight: 500; }
.btn-cancel { background: #27272a; border: none; color: #e4e4e7; padding: 5px 10px; border-radius: 6px; cursor: pointer; font-size: 12px; font-weight: 500; }
.status { padding: 4px 12px; border-radius: 20px; font-size: 12px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; display: inline-flex; align-items: center; gap: 6px; }
.status::before { content: ''; width: 6px; height: 6px; border-radius: 50%; background: currentColor; }
.status.on { background: rgba(16, 185, 129, 0.1); color: #10b981; border: 1px solid rgba(16, 185, 129, 0.2); }
.status.off { background: rgba(113, 113, 122, 0.1); color: #71717a; border: 1px solid rgba(113, 113, 122, 0.2); }
.mode-selector, .pattern-select { margin-bottom: 15px; }
.mode-selector select, .pattern-select { width: 100%; padding: 10px; border: 1px solid #27272a; border-radius: 8px; font-size: 14px; background: #09090b; color: #e4e4e7; cursor: pointer; outline: none; transition: border-color 0.2s; }
.mode-selector select:focus, .pattern-select:focus { border-color: #3b82f6; }
.control-panel { padding: 15px; background: #09090b; border-radius: 8px; border: 1px solid #27272a; }
.btn { padding: 8px 16px; border: none; border-radius: 6px; cursor: pointer; font-size: 13px; font-weight: 500; margin: 4px; transition: all 0.2s; }
.btn:hover { opacity: 0.9; }
.btn-on { background: #10b981; color: #fff; }
.btn-off, .btn-danger, .btn-reset { background: #ef4444; color: #fff; }
.btn-set { background: #3b82f6; color: #fff; }
.btn-sync { background: #27272a; color: #e4e4e7; border: 1px solid #3f3f46; }
.btn-sync:hover { background: #3f3f46; }
.btn-add { background: transparent; color: #a1a1aa; border: 1px dashed #3f3f46; width: 100%; margin-top: 10px; }
.btn-add:hover { border-color: #71717a; color: #e4e4e7; }
.btn-del { background: #ef4444; color: #fff; padding: 4px 8px; font-size: 11px; }
.input-group { margin: 10px 0; }
.input-group label { display: block; margin-bottom: 6px; font-size: 12px; color: #a1a1aa; }
.input-group input, input[type="text"] { width: 100%; padding: 10px; margin-bottom:10px; border: 1px solid #27272a; border-radius: 6px; background: #09090b; color: #f4f4f5; font-size: 14px; outline: none; }
.input-group input:focus, input[type="text"]:focus { border-color: #3b82f6; }
.time-input { display: flex; gap: 10px; }
.time-input input { flex: 1; text-align: center; font-variant-numeric: tabular-nums; }
.schedule-item { background: #18181b; padding: 15px; border-radius: 8px; margin-bottom: 10px; border: 1px solid #27272a; }
.schedule-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; }
.day-selector { display: flex; flex-wrap: wrap; gap: 6px; margin: 10px 0; }
.day-btn { padding: 6px 12px; border-radius: 4px; border: 1px solid #27272a; background: #09090b; color: #a1a1aa; cursor: pointer; font-size: 12px; transition: all 0.2s; }
.day-btn.active { background: rgba(59, 130, 246, 0.1); color: #3b82f6; border-color: rgba(59, 130, 246, 0.3); }
.timer-container { display: flex; flex-direction: column; align-items: center; padding: 20px; }
.timer-display { font-size: 32px; font-weight: 600; text-align: center; padding: 20px; font-variant-numeric: tabular-nums; color: #10b981; }
.timer-inactive { color: #52525b; }
.schedule-status { font-size: 12px; color: #a1a1aa; margin-top: 5px; }
.schedule-times { display: flex; gap: 15px; margin-top: 10px; }
.time-picker-group { flex: 1; }
.time-picker-group label { display: block; font-size: 11px; color: #a1a1aa; margin-bottom: 6px; text-transform: uppercase; letter-spacing: 0.5px; }
.time-picker-group input[type="time"] { width: 100%; padding: 10px; border: 1px solid #27272a; border-radius: 6px; background: #09090b; color: #f4f4f5; font-size: 14px; font-weight: 500; cursor: pointer; }
.time-picker-group input[type="time"]::-webkit-calendar-picker-indicator { filter: invert(1); cursor: pointer; opacity: 0.5; }
.btn-emergency { background: #dc2626; color: #fff; font-weight: 600; }
.btn-emergency.active { background: #10b981; }
.emergency-banner { background: rgba(220, 38, 38, 0.1); border: 1px solid rgba(220, 38, 38, 0.2); color: #ef4444; padding: 12px 20px; border-radius: 8px; margin-bottom: 20px; text-align: center; display: none; font-size: 14px; }
.emergency-banner.show { display: block; }
.emergency-overlay { display: none; position: fixed; inset: 0; background: rgba(9, 9, 11, 0.8); backdrop-filter: blur(4px); z-index: 100; justify-content: center; align-items: center; }
.emergency-overlay.show { display: flex; }
.emergency-modal { background: #18181b; padding: 30px; border-radius: 12px; text-align: center; border: 1px solid #27272a; max-width: 400px; box-shadow: 0 20px 40px rgba(0,0,0,0.4); }
.emergency-modal h2 { color: #ef4444; margin-bottom: 12px; font-size: 18px; }
.emergency-modal p { margin-bottom: 20px; color: #a1a1aa; font-size: 14px; }
.connection-status { position: fixed; top: 15px; right: 20px; padding: 6px 12px; border-radius: 20px; font-size: 11px; font-weight: 500; background: #18181b; display: flex; align-items: center; gap: 8px; z-index: 9999; border: 1px solid #27272a; }
.connection-status.connected { color: #10b981; }
.connection-status.disconnected { color: #ef4444; }
.status-dot { width: 6px; height: 6px; border-radius: 50%; background: currentColor; }

/* === Toast Notifications === */
.toast-container { position: fixed; bottom: 20px; right: 20px; z-index: 10000; display: flex; flex-direction: column; gap: 10px; }
.toast { padding: 12px 16px; border-radius: 8px; background: #18181b; color: #f4f4f5; box-shadow: 0 10px 30px rgba(0,0,0,0.5); transform: translateX(120%); opacity: 0; transition: all 0.3s ease; display: flex; align-items: center; gap: 10px; font-size: 13px; max-width: 300px; border: 1px solid #27272a; }
.toast.show { transform: translateX(0); opacity: 1; }
.toast.success { border-left: 3px solid #10b981; }
.toast.error { border-left: 3px solid #ef4444; }
.toast.warning { border-left: 3px solid #f59e0b; }
.toast.info { border-left: 3px solid #3b82f6; }
.toast-icon { font-size: 16px; }
.toast-close { margin-left: auto; background: none; border: none; color: #71717a; cursor: pointer; font-size: 14px; }
.toast-close:hover { color: #e4e4e7; }

/* === Modal Dialogs === */
.modal-overlay { display: none; position: fixed; inset: 0; background: rgba(9, 9, 11, 0.8); backdrop-filter: blur(4px); z-index: 10001; justify-content: center; align-items: center; opacity: 0; transition: opacity 0.2s ease; }
.modal-overlay.show { display: flex; opacity: 1; }
.modal-content { background: #18181b; padding: 24px; border-radius: 12px; text-align: center; border: 1px solid #27272a; max-width: 360px; width: 90%; box-shadow: 0 20px 40px rgba(0,0,0,0.4); transform: scale(0.95); transition: transform 0.2s ease; }
.modal-overlay.show .modal-content { transform: scale(1); }
.modal-icon { font-size: 32px; margin-bottom: 12px; }
.modal-content h2 { font-size: 18px; margin-bottom: 8px; font-weight: 600; color: #f4f4f5; }
.modal-content p { font-size: 13px; color: #a1a1aa; margin-bottom: 24px; line-height: 1.5; }
.modal-actions { display: flex; gap: 10px; justify-content: center; }

/* === Loading Overlay === */
.loading-overlay { display: none; position: fixed; inset: 0; background: rgba(9, 9, 11, 0.8); backdrop-filter: blur(4px); z-index: 9998; justify-content: center; align-items: center; }
.loading-overlay.show { display: flex; }
.loading-spinner { width: 40px; height: 40px; border: 2px solid #27272a; border-radius: 50%; border-top-color: #3b82f6; animation: btn-spin 0.8s linear infinite; }
.loading-text { margin-top: 12px; font-size: 13px; color: #a1a1aa; }
@keyframes btn-spin { to { transform: rotate(360deg); } }

/* === Admin Specific === */
.info-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #27272a; }
.info-row:last-child { border-bottom: none; }
.info-label { color: #a1a1aa; font-size: 13px; }
.info-val { font-weight: 500; font-size: 13px; color: #f4f4f5; }
.pins-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 15px; }
.pins-grid select { padding: 6px; background: #09090b; border: 1px solid #3f3f46; color: #fff; border-radius: 4px; width: 100%; }
#terminal-container { background: #09090b; border-radius: 8px; border: 1px solid #27272a; overflow: hidden; margin-bottom:15px; }
.terminal-header { display: flex; justify-content: space-between; align-items: center; padding: 8px 12px; background: #27272a; font-size: 12px; font-weight: 600; }
#serial-terminal { height: 200px; overflow-y: auto; padding: 12px; font-family: monospace; font-size: 12px; color: #10b981; line-height: 1.4; }
.log-time { color: #71717a; margin-right: 8px; }
.back-link { display: inline-block; margin-top: 10px; color: #3b82f6; text-decoration: none; font-size: 14px; font-weight: 500; }
.back-link:hover { text-decoration: underline; }
.live-preview { display: flex; justify-content: center; gap: 15px; margin: 20px 0; padding: 15px; background: #09090b; border-radius: 8px; border: 1px solid #27272a; }
.preview-relay { width: 40px; height: 40px; border-radius: 50%; background: #27272a; color: #71717a; display: flex; align-items: center; justify-content: center; font-weight: 600; border: 2px solid #3f3f46; transition: all 0.2s; }
.preview-relay.on { background: rgba(16, 185, 129, 0.2); color: #10b981; border-color: #10b981; box-shadow: 0 0 15px rgba(16, 185, 129, 0.4); }

/* === Mobile Responsiveness === */
@media (max-width: 768px) {
  body { padding: 12px; }
  h1 { font-size: 20px; margin-bottom: 16px; }
  .time-sync { flex-direction: column; gap: 12px; padding: 16px; text-align: center; }
  .relay-card, .card { padding: 16px; }
  .relay-header { flex-direction: column; align-items: stretch; gap: 12px; }
  .pins-grid { grid-template-columns: 1fr; }
}
</style>
</head>
<body>
<div class="container">
<h1>🔧 Admin Panel</h1>

<div class="card">
  <h3>📊 System Info</h3>
  <div class="info-row"><span class="info-label">Free Heap:</span><span class="info-val" id="heap">...</span></div>
  <div class="info-row"><span class="info-label">Uptime:</span><span class="info-val" id="uptime">...</span></div>
  <div class="info-row"><span class="info-label">WiFi AP IP:</span><span class="info-val" id="ip">...</span></div>
  <div class="info-row"><span class="info-label">RTC Time:</span><span class="info-val" id="rtcTime" style="color:#0f0">...</span></div>
  <div class="info-row"><span class="info-label">Memory Time:</span><span class="info-val" id="memTime">...</span></div>
  <div class="info-row"><span class="info-label">RTC Stats:</span><span class="info-val" id="rtcStats">...</span></div>
</div>

<div class="card">
  <h3>📡 WiFi Configuration</h3>
  <input type="text" id="ssid" placeholder="AP SSID">
  <input type="text" id="pass" placeholder="AP Password (min 8 chars)">
  <button class="btn btn-save" onclick="saveWifi()">Save WiFi</button>
</div>

<div class="card">
  <h3>⏰ RTC Configuration (DS3231)</h3>
  <label style="display:flex;align-items:center;cursor:pointer;margin-bottom:10px">
    <input type="checkbox" id="useRTC" style="width:20px;height:20px;margin:0 10px 0 0">
    <span style="font-weight:bold">Enable External RTC</span>
  </label>
  <p style="font-size:13px;color:#f1c40f;margin-bottom:10px;background:rgba(241,196,15,0.1);padding:8px;border-radius:5px">
    ⚠️ <strong>WARNING:</strong> RTC params D1 &amp; D2 used for I2C.<br>
    Ensure no Relays use D1/D2 before enabling!
  </p>
  <button class="btn btn-save" onclick="saveRTC()">Save RTC Settings</button>
</div>

<div class="card">
  <h3>📊 RTC Activity Log</h3>
  <div id="rtc-log-container">
    <div class="terminal-header">
      <span>RTC Sync Status</span>
      <button class="btn-clear" onclick="clearRTCLog()">Clear</button>
    </div>
    <div id="rtc-terminal" style="height:150px;overflow-y:auto;background:#0a0a0a;padding:10px;font-family:monospace;font-size:12px;border-radius:8px;color:#0f0">-- RTC Log Initialized --</div>
  </div>
  <div style="margin-top:10px;font-size:11px;opacity:0.7">
    <span id="rtc-stats">OK: 0 | ERR: 0 | Pending: No</span>
  </div>
</div>

<div class="card">
  <h3>🏷️ Hostname</h3>
  <input type="text" id="host" placeholder="Device Hostname">
  <button class="btn btn-save" onclick="saveHost()">Save Hostname</button>
</div>

<div class="card">
  <h3>🔌 Relay Pins Config</h3>
  <div class="pins-grid" id="pinConfig"></div>
  <button class="btn btn-save" onclick="savePins()">Save Pins</button>
</div>

<div class="card">
    <h3>🖥️ Web Serial Monitor</h3>
    <div id="terminal-container">
        <div class="terminal-header">
            <span>Console Output</span>
            <button class="btn-clear" onclick="clearTerminal()">Clear</button>
        </div>
        <div id="serial-terminal">-- System Initialized --</div>
    </div>
</div>

<div class="card">
  <h3>📦 Firmware Update (OTA)</h3>
  <div class="info-row">
    <span class="info-label">Current Version:</span>
    <span class="info-val" id="fwVersion" style="color:#38ef7d">...</span>
  </div>
  <div class="info-row">
    <span class="info-label">Build Date:</span>
    <span class="info-val" id="fwDate">...</span>
  </div>

  <div style="margin-top:15px;padding:15px;background:rgba(241,196,15,0.1);border-radius:8px">
    <p style="font-size:12px;color:#f1c40f;margin-bottom:10px">
      ⚠️ <strong>Cara Update:</strong><br>
      Arduino IDE → Sketch → Export Compiled Binary → Upload file .bin
    </p>

    <input type="file" id="otaFile" accept=".bin"
           style="width:100%;padding:10px;background:rgba(0,0,0,0.3);border:1px dashed rgba(255,255,255,0.3);border-radius:8px;color:#fff;margin-bottom:10px">
    <button class="btn btn-save" style="width:100%" onclick="uploadFirmware()">
      📤 Upload Firmware
    </button>

    <div id="otaProgress" style="display:none;margin-top:10px">
      <div style="background:rgba(0,0,0,0.3);border-radius:10px;overflow:hidden">
        <div id="otaBar" style="height:20px;background:linear-gradient(90deg,#11998e,#38ef7d);width:0%;transition:width 0.3s"></div>
      </div>
      <p id="otaStatus" style="text-align:center;margin-top:5px;font-size:12px">Uploading...</p>
    </div>
  </div>
</div>

<div class="card">
  <h3>🎭 Light Show Testing</h3>
  <p style="font-size:12px;opacity:0.7;margin-bottom:15px">
    Test relays with cinematic patterns. <strong>Best for Manual Mode!</strong>
  </p>

  <label>Effect Pattern:</label>
  <select id="testPattern" class="pattern-select">
    <optgroup label="🔧 Basic Testing">
      <option value="1">⚡ Sequential</option>
      <option value="2">🌊 Wave</option>
      <option value="3">💫 All Blink</option>
      <option value="4">🎲 Random</option>
    </optgroup>

    <optgroup label="🎬 Cinematic">
      <option value="5">🎬 Theater Chase</option>
      <option value="6">⚡ Strobe Flash</option>
      <option value="7">💓 Heartbeat</option>
      <option value="8">🌌 Sparkle Stars</option>
      <option value="9">🎪 Carnival</option>
      <option value="10">🌈 Rainbow Wave</option>
    </optgroup>

    <optgroup label="🚨 Emergency">
      <option value="11">🚨 Police Lights</option>
      <option value="12">🚒 Fire Truck</option>
      <option value="13">⚠️ SOS Alert</option>
    </optgroup>

    <optgroup label="🎉 Party">
      <option value="14">🎉 Party Mode</option>
      <option value="15">💥 Explosion</option>
      <option value="16">🎆 Fireworks</option>
      <option value="17">😴 Breathing</option>
    </optgroup>

    <optgroup label="🌠 Advanced">
      <option value="18">🌠 Comet Tail</option>
      <option value="19">🔄 Rotation</option>
    </optgroup>
  </select>

  <label>Speed: <span id="speedValue">Medium</span></label>
  <input type="range" id="testSpeed" min="1" max="5" value="3" step="1"
         style="width:100%;margin-bottom:5px"
         oninput="updateSpeed(this.value)">
  <div class="speed-labels">
    <span>Slow</span><span>→</span><span>Fast</span>
  </div>

  <label style="display:flex;align-items:center;gap:8px;margin-bottom:15px">
    <input type="checkbox" id="intensityMode" style="width:18px;height:18px">
    <span>⚡ High Intensity Mode</span>
  </label>

  <div class="test-buttons">
    <button class="btn btn-save" onclick="startLightTest()" id="btnStartTest">▶️ Start</button>
    <button class="btn btn-danger" onclick="stopLightTest()" id="btnStopTest" disabled>⏹️ Stop</button>
  </div>

  <div class="live-preview">
    <div class="preview-relay" id="prev0">1</div>
    <div class="preview-relay" id="prev1">2</div>
    <div class="preview-relay" id="prev2">3</div>
    <div class="preview-relay" id="prev3">4</div>
  </div>

  <div id="testInfo" style="display:none">
    <strong id="patternName">-</strong> | Cycles: <span id="cycleCount">0</span>
  </div>
</div>

<div class="card">
  <h3>⚠️ Danger Zone</h3>
  <button class="btn btn-reset" onclick="factoryReset()">⚠️ Reset ke Factory Default</button>
  <button class="btn btn-danger" onclick="restartESP()">🔄 Restart ESP8266</button>
</div>

<a href="/" class="back-link">← Back to Dashboard</a>

</div>

<!-- Toast Container -->
<div id="toastContainer" class="toast-container"></div>

<!-- Confirm Modal -->
<div id="confirmModal" class="modal-overlay">
  <div class="modal-content">
    <div class="modal-icon" id="confirmIcon">⚠️</div>
    <h2 id="confirmTitle">Konfirmasi</h2>
    <p id="confirmMessage">Apakah Anda yakin?</p>
    <div class="modal-actions">
      <button class="btn" style="background:rgba(255,255,255,0.2)" id="confirmCancel">Batal</button>
      <button class="btn btn-danger" id="confirmOk">Ya, Lanjutkan</button>
    </div>
  </div>
</div>

<script>
let confirmResolver = null;

function showToast(message, type = 'success', duration = 3000) {
  const container = document.getElementById('toastContainer');
  const toast = document.createElement('div');
  toast.className = `toast ${type}`;
  const icons = { success: '✓', error: '✕', warning: '⚠', info: 'ℹ' };
  toast.innerHTML = `<span class="toast-icon">${icons[type] || '•'}</span><span>${message}</span><button class="toast-close" onclick="this.parentElement.remove()">×</button>`;
  container.appendChild(toast);
  requestAnimationFrame(() => toast.classList.add('show'));
  setTimeout(() => { toast.classList.remove('show'); setTimeout(() => toast.remove(), 400); }, duration);
}

function showConfirm(title, message, icon = '⚠️') {
  return new Promise((resolve) => {
    document.getElementById('confirmTitle').textContent = title;
    document.getElementById('confirmMessage').innerHTML = message;
    document.getElementById('confirmIcon').textContent = icon;
    document.getElementById('confirmModal').classList.add('show');
    confirmResolver = resolve;
  });
}
document.getElementById('confirmOk').onclick = () => { document.getElementById('confirmModal').classList.remove('show'); if(confirmResolver) confirmResolver(true); };
document.getElementById('confirmCancel').onclick = () => { document.getElementById('confirmModal').classList.remove('show'); if(confirmResolver) confirmResolver(false); };

function setBtnLoading(btn, loading) {
  if(loading) { btn.classList.add('loading'); btn.disabled = true; }
  else { btn.classList.remove('loading'); btn.disabled = false; }
}

const PIN_MAP = {
  16: 'D0', 5: 'D1', 4: 'D2', 0: 'D3',
  2: 'D4', 14: 'D5', 12: 'D6', 13: 'D7', 15: 'D8'
};

function loadData() {
  fetch('/admin/data').then(r => r.json()).then(d => {
    document.getElementById('heap').innerText = d.heap + ' bytes';
    document.getElementById('uptime').innerText = Math.floor(d.uptime/1000) + 's';
    document.getElementById('ip').innerText = d.ip;

      document.getElementById('rtcTime').innerText = d.rtcTime || 'N/A';
      document.getElementById('rtcTime').style.color = (d.rtcTime && d.rtcTime !== 'N/A') ? '#0f0' : '#888';

      const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
      document.getElementById('memTime').innerText = d.memTime + ' (' + DAYS[d.memDow] + ')';

      document.getElementById('rtcStats').innerHTML =
        '<span style="color:#0f0">OK:' + d.rtcOK + '</span> | ' +
        '<span style="color:#f55">ERR:' + d.rtcERR + '</span>';

      document.getElementById('ssid').value = d.ssid;
    document.getElementById('pass').value = d.pass;
    document.getElementById('host').value = d.host;
    if(d.useRTC) document.getElementById('useRTC').checked = true;

    document.getElementById('fwVersion').innerText = 'v' + d.fwVersion;
    document.getElementById('fwDate').innerText = d.fwDate;

    let html = '';
    for(let i=0; i<4; i++) {
      html += `<div style="margin-bottom:10px">
        <label style="font-size:12px;opacity:0.7">Relay ${i+1}</label>
        <div style="display:flex;gap:5px">
           <select id="pin${i}">`;
      for(let p in PIN_MAP) {
         let sel = (d.pins[i] == p) ? 'selected' : '';
         html += `<option value="${p}" ${sel}>${PIN_MAP[p]}</option>`;
      }
      html += `</select>
           <label style="display:flex;align-items:center;background:rgba(255,255,255,0.1);padding:0 10px;border-radius:6px;cursor:pointer">
             <input type="checkbox" id="en${i}" ${(d.mask&(1<<i))?'checked':''} style="width:18px;height:18px;margin:0 5px 0 0">
             <span style="font-size:12px">Enable</span>
           </label>
        </div>
      </div>`;
    }
    document.getElementById('pinConfig').innerHTML = html;
  });
}

function saveWifi() {
  let ssid = document.getElementById('ssid').value;
  let pass = document.getElementById('pass').value;
  if(pass.length < 8) { showToast('Password minimal 8 karakter!', 'warning'); return; }
  fetch(`/admin/savewifi?ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}`)
    .then(r => r.text())
    .then(() => showToast('✓ WiFi configuration saved!', 'success'))
    .catch(() => showToast('Failed to save WiFi!', 'error'));
}

function saveHost() {
  let host = document.getElementById('host').value;
  fetch(`/admin/savehost?host=${encodeURIComponent(host)}`)
    .then(r => r.text())
    .then(() => showToast('✓ Hostname saved!', 'success'))
    .catch(() => showToast('Failed to save hostname!', 'error'));
}

function saveRTC() {
  let enabled = document.getElementById('useRTC').checked ? 1 : 0;
  fetch('/admin/savertc?enabled='+enabled)
    .then(r => r.text())
    .then(() => showToast(enabled ? '✓ RTC Enabled!' : 'RTC Disabled', enabled ? 'success' : 'info'))
    .catch(() => showToast('Failed to save RTC settings!', 'error'));
}

function savePins() {
  let q = '';
  let mask = 0;
  for(let i=0; i<4; i++) {
     q += `&p${i}=` + document.getElementById('pin'+i).value;
     if(document.getElementById('en'+i).checked) mask |= (1 << i);
  }
  fetch('/admin/savepins?m='+mask+q)
    .then(r => r.text())
    .then(() => showToast('✓ Pin configuration saved!', 'success'))
    .catch(() => showToast('Failed to save pins!', 'error'));
}

async function factoryReset() {
  const confirmed = await showConfirm(
    '⚠️ Factory Reset',
    'Yakin reset semua konfigurasi?<br><br>• Data jadwal akan hilang<br>• Nama relay akan direset<br>• ESP akan restart',
    '🗑️'
  );
  if (confirmed) {
    showToast('Resetting... ESP will restart', 'warning', 5000);
    fetch('/admin/reset').then(() => {
      setTimeout(() => location.reload(), 3000);
    }).catch(() => showToast('Reset failed!', 'error'));
  }
}

async function restartESP() {
  const confirmed = await showConfirm(
    '🔄 Restart ESP8266',
    'ESP akan direstart.<br>Halaman akan refresh otomatis.',
    '🔄'
  );
  if (confirmed) {
    showToast('Restarting ESP8266...', 'info', 5000);
    fetch('/admin/restart').then(() => {
      setTimeout(() => location.reload(), 3000);
    }).catch(() => showToast('Restart failed!', 'error'));
  }
}

function connectWS() {
    const ws = new WebSocket('ws://' + location.hostname + ':81/');
    const term = document.getElementById('serial-terminal');

    ws.onmessage = function(e) {
        try {
            const data = JSON.parse(e.data);
            if(data.type === 'log') {
                const now = new Date();
                const timeStr = now.getHours().toString().padStart(2,'0') + ':' +
                              now.getMinutes().toString().padStart(2,'0') + ':' +
                              now.getSeconds().toString().padStart(2,'0');

                const line = document.createElement('div');
                line.innerHTML = `<span class="log-time">[${timeStr}]</span>${data.msg}`;
                term.appendChild(line);

                term.scrollTop = term.scrollHeight;

                while(term.childNodes.length > 100) {
                    term.removeChild(term.firstChild);
                }

                const msg = data.msg.toLowerCase();
                if(msg.includes('rtc') || msg.includes('sync') || msg.includes('boot:')) {
                    const rtcTerm = document.getElementById('rtc-terminal');
                    if(rtcTerm) {
                        const rtcLine = document.createElement('div');

                        let color = '#0f0';
                        if(msg.includes('err') || msg.includes('fail') || msg.includes('invalid')) {
                            color = '#f55';
                        } else if(msg.includes('attempt') || msg.includes('pending') || msg.includes('queued')) {
                            color = '#ff0';
                        }
                        rtcLine.innerHTML = `<span style="color:#888">[${timeStr}]</span> <span style="color:${color}">${data.msg}</span>`;
                        rtcTerm.appendChild(rtcLine);
                        rtcTerm.scrollTop = rtcTerm.scrollHeight;
                        while(rtcTerm.childNodes.length > 50) {
                            rtcTerm.removeChild(rtcTerm.firstChild);
                        }
                    }
                }
            }
        } catch(err) { /* Not a log message, ignore */ }
    };

    ws.onclose = () => setTimeout(connectWS, 2000);
}

function clearTerminal() {
    document.getElementById('serial-terminal').innerHTML = '-- Terminal Cleared --\n';
}

function clearRTCLog() {
    document.getElementById('rtc-terminal').innerHTML = '-- RTC Log Cleared --';
}

const PATTERN_NAMES = [
  '', 'Sequential', 'Wave', 'All Blink', 'Random',
  'Theater Chase', 'Strobe', 'Heartbeat', 'Sparkle',
  'Carnival', 'Rainbow Wave', 'Police', 'Fire Truck',
  'SOS Alert', 'Party', 'Explosion', 'Fireworks',
  'Breathing', 'Comet Tail', 'Rotation'
];
const SPEED_DELAYS = [2000, 1500, 1000, 500, 200];
const SPEED_LABELS = ['Very Slow', 'Slow', 'Medium', 'Fast', 'Very Fast'];

let lightTestInterval = null;
let lightTestStep = 0;
let lightTestCycles = 0;
let lastLightStates = [false, false, false, false];

function updateSpeed(value) {
  document.getElementById('speedValue').innerText = SPEED_LABELS[value - 1];
}

function startLightTest() {
  let pattern = parseInt(document.getElementById('testPattern').value);
  let speed = parseInt(document.getElementById('testSpeed').value);
  let delay = SPEED_DELAYS[speed - 1];

  if(lightTestInterval) stopLightTest();

  lightTestStep = 0;
  lightTestCycles = 0;
  document.getElementById('testInfo').style.display = 'block';
  document.getElementById('patternName').innerText = PATTERN_NAMES[pattern];
  document.getElementById('btnStartTest').disabled = true;
  document.getElementById('btnStopTest').disabled = false;

  lightTestInterval = setInterval(function() {
    runLightPattern(pattern);
    if(lightTestStep % 10 === 0) lightTestCycles++;
    document.getElementById('cycleCount').innerText = lightTestCycles;
  }, delay);
}

function stopLightTest() {
  if(lightTestInterval) {
    clearInterval(lightTestInterval);
    lightTestInterval = null;
  }

  for(let i = 0; i < 4; i++) {
    fetch('/relay?r=' + i + '&s=0');
  }
  applyLightStates([false, false, false, false]);

  document.getElementById('testInfo').style.display = 'none';
  document.getElementById('btnStartTest').disabled = false;
  document.getElementById('btnStopTest').disabled = true;
  lightTestStep = 0;
  lightTestCycles = 0;
}

function runLightPattern(pattern) {
  let states = [false, false, false, false];
  let step = lightTestStep;
  let highIntensity = document.getElementById('intensityMode').checked;

  switch(pattern) {
    case 1:
      states[step % 4] = true;
      break;

    case 2:
      let seq = [0, 1, 2, 3, 2, 1];
      states[seq[step % seq.length]] = true;
      break;

    case 3:
      let allOn = step % 2 === 0;
      states = [allOn, allOn, allOn, allOn];
      break;

    case 4:
      for(let i = 0; i < 4; i++) {
        states[i] = Math.random() > 0.5;
      }
      break;

    case 5:
      states[step % 4] = true;
      states[(step + 2) % 4] = true;
      break;

    case 6:
      let strobe = step % 2 === 0;
      states = [strobe, strobe, strobe, strobe];
      break;

    case 7:
      if(step % 8 < 2 || (step % 8 >= 3 && step % 8 < 5)) {
        states = [true, true, true, true];
      }
      break;

    case 8:
      let sparkle = Math.floor(Math.random() * 4);
      states[sparkle] = true;
      if(Math.random() > 0.7) states[(sparkle + 1) % 4] = true;
      break;

    case 9:
      if(step % 2 === 0) {
        states[0] = states[1] = true;
      } else {
        states[2] = states[3] = true;
      }
      break;

    case 10:
      let pos = step % 7;
      if(pos <= 3) states[pos] = true;
      if(pos > 0 && pos <= 4) states[pos - 1] = true;
      break;

    case 11:
      let side = Math.floor(step / 2) % 2;
      if(side === 0) {
        states[0] = states[1] = step % 2 === 0;
      } else {
        states[2] = states[3] = step % 2 === 0;
      }
      break;

    case 12:
      if(step % 5 < 4) {
        states[step % 4] = true;
      }
      break;

    case 13:
      let sos = [1,0,1,0,1,0,0,1,1,1,0,1,1,1,0,1,1,1,0,0,1,0,1,0,1,0,0,0];
      let sosOn = sos[step % sos.length] === 1;
      states = [sosOn, sosOn, sosOn, sosOn];
      break;

    case 14:
      if(step % 6 < 3) {
        for(let i = 0; i < 4; i++) {
          states[i] = Math.random() > 0.5;
        }
      }
      break;

    case 15:
      let phase = step % 20;
      if(phase < 10) {
        let count = Math.floor(phase / 3);
        for(let i = 0; i < count && i < 4; i++) states[i] = true;
      } else if(phase === 10) {
        states = [true, true, true, true];
      } else if(phase < 15) {
        states = [phase % 2 === 0, phase % 2 === 0, phase % 2 === 0, phase % 2 === 0];
      }
      break;

    case 16:
      if(step % 8 === 0 || step % 8 === 1) {
        states = [true, true, true, true];
      } else if(step % 8 === 2) {
        let center = Math.floor(Math.random() * 4);
        states[center] = true;
      }
      break;

    case 17:
      let breathe = Math.sin(step * 0.3) > 0;
      states = [breathe, breathe, breathe, breathe];
      break;

    case 18:
      states[step % 4] = true;
      states[(step + 3) % 4] = true;
      break;

    case 19:
      states[step % 4] = true;
      break;
  }

  if(highIntensity && (pattern === 6 || pattern === 11 || pattern === 14)) {
    if(step % 4 < 2) {
      states = states.map(function() { return false; });
    }
  }

  applyLightStates(states);
  lightTestStep++;
}

function applyLightStates(states) {

  for(let i = 0; i < 4; i++) {
    document.getElementById('prev' + i).classList.toggle('on', states[i]);
  }

  for(let i = 0; i < 4; i++) {
    if(states[i] !== lastLightStates[i]) {
      fetch('/relay?r=' + i + '&s=' + (states[i] ? 1 : 0));
      lastLightStates[i] = states[i];
    }
  }
}

window.addEventListener('beforeunload', function() {
  if(lightTestInterval) stopLightTest();
});

function uploadFirmware() {
  var file = document.getElementById('otaFile').files[0];
  if(!file) {
    showToast('Pilih file .bin terlebih dahulu!', 'warning');
    return;
  }

  if(!file.name.endsWith('.bin')) {
    showToast('File harus berformat .bin!', 'error');
    return;
  }

  if(!confirm('Update firmware ke file: ' + file.name + '?\\n\\nESP akan restart setelah update selesai.')) {
    return;
  }

  var formData = new FormData();
  formData.append('update', file);

  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/update', true);
  xhr.setRequestHeader('Authorization', 'Basic ' + btoa('admin:admin123'));

  document.getElementById('otaProgress').style.display = 'block';

  xhr.upload.onprogress = function(e) {
    if(e.lengthComputable) {
      var pct = Math.round((e.loaded / e.total) * 100);
      document.getElementById('otaBar').style.width = pct + '%';
      document.getElementById('otaStatus').innerText = 'Uploading... ' + pct + '%';
    }
  };

  xhr.onload = function() {
    if(xhr.status === 200) {
      document.getElementById('otaStatus').innerText = 'Success! Restarting ESP...';
      document.getElementById('otaBar').style.width = '100%';
      showToast('Firmware updated! ESP akan restart...', 'success', 10000);
      setTimeout(function() { location.reload(); }, 8000);
    } else {
      document.getElementById('otaStatus').innerText = 'Failed: ' + xhr.statusText;
      showToast('Update gagal: ' + xhr.responseText, 'error');
    }
  };

  xhr.onerror = function() {
    document.getElementById('otaStatus').innerText = 'Connection Error!';
    showToast('Koneksi terputus!', 'error');
  };

  xhr.send(formData);
}

connectWS();
loadData();
</script>
</body>
</html>
)=====";

void handleRoot();
void handleStatus();
void handleRelay();
void handleMode();
void handleTimer();
void handleSetTime();
void handleSetName();
void handleAddSchedule();
void handleDelSchedule();
void handleSetSchedule();
void handleToggleDay();
void handleEmergency();
void handleAdmin();
void handleAdminData();
void handleAdminSaveWifi();
void handleAdminSaveHost();
void handleAdminSaveRTC();
void handleAdminSavePins();
void handleReset();
void handleRestart();
void handleSetCycle();
void handleAll();
void checkSchedules(int relay);
String getStatusJSON();
void broadcastStatus();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);

void setup() {
  Serial.begin(115200);
  delay(500);
  webLog("\n\n=== ESP8266 Relay Controller v2.0 ===");

  EEPROM.begin(EEPROM_SIZE);

  loadConfig();
  loadEmergency();
  loadRTCConfig();

  for(int i = 0; i < 4; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH);
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);

  if (MDNS.begin(hostname)) {
    webLog("MDNS responder started");
  }

  webLog("AP IP: " + WiFi.softAPIP().toString());

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/relay", handleRelay);
  server.on("/mode", handleMode);
  server.on("/timer", handleTimer);
  server.on("/settime", handleSetTime);
  server.on("/setname", handleSetName);
  server.on("/addschedule", handleAddSchedule);
  server.on("/delschedule", handleDelSchedule);
  server.on("/setschedule", handleSetSchedule);
  server.on("/toggleday", handleToggleDay);
  server.on("/emergency", handleEmergency);
  server.on("/setcycle", handleSetCycle);
  server.on("/all", handleAll);

  server.on("/admin", handleAdmin);
  server.on("/admin/data", handleAdminData);
  server.on("/admin/savewifi", handleAdminSaveWifi);
  server.on("/admin/savehost", handleAdminSaveHost);
  server.on("/admin/savertc", handleAdminSaveRTC);
  server.on("/admin/savepins", handleAdminSavePins);
  server.on("/admin/reset", handleReset);
  server.on("/admin/restart", handleRestart);

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  httpUpdater.setup(&server, "/update", "admin", ADMIN_PASSWORD);
  webLog("OTA Update ready at /update");

  server.begin();
  webLog("Web server started");
}

void loop() {
  webSocket.loop();
  MDNS.update();
  server.handleClient();

  static unsigned long lastWSHeartbeat = 0;
  if (millis() - lastWSHeartbeat >= 5000) {
    lastWSHeartbeat = millis();
    broadcastStatus();
  }

  processRTCSet();

  if(useRTC && (millis() - lastRTCSyncMillis >= rtcSyncInterval)) {
    syncWithRTC();
  }

  if(millis() - lastTimeSync >= 60000) {

    if(useRTC && lastRTCSyncMillis > 0) {
      unsigned long elapsedSeconds = (millis() - lastRTCSyncMillis) / 1000;
      unsigned long currentTimestamp = lastRTCTimestamp + elapsedSeconds;

      currentHour = (currentTimestamp / 3600) % 24;
      currentMinute = (currentTimestamp / 60) % 60;

      if(currentTimestamp >= 86400) {

        lastRTCTimestamp = currentTimestamp % 86400;
        lastRTCSyncMillis = millis();
        currentDayOfWeek = (currentDayOfWeek + 1) % 7;
      }
    } else {

      currentMinute++;
      if(currentMinute >= 60) {
        currentMinute = 0;
        currentHour++;
        if(currentHour >= 24) {
          currentHour = 0;
          currentDayOfWeek = (currentDayOfWeek + 1) % 7;
        }
      }
    }
    lastTimeSync = millis();
    saveTime();
    saveTimerData();
  }

  if(emergencyMode) return;

  for(int i = 0; i < 4; i++) {
    if(!(relayEnabledMask & (1 << i))) {
        digitalWrite(relayPins[i], HIGH);
        relayStatus[i] = false;
        continue;
    }

    if(relayMode[i] == 2 && timerDuration[i] > 0) {
      unsigned long elapsed = millis() - timerStart[i];
      if(elapsed >= timerDuration[i]) {
        digitalWrite(relayPins[i], HIGH);
        relayStatus[i] = false;
        timerDuration[i] = 0;
        saveRelayStatus();
      }
    }

    if(relayMode[i] == 1) {
      checkSchedules(i);
    }

    if(relayMode[i] == 3) {
      if(cycleOnDuration[i] > 0 && cycleOffDuration[i] > 0) {
          unsigned long now = millis();
          unsigned long duration = cycleState[i] ? cycleOnDuration[i] : cycleOffDuration[i];

          if(lastCycleSwitch[i] == 0) {
            lastCycleSwitch[i] = now;
            cycleState[i] = true;
            digitalWrite(relayPins[i], LOW);
            relayStatus[i] = true;
          } else if (now - lastCycleSwitch[i] >= duration * 60000UL) {
            cycleState[i] = !cycleState[i];
            lastCycleSwitch[i] = now;
            digitalWrite(relayPins[i], cycleState[i] ? LOW : HIGH);
            relayStatus[i] = cycleState[i];
          }
      }
    }
  }
}

void checkSchedules(int relay) {
  int nowTime = currentHour * 60 + currentMinute;

  for(int j = MAX_SCHEDULES - 1; j >= 0; j--) {
    if(!schedules[relay][j].enabled) continue;

    if((schedules[relay][j].dayMask & (1 << currentDayOfWeek)) == 0) continue;

    int onTime = schedules[relay][j].onHour * 60 + schedules[relay][j].onMin;
    int offTime = schedules[relay][j].offHour * 60 + schedules[relay][j].offMin;

    if(onTime == nowTime && !relayStatus[relay]) {
      digitalWrite(relayPins[relay], LOW);
      relayStatus[relay] = true;
      saveRelayStatus();
      webLog("Relay " + String(relay+1) + " ON by schedule " + String(j+1));
      return;
    }
    if(offTime == nowTime && relayStatus[relay]) {
      digitalWrite(relayPins[relay], HIGH);
      relayStatus[relay] = false;
      saveRelayStatus();
      webLog("Relay " + String(relay+1) + " OFF by schedule " + String(j+1));
      return;
    }
  }
}

void handleRoot() {
  server.send(200, "text/html", MAIN_page);
}

void handleStatus() {
  server.send(200, "application/json", getStatusJSON());
}

void handleRelay() {

  if(emergencyMode) {
    server.send(403, "text/plain", "BLOCKED_EMERGENCY");
    return;
  }

  int relay = server.arg("r").toInt();
  int state = server.arg("s").toInt();

  if(relay >= 0 && relay < 4) {
    if(!(relayEnabledMask & (1 << relay))) {
      server.send(403, "text/plain", "DISABLED");
      return;
    }
    digitalWrite(relayPins[relay], state ? LOW : HIGH);
    relayStatus[relay] = state;
    if(relayMode[relay] == 0) {
      webLog("Relay " + String(relay + 1) + " Manual: " + String(state ? "ON" : "OFF"));
      saveRelayStatus();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleSetTime() {
  currentHour = server.arg("h").toInt();
  currentMinute = server.arg("m").toInt();
  currentDayOfWeek = server.arg("d").toInt();

  lastRTCTimestamp = (unsigned long)currentHour * 3600 +
                     (unsigned long)currentMinute * 60;
  lastRTCSyncMillis = millis();
  lastTimeSync = millis();

  saveTime();

  if(useRTC) {
    startRTCSet(currentHour, currentMinute, 0, currentDayOfWeek);
  }

  webLogf("Time set by user: %02d:%02d dow:%d", currentHour, currentMinute, currentDayOfWeek);
  server.send(200, "text/plain", "OK");
}

void handleMode() {
  int relay = server.arg("r").toInt();
  int mode = server.arg("m").toInt();

  if(relay >= 0 && relay < 4) {
    relayMode[relay] = mode;
    if(mode != 2) {
      timerDuration[relay] = 0;
    }
    saveConfig();
  }
  server.send(200, "text/plain", "OK");
}

void handleTimer() {
  int relay = server.arg("r").toInt();
  int duration = server.arg("d").toInt();

  if(relay >= 0 && relay < 4) {
    if(duration > 0) {
      timerDuration[relay] = duration * 1000UL;
      timerStart[relay] = millis();
      digitalWrite(relayPins[relay], LOW);
      relayStatus[relay] = true;
      saveTimerData();
    } else {
      timerDuration[relay] = 0;
      digitalWrite(relayPins[relay], HIGH);
      relayStatus[relay] = false;
      saveTimerData();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleSetName() {
  int relay = server.arg("r").toInt();
  String name = server.arg("name");

  if(relay >= 0 && relay < 4 && name.length() > 0) {
    name.toCharArray(relayNames[relay], 17);
    saveConfig();
  }
  server.send(200, "text/plain", "OK");
}

void handleAddSchedule() {
  int relay = server.arg("r").toInt();

  if(relay >= 0 && relay < 4) {

    for(int j = 0; j < MAX_SCHEDULES; j++) {
      if(!schedules[relay][j].enabled) {
        schedules[relay][j].enabled = true;
        schedules[relay][j].onHour = 6;
        schedules[relay][j].onMin = 0;
        schedules[relay][j].offHour = 18;
        schedules[relay][j].offMin = 0;
        schedules[relay][j].dayMask = 0x7F;
        saveConfig();
        break;
      }
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleDelSchedule() {
  int relay = server.arg("r").toInt();
  int slot = server.arg("slot").toInt();

  if(relay >= 0 && relay < 4 && slot >= 0 && slot < MAX_SCHEDULES) {
    schedules[relay][slot].enabled = false;
    saveConfig();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetSchedule() {
  int relay = server.arg("r").toInt();
  int slot = server.arg("slot").toInt();

  if(relay >= 0 && relay < 4 && slot >= 0 && slot < MAX_SCHEDULES) {
    schedules[relay][slot].onHour = server.arg("onH").toInt();
    schedules[relay][slot].onMin = server.arg("onM").toInt();
    schedules[relay][slot].offHour = server.arg("offH").toInt();
    schedules[relay][slot].offMin = server.arg("offM").toInt();
    saveConfig();
  }
  server.send(200, "text/plain", "OK");
}

void handleToggleDay() {
  int relay = server.arg("r").toInt();
  int slot = server.arg("slot").toInt();
  int day = server.arg("day").toInt();

  if(relay >= 0 && relay < 4 && slot >= 0 && slot < MAX_SCHEDULES && day >= 0 && day < 7) {
    schedules[relay][slot].dayMask ^= (1 << day);
    saveConfig();
  }
  server.send(200, "text/plain", "OK");
}

void handleEmergency() {
  String action = server.arg("action");

  if(action == "on") {
    emergencyMode = true;
    webLog("!!! EMERGENCY STOP ACTIVATED !!!");
    saveEmergency();

    for(int i = 0; i < 4; i++) {
      digitalWrite(relayPins[i], HIGH);
      relayStatus[i] = false;
    }
    saveRelayStatus();

    server.send(200, "text/plain", "EMERGENCY_ON");

    delay(500);
    ESP.restart();
  }
  else if(action == "off") {
    emergencyMode = false;
    webLog("Emergency mode deactivated");
    saveEmergency();
    server.send(200, "text/plain", "EMERGENCY_OFF");
  }
  else {
    server.send(400, "text/plain", "Invalid action");
  }
}

bool requireAuth() {
  if(!server.authenticate("admin", ADMIN_PASSWORD)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

void handleAdmin() {
  if(!requireAuth()) return;
  server.send(200, "text/html", ADMIN_page);
}

void handleAdminData() {
  if(!requireAuth()) return;

  String json = "{";
  json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"uptime\":" + String(millis()) + ",";
  json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"ssid\":\"" + String(ap_ssid) + "\",";
  json += "\"pass\":\"" + String(ap_password) + "\",";
  json += "\"host\":\"" + String(hostname) + "\",";
  json += "\"useRTC\":" + String(useRTC ? "true" : "false") + ",";
  json += "\"fwVersion\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"fwDate\":\"" + String(BUILD_DATE) + " " + String(BUILD_TIME) + "\",";

  char rtcTimeStr[20] = "N/A";
  if(rtcLastHour >= 0) {
    sprintf(rtcTimeStr, "%02d:%02d:%02d", rtcLastHour, rtcLastMin, rtcLastSec);
  }
  json += "\"rtcTime\":\"" + String(rtcTimeStr) + "\",";
  json += "\"rtcOK\":" + String(rtcReadSuccess) + ",";
  json += "\"rtcERR\":" + String(rtcReadFailed) + ",";

  char memTimeStr[20];
  sprintf(memTimeStr, "%02d:%02d", currentHour, currentMinute);
  json += "\"memTime\":\"" + String(memTimeStr) + "\",";
  json += "\"memDow\":" + String(currentDayOfWeek) + ",";

  json += "\"mask\":" + String(relayEnabledMask) + ",";
  json += "\"pins\":[";
  for(int i=0; i<4; i++) {
    json += String(relayPins[i]);
    if(i<3) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleAdminSaveWifi() {
  if(!requireAuth()) return;
  String s = server.arg("ssid");
  String p = server.arg("pass");

  if(s.length() > 0 && p.length() >= 8) {
    s.toCharArray(ap_ssid, 33);
    p.toCharArray(ap_password, 33);

    for(int i=0; i<32; i++) {
        EEPROM.write(EEPROM_WIFI_START + i, ap_ssid[i]);
        EEPROM.write(EEPROM_PASS_START + i, ap_password[i]);
    }
    EEPROM.commit();
    server.send(200, "text/plain", "WiFi disimpan! Restart ESP untuk menerapkan.");
  } else {
    server.send(400, "text/plain", "Invalid Data");
  }
}

void handleAdminSaveHost() {
  if(!requireAuth()) return;
  String h = server.arg("host");
  if(h.length() > 0) {
    h.toCharArray(hostname, 33);
    for(int i=0; i<32; i++) EEPROM.write(EEPROM_HOST_START + i, hostname[i]);
    EEPROM.commit();
    server.send(200, "text/plain", "Hostname disimpan!");
  } else {
    server.send(400, "text/plain", "Invalid Data");
  }
}

void handleAdminSaveRTC() {
  if(!requireAuth()) return;
  useRTC = server.arg("enabled").toInt() == 1;
  EEPROM.write(EEPROM_RTC_FLAG, useRTC ? 1 : 0);
  EEPROM.commit();

  if(useRTC) {
    Wire.begin(D2, D1);
  }

  server.send(200, "text/plain", useRTC ? "RTC Enabled (D1+D2)" : "RTC Disabled");
}

void handleAdminSavePins() {
  if(!requireAuth()) return;

  if(server.hasArg("m")) {
      relayEnabledMask = server.arg("m").toInt();
      EEPROM.write(EEPROM_RELAY_ENABLE, relayEnabledMask);
  }

  for(int i=0; i<4; i++) {
    if(server.hasArg("p"+String(i))) {
        relayPins[i] = server.arg("p"+String(i)).toInt();
        EEPROM.write(EEPROM_PINS_START + i, relayPins[i]);
    }
  }
  EEPROM.commit();
  server.send(200, "text/plain", "Pin Layout disimpan! Restart ESP rekomendasi.");
}

void handleSetCycle() {
  int r = server.arg("r").toInt();
  int on = server.arg("on").toInt();
  int off = server.arg("off").toInt();

  if(r >= 0 && r < 4) {
    cycleOnDuration[r] = on;
    cycleOffDuration[r] = off;
    saveCycleData(r);

    cycleState[r] = false;
    lastCycleSwitch[r] = 0;
  }
  server.send(200, "text/plain", "OK");
}

void handleAll() {
  int s = server.arg("s").toInt();

  for(int i=0; i<4; i++) {
    if(!(relayEnabledMask & (1 << i))) continue;

    relayMode[i] = 0;
    EEPROM.write(EEPROM_MODE_START + i, 0);

    digitalWrite(relayPins[i], s ? LOW : HIGH);
    relayStatus[i] = s == 1;

    timerDuration[i] = 0;
  }

  saveConfig();
  saveRelayStatus();
  server.send(200, "text/plain", "OK");
}

void handleReset() {
  if(!requireAuth()) return;

  EEPROM.write(EEPROM_INIT_FLAG, 0);
  EEPROM.commit();

  server.send(200, "text/plain", "Resetting...");
  delay(1000);
  ESP.restart();
}

void handleRestart() {
  if(!requireAuth()) return;
  server.send(200, "text/plain", "Restarting...");
  delay(1000);
  ESP.restart();
}

String getStatusJSON() {
  String json = "{\"hour\":" + String(currentHour);
  json += ",\"minute\":" + String(currentMinute);
  json += ",\"dayOfWeek\":" + String(currentDayOfWeek);

  json += ",\"status\":[";
  for(int i = 0; i < 4; i++) {
    json += relayStatus[i] ? "true" : "false";
    if(i < 3) json += ",";
  }

  json += "],\"mode\":[";
  for(int i = 0; i < 4; i++) {
    json += String(relayMode[i]);
    if(i < 3) json += ",";
  }

  json += "],\"cycleOn\":[";
  for(int i = 0; i < 4; i++) { json += String(cycleOnDuration[i]); if(i < 3) json += ","; }
  json += "],\"cycleOff\":[";
  for(int i = 0; i < 4; i++) { json += String(cycleOffDuration[i]); if(i < 3) json += ","; }

  json += "],\"names\":[";
  for(int i = 0; i < 4; i++) {
    json += "\"" + String(relayNames[i]) + "\"";
    if(i < 3) json += ",";
  }

  json += "],\"schedules\":[";
  for(int i = 0; i < 4; i++) {
    json += "[";
    for(int j = 0; j < MAX_SCHEDULES; j++) {
      json += "{\"onH\":" + String(schedules[i][j].onHour);
      json += ",\"onM\":" + String(schedules[i][j].onMin);
      json += ",\"offH\":" + String(schedules[i][j].offHour);
      json += ",\"offM\":" + String(schedules[i][j].offMin);
      json += ",\"dayMask\":" + String(schedules[i][j].dayMask);
      json += ",\"enabled\":" + String(schedules[i][j].enabled ? "true" : "false") + "}";
      if(j < MAX_SCHEDULES-1) json += ",";
    }
    json += "]";
    if(i < 3) json += ",";
  }

  json += "],\"timer\":[";
  for(int i = 0; i < 4; i++) {
     unsigned long remaining = 0;
     if(relayMode[i] == 2 && timerDuration[i] > 0) {
        unsigned long elapsed = millis() - timerStart[i];
        if(elapsed < timerDuration[i]) remaining = (timerDuration[i] - elapsed) / 1000;
     }
     json += String(remaining);
     if(i < 3) json += ",";
  }

  json += "],\"timerMax\":[";
  for(int i = 0; i < 4; i++) {
     unsigned long maxDuration = timerDuration[i] / 1000;
     json += String(maxDuration);
     if(i < 3) json += ",";
  }

  json += "],\"emergency\":";
  json += emergencyMode ? "true" : "false";
  json += ",\"mask\":" + String(relayEnabledMask);
  json += "}";
  return json;
}

void broadcastStatus() {
  String json = getStatusJSON();
  webSocket.broadcastTXT(json);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      webLogf("[%u] Disconnected!", num);
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        webLogf("[%u] Connected from %d.%d.%d.%d", num, ip[0], ip[1], ip[2], ip[3]);

        String initialStatus = getStatusJSON();
        webSocket.sendTXT(num, initialStatus);
      }
      break;
    case WStype_TEXT:

      break;
  }
}

void webLog(String msg) {
  Serial.println(msg);

  String logMsg = "{\"type\":\"log\",\"msg\":\"" + msg + "\"}";
  webSocket.broadcastTXT(logMsg);
}

void webLogf(const char* format, ...) {
  char loc_buf[128];
  va_list arg;
  va_start(arg, format);
  vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
  va_end(arg);
  webLog(String(loc_buf));
}