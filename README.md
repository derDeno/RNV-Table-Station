# RNV-Table-Station

# RNV Table Station

This is a simple Esp32 Project with the Goal to simulate on the screen a stop Station of the RNV (Rhein-Neckar-Verkehr).
The user should use the webui to config which Station they want to monitor and the screen will simulate a Station screen showing the next stopping Train or bus at the Station with the minutes until Departure.


## Hardware
We are using this hardware:

- ESP32 C6
	- 4 MB FLash
- Touch LCD 
	- 1.47 inch
	- 172 x 320 Pixel
	- Support for LVGL GUI


## GUI

The on screen gui should basically look like this:


Linie              Abfahrt
__________________________

3 Sandhofen        2 min
1 Schönau          12 min


When the device is no Setup it should Display a QR code for the WiFi ap Setup.


The touch function will be used for swipe gestures, so user can define multiple Slots and stations and can swipe hem through.


## WebUI

This will be a simple ui for WiFi Setup, and Station Setup including the Slot assignment and selecting which Slot should be the Default one on boot.

We also want to support OTA.


## Tech
We want to use LittleFs for the filesystem. we also want to use a Dedicated data Folder with the webui in html form there.
OTA support too. The webui doesnt Need any authentification.


## API

RNV has an api from where we can load all data we Need.
They use opendata as a Plattform.

This are our test creds:

clientID=x
tenantID=x
clientSecret=x
resource=x
hostname=https://graphql-sandbox-dds.rnv-online.de

In the assets Folder are the api Schema and documentation.


Happy Building!
