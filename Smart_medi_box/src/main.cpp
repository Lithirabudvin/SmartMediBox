#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <vector>

// OLED Display Configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// WiFi Configuration
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// MQTT Configuration
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

// Topics
#define LDR_TOPIC "220077L/medibox/ldr"
#define TEMP_TOPIC "220077L/medibox/temperature"
#define CONFIG_TOPIC "220077L/medibox/config"
#define SERVO_TOPIC "220077L/medibox/servo_angle"
#define DEBUG_TOPIC "220077L/medibox/debug"

// Hardware Configuration
#define LDR_PIN 34
#define DHT_PIN 4
#define SERVO_PIN 5
#define DHT_TYPE DHT22
#define BUZZER 18
#define LED_1 15
#define LED_2 2
#define MENU_BUTTON 13 
#define CANCEL 14
#define UP 35
#define DOWN 32
#define OK 33

// Time Configuration
#define NTP_SERVER "pool.ntp.org"
#define UTC_OFFSET 5.5 * 3600  // 5.5 hours for IST (Colombo time)
#define UTC_OFFSET_DST 0

// Healthy thresholds
#define TEMP_LOW -20
#define TEMP_HIGH 100
#define HUMID_LOW 0
#define HUMID_HIGH 120


// Default Parameters
#define DEFAULT_SAMPLE_INTERVAL 1000
#define DEFAULT_SEND_INTERVAL 5000
#define DEFAULT_THETA_OFFSET 30
#define DEFAULT_CONTROL_FACTOR 0.75
#define DEFAULT_T_MED 30


// Alarm structure
struct Alarm {
  int hours;
  int minutes;
  bool enabled;
  bool triggered;
};

// Global Objects
WiFiClient espClient;
PubSubClient client(espClient);
DHT dht(DHT_PIN, DHT_TYPE);
Servo servo;
std::vector<Alarm> alarms;

// Configuration Parameters
unsigned long sampleInterval = DEFAULT_SAMPLE_INTERVAL;
unsigned long sendInterval = DEFAULT_SEND_INTERVAL;
float thetaOffset = DEFAULT_THETA_OFFSET;
float controlFactor = DEFAULT_CONTROL_FACTOR;
float tMed = DEFAULT_T_MED;

// Data Storage
const int MAX_SAMPLES = 50;
float ldrSamples[MAX_SAMPLES];
int sampleCount = 0;
unsigned long lastSampleTime = 0;
unsigned long lastSendTime = 0;

// Time variables
struct tm timeinfo;
unsigned long lastAlarmCheck = 0;
bool snoozing = false;
unsigned long snoozeEndTime = 0;

// Menu System
int current_mode = 0;
const String menu_options[] = {
  "Set Time Zone",
  "Set Alarm",
  "View Alarms",
  "Delete Alarm",
  "Disable All"
};
const int MAX_MODES = sizeof(menu_options)/sizeof(menu_options[0]);
int Atone[]{100,200,300,400,300,200,100};

void debugPrint(String message) {
  Serial.println("[DEBUG] " + message);
  client.publish(DEBUG_TOPIC, message.c_str());
}

void print_line(String text, int text_size, int row, int column) {
  display.setTextSize(text_size);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(column, row);
  display.print(text);
}

void printSystemStatus() {
  display.clearDisplay();
  print_line("SYSTEM STATUS", 1, 0, 0);
  print_line("WiFi: " + String(WiFi.status() == WL_CONNECTED ? "OK" : "OFF"), 1, 10, 0);
  print_line("MQTT: " + String(client.connected() ? "OK" : "OFF"), 1, 20, 0);
  print_line("Sample: " + String(sampleInterval/1000) + "s", 1, 30, 0);
  print_line("Send: " + String(sendInterval/1000) + "s", 1, 40, 0);
  display.display();
  delay(2000);
}

void setup_wifi() {
  display.clearDisplay();
  print_line("Connecting WiFi", 1, 0, 0);
  display.display();
  
  WiFi.begin(ssid, password);
  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    debugPrint("WiFi connected");
    debugPrint("IP: " + WiFi.localIP().toString());
    
    // Configure time
    configTime(UTC_OFFSET, UTC_OFFSET_DST, NTP_SERVER);
  } else {
    debugPrint("WiFi connection failed!");
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  debugPrint("MQTT Msg: " + String(topic) + " - " + message);

  if (String(topic) == CONFIG_TOPIC) {
    int comma1 = message.indexOf(',');
    int comma2 = message.indexOf(',', comma1 + 1);
    int comma3 = message.indexOf(',', comma2 + 1);

    if (comma1 != -1 && comma2 != -1 && comma3 != -1) {
      sampleInterval = message.substring(0, comma1).toInt() * 1000UL;
      sendInterval = message.substring(comma1 + 1, comma2).toInt() * 1000UL;
      thetaOffset = message.substring(comma2 + 1, comma3).toFloat();
      controlFactor = message.substring(comma3 + 1, message.indexOf(',', comma3 + 1)).toFloat();
      tMed = message.substring(message.lastIndexOf(',') + 1).toFloat();
      
      debugPrint("New Config:");
      debugPrint("Sample: " + String(sampleInterval/1000) + "s");
      debugPrint("Send: " + String(sendInterval/1000) + "s");
      debugPrint("θ_offset: " + String(thetaOffset) + "°");
      debugPrint("γ: " + String(controlFactor));
      debugPrint("T_med: " + String(tMed) + "°C");
      
      display.clearDisplay();
      print_line("Config Updated", 1, 0, 0);
      display.display();
      delay(1000);
    }
  }
}

bool connectToBroker() {
  String clientId = "ESP32Client-" + String(random(0xffff), HEX);
  if (client.connect(clientId.c_str())) {
    client.subscribe(CONFIG_TOPIC);
    return true;
  }
  return false;
}

void reconnect() {
  static unsigned long lastAttempt = 0;
  if (millis() - lastAttempt < 5000) return;
  lastAttempt = millis();

  if (!connectToBroker()) {
    debugPrint("MQTT failed: " + String(client.state()));
  }
}

float readLDR() {
  int rawValue = analogRead(LDR_PIN);
  float normalized = (float)rawValue / 4000.0;
  normalized = constrain(normalized, 0.0, 1.0);

  Serial.print("LDR - Raw: ");
  Serial.print(rawValue);
  Serial.print(" Norm: ");
  Serial.println(normalized);
  
  return normalized;
}

float readTemperature() {
  float temp = dht.readTemperature();
  if (isnan(temp)) {
    debugPrint("DHT Read Failed!");
    return NAN;
  }
  
  float humid = dht.readHumidity();
  if(!isnan(humid)) {
    if(temp < TEMP_LOW || temp > TEMP_HIGH || humid < HUMID_LOW || humid > HUMID_HIGH) {
      digitalWrite(LED_2, HIGH);
      tone(BUZZER, 1000);
    } else {
      digitalWrite(LED_2, LOW);
      noTone(BUZZER);
    }
  }
  
  return temp;
}

float calculateServoAngle(float lightIntensity, float temperature) {
  if (isnan(temperature)) return thetaOffset;

  float ts = sampleInterval / 1000.0;
  float tu = sendInterval / 1000.0;
  float ratio = ts / tu;

  float angle = thetaOffset + (180 - thetaOffset) * lightIntensity * controlFactor *
               log(ratio) * (temperature / tMed);
  angle = constrain(angle, thetaOffset, 180);
  
  debugPrint("Servo Calc: Light=" + String(lightIntensity) +
             " Temp=" + String(temperature) +
             " → Angle=" + String(angle));
  return angle;
}

void publishData(float intensity, float temp, float angle) {
  bool pub1 = client.publish(LDR_TOPIC, String(intensity).c_str());
  bool pub2 = client.publish(TEMP_TOPIC, String(temp).c_str());
  bool pub3 = client.publish(SERVO_TOPIC, String(angle).c_str());

  debugPrint("Published: " +
             String(pub1 ? "LDR " : "LDR_FAIL ") + String(intensity) + " | " +
             String(pub2 ? "TEMP " : "TEMP_FAIL ") + String(temp) + " | " +
             String(pub3 ? "ANGLE " : "ANGLE_FAIL ") + String(angle));
}

void update_display() {
  if(!getLocalTime(&timeinfo)) {
    Serial.println("Failed to get time");
    return;
  }

  display.clearDisplay();
  
  // Display time
  char timeStr[20];
  sprintf(timeStr, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  print_line(timeStr, 2, 0, 0);

  // Display date
  char dateStr[20];
  sprintf(dateStr, "%02d/%02d/%04d", timeinfo.tm_mday, timeinfo.tm_mon+1, timeinfo.tm_year+1900);
  print_line(dateStr, 1, 20, 0);

  // Display temp/humidity
  float temp = dht.readTemperature();
  float humid = dht.readHumidity();

  if(!isnan(temp) && !isnan(humid)) {
    print_line("T:" + String(temp,1) + "C H:" + String(humid,1) + "%", 1, 35, 0);
  } else {
    print_line("Sensor Error", 1, 35, 0);
  }

  display.display();
}

void ring_alarm(); // Function prototype
void stop_alarm(); // Function prototype
void snooze_alarm(); // Function prototype

void stop_alarm() {
  noTone(BUZZER);
  digitalWrite(LED_1, LOW);
  digitalWrite(LED_2, LOW);
}

void check_alarms() {
  if(snoozing) {
    if(millis() >= snoozeEndTime) {
      snoozing = false;
    } else {
      return;
    }
  }
  
  for(auto &alarm : alarms) {
    if(alarm.enabled && !alarm.triggered && 
       timeinfo.tm_hour == alarm.hours && 
       timeinfo.tm_min == alarm.minutes) {
      ring_alarm();
      alarm.triggered = true;
    }
  }
}

void ring_alarm() {
  display.clearDisplay();
  print_line("MEDICINE TIME!", 2, 0, 0);
  display.display();
  
  digitalWrite(LED_1, HIGH);
  digitalWrite(LED_2, HIGH);
  
  while(digitalRead(CANCEL) == HIGH && digitalRead(OK) == HIGH) {
    for (int r = 0; r<7; r++ ){
      tone(BUZZER, Atone[r]);
      delay(500);
    }
    noTone(BUZZER);
    delay(500);
    
    if(digitalRead(CANCEL) == LOW) {
      stop_alarm();
      break;
    }
    if(digitalRead(OK) == LOW) {
      snooze_alarm();
      break;
    }
  }
}

void snooze_alarm() {
  snoozing = true;
  snoozeEndTime = millis() + 300000; // 5 minutes
  stop_alarm();
  display.clearDisplay();
  print_line("Snoozed 5 mins", 1, 0, 0);
  display.display();
  delay(1000);
}

void run_mode(int mode); // Function prototype

void go_to_menu();

void go_to_menu() {
  int selectedOption = 0;
  bool inMenu = true;
  
  display.clearDisplay();
  print_line("MEDIBOX MENU", 1, 0, 0);
  for (int i = 0; i < MAX_MODES; i++) {
    String displayText = (i == selectedOption) ? "> " + menu_options[i] : "  " + menu_options[i];
    print_line(displayText, 1, 15 + (i * 10), 0);
  }
  display.display();

  // Forward declaration for run_mode
  void run_mode(int mode);

  while (inMenu) {
    if (digitalRead(MENU_BUTTON) == LOW) {
      delay(150);
      while (digitalRead(MENU_BUTTON) == LOW);
      inMenu = false;
      break;
    }

    if (digitalRead(UP) == LOW) {
      delay(150);
      while (digitalRead(UP) == LOW);
      selectedOption = (selectedOption - 1 + MAX_MODES) % MAX_MODES;
    } 
    else if (digitalRead(DOWN) == LOW) {
      delay(150);
      while (digitalRead(DOWN) == LOW);
      selectedOption = (selectedOption + 1) % MAX_MODES;
    }
    else if (digitalRead(OK) == LOW) {
      delay(150);
      while (digitalRead(OK) == LOW);
      run_mode(selectedOption);
    }

    // Update menu display
    display.clearDisplay();
    print_line("MEDIBOX MENU", 1, 0, 0);
    for (int i = 0; i < MAX_MODES; i++) {
      String displayText = (i == selectedOption) ? "> " + menu_options[i] : "  " + menu_options[i];
      print_line(displayText, 1, 15 + (i * 10), 0);
    }
    display.display();
    
    delay(10);
  }
}

void set_time_zone() {
  display.clearDisplay();
  print_line("Set Time Zone", 1, 0, 0);
  print_line("Feature not", 1, 15, 0);
  print_line("implemented", 1, 25, 0);
  display.display();
  delay(1500);
}

// Simple implementation for set_alarm()
void set_alarm() {
  int hour = 0;
  int minute = 0;
  bool setting = true;

  display.clearDisplay();
  print_line("Set Alarm", 1, 0, 0);
  print_line("Use UP/DOWN", 1, 15, 0);
  print_line("OK: Next, CANCEL: Exit", 1, 25, 0);
  display.display();
  delay(1000);

  // Set hour
  while (setting) {
    display.clearDisplay();
    print_line("Set Hour:", 1, 0, 0);
    print_line(String(hour), 2, 15, 0);
    display.display();

    if (digitalRead(UP) == LOW) {
      hour = (hour + 1) % 24;
      delay(150);
      while (digitalRead(UP) == LOW);
    }
    if (digitalRead(DOWN) == LOW) {
      hour = (hour - 1 + 24) % 24;
      delay(150);
      while (digitalRead(DOWN) == LOW);
    }
    if (digitalRead(OK) == LOW) {
      delay(150);
      while (digitalRead(OK) == LOW);
      break;
    }
    if (digitalRead(CANCEL) == LOW) {
      delay(150);
      while (digitalRead(CANCEL) == LOW);
      return;
    }
    delay(10);
  }

  // Set minute
  setting = true;
  while (setting) {
    display.clearDisplay();
    print_line("Set Minute:", 1, 0, 0);
    print_line(String(minute), 2, 15, 0);
    display.display();

    if (digitalRead(UP) == LOW) {
      minute = (minute + 1) % 60;
      delay(150);
      while (digitalRead(UP) == LOW);
    }
    if (digitalRead(DOWN) == LOW) {
      minute = (minute - 1 + 60) % 60;
      delay(150);
      while (digitalRead(DOWN) == LOW);
    }
    if (digitalRead(OK) == LOW) {
      delay(150);
      while (digitalRead(OK) == LOW);
      break;
    }
    if (digitalRead(CANCEL) == LOW) {
      delay(150);
      while (digitalRead(CANCEL) == LOW);
      return;
    }
    delay(10);
  }

  // Add alarm
  alarms.push_back({hour, minute, true, false});
  display.clearDisplay();
  print_line("Alarm Set!", 1, 0, 0);
  char buf[16];
  sprintf(buf, "%02d:%02d", hour, minute);
  print_line(buf, 2, 15, 0);
  display.display();
  delay(1500);
}

void view_alarms() {
  display.clearDisplay();
  print_line("Alarms:", 1, 0, 0);
  int y = 12;
  if (alarms.empty()) {
    print_line("No alarms set", 1, y, 0);
  } else {
    for (size_t i = 0; i < alarms.size() && y < SCREEN_HEIGHT - 10; ++i) {
      char buf[20];
      sprintf(buf, "%02d:%02d %s", alarms[i].hours, alarms[i].minutes, alarms[i].enabled ? "ON" : "OFF");
      print_line(String(buf), 1, y, 0);
      y += 10;
    }
  }
  display.display();
  delay(2000);
}

void delete_alarm() {
  if (alarms.empty()) {
    display.clearDisplay();
    print_line("No alarms to", 1, 0, 0);
    print_line("delete.", 1, 10, 0);
    display.display();
    delay(1500);
    return;
  }

  int selected = 0;
  bool deleting = true;

  while (deleting) {
    display.clearDisplay();
    print_line("Delete Alarm", 1, 0, 0);
    for (size_t i = 0; i < alarms.size() && i < 5; ++i) {
      String prefix = (i == selected) ? "> " : "  ";
      char buf[20];
      sprintf(buf, "%02d:%02d %s", alarms[i].hours, alarms[i].minutes, alarms[i].enabled ? "ON" : "OFF");
      print_line(prefix + String(buf), 1, 12 + (i * 10), 0);
    }
    display.display();

    if (digitalRead(UP) == LOW) {
      selected = (selected - 1 + alarms.size()) % alarms.size();
      delay(150);
      while (digitalRead(UP) == LOW);
    }
    if (digitalRead(DOWN) == LOW) {
      selected = (selected + 1) % alarms.size();
      delay(150);
      while (digitalRead(DOWN) == LOW);
    }
    if (digitalRead(OK) == LOW) {
      alarms.erase(alarms.begin() + selected);
      display.clearDisplay();
      print_line("Alarm Deleted!", 1, 0, 0);
      display.display();
      delay(1000);
      deleting = false;
      while (digitalRead(OK) == LOW);
    }
    if (digitalRead(CANCEL) == LOW) {
      delay(150);
      while (digitalRead(CANCEL) == LOW);
      deleting = false;
    }
    delay(10);
  }
}

void run_mode(int mode) {
  switch(mode) {
    case 0: set_time_zone(); break;
    case 1: set_alarm(); break;
    case 2: view_alarms(); break;
    case 3: delete_alarm(); break;
    case 4: 
      for(auto &alarm : alarms) {
        alarm.enabled = false;
      }
      display.clearDisplay();
      print_line("All alarms disabled", 1, 0, 0);
      display.display();
      delay(1000);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  debugPrint("Medibox System Starting...");

  // Initialize hardware
  pinMode(LDR_PIN, INPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(LED_1, OUTPUT);
  pinMode(LED_2, OUTPUT);
  pinMode(MENU_BUTTON, INPUT_PULLUP);
  pinMode(CANCEL, INPUT_PULLUP);
  pinMode(UP, INPUT_PULLUP);
  pinMode(DOWN, INPUT_PULLUP);
  pinMode(OK, INPUT_PULLUP);
  
  dht.begin();
  servo.attach(SERVO_PIN);
  servo.write(thetaOffset);

  // Initialize display
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    debugPrint("Display init failed!");
    while(1);
  }
  display.clearDisplay();
  print_line("MediBox Starting", 1, 0, 0);
  display.display();
  delay(1000);

  // Initialize with 2 alarms
  alarms.push_back({0, 0, true, false});
  alarms.push_back({0, 0, true, false});

  // Connect to WiFi
  setup_wifi();

  // Configure MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setKeepAlive(60);
  client.setSocketTimeout(30);
}

void loop() {
  // Maintain connections
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi();
    delay(1000);
    return;
  }

  if (!client.connected()) {
    reconnect();
  } else {
    client.loop();
  }

  // Update display every second
  static unsigned long lastDisplayUpdate = 0;
  if(millis() - lastDisplayUpdate >= 1000) {
    update_display();
    lastDisplayUpdate = millis();
  }

  // Check menu button
  if (digitalRead(MENU_BUTTON) == LOW) {
    delay(200);
    go_to_menu();
  }
  
  // Check alarms every second
  if(millis() - lastAlarmCheck > 1000) {
    check_alarms();
    lastAlarmCheck = millis();
  }

  // Sampling logic
  unsigned long currentTime = millis();

  // Take LDR samples
  if(currentTime - lastSampleTime >= sampleInterval) {
    lastSampleTime = currentTime;
    
    if(sampleCount < MAX_SAMPLES) {
      ldrSamples[sampleCount] = readLDR();
      sampleCount++;
    }
  }

  // Send averaged data
  if(currentTime - lastSendTime >= sendInterval && sampleCount > 0) {
    lastSendTime = currentTime;

    // Calculate average light intensity
    float sum = 0;
    for(int i = 0; i < sampleCount; i++) {
      sum += ldrSamples[i];
    }
    float avgIntensity = sum / sampleCount;

    // Read temperature and calculate angle
    float temperature = readTemperature();
    float angle = calculateServoAngle(avgIntensity, temperature);

    // Update servo position
    servo.write(angle);

    // Publish data
    publishData(avgIntensity, temperature, angle);

    // Reset sample counter
    sampleCount = 0;
  }
  
  delay(10);
}