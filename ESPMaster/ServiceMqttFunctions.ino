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

  SerialPrintln("Initialising MQTT");
  SerialPrintln("-- Client Id: " + mqttUniqueClientId);
  SerialPrintln("-- Broker: " + String(mqttServer) + ":" + String(mqttPort));
  SerialPrintln("-- Command Topic: " + mqttCommandTopic);
  SerialPrintln("-- State Topic: " + mqttStateTopic);

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
  document["countdownToDateUnix"] = atol(countdownToDateUnix.c_str());
  document["lastTimeReceivedMessageDateTime"] = lastReceivedMessageDateTime;
  document["version"] = espVersion;

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

  JsonDocument commandDocument;
  DeserializationError deserialisationError = deserializeJson(commandDocument, payloadString);

  if (deserialisationError) {
    SerialPrintln("Invalid MQTT command JSON received, ignoring");
    return;
  }

  applyMqttCommand(commandDocument);
}

//Applies a command JSON document received over MQTT, mirroring what the web UI form submission does.
//Supported fields (all optional): alignment, flapSpeed, deviceMode, inputText, countdownDateTimeUnix,
//scheduleEnabled, scheduledDateTimeUnix, scheduleShowIndefinitely
void applyMqttCommand(JsonDocument &commandDocument) {
  lastReceivedMessageDateTime = timezone.dateTime("d M y H:i:s");

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

      if (newDeviceModeValue == DEVICE_MODE_TEXT || newDeviceModeValue == DEVICE_MODE_CLOCK || newDeviceModeValue == DEVICE_MODE_DATE || newDeviceModeValue == DEVICE_MODE_COUNTDOWN) {
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

    if (deviceMode == DEVICE_MODE_TEXT && commandDocument["inputText"].is<const char*>()) {
      inputText = commandDocument["inputText"].as<String>();
    }
  }

  publishMqttState();
}

#endif
