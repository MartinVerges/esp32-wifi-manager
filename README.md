# ESP32 Wifi Manager 

This wifi manager runs your ESP32 in a AP+STA mode.
This way you can easily start a softAP and scan for Wifi Networks while beeing conntected to some wifi.

## Why?

Because all the available solution that I found, where unable to fullfill my requirements.

## Focus of this Project

It is only made and tested on ESP32 Microcontrollers.

## How is it working?

1) First of all, if there is no configuration of a known SSID, it starts an AP.
   Within the AP you can use your own WebUI using RESTful APIs to configure the AP.

2) When a initial SSID is stored into the local database, the background task will try to connect to it.
   On success, the AP will timeout and close it self.
   You can close the AP if you like to speed that up.

3) It's possible to add multiple SSIDs to connect to.
   If there are more known SSIDs, the WifiManager will scan for SSIDs in range and pick the strongest Signal to connect to.

4) The background task monitors the Wifi status and reconnect if required.

## What do I need to do?

If you want to use this wifi manager, you have to create your own Web UI for it!
It won't give you a premade styled UI to configure the SSID credentials.

## APIs

| Method | Request URL          | Json Body                                    | Info                                                      |
| ------ | -------------------- | -------------------------------------------- | --------------------------------------------------------- |
| GET    | /api/wifi/configlist | none                                         | Get the configured SSID AP list                           |
| GET    | /api/wifi/scan       | none                                         | Scan for Networks in Range                                |
| GET    | /api/wifi/status     | none                                         | Show Status of the ESP32                                  |
| POST   | /api/wifi/add        | `{ "apName": "mySSID", "apPass": "secret" }` | Add a new SSID to the AP list                             |
| DELETE | /api/wifi/id         | `{ "id": 1 }`                                | Drop the AP list entry using the ID                       |
| DELETE | /api/wifi/apName     | `{ "apName": "mySSID" }`                     | Drop the AP list entries identified by the AP (SSID) Name |

## Dependencies

This Wifi manager depends on some external libraries to provide the functionality.
These are:

* Arduino 
* Preferences
* ESPAsyncWebServer
* ArduinoJson

# License

esp32-wifi-manager (c) by Martin Verges.

esp32-wifi-manager is licensed under a Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License.

You should have received a copy of the license along with this work.
If not, see <http://creativecommons.org/licenses/by-nc-sa/4.0/>.

## Commercial Licenses 

If you want to use this software on a comercial product, you can get an available commercial license on request.

