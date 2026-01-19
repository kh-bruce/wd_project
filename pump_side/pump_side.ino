/**
  2023水鴨計畫
  board: nodemcu (esp32)
**/
#include <esp_task_wdt.h>
#define WDT_TIMEOUT 30
#define WOOOOOOOOOF 18 // must smaller then WDT_TIMEOUT
int last = 0;
bool stop_wdt = false; // set to true to reset devices
#include <Arduino.h>
#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#elif defined(ESP8266) clearyncTCP.h >
#endif
#include <ESPAsyncWebSrv.h>
AsyncWebServer server(80);
const char *ssid = "iHome";
const char *password = "e35792468";
const char *PARAM_INPUT_WATERLEVEL = "message"; //test
const char *PARAM_INPUT_FRONTDOOR = "frontdoor";
const char *PARAM_INPUT_SETMAXLEVEL = "setmaxlevel";
const char *PARAM_INPUT_SETMINLEVEL = "setminlevel";
String message = "null";
#include <arduino-timer.h>
auto timer_blink = timer_create_default();
auto timer_1 = timer_create_default(); // auto start pump when bootup
auto timer_2 = timer_create_default(); // 過熱保護
auto timer_3 = timer_create_default(); // 過熱保護復歸
auto timer_relay = timer_create_default(); // 鐵門 frontdoor relay
auto timer_bad_connection = timer_create_default(); // how long till enter "no conn mode"
auto timer_ntp = timer_create_default(); // 20230808 ntp 時間功能
const int normal_blink_interval = 1000; // ms // when normal -> waiting & pump is on
const int overheated_blink_interval = 250; // ms // when over heat protecting
const int badconnmode_blink_interval = 50; // ms // when "no conn mode" active
const int timer_1_delay = (WDT_TIMEOUT + 1) * 1000; // ms // how long after bootup
const int timer_2_interval = 20 * 60 * 1000; // ms // 多久時間後啟動過熱保護
const int timer_3_interval = 10 * 60 * 1000; // ms // 過熱保護的停機散熱時間
const int relay_open_interval = 300; // 控制遙控器點擊的停留時間
const int timer_bad_connection_delay = 1 * 60 * 1000; // ms // how long till enter "no conn mode"
const int timer_ntp_interval = 10 * 60 * 1000; // 20230808 ntp 時間功能 // 檢查是否在pre_fill_up的時間範圍內
float MAX_WATER_LEVEL = 120; // 實測最大值 83 // 2023111月底外部最大壓力測試 122
float MIN_WATER_LEVEL = 70; // 實測最小值 46
float DEFICIENT_WATER_LEVEL = 70; // 預先補水啟動補水之水位 // Deficient // Insufficient
const int isTimeInRange_min = 20; // 預先補水功能 補水時間區間開始
const int isTimeInRange_max = 23; // 預先補水功能 補水時間區間結束
const int isBadTimeBetween_from = 23; // badtime start at (default 22)
const int isBadTimeBetween_to = 6; // badtime ends at (default 6)
bool blink = true;
bool bad_conn_mode = false;
#define GPIO4PUMP 4 // motor // 麵包板7
#define GPIO4UP 16 //up // 麵包板8
#define GPIO4DOWN 17 //down // 麵包板9
#define GPIO4STOP 18 //stop // 麵包板11
// 右邊6負極
enum pumpStatus {
  RUNNING, // 運作中
  STOPPED, // 已停止
  OVERHEAT_PROTECTION // 過熱保護
};
typedef enum pumpStatus PumpStatus;
PumpStatus pump_status = STOPPED; // 初始化為已停止狀態
long ms = 0;
// 20230808 ntp 時間功能
#include <NTPClient.h>
#include <WiFiUdp.h>
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");
// reset_bad_conn_timer 找錯中...
int bad_conn_count = 0;
int cannotcanceltimerrrrrr = 0;
bool overheat(void *argument /* optional argument given to in/at/every */) {
    Serial.println("overheat overheat overheat");
    request_pump_to(OVERHEAT_PROTECTION);
    timer_2.cancel();
    Serial.println("log for timer1");
    return true; // to repeat the action - false to stop
}
void pump_run(long forHowLong_ms = timer_2_interval){
  timer_2.cancel();
  digitalWrite(GPIO4PUMP, LOW);
  pump_status = RUNNING;
  ms = millis();
  timer_2.in(forHowLong_ms, overheat);
}
void pump_stop(){
    timer_2.cancel();
    timer_3.cancel();
    digitalWrite(GPIO4PUMP, HIGH);
    pump_status = STOPPED;
    ms = millis();
}
bool recover_from_overheat(void *argument /* optional argument given to in/at/every */) {
    timer_3.cancel();
    
    set_timer_blink_interval_to(normal_blink_interval);
    Serial.println("recover from overheat (過熱保護復歸)");
    pump_status = STOPPED;
    ms = millis();
    if (!isBadTime()) request_pump_to(RUNNING); // check_water_level(); // pump_run();
    // reset_bad_conn_timer(); // 以防進入 bad conn mode 然後又在這邊被打開... 不會發生因為request_pump_to()裡面有擋了
    Serial.println("log for timer2");
    return true; // to repeat the action - false to stop
}
void pump_overheat_protect(long forHowLong_ms = timer_3_interval){
  timer_3.cancel();
  set_timer_blink_interval_to(overheated_blink_interval);
  digitalWrite(GPIO4PUMP, HIGH);
  pump_status = OVERHEAT_PROTECTION;
  ms = millis();
  timer_3.in(forHowLong_ms, recover_from_overheat);
}
void request_pump_to(PumpStatus status) {
  
  if (bad_conn_mode) return;
  switch (status) {  
    case RUNNING:
      if (pump_status == RUNNING){
        Serial.println("pump is running");
      }else if (pump_status == OVERHEAT_PROTECTION){
        Serial.println("pump is now in overheat protection");
      }else{
        Serial.println("pump_run");
        pump_run();
      }
      break;
    case STOPPED:
      pump_stop();
      // if (pump_status == STOPPED){
      //   Serial.println("pump is stopped");
      // }else{
      //   Serial.println("pump_stop");
      //   pump_stop();
      // }
      break;
    case OVERHEAT_PROTECTION:
      if (pump_status == OVERHEAT_PROTECTION){
      }else{
        Serial.println("pump_overheat_protect");
        pump_overheat_protect();
      }
      break;
    default:
      Serial.println("Unknown pump status");
      break;
  }
}
bool go_bad_conn_mode (void *argument /* optional argument given to in/at/every */) {
  Serial.println("[go_bad_conn_mode] triggered ! !");
  Serial.println("[go_bad_conn_mode] triggered ! !");
  bad_conn_count++;
  bad_conn_mode = true;
  pump_stop(); // force stop pump
  set_timer_blink_interval_to(badconnmode_blink_interval);
  Serial.println("log for timer3");
  return true;
}
void reset_bad_conn_timer() {
  Serial.println("log for reset_bad_conn_timer1");
  static Timer<>::Task event;
  Serial.println("log for reset_bad_conn_timer2");
  if (!timer_bad_connection.empty()) {
    Serial.println("log for reset_bad_conn_timer3");
    timer_bad_connection.cancel(event);
    if (!timer_bad_connection.empty()) cannotcanceltimerrrrrr++;
    Serial.println("log for reset_bad_conn_timer4");
  }
  Serial.println("log for reset_bad_conn_timer5");
  // delay(1);
  Serial.println("log for reset_bad_conn_timer6");
  event = timer_bad_connection.in(timer_bad_connection_delay, go_bad_conn_mode);
  Serial.println("log for reset_bad_conn_timer7");
  if (bad_conn_mode) set_timer_blink_interval_to(normal_blink_interval);
  Serial.println("log for reset_bad_conn_timer8");
  bad_conn_mode = false;
}
void check_water_level (float desiredMinWaterLevel, bool if_reset_bad_conn_timer) {
  if (message.toFloat() != 0) {
    float num = message.toFloat();
    Serial.println("[check_water_level] Water level: " + String(num) + ". (desiredMinWaterLevel: " + desiredMinWaterLevel + ", MAX_WATER_LEVEL: " + MAX_WATER_LEVEL + ")");
    Serial.println("log for check_water_level1");
    if (if_reset_bad_conn_timer) reset_bad_conn_timer();
    Serial.println("log for check_water_level2");
    if (num < desiredMinWaterLevel){
      Serial.println(" !! BELOW desiredMinWaterLevel (" + String(desiredMinWaterLevel) + ") !! ");
      //requested the pump to start
      request_pump_to(RUNNING);
    }else if (num > MAX_WATER_LEVEL){
      Serial.println(" !! OVER MAX_WATER_LEVEL (" + String(MAX_WATER_LEVEL) + ") !! ");
      //requested the pump to stop
      request_pump_to(STOPPED); //lock from entering RUNNING status for a period of time after pump stopped
    }
    Serial.println("log for check_water_level3");
  } else {
    Serial.println("[check_water_level] Waiting for water level data from 4F. (or given data is not valid)");
  }
}
bool relay_reset(void *argument /* optional argument given to in/at/every */) {
  timer_relay.cancel();
  digitalWrite(GPIO4UP, HIGH);
  digitalWrite(GPIO4DOWN, HIGH);
  digitalWrite(GPIO4STOP, HIGH);
  Serial.println("log for timer4");
  return true;
}
void frontdoor_control(String value){ // frontdoor control/relay control
  if (value == "up"){
    digitalWrite(GPIO4UP, LOW);
    timer_relay.cancel();
    timer_relay.in(relay_open_interval, relay_reset);
  }else if (value == "down"){
    digitalWrite(GPIO4DOWN, LOW);
    timer_relay.cancel();
    timer_relay.in(relay_open_interval, relay_reset);
  }else if (value == "stop"){
    digitalWrite(GPIO4STOP, LOW);
    timer_relay.cancel();
    timer_relay.in(relay_open_interval, relay_reset);
  }
}
bool blink_f (void *argument /* optional argument given to in/at/every */) {
  blink = !blink;
  if (blink) digitalWrite(2, HIGH);
  else digitalWrite(2, LOW);
  return true;
}
bool fill_up (void *argument /* optional argument given to in/at/every */) {
  request_pump_to(RUNNING);
  Serial.println("log for timer6");
  return true;
}
// 20230808 ntp 時間功能
bool isTimeInRange (void *argument /* optional argument given to in/at/every */) {
  if (timeClient.update() < 0) { // Get the current time from NTP server
  Serial.println("log for timer7");
    return true; // Failed to update time
  }
  // Extract hours from the formatted time string
  String formattedTime = timeClient.getFormattedTime();
  int hours = formattedTime.substring(0, 2).toInt(); // Extract first two characters and convert to integer
  // Check if the time is between 20 and 22
  if (hours >= isTimeInRange_min && hours < isTimeInRange_max) {
    Serial.println("Current time is between 20:00 and 22:00, check_water_level with desiredMinWaterLevel as: " + String(DEFICIENT_WATER_LEVEL) + " (" + formattedTime + ")");
    check_water_level(DEFICIENT_WATER_LEVEL, false);
  } else {
    Serial.println("Current time is NOT between 20:00 and 22:00 (" + formattedTime + ")");
  }
  return true;
}
bool isBadTime() {
  if (timeClient.update() < 0) { // Get the current time from NTP server
    return false; // Failed to update time
  }
  // Extract hours from the formatted time string
  String formattedTime = timeClient.getFormattedTime();
  int hours = formattedTime.substring(0, 2).toInt(); // Extract first two characters and convert to integer
  // Check if the time is between 22 and 6
  if (hours >= isBadTimeBetween_from || hours < isBadTimeBetween_to) {
    Serial.println("isBadtime : True (" + formattedTime + ")");
    return true;
  } else {
    Serial.println("isBadtime : False (" + formattedTime + ")");
    return false;
  }
}
void setup() {
  Serial.begin(115200);
  Serial.println("Configuring WDT...");
  esp_task_wdt_init(WDT_TIMEOUT, true); //enable panic so ESP32 restarts
  esp_task_wdt_add(NULL);
  last = millis();
  pinMode(2, OUTPUT);
  digitalWrite(2, HIGH);
  pinMode(GPIO4PUMP, OUTPUT);
  digitalWrite(GPIO4PUMP, HIGH);
  pinMode(GPIO4UP, OUTPUT); //up
  digitalWrite(GPIO4UP, HIGH);
  pinMode(GPIO4DOWN, OUTPUT); //down
  digitalWrite(GPIO4DOWN, HIGH);
  pinMode(GPIO4STOP, OUTPUT); //stop
  digitalWrite(GPIO4STOP, HIGH);

  String myip = "192.168.1.217";
  IPAddress staticIP(192, 168, 1, 217);
  IPAddress gateway(192, 168, 1, 200);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.config(staticIP, gateway, subnet);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.printf("ERROR! >> WiFi Failed!");
    ESP.restart();
    return;
  }
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  if (WiFi.localIP().toString().equals(myip) == false) {
    Serial.println("ERROR! >> WRONG IP Address, please check Router's setting (now ip:" + WiFi.localIP().toString() + ")");
    ESP.restart();
    return;
  }
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String s1 = "P\nR\nO\nJ\nE\nC\nT\nSHUI\nYAAA\n\nFile name: \"wd_project_pump_side.ino\".";
    String s2 = "\n\nWater Tank level now = " + message + ".";
    String s2_2 = "\n\n(auto control range (water level): " + String(MIN_WATER_LEVEL) + "~" + String(MAX_WATER_LEVEL) + ")";
    String s2_3 = "\n\n(pre_fill_up time range: " + String(isTimeInRange_min) + ":00 ~ " + String(isTimeInRange_max) + ":00, trigger water level : " + String(DEFICIENT_WATER_LEVEL) + ")";
    String s3 = "\n\n(Bad connection mode status = " + String(bad_conn_mode) + ". (count = " + bad_conn_count + ") (cancel timer failed " + cannotcanceltimerrrrrr + " times)";
    String s4 = "\n\nNow pump status = ";
    float time_since_last_state_change = (millis() - ms) / 1000.0 / 60.0;
    String s4_2 = ". (time since last state change: " + String(time_since_last_state_change) + " minutes)";
    String s5 = "\n\n(isBadtime : "; //is bad time
    String s5_2 = ". (badtime form " + String(isBadTimeBetween_from) + ":00 ~ " + String(isBadTimeBetween_to) + ":00)";
    timeClient.update(); // Get the current time from NTP server
    String formattedTime = timeClient.getFormattedTime();
    String s6 = "\n\n(web page update at: " + formattedTime + ")";
    String s7 = "\n\n(upTime: " + String(millis()/1000) + "s)";
    if (isBadTime()) { // s5
      s5 += "True)";
    } else {
      s5 += "False)";
    }
    switch (pump_status) { // s4_2
      case RUNNING:
        s4 += "RUNNING";
        break;
      case STOPPED:
        s4 += "STOPPED";
        break;
      case OVERHEAT_PROTECTION:
        s4 += "OVERHEAT_PROTECTION";
        break;
      default:
        s4 += "Unknown Status";
        break;
  }
    request->send(200, "text/plain", s1 + s2 + s4 + s4_2 + s2_2 + s2_3 + s3 + s5 + s5_2 + s6 + s7);
  });
  server.on("/info/", HTTP_GET, [](AsyncWebServerRequest *request) { 
    // String htmlContent = "<html><body><input type=button value=hello><p>a1: " + message + "</p></body></html>";
    String htmlContent = getHtmlContent();
    request->send(200, "text/html", htmlContent);
  });
  // Send a GET request to <IP>/get?message=<message>
  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request) {
    String temp = "";
    if (request->hasParam(PARAM_INPUT_WATERLEVEL)) {
      temp = request->getParam(PARAM_INPUT_WATERLEVEL)->value();
      message = temp;
    } else if (request->hasParam(PARAM_INPUT_FRONTDOOR)){
      String value = request->getParam(PARAM_INPUT_FRONTDOOR)->value();
      temp = "recived command: frontdoor= " + value;
      frontdoor_control(value);
    } else if (request->hasParam(PARAM_INPUT_SETMAXLEVEL)){
      String value = request->getParam(PARAM_INPUT_SETMAXLEVEL)->value();
      temp = "recived command: set max water level to " + value;
      float f = float(value.toInt());
      if (f > MIN_WATER_LEVEL) {
        MAX_WATER_LEVEL  = f;
        DEFICIENT_WATER_LEVEL = (MAX_WATER_LEVEL - MIN_WATER_LEVEL) * 0.3 + MIN_WATER_LEVEL;
      }
    } else if (request->hasParam(PARAM_INPUT_SETMINLEVEL)){
      String value = request->getParam(PARAM_INPUT_SETMINLEVEL)->value();
      temp = "recived command: set min water level to " + value;
      float f = float(value.toInt());
      if (f < MAX_WATER_LEVEL) {
        MIN_WATER_LEVEL = f;
        DEFICIENT_WATER_LEVEL = (MAX_WATER_LEVEL - MIN_WATER_LEVEL) * 0.3 + MIN_WATER_LEVEL;
      }
    } else {
      message = temp;
      temp = "No message sent";
    }
    Serial.println("\tHello, GET: " + temp);
    request->send(200, "text/plain", "Hello, GET: " + temp);
    check_water_level(MIN_WATER_LEVEL, true);
  });
  // Send a POST request to <IP>/post with a form field message set to <message>
  server.on("/post", HTTP_POST, [](AsyncWebServerRequest *request) {
    String message;
    if (request->hasParam(PARAM_INPUT_WATERLEVEL, true)) {
      message = request->getParam(PARAM_INPUT_WATERLEVEL, true)->value();
    } else {
      message = "No message sent";
    }
    Serial.println("\tHello, POST: " + message);
    request->send(200, "text/plain", "Hello, POST: " + message);
    check_water_level(MIN_WATER_LEVEL, true);
  });
  server.onNotFound([](AsyncWebServerRequest *request){
  request->send(404, "text/plain", "Not found");
  });
  server.begin();
  // 20230808 ntp 時間功能
  timeClient.begin(); // Initialize and configure NTP client
  timeClient.setTimeOffset(8 * 3600); // Set time offset to +8 hours (8 * 3600 seconds)
  timer_ntp.every(timer_ntp_interval, isTimeInRange);
  set_timer_blink_interval_to(normal_blink_interval);
  timer_1.in(timer_1_delay, fill_up);
  reset_bad_conn_timer(); // timer_bad_connection
  ms = millis();
}
String getHtmlContent(){
  return "<!DOCTYPE html>\n\
<html>\n\
<head>\n\
  <title>iHOME remote</title>\n\
  <style>\n\
    * {\n\
      box-sizing: border-box;\n\
    }\n\
    body {\n\
      margin: 0;\n\
      padding: 20px;\n\
    }\n\
    .remote-container {\n\
      width: 100%;\n\
      height: 100vh;\n\
      background-color: silver;\n\
      display: flex;\n\
      flex-direction: column;\n\
      align-items: center;\n\
      justify-content: center;\n\
      border-radius: 20px;\n\
      padding: 20px;\n\
    }\n\
    .panel {\n\
      width: 80%;\n\
      max-width: 400px;\n\
      height: 90%;\n\
      max-height: 600px;\n\
      background-color: white;\n\
      display: flex;\n\
      flex-direction: column;\n\
      align-items: center;\n\
      justify-content: center;\n\
      padding: 20px;\n\
      border-radius: 20px;\n\
    }\n\
    .button-row {\n\
      display: flex;\n\
      justify-content: center;\n\
      margin-bottom: 20px;\n\
    }\n\
    .button {\n\
      display: flex;\n\
      flex-direction: column;\n\
      align-items: center;\n\
      justify-content: center;\n\
      width: 80px;\n\
      height: 80px;\n\
      background-color: #ccc;\n\
      margin: 10px;\n\
      border-radius: 50%;\n\
      font-size: 24px;\n\
      text-decoration: none;\n\
      color: black;\n\
      transition: background-color 0.3s;\n\
    }\n\
    .button:hover {\n\
      background-color: #ddd;\n\
    }\n\
    .led {\n\
      width: 20px;\n\
      height: 20px;\n\
      border-radius: 50%;\n\
      background-color: black;\n\
      transition: background-color 0.3s;\n\
      margin-bottom: 20px;\n\
    }\n\
    .led.on {\n\
      background-color: red;\n\
    }\n\
    .caption {\n\
      margin-top: 10px;\n\
      font-size: 14px;\n\
      font-weight: bold;\n\
    }\n\
    @media only screen and (max-width: 768px) {\n\
      /* For mobile devices */\n\
      .remote-container {\n\
        padding: 10px;\n\
      }\n\
      .panel {\n\
        width: 90%;\n\
        height: 90%;\n\
        max-height: 600px;\n\
      }\n\
    }\n\
  </style>\n\
</head>\n\
<body>\n\
  <div class=\"remote-container\">\n\
    <div class=\"panel\">\n\
      <div class=\"led\"></div>\n\
      <div class=\"button-row\">\n\
        <a href=\"http://192.168.0.112/get?frontdoor=up\" target=\"_blank\" class=\"button\" onclick=\"event.preventDefault(); activateLed(); openUrl(this.href); playSound();\">UPUP</a>\n\
        <a href=\"http://192.168.0.112/get?frontdoor=down\" target=\"_blank\" class=\"button\" onclick=\"event.preventDefault(); activateLed(); openUrl(this.href); playSound();\">DOWN</a>\n\
      </div>\n\
      <a href=\"http://192.168.0.112/get?frontdoor=stop\" target=\"_blank\" class=\"button\" onclick=\"event.preventDefault(); activateLed(); openUrl(this.href); playSound();\">STOP</a>\n\
    </div>\n\
  </div>\n\
  <audio id=\"click-sound\" src=\"https://example.com/click.mp3\"></audio>\n\
  <script>\n\
    function activateLed() {\n\
      var led = document.querySelector('.led');\n\
      led.classList.add('on');\n\
      setTimeout(function() {\n\
        led.classList.remove('on');\n\
      }, 2000);\n\
    }\n\
    function openUrl(url) {\n\
      window.open(url, '_blank');\n\
    }\n\
    function playSound() {\n\
      document.getElementById('click-sound').play();\n\
    }\n\
  </script>\n\
</body>\n\
</html>";
}
void set_timer_blink_interval_to(int interval) {
  Serial.println("set_timer_blink_interval_to: " + interval);
  timer_blink.cancel();
  timer_blink.every(interval, blink_f);
}
void loop() {
  timer_1.tick();
  timer_blink.tick();
  timer_2.tick();
  timer_3.tick();
  timer_relay.tick();
  timer_bad_connection.tick();
  timer_ntp.tick();
  if ((millis() - last >= (WOOOOOOOOOF * 1000)) && (stop_wdt != true)) {
    // Serial.println("Resetting WDT...");
    esp_task_wdt_reset();
    last = millis();
  }
}
