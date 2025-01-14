## Local build configuration
## Parameters configured here will override default and ENV values.
## Uncomment and change examples:

## Add your source directories here separated by space
# MODULES = app
# EXTRA_INCDIR = include

## ESP_HOME sets the path where ESP tools and SDK are located.
## Windows:
#ESP_HOME = c:/Espressif

## MacOS / Linux:
#ESP_HOME = /opt/esp-open-sdk

## SMING_HOME sets the path where Sming framework is located.
## Windows:
#SMING_HOME = c:/tools/sming/Sming 

## MacOS / Linux
#SMING_HOME = /opt/Sming/Sming

## COM port parameter is reqruied to flash firmware correctly.
## Windows: 
# COM_PORT = COM3

## MacOS / Linux:
#COM_PORT = /dev/ttyUSB0

## Com port speed
# COM_SPEED	= 115200

COMPONENT_DEPENDS += Ota

# Arduino-style libraries.
ARDUINO_LIBRARIES := APA102 DHTesp BME280 MCP23008 HX711 SDCard

HWCONFIG += ota


## Configure flash parameters (for ESP12-E and other new boards):
#SPI_MODE = dio

#### overridable rBoot options ####
## use rboot build mode
RBOOT_ENABLED ?= 1
## enable big flash support (for multiple roms, each in separate 1mb block of flash)
RBOOT_BIG_FLASH ?= 1
## two rom mode (where two roms sit in the same 1mb block of flash)
RBOOT_TWO_ROMS  ?= 0
## size of the flash chip
#SPI_SIZE        ?= 4M

## SPIFFS options
# DISABLE_SPIFFS = 1
SPIFF_FILES = files

## size of the spiffs to create
#SPIFF_SIZE      ?= 65536

## flash offsets for spiffs, set if using two rom mode or not on a 4mb flash
## (spiffs location defaults to the mb after the rom slot on 4mb flash)
#RBOOT_SPIFFS_0  ?= 0x100000
#RBOOT_SPIFFS_1  ?= 0x300000

#ESPTOOL2 = $(ESP_HOME)/esptool2/esptool2

#LIBS := $(LIBS) stdc++ supc++ microc

# Firmware version
#include version
VERSION = 0.2
USER_CFLAGS = -DVERSION="\"$(VERSION)\""

# Environment options:
## WiFi
# WiFi SSID for the network to connect to and its password.
#WIFI_SSID = MyWiFiNetwork
WIFI_SSID = CIA7430QX
#WIFI_PWD = MyWiFiPassword
WIFI_PWD = 84697060122592298009

## MQTT
# MQTT host and port.
#MQTT_HOST = mqtt.host.net
#MQTT_HOST = orangepizero
#MQTT_HOST = servepi
MQTT_HOST = 192.168.31.105
# For SSL support, uncomment the following line or compile with this parameter.
#ENABLE_SSL=1
# MQTT SSL port (for example):
ifdef ENABLE_SSL
MQTT_PORT = 8883 
else
MQTT_PORT = 1883
endif

# Uncomment if password authentication is used.
# USE_MQTT_PASSWORD=1
# MQTT username & password (if needed):
# MQTT_USERNAME = esp8266
# MQTT_PWD = ESPassword

# MQTT topic prefix: added to all MQTT subscriptions and publications.
# Can be left empty, but must be defined.
# If not left empty, should end with a '/' to avoid merging with topic names.
MQTT_PREFIX = 

# MQTT URL. This uses the earlier defined MQTT details to compose a full URL.
# No editing required.
ifdef ENABLE_SSL
MQTT_URL := mqtts://
else
MQTT_URL := mqtt://
endif

ifdef USE_MQTT_PASSWORD
MQTT_URL := $(MQTT_URL)$(MQTT_USERNAME):$(MQTT_PWD)@
endif

MQTT_URL := $(MQTT_URL)$(MQTT_HOST):$(MQTT_PORT)

## OTA
# OTA (update) URL. Only change the host name (and port).
#OTA_URL = http://ota.host.net/ota.php?uid=
#OTA_URL = http://orangepizero/ota.php?uid=
#OTA_URL = http://servepi/ota.php?uid=
OTA_URL = http://192.168.31.105/ota.php?uid=

# Pass flags to compiler
USER_CFLAGS := $(USER_CFLAGS) -D__WIFI_SSID="\"$(WIFI_SSID)"\"
USER_CFLAGS := $(USER_CFLAGS) -D__WIFI_PWD="\"$(WIFI_PWD)"\"
USER_CFLAGS := $(USER_CFLAGS) -DMQTT_URL="\"$(MQTT_URL)"\"
USER_CFLAGS := $(USER_CFLAGS) -DMQTT_HOST="\"$(MQTT_HOST)"\"
# USER_CFLAGS := $(USER_CFLAGS) -DMQTT_PORT="$(MQTT_PORT)"
# USER_CFLAGS := $(USER_CFLAGS) -DMQTT_USERNAME="\"$(MQTT_USERNAME)"\"
USER_CFLAGS := $(USER_CFLAGS) -DOTA_URL="\"$(OTA_URL)"\"
USER_CFLAGS := $(USER_CFLAGS) -DMQTT_PWD="\"$(MQTT_PWD)"\"
ifdef USE_MQTT_PASSWORD
USER_CFLAGS := $(USER_CFLAGS) -DUSE_MQTT_PASSWORD="\"$(USE_MQTT_PASSWORD)"\"
endif
USER_CFLAGS := $(USER_CFLAGS) -DMQTT_PREFIX="\"$(MQTT_PREFIX)"\"
