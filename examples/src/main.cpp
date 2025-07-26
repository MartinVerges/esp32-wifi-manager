/**
 * ESP32 Wifi Manager
 * @file basic-usage.cpp
 * @brief Short minimal example to make use of the ESP32 Wifi Manager
 * @author Martin Verges <martin@verges.cc>
 * @copyright 2022-2025
 *
 * Licensed under CC BY-NC-SA 4.0
 * (Attribution-NonCommercial-ShareAlike 4.0 International)
**/

#include <Arduino.h>
#include "wifimanager.h"

// Create a instance of the WifiManager
WIFIMANAGER WifiManager;
AsyncWebServer webServer(80);

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 WiFi Manager Example ===");
  Serial.println("Starting WiFi Manager...");

  WifiManager.startBackgroundTask();        // Run the background task to take care of our Wifi
  WifiManager.fallbackToSoftAp(true);       // Run a SoftAP if no known AP can be reached
  WifiManager.attachWebServer(&webServer);  // Attach our API to the HTTP Webserver 
  WifiManager.attachUI();                   // Attach the UI to the Webserver
 
  // Run the Webserver and add your webpages to it
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String response = R"html(
<!DOCTYPE html>
<html><head><title>ESP32 WiFi Manager Demo</title></head>
<body style="font-family: Arial, sans-serif; margin: 40px;">
  <h1>ESP32 WiFi Manager Demo</h1>
  <p>Welcome to the ESP32 WiFi Manager example!</p>
  <ul>
    <li><a href="/wifi">WiFi Configuration Panel</a></li>
    <li><a href="/api/wifi/status">WiFi Status (JSON API)</a></li>
    <li><a href="/api/wifi/configlist">Saved Networks (JSON API)</a></li>
  </ul>
  <hr>
  <p><small>ESP32 WiFi Manager (c) 2022-2025 by Martin Verges</small></p>
</body></html>
    )html";
    request->send(200, "text/html", response);
  });

  // Start BOTH servers for HTTP and HTTPS captive portal compatibility
  webServer.begin();
  Serial.println("HTTP server started on port 80");
  // End Webserver
}

void loop() {
  // your special code to do some good stuff
  delay(500);

  // You can use a GPIO or other event to start a softAP, please replace false with a meaningful event
  if (false) {
    // While we are connected to some WIFI, we can start up a softAP
    // The AP will automatically shut down after some time if no client is connected
    WifiManager.startSoftAP();
  }
}
