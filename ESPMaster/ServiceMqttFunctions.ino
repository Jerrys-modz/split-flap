#if MQTT_ENABLE == true

unsigned long previousMqttReconnectAttemptMillis = 0;
const unsigned long mqttReconnectIntervalMillis = 5000;

unsigned long previousMqttStatePublishMillis = 0;

//Sets up topics and makes the first attempt to connect to the configured MQTT broker
void initMqtt() {
  mqttUniqueClientId = String(mqttClientId) + "-" + String(ESP.getChipId(), HEX);
  mqttAvailabilityTopic = String(mqttTopicPrefix) + "/availability";
  mqttStateTopic = String(mqttTopicPrefix) + "/state";
  mqttCommandTopic = String(mqttTopicPrefix) + "/set";
  mqttHomeAssistantStatusTopic = String(mqttHomeAssistantDiscoveryPrefix) + "/status";

  SerialPrintln("Initialising MQTT");
  SerialPrintln("-- Client Id: " + mqttUniqueClientId);
  SerialPrintln("-- Broker: " + String(mqttServer) + ":" + String(mqttPort));
  SerialPrintln("-- Command Topic: " + mqttCommandTopic);
  SerialPrintln("-- State Topic: " + mqttStateTopic);
  SerialPrint("-- Home Assistant Discovery: ");
  SerialPrintln(mqttHomeAssistantDiscoveryEnabled ? "Enabled" : "Disabled");

  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);

  mqttReconnect();
}

//Keeps the MQTT connection alive, handles reconnecting and publishes a periodic state "heartbeat".
//Call this every loop() iteration
void mqttLoop() {
  if (!mqttClient.connected()) {
    unsigned long currentMillis = millis();

    if (currentMillis - previousMqttReconnectAttemptMillis >= mqttReconnectIntervalMillis) {
      previousMqttReconnectAttemptMillis = currentMillis;
      mqttReconnect();
    }

    return;
  }

  mqttClient.loop();

  unsigned long currentMillis = millis();
  if (currentMillis - previousMqttStatePublishMillis >= (unsigned long)mqttStatePublishIntervalSeconds * 1000) {
    previousMqttStatePublishMillis = currentMillis;
    publishMqttState();
  }
}

//Attempts a single connection to the MQTT broker, subscribing to the command topic on success
void mqttReconnect() {
  SerialPrintln("Attempting MQTT connection...");

  bool connected;
  if (strlen(mqttUsername) > 0) {
    connected = mqttClient.connect(mqttUniqueClientId.c_str(), mqttUsername, mqttPassword, mqttAvailabilityTopic.c_str(), 0, true, "offline");
  }
  else {
    connected = mqttClient.connect(mqttUniqueClientId.c_str(), mqttAvailabilityTopic.c_str(), 0, true, "offline");
  }

  if (connected) {
    SerialPrintln("MQTT Connected!");

    mqttClient.publish(mqttAvailabilityTopic.c_str(), "online", true);
    mqttClient.subscribe(mqttCommandTopic.c_str());

    if (mqttHomeAssistantDiscoveryEnabled) {
      //Also listen for Home Assistant's MQTT birth message, so we can re-publish discovery/state if it
      //(re)starts after us rather than waiting on it to notice us via our next reconnect
      mqttClient.subscribe(mqttHomeAssistantStatusTopic.c_str());
      publishHomeAssistantDiscovery();
    }

    publishMqttState();
  }
  else {
    SerialPrint("MQTT Connection failed, rc=");
    SerialPrintln(mqttClient.state());
  }
}

//Publishes the current device state as JSON to the state topic
void publishMqttState() {
  if (!mqttClient.connected()) {
    return;
  }

  JsonDocument document;
  document["deviceMode"] = deviceMode;
  document["alignment"] = alignment;
  document["flapSpeed"] = flapSpeed;
  document["inputText"] = lastWrittenText;
  document["mqttInputText"] = mqttInputText;
  document["countdownToDateUnix"] = atol(countdownToDateUnix.c_str());
  document["lastTimeReceivedMessageDateTime"] = lastReceivedMessageDateTime;
  document["version"] = espVersion;

  //Diagnostics
  document["wifiRssi"] = WiFi.RSSI();
  document["freeHeap"] = ESP.getFreeHeap();
  document["uptimeSeconds"] = millis() / 1000;

  //Reflects whatever was last observed during a real write, not a fresh I2C poll, so as to not add extra
  //bus traffic/contention just for a diagnostic read
  bool anyUnitMoving = false;
  for (int unitIndex = 0; unitIndex < UNITS_AMOUNT; unitIndex++) {
    if (displayState[unitIndex] != 0) {
      anyUnitMoving = true;
      break;
    }
  }
  document["displayMoving"] = anyUnitMoving;

  String jsonString;
  serializeJson(document, jsonString);

  SerialPrintln("Publishing MQTT State: " + jsonString);
  mqttClient.publish(mqttStateTopic.c_str(), jsonString.c_str(), true);
}

//Handles an incoming message on the command topic
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  SerialPrint("MQTT Message Received on Topic: ");
  SerialPrintln(topic);

  String payloadString;
  for (unsigned int payloadIndex = 0; payloadIndex < length; payloadIndex++) {
    payloadString += (char)payload[payloadIndex];
  }

  SerialPrintln("MQTT Payload: " + payloadString);

  //Home Assistant's birth message, not one of our own JSON commands - re-announce ourselves
  if (mqttHomeAssistantDiscoveryEnabled && String(topic) == mqttHomeAssistantStatusTopic) {
    if (payloadString == "online") {
      SerialPrintln("Home Assistant came online, re-publishing discovery configs and state");
      publishHomeAssistantDiscovery();
      publishMqttState();
    }

    return;
  }

  JsonDocument commandDocument;
  DeserializationError deserialisationError = deserializeJson(commandDocument, payloadString);

  if (deserialisationError) {
    SerialPrintln("Invalid MQTT command JSON received, ignoring");
    return;
  }

  applyMqttCommand(commandDocument);
}

//Applies a command JSON document received over MQTT, mirroring what the web UI form submission does.
//Supported fields (all optional): action, alignment, flapSpeed, deviceMode, inputText, countdownDateTimeUnix,
//scheduleEnabled, scheduledDateTimeUnix, scheduleShowIndefinitely
void applyMqttCommand(JsonDocument &commandDocument) {
  lastReceivedMessageDateTime = timezone.dateTime("d M y H:i:s");

  //One-shot action triggers (mirroring the web UI's Actions card), same as pressing a button - not
  //combined with any of the state fields below, so handle and bail out early
  if (commandDocument["action"].is<const char*>()) {
    String action = commandDocument["action"].as<String>();

    if (action == MQTT_ACTION_REBOOT) {
      SerialPrintln("MQTT Action: Reboot requested");
      isPendingReboot = true;
    }
    else if (action == MQTT_ACTION_RESET_UNITS) {
      SerialPrintln("MQTT Action: Reset Units requested");
      isPendingUnitsReset = true;
    }
#if WIFI_USE_DIRECT == false
    else if (action == MQTT_ACTION_RESET_WIFI) {
      SerialPrintln("MQTT Action: Reset WiFi requested");
      isPendingWifiReset = true;
    }
#endif
#if OTA_ENABLE == true
    else if (action == MQTT_ACTION_OTA) {
      SerialPrintln("MQTT Action: OTA Mode requested");
      enterOtaMode();
    }
#endif
    else {
      SerialPrintln("MQTT Action provided was not valid. Value: " + action);
    }

    publishMqttState();
    return;
  }

  if (commandDocument["alignment"].is<const char*>()) {
    String newAlignmentValue = commandDocument["alignment"].as<String>();

    if (newAlignmentValue == ALIGNMENT_MODE_LEFT || newAlignmentValue == ALIGNMENT_MODE_CENTER || newAlignmentValue == ALIGNMENT_MODE_RIGHT) {
      if (alignment != newAlignmentValue) {
        alignment = newAlignmentValue;
        alignmentUpdated = true;

        writeFile(LittleFS, alignmentPath, alignment.c_str());
        SerialPrintln("MQTT Alignment Updated: " + alignment);
      }
    }
    else {
      SerialPrintln("MQTT Alignment provided was not valid. Value: " + newAlignmentValue);
    }
  }

  if (!commandDocument["flapSpeed"].isNull()) {
    String newFlapSpeedValue = commandDocument["flapSpeed"].as<String>();

    if (flapSpeed != newFlapSpeedValue) {
      flapSpeed = newFlapSpeedValue;

      writeFile(LittleFS, flapSpeedPath, flapSpeed.c_str());
      SerialPrintln("MQTT Flap Speed Updated: " + flapSpeed);
    }
  }

  if (!commandDocument["countdownDateTimeUnix"].isNull()) {
    String newCountdownToDateUnixValue = commandDocument["countdownDateTimeUnix"].as<String>();

    if (countdownToDateUnix != newCountdownToDateUnixValue) {
      countdownToDateUnix = newCountdownToDateUnixValue;

      writeFile(LittleFS, countdownPath, countdownToDateUnix.c_str());
      SerialPrintln("MQTT Countdown Date Time Unix Updated: " + countdownToDateUnix);
    }
  }

  bool scheduleEnabled = commandDocument["scheduleEnabled"] | false;

  //Same as the web UI form: a scheduled message just gets queued up, device mode is left alone.
  //Otherwise, apply the device mode/text change directly
  if (scheduleEnabled) {
    String scheduledMessageText = commandDocument["inputText"] | "";
    long scheduledDateTimeUnix = commandDocument["scheduledDateTimeUnix"] | 0;
    bool showIndefinitely = commandDocument["scheduleShowIndefinitely"] | false;

    addAndPersistScheduledMessage(scheduledMessageText, scheduledDateTimeUnix, showIndefinitely);
    SerialPrintln("MQTT New Scheduled Message added");
  }
  else {
    if (commandDocument["deviceMode"].is<const char*>()) {
      String newDeviceModeValue = commandDocument["deviceMode"].as<String>();

      if (newDeviceModeValue == DEVICE_MODE_TEXT || newDeviceModeValue == DEVICE_MODE_CLOCK || newDeviceModeValue == DEVICE_MODE_DATE || newDeviceModeValue == DEVICE_MODE_COUNTDOWN || newDeviceModeValue == DEVICE_MODE_MQTT) {
        if (deviceMode != newDeviceModeValue) {
          deviceMode = newDeviceModeValue;

          writeFile(LittleFS, deviceModePath, deviceMode.c_str());
          SerialPrintln("MQTT Device Mode Set: " + deviceMode);
        }
      }
      else {
        SerialPrintln("MQTT Device Mode provided was not valid. Invalid Value: " + newDeviceModeValue);
      }
    }

    //Text mode text comes from the web UI form, MQTT mode text comes from here. Once in MQTT mode,
    //text can keep being pushed with just {"inputText": "..."} without having to re-specify deviceMode each time
    if (deviceMode == DEVICE_MODE_TEXT && commandDocument["inputText"].is<const char*>()) {
      inputText = commandDocument["inputText"].as<String>();
    }
    else if (deviceMode == DEVICE_MODE_MQTT && commandDocument["inputText"].is<const char*>()) {
      mqttInputText = commandDocument["inputText"].as<String>();
      SerialPrintln("MQTT Mode Text Updated: " + mqttInputText);
    }
  }

  publishMqttState();
}

//Builds the "<discoveryPrefix>/<component>/<nodeId>/<objectId>/config" topic for a Home Assistant
//MQTT Discovery entity
String buildHomeAssistantDiscoveryTopic(const char* component, const char* objectId) {
  return String(mqttHomeAssistantDiscoveryPrefix) + "/" + component + "/" + mqttUniqueClientId + "/" + objectId + "/config";
}

//Adds the device/availability fields shared by every Home Assistant discovery entity (so they all group
//under one device and share our existing availability topic), then serializes and publishes the finished
//config, retained, to its discovery topic
void publishHomeAssistantDiscoveryConfig(const char* component, const char* objectId, JsonDocument &document) {
  document["availability_topic"] = mqttAvailabilityTopic;
  document["payload_available"] = "online";
  document["payload_not_available"] = "offline";

  document["device"]["identifiers"][0] = mqttUniqueClientId;
  document["device"]["name"] = mqttClientId;
  document["device"]["manufacturer"] = "Split-Flap";
  document["device"]["model"] = "ESP8266 Split-Flap Display";
  document["device"]["sw_version"] = espVersion;

  String topic = buildHomeAssistantDiscoveryTopic(component, objectId);
  String payload;
  serializeJson(document, payload);

  mqttClient.publish(topic.c_str(), payload.c_str(), true);
}

//Publishes Home Assistant MQTT Discovery configs for our entities - Mode/Alignment selects, a Flap Speed
//number, a MQTT Text text box, and a Last Message sensor - all grouped under one Home Assistant device.
//Called on every (re)connect and whenever Home Assistant announces itself via its birth message
void publishHomeAssistantDiscovery() {
  SerialPrintln("Publishing Home Assistant MQTT Discovery configs");

  //Device mode select - lets Home Assistant switch between text/clock/date/countdown/mqtt
  {
    JsonDocument document;
    document["name"] = "Mode";
    document["unique_id"] = mqttUniqueClientId + "_mode";
    document["command_topic"] = mqttCommandTopic;
    document["command_template"] = "{\"deviceMode\": \"{{ value }}\"}";
    document["state_topic"] = mqttStateTopic;
    document["value_template"] = "{{ value_json.deviceMode }}";

    JsonArray options = document["options"].to<JsonArray>();
    options.add(DEVICE_MODE_TEXT);
    options.add(DEVICE_MODE_CLOCK);
    options.add(DEVICE_MODE_DATE);
    options.add(DEVICE_MODE_COUNTDOWN);
    options.add(DEVICE_MODE_MQTT);

    publishHomeAssistantDiscoveryConfig("select", "mode", document);
  }

  //Text alignment select
  {
    JsonDocument document;
    document["name"] = "Alignment";
    document["unique_id"] = mqttUniqueClientId + "_alignment";
    document["command_topic"] = mqttCommandTopic;
    document["command_template"] = "{\"alignment\": \"{{ value }}\"}";
    document["state_topic"] = mqttStateTopic;
    document["value_template"] = "{{ value_json.alignment }}";

    JsonArray options = document["options"].to<JsonArray>();
    options.add(ALIGNMENT_MODE_LEFT);
    options.add(ALIGNMENT_MODE_CENTER);
    options.add(ALIGNMENT_MODE_RIGHT);

    publishHomeAssistantDiscoveryConfig("select", "alignment", document);
  }

  //Flap speed number
  {
    JsonDocument document;
    document["name"] = "Flap Speed";
    document["unique_id"] = mqttUniqueClientId + "_flap_speed";
    document["command_topic"] = mqttCommandTopic;
    document["command_template"] = "{\"flapSpeed\": \"{{ value }}\"}";
    document["state_topic"] = mqttStateTopic;
    document["value_template"] = "{{ value_json.flapSpeed }}";
    document["min"] = 1;
    document["max"] = 100;
    document["unit_of_measurement"] = "%";

    publishHomeAssistantDiscoveryConfig("number", "flap_speed", document);
  }

  //MQTT mode text box - entering a value here switches the device into MQTT mode and shows it, matching
  //what a plain {"inputText": "..."} command does once already in MQTT mode
  {
    JsonDocument document;
    document["name"] = "MQTT Text";
    document["unique_id"] = mqttUniqueClientId + "_mqtt_text";
    document["command_topic"] = mqttCommandTopic;
    document["command_template"] = "{\"deviceMode\": \"mqtt\", \"inputText\": \"{{ value }}\"}";
    document["state_topic"] = mqttStateTopic;
    document["value_template"] = "{{ value_json.mqttInputText }}";
    document["max"] = 75;

    publishHomeAssistantDiscoveryConfig("text", "mqtt_text", document);
  }

  //Last message sensor - whatever is actually currently shown on the display, regardless of mode
  {
    JsonDocument document;
    document["name"] = "Last Message";
    document["unique_id"] = mqttUniqueClientId + "_last_message";
    document["state_topic"] = mqttStateTopic;
    document["value_template"] = "{{ value_json.inputText }}";
    document["icon"] = "mdi:card-text-outline";

    publishHomeAssistantDiscoveryConfig("sensor", "last_message", document);
  }

  //Countdown target date. The entered date is treated as UTC (Home Assistant's as_timestamp()/
  //timestamp_custom() default local-time behaviour is worked around below) - if that ends up a day off
  //against your timezone, adjust to taste
  {
    JsonDocument document;
    document["name"] = "Countdown Date";
    document["unique_id"] = mqttUniqueClientId + "_countdown_date";
    document["command_topic"] = mqttCommandTopic;
    document["command_template"] = "{\"countdownDateTimeUnix\": {{ as_timestamp(value) | int }}}";
    document["state_topic"] = mqttStateTopic;
    document["value_template"] = "{{ (value_json.countdownToDateUnix | int | timestamp_custom('%Y-%m-%d', False)) if (value_json.countdownToDateUnix | int) > 0 else None }}";

    publishHomeAssistantDiscoveryConfig("date", "countdown_date", document);
  }

  //WiFi signal strength
  {
    JsonDocument document;
    document["name"] = "WiFi Signal";
    document["unique_id"] = mqttUniqueClientId + "_wifi_rssi";
    document["state_topic"] = mqttStateTopic;
    document["value_template"] = "{{ value_json.wifiRssi }}";
    document["device_class"] = "signal_strength";
    document["state_class"] = "measurement";
    document["unit_of_measurement"] = "dBm";
    document["entity_category"] = "diagnostic";

    publishHomeAssistantDiscoveryConfig("sensor", "wifi_rssi", document);
  }

  //Free heap - handy for spotting a slow memory leak on a device that otherwise runs for months at a time
  {
    JsonDocument document;
    document["name"] = "Free Heap";
    document["unique_id"] = mqttUniqueClientId + "_free_heap";
    document["state_topic"] = mqttStateTopic;
    document["value_template"] = "{{ value_json.freeHeap }}";
    document["state_class"] = "measurement";
    document["unit_of_measurement"] = "B";
    document["entity_category"] = "diagnostic";
    document["icon"] = "mdi:memory";

    publishHomeAssistantDiscoveryConfig("sensor", "free_heap", document);
  }

  //Uptime - resets to 0 both on an actual reboot and, since it's derived from millis(), roughly every 49.7
  //days if the device runs that long without one
  {
    JsonDocument document;
    document["name"] = "Uptime";
    document["unique_id"] = mqttUniqueClientId + "_uptime";
    document["state_topic"] = mqttStateTopic;
    document["value_template"] = "{{ value_json.uptimeSeconds }}";
    document["device_class"] = "duration";
    document["unit_of_measurement"] = "s";
    document["entity_category"] = "diagnostic";

    publishHomeAssistantDiscoveryConfig("sensor", "uptime", document);
  }

  //Display moving - reflects the last observed state from a real write rather than polling fresh, see
  //publishMqttState()
  {
    JsonDocument document;
    document["name"] = "Display Moving";
    document["unique_id"] = mqttUniqueClientId + "_display_moving";
    document["state_topic"] = mqttStateTopic;
    document["value_template"] = "{{ 'ON' if value_json.displayMoving else 'OFF' }}";
    document["entity_category"] = "diagnostic";
    document["icon"] = "mdi:rotate-3d-variant";

    publishHomeAssistantDiscoveryConfig("binary_sensor", "display_moving", document);
  }

  //Action buttons, mirroring the web UI's "Actions" card
  {
    JsonDocument document;
    document["name"] = "Reboot";
    document["unique_id"] = mqttUniqueClientId + "_reboot";
    document["command_topic"] = mqttCommandTopic;
    document["payload_press"] = "{\"action\": \"reboot\"}";
    document["device_class"] = "restart";
    document["entity_category"] = "config";

    publishHomeAssistantDiscoveryConfig("button", "reboot", document);
  }

  {
    JsonDocument document;
    document["name"] = "Reset Unit Calibration";
    document["unique_id"] = mqttUniqueClientId + "_reset_units";
    document["command_topic"] = mqttCommandTopic;
    document["payload_press"] = "{\"action\": \"resetUnits\"}";
    document["entity_category"] = "config";
    document["icon"] = "mdi:restart-alert";

    publishHomeAssistantDiscoveryConfig("button", "reset_units", document);
  }

#if WIFI_USE_DIRECT == false
  {
    JsonDocument document;
    document["name"] = "Reset WiFi";
    document["unique_id"] = mqttUniqueClientId + "_reset_wifi";
    document["command_topic"] = mqttCommandTopic;
    document["payload_press"] = "{\"action\": \"resetWifi\"}";
    document["entity_category"] = "config";
    document["icon"] = "mdi:wifi-off";

    publishHomeAssistantDiscoveryConfig("button", "reset_wifi", document);
  }
#endif

#if OTA_ENABLE == true
  {
    JsonDocument document;
    document["name"] = "OTA Mode";
    document["unique_id"] = mqttUniqueClientId + "_ota";
    document["command_topic"] = mqttCommandTopic;
    document["payload_press"] = "{\"action\": \"ota\"}";
    document["entity_category"] = "config";
    document["icon"] = "mdi:upload";

    publishHomeAssistantDiscoveryConfig("button", "ota", document);
  }
#endif
}

#endif
