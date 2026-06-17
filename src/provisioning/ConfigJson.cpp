/*!
 * @file ConfigJson.cpp
 *
 * Wippersnapper JSON Config File Converters
 *
 * Adafruit invests time and resources providing this open source code,
 * please support Adafruit and open-source hardware by purchasing
 * products from Adafruit!
 *
 * Copyright (c) Brent Rubell 2024 for Adafruit Industries.
 *
 * BSD license, all text here must be included in any redistribution.
 *
 */
#include "ConfigJson.h"

// Converts a network configuration structure to a JSON variant
void convertToJson(const networkConfig &src, JsonVariant dst) {
  dst["network_ssid"] = src.ssid;
  dst["network_password"] = src.pass;
}

// Extracts a network configuration structure from a JSON variant
void convertFromJson(JsonVariantConst src, networkConfig &dst) {
  strlcpy(dst.ssid, src["network_ssid"] | "unset-ssid", sizeof(dst.ssid));
  strlcpy(dst.pass, src["network_password"] | "", sizeof(dst.pass));
}

// Converts a secretsConfig structure to a JSON variant
void convertToJson(const secretsConfig &src, JsonVariant dst) {
  dst["io_username"] = src.aio_user;
  dst["io_key"] = src.aio_key;
  dst["network_type_wifi"] = src.network;
  dst["status_pixel_brightness"] = src.status_pixel_brightness;
  JsonObject lowPower = dst["magtag_low_power"].to<JsonObject>();
  lowPower["enabled"] = src.magtag_low_power.enabled;
  lowPower["display_status_bar"] = src.magtag_low_power.display_status_bar;
  lowPower["sleep_interval_minutes"] =
      src.magtag_low_power.sleep_interval_minutes;
  lowPower["awake_window_minutes"] =
      src.magtag_low_power.awake_window_minutes;
  lowPower["sleep_message"] = src.magtag_low_power.sleep_message;
}

// Extracts a JSON file to a secretsConfig structure
void convertFromJson(JsonVariantConst src, secretsConfig &dst) {
  // Parse network credentials from secrets
  dst.network = src["network_type_wifi"];
  // Parse IO credentials from secrets
  strlcpy(dst.aio_user, src["io_username"] | "YOUR_IO_USERNAME_HERE",
          sizeof(dst.aio_user));
  strlcpy(dst.aio_key, src["io_key"] | "YOUR_IO_KEY_HERE", sizeof(dst.aio_key));
  strlcpy(dst.aio_url, src["io_url"] | "io.adafruit.com", sizeof(dst.aio_url));
  // Parse status pixel brightness from secrets
  dst.status_pixel_brightness = src["status_pixel_brightness"] | 0.2;
  // Parse MQTT port from secrets, if exists
  dst.io_port = src["io_port"] | 8883;
  JsonVariantConst lowPower = src["magtag_low_power"];
  dst.magtag_low_power.enabled = lowPower["enabled"] | false;
  dst.magtag_low_power.display_status_bar =
      lowPower["display_status_bar"] |
      (dst.magtag_low_power.enabled ? false : true);
  dst.magtag_low_power.sleep_interval_minutes =
      lowPower["sleep_interval_minutes"] | 180;
  dst.magtag_low_power.awake_window_minutes =
      lowPower["awake_window_minutes"] | 10;
  strlcpy(dst.magtag_low_power.sleep_message,
          lowPower["sleep_message"] | "zzz",
          sizeof(dst.magtag_low_power.sleep_message));
}
