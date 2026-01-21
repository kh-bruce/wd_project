// //bruce de esp32
#define BLYNK_TEMPLATE_ID           "TMPLKMkMTv44"
#define BLYNK_TEMPLATE_NAME         "Quickstart Device"
#define BLYNK_AUTH_TOKEN            "qx7zGF3K-ES--_B3S3OhpQKSV-KhIK-k"

//bruce nodemcu32
// #define BLYNK_TEMPLATE_ID "TMPLKMkMTv44"
// #define BLYNK_TEMPLATE_NAME "Quickstart Template"
// #define BLYNK_AUTH_TOKEN "PJxIq7qD2g9boVvc2SZQZWsdVgRANrTb"

/* Comment this out to disable prints and save space */
#define BLYNK_PRINT Serial
#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
char ssid[] = "iHome";
char pass[] = "e35792468";
BlynkTimer timer;  
#define INPIN 32

#include <esp_task_wdt.h>
#define WDT_TIMEOUT 25
int last = millis();

#include <stdio.h>
#include <math.h>

#include <HTTPClient.h>
HTTPClient http;

void reset_wdt(){
  if (millis() - last >= 5000) {
      // Serial.println("Resetting WDT...");
      esp_task_wdt_reset();
      last = millis();
  }
}

void setup(){
  // Debug console
  Serial.begin(115200);
  delay(100);

  // go_init();
  // Serial.printf("go_init2-1: %lf\n\n", getStd_dev());
  // delay(100);

  Serial.println("Configuring WDT...");
  esp_task_wdt_init(WDT_TIMEOUT, false); //enable panic so ESP32 restarts
  esp_task_wdt_add(NULL);

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  // You can also specify server:
  //Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass, "blynk.cloud", 80);
  //Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass, IPAddress(192,168,1,100), 8080);
  
  timer.setInterval(3000, myTimerEvent);
  timer.setInterval(100, reset_wdt);
  timer.setInterval(1, collectDate);
  timer.setInterval(1000, sendData);
}

// This function is called every time the device is connected to the Blynk.Cloud
// BLYNK_CONNECTED()
// {
//   // Change Web Link Button message to "Congratulations!"
//   Blynk.setProperty(V3, "offImageUrl", "https://static-image.nyc3.cdn.digitaloceanspaces.com/general/fte/congratulations.png");
//   Blynk.setProperty(V3, "onImageUrl",  "https://static-image.nyc3.cdn.digitaloceanspaces.com/general/fte/congratulations_pressed.png");
//   Blynk.setProperty(V3, "url", "https://docs.blynk.io/en/getting-started/what-do-i-need-to-blynk/how-quickstart-device-was-made");
// }

#define DEPTH_COUNT 9
float depth[DEPTH_COUNT] = {};
int depth_index;
float avg_max = 0.0;
float avg_min = 401.0;
bool needUpdate = false;

String myip = "192.168.1.217";

void go_init(){
  float depth_init;
  int count = 5000;
  int d = 2;
  Serial.printf("collecting init ref value... ( %d * %d )\n", count, d);
  for (int i=0; i<count; i++){
    float analog = analogRead(INPIN) / 10;
    depth_init += analog;
    delay(d);
  }
  depth_init = depth_init / count;
  for (int i=0; i<DEPTH_COUNT; i++) depth[i] = depth_init;
  depth_index = 0;

  Serial.printf("ref value: %f\n", depth_init);
}

double getStd_dev(){
  int dd = 5;
  // int count = 10;
  // double data[10] = {5.5, 5.6, 5.4, 5.7, 5.3, 5.5, 5.5, 5.5, 5.5, 5.5};
  // int count = 40;
  // double data[40] = {55, 55, 57, 58, 55, 55, 51, 54, 54, 55, 55, 55, 57, 55, 55, 56, 56, 56, 55, 55, 55, 54, 55, 55, 55, 55, 53, 55, 52, 55, 55, 53, 53, 55, 58, 56, 56, 55, 55, 55};
  int count = 500;
  double data[500] = {};
  //標準差
  int valid_count = 0;
  double sum, mean, std_dev, variance = 0.0;

  for (int i=0; i<count; i++){
    double analog = analogRead(INPIN) / 10;
    data[i] = analog;
    sum += analog;
    delay(dd);
    // Serial.println(data[i]);
  }
  Serial.print("sum: ");
  Serial.println(sum);
  mean = (double) sum / count;
  Serial.print("avg: ");
  Serial.println(mean);

  for (int i=0; i<count; i++){
      variance += pow(data[i] - mean, 2);
  }
  variance /= count; //方差
  std_dev = sqrt(variance);
  Serial.print("std_dev: ");
  Serial.println(std_dev);

  sum = 0;
  for (int i=0; i<count; i++){
      if (fabs(data[i] - mean) <= (0.5 * std_dev)) {
          // printf("%d正常水位: %lf\n",i , data[i]);
          sum += data[valid_count];
          valid_count++;
      }
  }
  Serial.printf("valid_count: %d out of %d, avg = %lf\n", valid_count, count, sum/valid_count);

  return std_dev;
}

BLYNK_WRITE(V0) { //called when V0 updated (sync)
  Serial.print("blynk_write: ");
  Serial.println(param[0].asInt());
}

BLYNK_WRITE(V21) { //called when V21 updated. send set max level value to motor side
  int v = param[0].asInt();
  Serial.print("blynk_write set max level (V21): " + v);
  setMaxLevel(v);
}
void setMaxLevel(int v){
  String url = "http://" + myip + "/get?setmaxlevel=" + String(v);
  sendHttpGet(url);
}

BLYNK_WRITE(V22) { //called when V22 updated. send set min level value to motor side
  int v = param[0].asInt();
  Serial.print("blynk_write set min level (V22): " + v);
  setMinLevel(v);
}
void setMinLevel(int v){
  String url = "http://" + myip + "/get?setminlevel=" + String(v);
  sendHttpGet(url);
}

bool sendHttpGet(String url) {
    http.setTimeout(1000);
    http.begin(url);
    int httpResponseCode = http.GET();
    String response = http.getString(); // 讀取回應的內容至字串

    if (httpResponseCode == 200) {
      Serial.print("httpResponseCode = 200, response: ");
      Serial.println(response);
      return true;
    }else{
      return false;
    }
}

// This function sends Arduino's uptime every second to Virtual Pin 2.
void myTimerEvent(){
  // Serial.println(millis() / 1000);
  Blynk.virtualWrite(V2, millis() / 1000);
}

// void sendData(){
//   float oneMinAgo = 0;
//   // int oneMinCount = 0;
//   float max = 0;
//   float min = 401;
//   while(true){
//     float avg = 0;
//     int count = 3000;
//     for (int i=0; i<count; i++){
//       float analog = analogRead(INPIN) / 10;
//       avg += analog;
//       delay(5);
//     }
//     avg = avg / count;
//     if (max < avg) max = avg;
//     if (min > avg) min = avg;
    
//     Blynk.virtualWrite(V10, avg);
//     Serial.print("value: ");
//     Serial.print(avg);
    
//     Serial.println(millis() / 1000);
//     Blynk.virtualWrite(V2, millis() / 1000);
//   }
// }

bool over1loop = false;

void sendData(){
  if (needUpdate){
    double sum, avg = 0;

    if (over1loop){
      for (int i=0; i<DEPTH_COUNT; i++) sum += depth[i];
      avg = sum / DEPTH_COUNT;
    }else{
      for (int i=0; i<depth_index; i++) sum += depth[i];
      avg = sum / depth_index;
    }
    
    if (avg_max < avg) avg_max = avg;
    if (avg_min > avg) avg_min = avg;

    //http GET send to 1f motor esp32
    String url = "http://" + myip + "/get?message=" + String(avg, 4);
    Serial.printf("sendData: %lf, max: %lf, min: %lf\n", avg, avg_max, avg_min);
    bool result = sendHttpGet(url);

    needUpdate = false;

    Blynk.virtualWrite(V3, result ? 1 : 0);
    Blynk.virtualWrite(V10, avg);
    Blynk.virtualWrite(V11, avg_max);
    Blynk.virtualWrite(V12, avg_min);
  }
}

float temp = 0;
#define TIMES2AVG 5000 //取幾次做平均
int count = 0;

void collectDate(){
    temp += analogRead(INPIN) / 10;
    count += 1;
    if (count >= TIMES2AVG){
      float avg;
      avg = temp/count;
      temp = 0;
      count = 0;
      
      // Serial.printf("\n[%d]before depth_index [%lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf]\n", depth_index, depth[0], depth[1], depth[2], depth[3], depth[4], depth[5], depth[6], depth[7], depth[8], depth[9]);
      depth[depth_index] = avg;
      if (depth_index >= DEPTH_COUNT-1) {
        over1loop = true;
        depth_index = 0;
      }else depth_index++;
      // Serial.printf("[%d]after depth_index [%lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf]\n\n", depth_index, depth[0], depth[1], depth[2], depth[3], depth[4], depth[5], depth[6], depth[7], depth[8], depth[9]);

      needUpdate = true;
    }
}

void loop(){
  Blynk.run();
  timer.run();
}
