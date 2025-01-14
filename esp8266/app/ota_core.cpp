/*
	ota_core.h - Header for the OtaCore class.
	
	Revision 3
	
	Features:
			- Defines the basic class needed for ESP8266 OTA functionality.
			- Allows for additional modules to be registered.
			
	Notes:
			- 
			
	2017/03/13, Maya Posch
	2017/11/15, Maya Posch
	2018/08/28, Maya Posch
	2022/08/25, Maya Posch - Added NyanSD service discovery.
*/


#include "ota_core.h"

#include "base_module.h"

#define SPI_SCLK 14
#define SPI_MOSI 13
#define SPI_MISO 12
#define SPI_CS 15


#include <esp_spi_flash.h>
#include <Network/Mqtt/MqttBuffer.h>
#include <Ota/Manager.h>

#include "nyansd_client.h"


// Static initialisations.
Timer OtaCore::procTimer;
RbootHttpUpdater* OtaCore::otaUpdater = 0;
MqttClient* OtaCore::mqtt = 0;
String OtaCore::MAC;
HashMap<String, topicCallback>* OtaCore::topicCallbacks = new HashMap<String, topicCallback>();
HardwareSerial OtaCore::Serial1(UART_ID_1); // UART 0 is 'Serial'.
String OtaCore::location;
String OtaCore::ota_url;
String OtaCore::mqtt_url;
String OtaCore::version = VERSION;
int OtaCore::sclPin = SCL_PIN; // default.
int OtaCore::sdaPin = SDA_PIN; // default.
bool OtaCore::i2c_active = false;
bool OtaCore::spi_active = false;
uint32 OtaCore::esp8266_pins = 0x0;

//const Url mqtt_url(MQTT_URL);


// Utility function to circumvent a bug in older versions of the 
// Sming FileSystem class for fileGetContent(). 
// It doesn't deal with null bytes very well, which is a problem for 
// binary files like our DER-formatted certificate and key.
String getFileContent(const String fileName) {
	file_t file = fileOpen(fileName.c_str(), File::ReadOnly);
	
	// Get size
	fileSeek(file, 0, SeekOrigin::End);
	int size = fileTell(file);
	if (size <= 0) 	{
		fileClose(file);
		return "";
	}
	
	fileSeek(file, 0, SeekOrigin::Start);
	char* buffer = new char[size + 1];
	buffer[size] = 0;
	fileRead(file, buffer, size);
	fileClose(file);
	String res(buffer, size);
	delete[] buffer;
	return res;
}


// Utility function to write strings to a file. Replaces the default
// fileSetContent() function in FileSystem.cpp since it does not support
// binary strings.
void setFileContent(const String &fileName, const String &content) {
	file_t file = fileOpen(fileName.c_str(), File::CreateNewAlways | File::WriteOnly);
	fileWrite(file, content.c_str(), content.length());
	fileClose(file);
}


bool readIntoFileBuffer(const String filename, char* &buffer, unsigned int &size) {
	file_t file = fileOpen(filename.c_str(), File::ReadOnly);
	
	// Get size
	fileSeek(file, 0, SeekOrigin::End);
	size = fileTell(file);
	if (size <= 0) 	{
		fileClose(file);
		return true;
	}
	
	fileSeek(file, 0, SeekOrigin::Start);
	buffer = new char[size + 1];
	buffer[size] = 0;
	fileRead(file, buffer, size);
	fileClose(file);
	return true;
}


Storage::Partition spiffsPartition;


Storage::Partition findSpiffsPartition(Storage::Partition partition) {
	String name = F("spiffs");
	name += OtaManager.getSlot(partition);
	Storage::Partition part = Storage::findPartition(name);
	if (!part) {
		debug_w("Partition '%s' not found", name.c_str());
	}
	
	return part;
}


// --- INIT ---
// Initialise the static class.
bool OtaCore::init(onInitCallback cb) {
	Serial.begin(9600);

	Serial1.begin(SERIAL_BAUD_RATE); // 115200 by default
	Serial1.systemDebugOutput(true); // Debug output to serial
	
	// Initialise the sub module system.
	BaseModule::init();
	
	//spiffs_mount(); // Mount file system, in order to work with files.
	
	// Mount the SpifFS manually. Automatic mounting is not
	// compatible with RBoot (yet):
	// https://github.com/SmingHub/Sming/issues/1009
	int slot = rboot_get_current_rom();
	/*u32_t offset;
	if (slot == 0) { offset = 0x100000; }
	else { offset = 0x300000; }
	spiffs_mount_manual(offset, 65536);*/
	// Find the n'th SPIFFS partition
	//Storage::Partition part = Storage::PartitionTable().find(Storage::Partition::SubType::Data::spiffs, slot);
	//Storage::Partition part = Storage::PartitionTable().find(Storage::Partition::SubType::Data::spiffs, slot);
	//Storage::Partition part = Storage::findDefaultPartition(Storage::Partition::SubType::Data::spiffs);
	/* Storage::Partition part = OtaManager.getRunningPartition();
	if(part) {
	   //debugf("trying to mount SPIFFS at %x, length %d", part.address(), part.size());
	   spiffs_mount(part);
	} else {
	   debug_e("SPIFFS partition missing for slot #%u", slot);
	} */
	
	// mount spiffs
	Storage::Partition partition = OtaManager.getRunningPartition();
	//spiffsPartition = findSpiffsPartition(partition);
	String name = F("spiffs");
	name += slot;
	spiffsPartition = Storage::findPartition(name);
	//if (spiffsPartition) {
		debugf("trying to mount %s @ 0x%08x, length %d", name.c_str(), spiffsPartition.address(),
			   spiffsPartition.size());
		spiffs_mount(spiffsPartition);
	//}
	
	// Print debug info.
	Serial1.printf("\r\nSDK: v%s\r\n", system_get_sdk_version());
    Serial1.printf("Free Heap: %d\r\n", system_get_free_heap_size());
    Serial1.printf("CPU Frequency: %d MHz\r\n", system_get_cpu_freq());
    Serial1.printf("System Chip ID: %x\r\n", system_get_chip_id());
    Serial1.printf("SPI Flash ID: %x\r\n", spi_flash_get_id());
	
	mqtt = new MqttClient();
	
	Serial1.printf("\r\nCurrently running rom %d.\r\n", slot);
	
	// Try to read the WiFi credentials from 'wifi_creds.txt' on SPIFFS.
	// TODO: If file doesn't exist, use stored values.
	String ssid;
	String pwd;
	if (fileExist("location.txt")) {
		String wifi = getFileContent("wifi_creds.txt");
		
		// WiFi SSID and password are separated by a question mark '?'.
		// <SSID>?<password>
		ssid = wifi.substring(0, wifi.indexOf('?'));
		pwd = wifi.substring(wifi.indexOf('?') + 1);
	}
	else {
		Serial1.printf("No wifi_creds.txt file found in SPIFFS.\r\n");
		return false;
	}

	//WifiStation.config(__WIFI_SSID, __WIFI_PWD);
	WifiStation.config(ssid, pwd);
	WifiStation.enable(true);
	WifiStation.connect();
	WifiAccessPoint.enable(false);

	// Run success callback or failure callback depending on connection result.
	WifiEvents.onStationGotIP(OtaCore::connectOk);
	WifiEvents.onStationDisconnect(OtaCore::connectFail);
	
	// Call init callback.
	(*cb)();
	
	return true;
}


// --- OTA UPDATE CALLBACK ---
void OtaCore::otaUpdate_CallBack(RbootHttpUpdater& update, bool result) {
	OtaCore::log(LOG_INFO, "In OTA callback...");
	if (result == true) { // success
		uint8 slot = rboot_get_current_rom();
		if (slot == 0) { slot = 1; } else { slot = 0; }
		
		// Set to boot new ROM and restart.
		Serial1.printf("Firmware updated, rebooting to ROM slot %d...\r\n", slot);
		OtaCore::log(LOG_INFO, "Firmware updated, restarting...");
		rboot_set_current_rom(slot);
		System.restart();
	} 
	else { // fail
		OtaCore::log(LOG_ERROR, "Firmware update failed.");
	}
}


// --- REGISTER TOPIC ---
// Registers an MQTT topic and an associated callback.
bool OtaCore::registerTopic(String topic, topicCallback cb) {
	OtaCore::mqtt->subscribe(topic);
	(*topicCallbacks)[topic] = cb;
	return true;
}


// --- DEREGISTER TOPIC ---
// Deregisters an MQTT topic.
bool OtaCore::deregisterTopic(String topic) {
	OtaCore::mqtt->unsubscribe(topic);
	if (topicCallbacks->contains(topic)) {
		topicCallbacks->remove(topic);
	}
	
	return true;
}


// --- PUBLISH ---
// Publishes the provided message to the MQTT broker on the specified topic.
bool OtaCore::publish(String topic, String message, int qos /* = 1 */) {
	OtaCore::mqtt->publish(topic, message, (mqtt_qos_t) qos);
	return true;
}


// --- OTA UPDATE ---
void OtaCore::otaUpdate() {
	//Serial1.printf("Updating firmware from URL: %s...", OTA_URL);
	OtaCore::log(LOG_INFO, "Updating firmware from URL: " + String(ota_url));
	
	// Needs a clean object, otherwise if run before & failed, 
	// it'll not run again.
	if (otaUpdater) { delete otaUpdater; }
	otaUpdater = new RbootHttpUpdater();
	
	// Select ROM slot to flash. This is always the other slot than which we
	// last booted from.
	rboot_config bootconf = rboot_get_config();
	uint8 slot = bootconf.current_rom;
	if (slot == 0) { slot = 1; } else { slot = 0; }

	// Flash new ROM to other ROM slot.
	// HTTP URL is hardcoded, providing our WiFi MAC as UID parameter.
	otaUpdater->addItem(bootconf.roms[slot], OTA_URL + MAC);
	
	// TODO: update SPIFFS as well?

	// Set a callback for the OTA procedure.
	otaUpdater->setCallback(OtaCore::otaUpdate_CallBack);
	otaUpdater->start(); // start update
}


// --- CHECK MQTT DISCONNECT ---
// Handle MQTT Disconnection events.
void OtaCore::checkMQTTDisconnect(TcpClient& client, bool flag) {	
	// Called whenever the MQTT connection has failed.
	if (flag == true) { Serial1.println("MQTT Broker disconnected."); }
	else { 
		String tHost = MQTT_HOST;
		Serial1.println("MQTT Broker " + tHost + " unreachable."); }
	
	// Restart connection attempt after 2 seconds.
	procTimer.initializeMs(2 * 1000, OtaCore::startMqttClient).start();
}


/* void onMessageDelivered(uint16_t msgId, int type) {
	Serial1.printf("Message with id %d and QoS %d was delivered successfully.", msgId, (type==MQTT_MSG_PUBREC? 2: 1));
} */


// --- START MQTT CLIENT ---
// Run MQTT client
void OtaCore::startMqttClient() {
	procTimer.stop();
	if (!mqtt->setWill("last/will","The connection from this device is lost:(", 
							MqttClient::getFlags(MQTT_QOS_AT_LEAST_ONCE, MQTT_RETAIN_TRUE))) {
		debugf("Unable to set mqtt will. Maybe not enough memory on the device.");
	}
	
	Serial1.println("MQTT broker: " + mqtt_url);
	
	mqtt->setMessageHandler(onMqttReceived);

	// Assign a disconnect callback function.
	mqtt->setCompleteDelegate(checkMQTTDisconnect);
	
#ifdef ENABLE_SSL
	//mqtt->connect(MAC, true, SSL_SERVER_VERIFY_LATER);
	mqtt->addSslOptions(SSL_SERVER_VERIFY_LATER);

	// DEBUG: list files on file system.
	/* Vector<String> filelist = fileList();    
    Serial1.println(" ");
    Serial1.println("Spiffs file system contents:");
    for (int i = 0; i < filelist.count(); i++ ) {
        Serial1.print(" ");
        Serial1.println(filelist.get(i) + " (" + String( fileGetSize(filelist.get(i))) + ")"     );
    } */
	
    Serial1.printf("Free Heap: %d\r\n", system_get_free_heap_size());
	
	// Read certificate and key in from SPIFFS.
	if (!fileExist("esp8266.client.crt.binary")) {
		Serial1.println("SSL CRT file is missing: esp8266.client.crt.binary.");
		return;
	}
	else if (!fileExist("esp8266.client.key.binary")) {
		Serial1.println("SSL key file is missing: esp8266.client.key.binary.");
		return;
	}
	
	unsigned int crtLength, keyLength;
	char* crtFile;
	char* keyFile;
	readIntoFileBuffer("esp8266.client.crt.binary", crtFile, crtLength);
	readIntoFileBuffer("esp8266.client.key.binary", keyFile, keyLength);
	
	Serial1.printf("keyLength: %d, crtLength: %d.\n", keyLength, crtLength);
    Serial1.printf("Free Heap: %d\r\n", system_get_free_heap_size());
	
	// If either file string is empty, opening the file failed.
	// TODO: handle.
	if (crtLength < 1 || keyLength < 1) {
		//
		Serial1.println("Failed to open certificate and/or key file.");
		return;
	}

	mqtt->setSslClientKeyCert((const uint8_t*) keyFile, keyLength,
							  (const uint8_t*) crtFile, crtLength, 0, true);
	delete[] keyFile;
	delete[] crtFile;
	
    Serial1.printf("Free Heap: %d\r\n", system_get_free_heap_size());
	
	//mqtt->connect(mqtt_url, MAC, true);
	
#elif defined USE_MQTT_PASSWORD
	//mqtt->connect(mqtt_url, MAC); // No SSL
#else
	//mqtt->connect(mqtt_url, MAC); // Anonymous login.
#endif

	// Connect.
	Url url(mqtt_url);
	mqtt->connect(url, MAC);
	
	// Subscribe to relevant topics.
	mqtt->subscribe(MQTT_PREFIX"upgrade");
	mqtt->subscribe(MQTT_PREFIX"presence/tell");
	mqtt->subscribe(MQTT_PREFIX"presence/ping");
	mqtt->subscribe(MQTT_PREFIX"presence/restart/#");
	mqtt->subscribe(MQTT_PREFIX"cc/" + MAC);
	
	// Subscribe to OTA URL topic to get the retained topic.
	mqtt->subscribe(MQTT_PREFIX"cc/ota_url");
	
	delay(100); // Wait for 100 ms to let subscriptions to be processed.
	
	// Announce presence to C&C.
	mqtt->publish(MQTT_PREFIX"cc/config", MAC);
}


void OtaCore::checkResponses() {
	procTimer.stop();
	
	if (!NyanSD_client::hasResponse()) {
		// Bail out.
		Serial.println(_F("Got no NyanSD responses for MQTT broker query."));
		return;
	}
	
	Serial.println(_F("Reading NyanSD responses for MQTT broker query..."));
	
	ServiceNode* responses = 0;
	uint32_t resnum = 0;
	uint32_t res = NyanSD_client::getResponses(responses, resnum);
	if (res != 0) {
		// Handle error.
		Serial.println(_F("Failed to get NyanSD responses: ") + String(res));
		return;
	}
	
	// Process received responses, we should have just a single MQTT server. Pick the first one
	// regardless.
	if (resnum == 0) {
		// No MQTT server found. Abort connecting.
		Serial.println(_F("Failed to find an MQTT server..."));
		return;
	}
	
	if (responses == 0) {
		Serial.println(_F("Responses is a null pointer."));
		return;
	}
	
	Serial.println(_F("Converting MQTT broker IPv4 address."));
	Serial.println(_F("IPv4 address: ") + String(responses->service->ipv4));
	
	String ipv4 = NyanSD_client::ipv4_uintToString(responses->service->ipv4);
	
	// Print IP.
	Serial.println(_F("Found MQTT server at: ") + ipv4);
	
	// Build MQTT URL.
	// Format: mqtt://<ipv4>:<port>
	mqtt_url = "mqtt://";
	mqtt_url += ipv4;
	mqtt_url += ":";
	mqtt_url.concat(responses->service->port);
	
	Serial.println(_F("MQTT URL: ") + mqtt_url);
	
	// Clean up NyanSD query resources.
	while (responses->next != 0) {
		ServiceNode* oldn = responses;
		responses = responses->next;
		delete[] oldn->service->ipv6;
		delete[] oldn->service->hostname;
		delete[] oldn->service->service;
		delete oldn->service;
		delete oldn;
	}
	
	delete responses;

	// Run MQTT client
	startMqttClient();
}


// --- CONNECT OK ---
// Will be called when the WiFi client has connected to the network.
void OtaCore::connectOk(IpAddress ip, IpAddress mask, IpAddress gateway) {
	Serial1.println("I'm CONNECTED. IP: " + ip.toString());
	
	// Get WiFi MAC address as this node's unique fingerprint.
	MAC = WifiStation.getMAC();
	Serial1.printf("MAC: %s.\n", MAC.c_str());
	
	// Set location from storage, if present.
	if (fileExist("location.txt")) {
		location = getFileContent("location.txt");
	}
	else {
		location = MAC;
	}
	
	
	// Run MQTT client.
	// It's important that the MQTT client is started before any modules, so
	// that any log output from those modules can be sent to the MQTT broker.
	//startMqttClient();
	
	// Use NyanSD to fetch the MQTT broker IP & port.
	uint16_t port = 11310; // TODO: use 1883 and 8883 instead?
	NYSD_query query;
	query.protocol = NYSD_PROTOCOL_ALL;
	char filter[] = "mqtt";
	query.filter = (char*) &filter;
	query.length = 4;
	uint8_t qnum = 1;
	uint32_t res = NyanSD_client::sendQuery(port, &query, qnum);
	if (res != 0) {
		// Handle error.
		Serial.println(_F("Failed to query for an MQTT server: ") + String(res));
		return;
	}
	
	procTimer.initializeMs(500, checkResponses).start();
	
	// Set configuration from storage, if present.
	if (fileExist("config.txt")) {
		String configStr = getFileContent("config.txt");
		uint32 config;
		configStr.getBytes((unsigned char*) &config, sizeof(uint32), 0);
		updateModules(config);
	}
}


// --- CONNECT FAIL ---
// Will be called when WiFi station timeout was reached
void OtaCore::connectFail(const String& ssid, MacAddress bssid, WifiDisconnectReason reason) {
	Serial1.println("I'm NOT CONNECTED. Need help :(");
	debugf("Disconnected from %s. Reason: %d", ssid.c_str(), reason);
	
	// Feed the watchdog timer to prevent a soft reset.
	WDT.alive();

	// Handle error.
	// Run success callback or failure callback depending on connection result.
	WifiEvents.onStationGotIP(OtaCore::connectOk);
	WifiEvents.onStationDisconnect(OtaCore::connectFail);
}


// --- ON MQTT RECEIVED ---
// Callback for MQTT message events.
int OtaCore::onMqttReceived(MqttClient& client, mqtt_message_t* payload) {
	String topic = MqttBuffer(payload->publish.topic_name);
	String message = MqttBuffer(payload->publish.content);
	Serial1.print(topic);
	Serial1.print(":\n"); // Prettify alignment for printing
	Serial1.println(message);
	
	log(LOG_DEBUG, topic + " - " + message);
	
	// If the topic is 'upgrade' with our MAC as message, start OTA update.
	if (topic == MQTT_PREFIX"upgrade" && message == MAC) {
		otaUpdate();
	}
	else if (topic == MQTT_PREFIX"presence/tell") {
		// Publish our MAC.
		mqtt->publish(MQTT_PREFIX"presence/response", MAC);
	}
	else if (topic == MQTT_PREFIX"presence/ping") {
		// Send a ping response containing our MAC.
		mqtt->publish(MQTT_PREFIX"presence/pong", MAC);
	}
	else if (topic == MQTT_PREFIX"presence/restart" && message == MAC) {
		System.restart();
	}
	else if (topic == MQTT_PREFIX"presence/restart/all") {
		System.restart();
	}
	else if (topic == MQTT_PREFIX"cc/ota_url") {
		ota_url = message;
	}
	else if (topic == MQTT_PREFIX"cc/" + MAC) {
		// We received a remote maintenance command. Execute it.
		// The message consists out of the following format:
		// <command string>;<payload string>
		int chAt = message.indexOf(';');
		String cmd = message.substring(0, chAt);
		++chAt;
		
		String msg(((char*) &message[chAt]), (message.length() - chAt));
		
		log(LOG_DEBUG, msg);
		
		Serial1.printf("Command: %s, Message: ", cmd.c_str());
		Serial1.println(msg);
		
		if (cmd == "mod") {
			// The second parameter should be the uint32 with bitflags.
			// These bit flags each match up with a module:
			// * 0x01: 	THPModule
			// * 0x02: 	CO2Module
			// * 0x04: 	JuraModule
			// * 0x08: 	JuraTermModule
			// * 0x10: 	MotionModule
			// * 0x20: 	PwmModule
			// * 0x40: 	IOModule
			// * 0x80: 	SwitchModule
			// * 0x100: PlantModule
			// ---
			// Of these, the CO2, Jura and JuraTerm modules are mutually
			// exclusive, since they all use the UART (Serial).
			// If two or more of these are still specified in the bitflags,
			// only the first module will be enabled and the others
			// ignored.
			//
			// The Switch module currently uses the same pins as the i2c bus,
			// as well as a number of the PWM pins (D5, 6).
			// This means that it excludes all modules but the MotionModule and
			// those using the UART.
			if (msg.length() != 4) { // Must be 32-bit integer.
				Serial1.printf("Payload size wasn't 4 bytes: %d\n", msg.length());
				return 1;
			}
			
			uint32 input;
			msg.getBytes((unsigned char*) &input, sizeof(uint32), 0);
			String byteStr;
			byteStr = "Received new configuration: ";
			byteStr += input;
			log(LOG_DEBUG, byteStr);
			updateModules(input);			
		}
		else if (cmd == "loc") {
			// Set the new location string if it's different.
			if (msg.length() < 1) { return 1; } // Incomplete message.
			if (location != msg) {
				location = msg;
				fileSetContent("location.txt", location); // Save to flash.
			}
		}
		else if (cmd == "mod_active") {
			// Return currently active modules.
			uint32 active_mods = BaseModule::activeMods();
			if (active_mods == 0) {
				mqtt->publish(MQTT_PREFIX"cc/response", MAC + ";0");
				return 1;
			}
			
			mqtt->publish(MQTT_PREFIX"cc/response", MAC + ";" + String((const char*) &active_mods, 4));
		}
		else if (cmd == "version") {
			// Return firmware version.
			mqtt->publish(MQTT_PREFIX"cc/response", MAC + ";" + version);
		}
		else if (cmd == "upgrade") {
			otaUpdate();
		}
	}
	else {
		if (topicCallbacks->contains(topic)) {
			(*((*topicCallbacks)[topic]))(message);
		}
	}
	
	return 0;
}


// --- UPDATE MODULES ---
void OtaCore::updateModules(uint32 input) {
	Serial1.printf("Input: %x, Active: %x.\n", input, BaseModule::activeMods());
	
	// Set the new configuration.
	BaseModule::newConfig(input);
	
	// Update the local copy of the configuration in storage if needed.
	if (BaseModule::activeMods() != input) {
		String content(((char*) &input), 4);
		setFileContent("config.txt", content);
	}
}


// --- MAP GPIO TO BIT ---
// Maps the given GPIO pin number to its position in the internal bitmask.
bool OtaCore::mapGpioToBit(int pin, ESP8266_pins &addr) {
	switch (pin) {
		case 0:
			addr = ESP8266_gpio00;
			break;
		case 1:
			addr = ESP8266_gpio01;
			break;
		case 2:
			addr = ESP8266_gpio02;
			break;
		case 3:
			addr = ESP8266_gpio03;
			break;
		case 4:
			addr = ESP8266_gpio04;
			break;
		case 5:
			addr = ESP8266_gpio05;
			break;
		case 9:
			addr = ESP8266_gpio09;
			break;
		case 10:
			addr = ESP8266_gpio10;
			break;
		case 12:
			addr = ESP8266_gpio12;
			break;
		case 13:
			addr = ESP8266_gpio13;
			break;
		case 14:
			addr = ESP8266_gpio14;
			break;
		case 15:
			addr = ESP8266_gpio15;
			break;
		case 16:
			addr = ESP8266_gpio16;
			break;
		default:
			log(LOG_ERROR, "Invalid pin number specified: " + String(pin));
			return false;
	};
	
	return true;
}


// --- LOG ---
void OtaCore::log(int level, String msg) {
	String out(level);
	out += " - " + msg;
	
	Serial1.println(out);
	mqtt->publish(MQTT_PREFIX"log/all", OtaCore::MAC + ";" + out);
}


// --- START I2C ---
// Start the i2c communication on the specified pins.
bool OtaCore::starti2c() {
	// Start i2c comms.
	if (i2c_active) { return true; }
	
	// Register the I2C pins.
	if (!claimPin(sdaPin)) { return false; }
	if (!claimPin(sclPin)) { return false; }
	
	// First perform a reset in case the slave device is stuck. For this we
	// pulse SCL 8 times.
	Wire.pins(sdaPin, sclPin);
	pinMode(sclPin, OUTPUT);
	for (int i = 0; i < 8; ++i) {
		digitalWrite(sclPin, HIGH);
		delayMicroseconds(3);
		digitalWrite(sclPin, LOW);
		delayMicroseconds(3);
	}
	  
	pinMode(sclPin, INPUT);
  
	// Next start the i2c bus.
	Wire.begin();
	i2c_active = true;
	
	return true;
}


// --- START SPI ---
// Start the SPI communication on the specified pins.
bool OtaCore::startSPI() {
	if (spi_active) { return true; }
	
	// Register the pins.
	if (!claimPin(SPI_SCLK)) { return false; }
	if (!claimPin(SPI_MOSI)) { return false; }
	if (!claimPin(SPI_MISO)) { return false; }
	if (!claimPin(SPI_CS)) { return false; }
	
	// Start the SPI bus.
	SPI.begin();
	spi_active = true;
	
	return true;
}


// --- CLAIM PIN ---
// Override for claimPin(ESP8266_pins pin).
// Converts the GPIO pin number to the position in the bit mask.
bool OtaCore::claimPin(int pin) {
	ESP8266_pins addr;
	if (!mapGpioToBit(pin, addr)) { return false; }
	
	return claimPin(addr);
}


// --- CLAIM PIN ---
// Reserves an output pin. Returns false if the pin is already in use.
bool OtaCore::claimPin(ESP8266_pins pin) {
	// Check the indicated pin. Return false if it's already set.
	// Otherwise set it and return true.
	if (esp8266_pins & pin) {
		log(LOG_ERROR, "Attempting to claim an already claimed pin: " + String(pin));
		log(LOG_DEBUG, String("Current claimed pins: ") + String(esp8266_pins));
		return false;
	}
	
	log(LOG_INFO, "Claiming pin position: " + String(pin));
	
	esp8266_pins |= pin;
	
	log(LOG_DEBUG, String("Claimed pin configuration: ") + String(esp8266_pins));
	
	return true;
}


// --- RELEASE PIN ---
bool OtaCore::releasePin(int pin) {
	ESP8266_pins addr;
	if (!mapGpioToBit(pin, addr)) { return false; }
	
	return releasePin(addr);
}


// --- RELEASE PIN ---
bool OtaCore::releasePin(ESP8266_pins pin) {
	if (!(esp8266_pins & pin)) {
		log(LOG_ERROR, "Attempting to release a pin which has not been set: " + String(pin));
		return false;
	}
		
	// Unset the indicated pin.
	esp8266_pins ^= pin;
	
	log(LOG_INFO, "Released pin position: " + String(pin));
	log(LOG_DEBUG, String("Claimed pin configuration: ") + String(esp8266_pins));
	
	return true;
}
