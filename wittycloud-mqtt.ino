/*
 * wittycloud-mqtt
 * (C) 2016 Christian Renz (MIT licensed)
 * https://github.com/crenz/wittycloud-mqtt
 */

/*
http://www.rapidtables.com/convert/color/hsv-to-rgb.htm
http://web.mit.edu/storborg/Public/hsvtorgb.c
*/

/**********************************************************************
 * Static configuration
 **********************************************************************/

const char *wifi_ssid = "---SSID---";
const char *wifi_pass = "---PASS---";

const char *mqtt_server = "---SERVER---";
const char *mqtt_id_template = "wittycloud-%02x%02x%02x%02x%02x%02x";
const char *mqtt_user = "";
const char *mqtt_pass = "";

const char *mqtt_topic_cmd_template = "home/sensors/wittycloud/%02x%02x%02x%02x%02x%02x/cmd";
const char *mqtt_topic_data_main_template = "home/sensors/wittycloud/%02x%02x%02x%02x%02x%02x/status";
const char *mqtt_topic_data_bme280_template = "home/sensors/wittycloud/%02x%02x%02x%02x%02x%02x/bme280";

const int sleepTimeS = 10;
const bool enableDeepSleep = false;

const int rotateRGBValues[] = {3, 7, 5, 7, 6, 7};
const int rotateRGBValuesLen = sizeof(rotateRGBValues) / sizeof(rotateRGBValues[0]);

#define ENABLE_BME_280_SUPPORT

#define SEALEVELPRESSURE_HPA (1013.25)



/**********************************************************************
 * Runtime configuration (can be updated through MQTT messages)
 **********************************************************************/

int sensorUpdateRateMS = 3000;
int rotateRateRGBMS = 1000;
bool rotateRGB = false;

/**********************************************************************
 **********************************************************************/

#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <MQTTClient.h>

#ifdef ENABLE_BME_280_SUPPORT
#include <Wire.h>
#include <SPI.h>
#include <BME280.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#endif

#define MIN(a, b) a > b ? b: a

/**********************************************************************
 * Definitions
 **********************************************************************/

#define PIN_LDR           A0
#define PIN_GPIO_16       16
#define PIN_GPIO_14       14
#define PIN_RGB_GREEN     12
#define PIN_RGB_BLUE      13
#define PIN_GPIO_5        5
#define PIN_BUTTON_USER   4
#define PIN_LED_BLUE      2
#define PIN_RGB_RED       15

#define PIN_SDA           14 //4
#define PIN_SCL           5


//ADC_MODE(ADC_VCC);


/**********************************************************************
 * Internal status
 **********************************************************************/

#ifdef ENABLE_BME_280_SUPPORT
Adafruit_BME280 bme; // I2C
bool bme280Initialized = false;
#endif

char sensor_buffer[512];

WiFiClient wifi;
MQTTClient mqtt;

unsigned long lastMillis = 0;

byte espMacAddress[6];

int rotateRGBIdx = 0;
int rotateRGBVal = 0;
unsigned long lastMillisRotate = 0;

bool prevButtonUser = false;

char mqtt_id[200];
char mqtt_topic_cmd[200];
char mqtt_topic_data_main[200];
char mqtt_topic_data_bme280[200];

/**********************************************************************
 * Helper functions: Logging
 **********************************************************************/

void logc(char * component) {
  Serial.print("[");
  Serial.print(component);
  Serial.print("] ");
}


void logm(char * message) {
  Serial.print(message);
}

void logmln(char * message) {
  Serial.println(message);
}

void logln(char * component, char * message) {
  logc(component);
  Serial.println(message);
}



/**********************************************************************
 * Helper functions: I/O
 **********************************************************************/

void io_button_user_setup() {
  pinMode(PIN_BUTTON_USER, INPUT);
}

int io_button_user() {
  return !digitalRead(PIN_BUTTON_USER);
}

void io_ldr_setup() {
  pinMode(PIN_LDR, INPUT);
}

int io_ldr() {
  return analogRead(PIN_LDR); 
}

void io_rgb_setup() {
  pinMode(PIN_RGB_RED, OUTPUT);
  pinMode(PIN_RGB_GREEN, OUTPUT);
  pinMode(PIN_RGB_BLUE, OUTPUT);
  io_rgb(false, false, false);
  logln("RGB", "OK");
}


void io_rgb(unsigned int red, unsigned int green, unsigned int blue) {
  analogWrite(PIN_RGB_RED, MIN(red, 1023));
  analogWrite(PIN_RGB_GREEN, MIN(green, 1023));
  analogWrite(PIN_RGB_BLUE, MIN(blue, 1023));
}

void io_rgb_rotate() {
  int brightness = 512;
  rotateRGBIdx = (rotateRGBIdx + 1) % rotateRGBValuesLen;
  rotateRGBVal = rotateRGBValues[rotateRGBIdx];
  io_rgb(brightness * (rotateRGBVal & 0x1), brightness * (rotateRGBVal & 0x2), brightness * (rotateRGBVal & 0x4));
}

void io_led_setup() {
  pinMode(PIN_LED_BLUE, OUTPUT);
  io_led(false);
  logln("LED", "OK");
}

void io_led(bool on) {
  digitalWrite(PIN_LED_BLUE, !on);
}

/* Helper functions: BME280 */

void io_bme280_setup() {
#ifdef ENABLE_BME_280_SUPPORT
  logc("BME280");
  pinMode(PIN_GPIO_16, OUTPUT);
  digitalWrite(PIN_GPIO_16, true);
  delay(100);
  Wire.begin(PIN_SDA, PIN_SCL);
  if (!bme.begin()) {
    Serial.println("Could not find a valid BME280 sensor, check wiring!");
    //while (1);
  } else {
    logmln("OK");
    bme280Initialized = true;
  }
#endif
}

/* Example payload:
 *  {"rgb": [1, 0, 1], "sensorUpdateRate": 2000, "rotateRGB": true, "rotateRateRGB": 100}
 */
void sensor_read() {
  StaticJsonBuffer<512> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  
//  root["supply_voltage"] = ESP.getVcc() / 1024.0f;
  root["ldr"] = io_ldr(); 
  root["button_user"] = io_button_user();
  root["rotateRGBVal"] = rotateRGBVal;

  Serial.print("[Sensors] ");
  root.prettyPrintTo(Serial);
  Serial.println();

  root.printTo(sensor_buffer, sizeof(sensor_buffer));
}

void bme280_read_publish() {
  StaticJsonBuffer<512> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  
#ifdef ENABLE_BME_280_SUPPORT
  if (bme280Initialized) {
    root["temp"] = bme.readTemperature();
    root["pressure"] = double_with_n_digits(bme.readPressure() / 100.0f, 6);
//    root["altitude"] = bme.readAltitude(SEALEVELPRESSURE_HPA);
    root["humidity"] = bme.readHumidity();

    root.printTo(sensor_buffer, sizeof(sensor_buffer));

    Serial.printf("[BME280] %s\n", sensor_buffer);
    bool result = mqtt.publish(mqtt_topic_data_bme280, sensor_buffer);
    Serial.printf("[BME280] MQTT publish result=%i\n", result);
  }

#endif
}

/**********************************************************************
 * Helper functions: Network
 **********************************************************************/

void wifi_setup() {
  logc("WiFi");

  WiFi.macAddress(espMacAddress);

  Serial.printf(" MAC address %02x:%02x:%02x:%02x:%02x:%02x ",
    espMacAddress[0],
    espMacAddress[1],
    espMacAddress[2],
    espMacAddress[3],
    espMacAddress[4],
    espMacAddress[5]);

  sprintf(mqtt_topic_cmd, mqtt_topic_cmd_template,
    espMacAddress[0],
    espMacAddress[1],
    espMacAddress[2],
    espMacAddress[3],
    espMacAddress[4],
    espMacAddress[5]);
  
  sprintf(mqtt_topic_data_main, mqtt_topic_data_main_template,
    espMacAddress[0],
    espMacAddress[1],
    espMacAddress[2],
    espMacAddress[3],
    espMacAddress[4],
    espMacAddress[5]);

  sprintf(mqtt_topic_data_bme280, mqtt_topic_data_bme280_template,
    espMacAddress[0],
    espMacAddress[1],
    espMacAddress[2],
    espMacAddress[3],
    espMacAddress[4],
    espMacAddress[5]);

  sprintf(mqtt_id, mqtt_id_template,
    espMacAddress[0],
    espMacAddress[1],
    espMacAddress[2],
    espMacAddress[3],
    espMacAddress[4],
    espMacAddress[5]);
  
  WiFi.begin(wifi_ssid, wifi_pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    io_rgb_rotate();
  }
  logm("Connected, IP address: ");
  Serial.println(WiFi.localIP());
}

/* Helper functions: MQTT */

void mqtt_setup() {
  logc("MQTT");
  mqtt.begin(mqtt_server, wifi);
  while (!mqtt.connect(mqtt_id, mqtt_user, mqtt_pass)) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("Connected");
  mqtt.subscribe(mqtt_topic_cmd);
}

void mqtt_loop() {
  mqtt.loop();
  delay(10);
  if (WiFi.status() != WL_CONNECTED) {
    wifi_setup();
  }
  if(!mqtt.connected()) {
    mqtt_setup();
  }
}

/*
 * Example payload:
 * 
 */

void messageReceived(String topic, String payload, char * bytes, unsigned int length) {
  logc("MQTT");
  Serial.print(" raw message: [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(payload);

  StaticJsonBuffer<200> jsonBuffer;  
  JsonObject& root = jsonBuffer.parseObject(payload);
  if (!root.success()) {
    Serial.println("parseObject() failed");
    return;
  }

  logc("MQTT");

  if (root.containsKey("rgb")) {
    unsigned int red = root["rgb"][0];
    unsigned int green = root["rgb"][1];
    unsigned int blue = root["rgb"][2];

    Serial.printf(" RGB: (%i,%i,%i)", red, green, blue);
    io_rgb(red, green, blue);
  }

  if (root.containsKey("led")) {
    bool led = root["led"];

    Serial.printf(" LED: %d", led);
    io_led(led);
  }

  if (root.containsKey("sensorUpdateRate")) {
    int rate = root["sensorUpdateRate"];

    Serial.printf(" sensorUpdateRate: %i", rate);
    sensorUpdateRateMS = rate;
  }

  if (root.containsKey("rotateRateRGB")) {
    int rate = root["rotateRateRGB"];

    Serial.printf(" rotateRateRGB: %i", rate);
    rotateRateRGBMS = rate;
  }

  if (root.containsKey("rotateRGB")) {
    int rotate = root["rotateRGB"];

    Serial.printf(" rotateRGB: %d", rotate);
    rotateRGB = rotate;
  }

  
  Serial.println();
}

/**********************************************************************
 * main
 **********************************************************************/

void setup() {
  Serial.begin(115200);

  Serial.println();
  Serial.println();
  Serial.println("--- wittycloud-mqtt starting --- ");

  int startMillis = millis();

  Serial.print("Reset reason: ");
  Serial.println(ESP.getResetReason());

  io_rgb_setup();
  io_ldr_setup();
  io_led_setup();
  io_button_user_setup();

  io_led(true);  
  io_rgb(true, false, false);
  io_bme280_setup();
  io_rgb(false, true, false);
  wifi_setup();
  io_rgb(false, false, true);
  mqtt_setup();
  
  io_led(false);
  io_rgb(false, false, false);

  Serial.printf("Start-up finished in %i ms\n", millis() - startMillis);
  Serial.println("-------------------------------- \n");
}


void loop() {
  // put your main code here, to run repeatedly:
  mqtt_loop();

  if (rotateRGB) {
    if(millis() - lastMillisRotate > rotateRateRGBMS) {
      lastMillisRotate = millis();
      io_rgb_rotate();
    }
  }

  bool bu = io_button_user();
  if (prevButtonUser != bu) {
    logc("Button");
    Serial.printf("User button = %d\n", bu);
    prevButtonUser = bu;
    sensor_read();
    mqtt.publish(mqtt_topic_data_main, sensor_buffer);
  }

  if (millis() - lastMillis > sensorUpdateRateMS) {
    io_led(true);
    lastMillis = millis();
    sensor_read();
    mqtt.publish(mqtt_topic_data_main, sensor_buffer);
    bme280_read_publish();
    io_led(false);
  }

  if (enableDeepSleep) {
      Serial.println("---- Starting deep sleep ----");
      ESP.deepSleep(sleepTimeS * 1000000, WAKE_RF_DEFAULT);
      delay(100);
  }
}

