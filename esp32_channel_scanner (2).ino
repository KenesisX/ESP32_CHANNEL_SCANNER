/*
  esp32_channel_scanner.ino
  =========================

  Turns an ESP32 into a lightweight Wi-Fi channel occupancy scanner
  that feeds spectrum_analyzer.html directly over WebSocket — no
  laptop/bridge script needed for this mode.

  WHAT IT DOES
  ------------
  It cycles through the 14 2.4GHz Wi-Fi channels, listens in
  promiscuous mode on each one just long enough to average the
  signal energy present, and reports ONE aggregate RSSI number per
  channel. That's it — think of it as a "how busy is each channel"
  view, the same thing WiFi Analyzer / inSSIDer apps show for
  picking a quiet channel for your router.

  WHAT IT DELIBERATELY DOES NOT DO
  ---------------------------------
  It does not record, store, or transmit the MAC address, SSID, or
  any other identifying field from the packets it hears — only the
  channel number and an averaged power level. It is not a device
  tracker, a packet logger, or a network scanner in the reconnaissance
  sense. If you need those fields for troubleshooting your own
  network, standard tools (Wireshark, airodump-ng on hardware you
  control) are the right tool — this sketch intentionally does not
  do that.

  DATA FORMAT
  -----------
  Matches the same JSON protocol as sdr_bridge.py, so
  spectrum_analyzer.html needs no changes:
    { "bins": [14 numbers], "centerMHz": 2437, "spanMHz": 72, "mode": "WIFI-CH" }

  The 14 values map to channels 1–14 (2412–2484 MHz). The browser
  page already resamples whatever bin count it receives, so this
  just works with the existing "DATA SOURCE" box.

  SETUP
  -----
  1. Arduino IDE > Boards Manager > install "esp32" (Espressif).
  2. Library Manager > install "WebSockets" by Markus Sattler
     (Links2004/arduinoWebSockets).
  3. Edit WIFI_SSID / WIFI_PASSWORD below (needed only so the ESP32
     joins your network and gets an IP to serve the WebSocket from —
     it still scans all 14 channels regardless of which one your
     router is on, hopping off briefly to sample and back).
  4. Flash to the ESP32, open Serial Monitor at 115200 to see its
     IP address.
  5. In spectrum_analyzer.html, put ws://<that-ip>:81 in the DATA
     SOURCE box and hit CONNECT.
*/

#include <WiFi.h>
#include <WebSocketsServer.h>
#include "esp_wifi.h"

// ---- fill these in ----
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
// ------------------------

WebSocketsServer wsServer(81);

const int NUM_CHANNELS = 14;
const uint32_t DWELL_MS = 150;   // time spent listening per channel

volatile int32_t rssiSum[NUM_CHANNELS];
volatile int32_t rssiCount[NUM_CHANNELS];
float channelLevels[NUM_CHANNELS];

int currentChannel = 1;
uint32_t lastHopTime = 0;

// Promiscuous callback: reads ONLY the RSSI and channel of whatever
// packet triggered it. No MAC, no SSID, no payload is inspected or
// stored anywhere.
void IRAM_ATTR promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  int8_t rssi = pkt->rx_ctrl.rssi;
  int ch = currentChannel - 1; // 0-indexed
  if (ch >= 0 && ch < NUM_CHANNELS) {
    rssiSum[ch] += rssi;
    rssiCount[ch] += 1;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  for (int i = 0; i < NUM_CHANNELS; i++) {
    rssiSum[i] = 0;
    rssiCount[i] = 0;
    channelLevels[i] = -100.0;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.println("Point spectrum_analyzer.html at ws://" + WiFi.localIP().toString() + ":81");

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&promiscuousCallback);
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

  wsServer.begin();
  lastHopTime = millis();
}

void hopToNextChannel() {
  // fold this channel's accumulated samples into a smoothed level
  int ch = currentChannel - 1;
  if (rssiCount[ch] > 0) {
    float avg = (float)rssiSum[ch] / (float)rssiCount[ch];
    channelLevels[ch] = channelLevels[ch] * 0.5f + avg * 0.5f; // simple smoothing
  } else {
    // no traffic heard on this channel this dwell — decay toward noise floor
    channelLevels[ch] = channelLevels[ch] * 0.9f + (-98.0f) * 0.1f;
  }
  rssiSum[ch] = 0;
  rssiCount[ch] = 0;

  currentChannel++;
  if (currentChannel > NUM_CHANNELS) currentChannel = 1;
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

void broadcastLevels() {
  String json = "{\"bins\":[";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    json += String(channelLevels[i], 1);
    if (i < NUM_CHANNELS - 1) json += ",";
  }
  // channel 1 = 2412 MHz ... channel 14 = 2484 MHz -> center ~2437, span ~72
  json += "],\"centerMHz\":2437,\"spanMHz\":72,\"mode\":\"WIFI-CH\"}";
  wsServer.broadcastTXT(json);
}

void loop() {
  wsServer.loop();

  if (millis() - lastHopTime >= DWELL_MS) {
    hopToNextChannel();
    lastHopTime = millis();
    broadcastLevels();
  }
}
