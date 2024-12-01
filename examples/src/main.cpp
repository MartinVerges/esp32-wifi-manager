/**
 * ESP32 Wifi Manager
 * @file basic-usage.cpp
 * @brief Short minimal example to make use of the ESP32 Wifi Manager
 * @author Martin Verges <martin@verges.cc>
 * @copyright 2022
 *
 * Licensed under CC BY-NC-SA 4.0
 * (Attribution-NonCommercial-ShareAlike 4.0 International)
**/

#include <Arduino.h>
#include "wifimanager.h"

// Create a instance of the WifiManager
WIFIMANAGER WifiManager;

// We do need the Webserver to attach our RESTful API
AsyncWebServer webServer(80);

void setup() {
  Serial.begin(115200);

  WifiManager.startBackgroundTask();        // Run the background task to take care of our Wifi
  WifiManager.fallbackToSoftAp(true);       // Run a SoftAP if no known AP can be reached
  WifiManager.attachWebServer(&webServer);  // Attach our API to the Webserver 
 
  // Run the Webserver and add your webpages to it
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "Hello World");
  });
  webServer.onNotFound([&](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });
  webServer.begin();
  // End Webserver
}

void loop() {
  // your special code to do some good stuff
  delay(500);

  // You can use a GPIO or other event to start a softAP, please replace false with a meaningful event
  if (false) {
    // While we are connected to some WIFI, we can start up a softAP
    // The AP will automatically shut down after some time if no client is connected
    WifiManager.runSoftAP();
  }
}
