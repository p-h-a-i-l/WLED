/*
 * MQTT communication protocol for home automation
 */

void parseMQTTBriPayload(char* payload)
{
  if      (strstr(payload, "ON") || strstr(payload, "on") || strstr(payload, "true")) {bri = briLast; colorUpdated(1);}
  else if (strstr(payload, "T" ) || strstr(payload, "t" )) {toggleOnOff(); colorUpdated(1);}
  else {
    uint8_t in = strtoul(payload, NULL, 10);
    if (in == 0 && bri > 0) briLast = bri;
    bri = in;
    colorUpdated(1);
  }
}


void onMqttConnect(bool sessionPresent)
{
  //(re)subscribe to required topics
  char subuf[38];
  strcpy(subuf, mqttDeviceTopic);

  if (mqttDeviceTopic[0] != 0)
  {
    strcpy(subuf, mqttDeviceTopic);
    mqtt->subscribe(subuf, 0);
    strcat(subuf, "/col");
    mqtt->subscribe(subuf, 0);
    strcpy(subuf, mqttDeviceTopic);
    strcat(subuf, "/api");
    mqtt->subscribe(subuf, 0);
  }

  if (mqttGroupTopic[0] != 0)
  {
    strcpy(subuf, mqttGroupTopic);
    mqtt->subscribe(subuf, 0);
    strcat(subuf, "/col");
    mqtt->subscribe(subuf, 0);
    strcpy(subuf, mqttGroupTopic);
    strcat(subuf, "/api");
    mqtt->subscribe(subuf, 0);
  }

  doSendHADiscovery = true;
  doPublishMqtt = true;
  DEBUG_PRINTLN("MQTT ready");
}


void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {

  DEBUG_PRINT("MQTT msg: ");
  DEBUG_PRINTLN(topic);
  DEBUG_PRINTLN(payload);

  //no need to check the topic because we only get topics we are subscribed to

  if (strstr(topic, "/col"))
  {
    colorFromDecOrHexString(col, (char*)payload);
    colorUpdated(1);
  } else if (strstr(topic, "/api"))
  {
    String apireq = "win&";
    apireq += (char*)payload;
    handleSet(nullptr, apireq);
  } else parseMQTTBriPayload(payload);
}


void publishMqtt()
{
  doPublishMqtt = false;
  if (mqtt == nullptr || !mqtt->connected()) return;
  DEBUG_PRINTLN("Publish MQTT");

  char s[10];
  char subuf[38];

  sprintf(s, "%ld", bri);
  strcpy(subuf, mqttDeviceTopic);
  strcat(subuf, "/g");
  mqtt->publish(subuf, 0, true, s);

  sprintf(s, "#%06X", col[3]*16777216 + col[0]*65536 + col[1]*256 + col[2]);
  strcpy(subuf, mqttDeviceTopic);
  strcat(subuf, "/c");
  mqtt->publish(subuf, 0, true, s);

  char apires[1024];
  XML_response(nullptr, false, apires);
  strcpy(subuf, mqttDeviceTopic);
  strcat(subuf, "/v");
  mqtt->publish(subuf, 0, true, apires);
}

const char HA_static_JSON[] PROGMEM = R"=====(,"bri_val_tpl":"{{value}}","rgb_cmd_tpl":"{{'#%02x%02x%02x' | format(red, green, blue)}}","rgb_val_tpl":"{{value[1:3]|int(base=16)}},{{value[3:5]|int(base=16)}},{{value[5:7]|int(base=16)}}","qos":0,"opt":true,"pl_on":"ON","pl_off":"OFF","fx_val_tpl":"{{value}}","fx_list":[)=====";

char* buffer;

void sendHADiscoveryMQTT()
{
//TODO: With LwIP 1 the ESP loses MQTT connection and causes memory leak when sending discovery packet
#if ARDUINO_ARCH_ESP32 || LWIP_VERSION_MAJOR > 1
/*

YYYY is device topic
XXXX is device name

Send out HA MQTT Discovery message on MQTT connect (~2.4kB):
{
"name": "XXXX",
"stat_t":"YYYY/c",
"cmd_t":"YYYY",
"rgb_stat_t":"YYYY/c",
"rgb_cmd_t":"YYYY/col",
"bri_cmd_t":"YYYY",
"bri_stat_t":"YYYY/g",
"bri_val_tpl":"{{value}}",
"rgb_cmd_tpl":"{{'#%02x%02x%02x' | format(red, green, blue)}}",
"rgb_val_tpl":"{{value[1:3]|int(base=16)}},{{value[3:5]|int(base=16)}},{{value[5:7]|int(base=16)}}",
"qos": 0,
"opt":true,
"pl_on": "ON",
"pl_off": "OFF",
"fx_cmd_t":"YYYY/api",
"fx_stat_t":"YYYY/api",
"fx_val_tpl":"{{value}}",
"fx_list":[
"[FX=00] Solid",
"[FX=01] Blink",
"[FX=02] ...",
"[FX=79] Ripple"
]
}

  */
  doSendHADiscovery = false;
  if (mqtt == nullptr || !mqtt->connected()) return;
  buffer = new char[2400];
  if (!buffer) {delete[] buffer; return;}
  
  char bufc[36], bufcol[38], bufg[36], bufapi[38];

  strcpy(bufc, mqttDeviceTopic);
  strcpy(bufcol, mqttDeviceTopic);
  strcpy(bufg, mqttDeviceTopic);
  strcpy(bufapi, mqttDeviceTopic);

  strcat(bufc, "/c");
  strcat(bufcol, "/col");
  strcat(bufg, "/g");
  strcat(bufapi, "/api");

  StaticJsonDocument<JSON_OBJECT_SIZE(9) +512> root;
  root["name"] = serverDescription;
  root["stat_t"] = bufc;
  root["cmd_t"] = mqttDeviceTopic;
  root["rgb_stat_t"] = bufc;
  root["rgb_cmd_t"] = bufcol;
  root["bri_cmd_t"] = mqttDeviceTopic;
  root["bri_stat_t"] = bufg;
  root["fx_cmd_t"] = bufapi;
  root["fx_stat_t"] = bufapi;

  size_t jlen = measureJson(root);
  //DEBUG_PRINTLN(jlen);
  serializeJson(root, buffer, jlen);

  //add values which don't change
  strcpy_P(buffer + jlen -1, HA_static_JSON);

  olen = 0;
  obuf = buffer + jlen -1 + strlen_P(HA_static_JSON);

  //add fx_list
  uint16_t jmnlen = strlen_P(JSON_mode_names);
  uint16_t nameStart = 0, nameEnd = 0;
  int i = 0;
  bool isNameStart = true;

  for (uint16_t j = 0; j < jmnlen; j++)
  {
    if (pgm_read_byte(JSON_mode_names + j) == '\"' || j == jmnlen -1)
    {
      if (isNameStart)
      {
        nameStart = j +1;
      }
      else
      {
        nameEnd = j;
        char mdnfx[64], mdn[56];
        uint16_t namelen = nameEnd - nameStart;
        strncpy_P(mdn, JSON_mode_names + nameStart, namelen);
        mdn[namelen] = 0;
        snprintf(mdnfx, 64, "\"[FX=%02d] %s\",", i, mdn);
        oappend(mdnfx);
        //DEBUG_PRINTLN(mdnfx);
        i++;
      }
      isNameStart = !isNameStart;
    }
  }
  olen--;
  oappend("]}");

  DEBUG_PRINT("HA Discovery Sending >>");
  DEBUG_PRINTLN(buffer);

  char pubt[25 + 12 + 8];
  strcpy(pubt, "homeassistant/light/");
  strcat(pubt, mqttClientID);
  strcat(pubt, "/config");
  bool success = mqtt->publish(pubt, 0, true, buffer);
  DEBUG_PRINTLN(success);
  yield();
  delete[] buffer;
#endif
}

bool initMqtt()
{
  lastMqttReconnectAttempt = millis();
  if (mqttServer[0] == 0 || !WLED_CONNECTED) return false;

  if (mqtt == nullptr) {
    mqtt = new AsyncMqttClient();
    mqtt->onMessage(onMqttMessage);
    mqtt->onConnect(onMqttConnect);
  }
  if (mqtt->connected()) return true;

  DEBUG_PRINTLN("Reconnecting MQTT");
  IPAddress mqttIP;
  if (mqttIP.fromString(mqttServer)) //see if server is IP or domain
  {
    mqtt->setServer(mqttIP, mqttPort);
  } else {
    mqtt->setServer(mqttServer, mqttPort);
  }
  mqtt->setClientId(mqttClientID);
  if (mqttUser[0] && mqttPass[0]) mqtt->setCredentials(mqttUser, mqttPass);
  mqtt->connect();
  return true;
}
