// Adafruit IO WipperSnapper Beta
//
//
// NOTE: This software is a BETA release and in active development.
// Please report bugs or errors to https://github.com/adafruit/Adafruit_Wippersnapper_Arduino/issues
//
//
// Adafruit invests time and resources providing this open source code.
// Please support Adafruit and open source hardware by purchasing
// products from Adafruit!
//
// Brent Rubell for Adafruit Industries, 2021-2022
//
// All text above must be included in any redistribution.

#include "Wippersnapper_Networking.h"
#include "components/display/controller.h"
#if defined(ARDUINO_MAGTAG29_ESP32S2)
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#endif
Wippersnapper_WiFi wipper;

// Enable debug output for beta builds
#define WS_DEBUG

#if defined(ARDUINO_MAGTAG29_ESP32S2)
static unsigned long magtagAwakeStarted = 0;
static uint32_t magtagAwakeWindowSecondsOverride = 0;
static char magtagSerialCommand[48];
static uint8_t magtagSerialCommandLen = 0;
static bool magtagLowPowerStarted = false;
static bool magtagDeepSleepStarting = false;

struct magtagQueuedButtonEvent {
  bool active;
  uint8_t pin;
  uint32_t queuedAt;
  uint32_t nextAttemptAt;
  uint32_t backoffMs;
};

static magtagQueuedButtonEvent magtagButtonEvent = {false, 0, 0, 0, 250};
static const uint32_t MAGTAG_BUTTON_EVENT_TIMEOUT_MS = 120000UL;
static const uint32_t MAGTAG_BUTTON_EVENT_MAX_BACKOFF_MS = 5000UL;
static const uint16_t MAGTAG_SLEEP_MARKER_SETTLE_MS = 250;

static uint32_t magtagAwakeWindowSeconds() {
  if (magtagAwakeWindowSecondsOverride > 0) {
    return magtagAwakeWindowSecondsOverride;
  }

  uint32_t minutes = WS._config.magtag_low_power.awake_window_minutes;
  if (minutes == 0) {
    minutes = 10;
  }
  return minutes * 60UL;
}

static uint32_t magtagSleepIntervalMinutes() {
  uint32_t minutes = WS._config.magtag_low_power.sleep_interval_minutes;
  return minutes == 0 ? 180 : minutes;
}

static void magtagPrintWakeReason() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  Serial.print("WS_MAGTAG_LOW_POWER_WAKE reason=");
  switch (cause) {
  case ESP_SLEEP_WAKEUP_TIMER:
    Serial.println("timer");
    break;
  case ESP_SLEEP_WAKEUP_EXT1:
    Serial.print("button mask=0x");
    Serial.println((uint32_t)esp_sleep_get_ext1_wakeup_status(), HEX);
    break;
  default:
    Serial.println("boot_or_reset");
    break;
  }
}

static void magtagPrepareWakeButtons() {
  const uint8_t wakePins[] = {BUTTON_A, BUTTON_B, BUTTON_C, BUTTON_D};
  for (uint8_t i = 0; i < sizeof(wakePins) / sizeof(wakePins[0]); i++) {
    pinMode(wakePins[i], INPUT_PULLUP);
  }
}

static void magtagPrepareWakeButtonsForSleep() {
  const uint8_t wakePins[] = {BUTTON_A, BUTTON_B, BUTTON_C, BUTTON_D};
  for (uint8_t i = 0; i < sizeof(wakePins) / sizeof(wakePins[0]); i++) {
    gpio_num_t pin = (gpio_num_t)wakePins[i];
    rtc_gpio_init(pin);
    rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(pin);
    rtc_gpio_pulldown_dis(pin);
  }
}

static void magtagPrintButtonLevels() {
  Serial.print(" button_a=");
  Serial.print(digitalRead(BUTTON_A));
  Serial.print(" button_b=");
  Serial.print(digitalRead(BUTTON_B));
  Serial.print(" button_c=");
  Serial.print(digitalRead(BUTTON_C));
  Serial.print(" button_d=");
  Serial.print(digitalRead(BUTTON_D));
}

static uint64_t magtagWakeButtonMask() {
  return (1ULL << BUTTON_A) | (1ULL << BUTTON_B) | (1ULL << BUTTON_C) |
         (1ULL << BUTTON_D);
}

static bool magtagWakePinFromMask(uint64_t mask, uint8_t *pin) {
  const uint8_t wakePins[] = {BUTTON_A, BUTTON_B, BUTTON_C, BUTTON_D};
  for (uint8_t i = 0; i < sizeof(wakePins) / sizeof(wakePins[0]); i++) {
    if ((mask & (1ULL << wakePins[i])) != 0) {
      *pin = wakePins[i];
      return true;
    }
  }
  return false;
}

static void magtagQueueButtonEvent(uint8_t pin) {
  magtagButtonEvent.active = true;
  magtagButtonEvent.pin = pin;
  magtagButtonEvent.queuedAt = millis();
  magtagButtonEvent.nextAttemptAt = magtagButtonEvent.queuedAt;
  magtagButtonEvent.backoffMs = 250;
  Serial.print("WS_MAGTAG_LOW_POWER_BUTTON_QUEUED pin=");
  Serial.println(pin);
}

static void magtagQueueWakeButtonIfNeeded() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) {
    return;
  }

  uint8_t pin = 0;
  if (magtagWakePinFromMask(esp_sleep_get_ext1_wakeup_status(), &pin)) {
    magtagQueueButtonEvent(pin);
  }
}

static bool magtagDeadlineReached(uint32_t startedAt, uint32_t timeoutMs) {
  return (int32_t)(millis() - startedAt - timeoutMs) >= 0;
}

static void magtagScheduleButtonRetry() {
  magtagButtonEvent.nextAttemptAt = millis() + magtagButtonEvent.backoffMs;
  if (magtagButtonEvent.backoffMs < MAGTAG_BUTTON_EVENT_MAX_BACKOFF_MS) {
    magtagButtonEvent.backoffMs *= 2;
    if (magtagButtonEvent.backoffMs > MAGTAG_BUTTON_EVENT_MAX_BACKOFF_MS) {
      magtagButtonEvent.backoffMs = MAGTAG_BUTTON_EVENT_MAX_BACKOFF_MS;
    }
  }
}

static void magtagProcessQueuedButtonEvent() {
  if (!magtagButtonEvent.active) {
    return;
  }

  if (magtagDeadlineReached(magtagButtonEvent.queuedAt,
                            MAGTAG_BUTTON_EVENT_TIMEOUT_MS)) {
    Serial.print("WS_MAGTAG_LOW_POWER_BUTTON_DROPPED pin=");
    Serial.print(magtagButtonEvent.pin);
    Serial.println(" reason=timeout");
    magtagButtonEvent.active = false;
    return;
  }

  if ((int32_t)(millis() - magtagButtonEvent.nextAttemptAt) < 0) {
    return;
  }

  if (WS._mqtt == nullptr || !WS._mqtt->connected() ||
      WS._topic_signal_device == nullptr || WS.isThrottleActive()) {
    magtagScheduleButtonRetry();
    return;
  }

  wippersnapper_signal_v1_CreateSignalRequest outgoingSignalMsg =
      wippersnapper_signal_v1_CreateSignalRequest_init_zero;
  if (!WS.encodePinEvent(&outgoingSignalMsg, magtagButtonEvent.pin, LOW)) {
    Serial.print("WS_MAGTAG_LOW_POWER_BUTTON_DROPPED pin=");
    Serial.print(magtagButtonEvent.pin);
    Serial.println(" reason=encode_failed");
    magtagButtonEvent.active = false;
    return;
  }

  size_t msgSz = 0;
  pb_get_encoded_size(&msgSz, wippersnapper_signal_v1_CreateSignalRequest_fields,
                      &outgoingSignalMsg);

  if (!WS._mqtt->publish(WS._topic_signal_device, WS._buffer_outgoing, msgSz,
                         1)) {
    Serial.print("WS_MAGTAG_LOW_POWER_BUTTON_RETRY pin=");
    Serial.println(magtagButtonEvent.pin);
    magtagScheduleButtonRetry();
    return;
  }

  Serial.print("WS_MAGTAG_LOW_POWER_BUTTON_PUBLISHED pin=");
  Serial.println(magtagButtonEvent.pin);
  magtagButtonEvent.active = false;
}

static void magtagEnterDeepSleep(uint32_t sleepSeconds) {
  if (magtagDeepSleepStarting) {
    return;
  }
  magtagDeepSleepStarting = true;

  if (sleepSeconds == 0) {
    sleepSeconds = magtagSleepIntervalMinutes() * 60UL;
  }

  Serial.print("WS_MAGTAG_LOW_POWER_SLEEP seconds=");
  Serial.println(sleepSeconds);

  if (WS._displayController != nullptr) {
    Serial.println("WS_MAGTAG_LOW_POWER_SLEEP_MARKER_BEGIN");
    WS._displayController->drawSleepMarkers(
        WS._config.magtag_low_power.sleep_message);
    Serial.println("WS_MAGTAG_LOW_POWER_SLEEP_MARKER_DONE");
    delay(MAGTAG_SLEEP_MARKER_SETTLE_MS);
    WS.feedWDT();
  }

  wipper.disconnect();

#ifdef NEOPIXEL_POWER
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, !NEOPIXEL_POWER_ON);
#endif

  esp_err_t timerWakeErr =
      esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
  magtagPrepareWakeButtons();
  magtagPrepareWakeButtonsForSleep();
  Serial.print("WS_MAGTAG_LOW_POWER_SLEEP_BUTTON_LEVELS");
  magtagPrintButtonLevels();
  Serial.println();
  esp_err_t buttonWakeErr = esp_sleep_enable_ext1_wakeup(
      magtagWakeButtonMask(), ESP_EXT1_WAKEUP_ANY_LOW);
  if (timerWakeErr != ESP_OK || buttonWakeErr != ESP_OK) {
    Serial.print("WS_MAGTAG_LOW_POWER_SLEEP_WAKE_ERROR timer=");
    Serial.print((int)timerWakeErr);
    Serial.print(" button=");
    Serial.println((int)buttonWakeErr);
    magtagDeepSleepStarting = false;
    return;
  }
  Serial.flush();
  esp_deep_sleep_start();
  Serial.println("WS_MAGTAG_LOW_POWER_SLEEP_START_RETURNED");
  magtagDeepSleepStarting = false;
}

static void magtagHandleLowPowerCommand(const char *cmd) {
  if (strncmp(cmd, "WSLP", 4) != 0) {
    return;
  }

  while (*cmd == ' ') {
    cmd++;
  }

  if (strncmp(cmd, "WSLP SLEEP", 10) == 0) {
    uint32_t sleepSeconds = atoi(cmd + 10);
    magtagEnterDeepSleep(sleepSeconds);
  } else if (strncmp(cmd, "WSLP AWAKE ", 11) == 0) {
    magtagAwakeWindowSecondsOverride = atoi(cmd + 11);
    magtagAwakeStarted = millis();
    Serial.print("WS_MAGTAG_LOW_POWER_AWAKE_OVERRIDE seconds=");
    Serial.println(magtagAwakeWindowSecondsOverride);
  } else if (strcmp(cmd, "WSLP BUTTON") == 0) {
    magtagAwakeStarted = millis();
    Serial.println("WS_MAGTAG_LOW_POWER_WAKE reason=button-emulated");
    magtagQueueButtonEvent(BUTTON_A);
  } else if (strcmp(cmd, "WSLP STATUS") == 0) {
    Serial.print("WS_MAGTAG_LOW_POWER_STATUS enabled=");
    Serial.print(WS._config.magtag_low_power.enabled ? 1 : 0);
    Serial.print(" awake_seconds=");
    Serial.print((millis() - magtagAwakeStarted) / 1000UL);
    Serial.print(" awake_window_seconds=");
    Serial.print(magtagAwakeWindowSeconds());
    Serial.print(" sleep_interval_minutes=");
    Serial.print(magtagSleepIntervalMinutes());
    Serial.print(" queued_button=");
    Serial.print(magtagButtonEvent.active ? 1 : 0);
    Serial.print(" wake_cause=");
    Serial.print((int)esp_sleep_get_wakeup_cause());
    Serial.print(" ext1_mask=0x");
    Serial.print((uint32_t)esp_sleep_get_ext1_wakeup_status(), HEX);
    magtagPrintButtonLevels();
    Serial.println();
  } else {
    Serial.println(
        "WS_MAGTAG_LOW_POWER_COMMANDS WSLP STATUS|BUTTON|AWAKE <seconds>|SLEEP [seconds]");
  }
}

static void magtagPollLowPowerSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      magtagSerialCommand[magtagSerialCommandLen] = '\0';
      magtagHandleLowPowerCommand(magtagSerialCommand);
      magtagSerialCommandLen = 0;
    } else if (magtagSerialCommandLen < sizeof(magtagSerialCommand) - 1) {
      magtagSerialCommand[magtagSerialCommandLen++] = c;
    } else {
      magtagSerialCommandLen = 0;
      Serial.println("WS_MAGTAG_LOW_POWER_COMMAND_TOO_LONG");
    }
  }
}

static void magtagBeginLowPower() {
  magtagAwakeStarted = millis();
  magtagLowPowerStarted = true;
  magtagPrepareWakeButtons();
  magtagPrintWakeReason();
  magtagQueueWakeButtonIfNeeded();

  if (WS._config.magtag_low_power.enabled) {
    Serial.print("WS_MAGTAG_LOW_POWER_ENABLED awake_window_seconds=");
    Serial.print(magtagAwakeWindowSeconds());
    Serial.print(" sleep_interval_minutes=");
    Serial.println(magtagSleepIntervalMinutes());
  }
}

static void magtagRunLowPower() {
  if (!magtagLowPowerStarted || magtagDeepSleepStarting) {
    return;
  }

  magtagPollLowPowerSerial();
  magtagProcessQueuedButtonEvent();

  if (!WS._config.magtag_low_power.enabled) {
    return;
  }

  if (magtagDeadlineReached(magtagAwakeStarted,
                            magtagAwakeWindowSeconds() * 1000UL)) {
    magtagEnterDeepSleep(0);
  }
}

void wsAppBackground() { magtagRunLowPower(); }
#endif

void setup() {
  // Provisioning must occur prior to serial init.
  wipper.provision();

  Serial.begin(115200);
  // while (!Serial) delay(10);
  Serial.print("WS_MAGTAG_LOCAL_BUILD_MARKER ");
  Serial.print(__DATE__);
  Serial.print(" ");
  Serial.println(__TIME__);

#if defined(ARDUINO_MAGTAG29_ESP32S2)
  magtagBeginLowPower();
#endif
  wipper.connect();

}

void loop() {
  static unsigned long lastMarker = 0;

  if (millis() - lastMarker >= 10000) {
    lastMarker = millis();
    Serial.println("WS_MAGTAG_LOCAL_BUILD_MARKER running");
  }

#if defined(ARDUINO_MAGTAG29_ESP32S2)
  magtagRunLowPower();
#endif
  wipper.run();
#if defined(ARDUINO_MAGTAG29_ESP32S2)
  magtagRunLowPower();
#endif
}
