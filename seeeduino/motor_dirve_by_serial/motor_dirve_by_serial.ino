#include "Seeed_BME280.h"
#include <Wire.h>

int motorPin_L_DIR = 12;
int motorPin_L_BRK = 9;
int motorPin_L_SPD = 3;
int motorPin_R_DIR = 13;
int motorPin_R_BRK = 8;
int motorPin_R_SPD = 11;

unsigned long lastmillis = 0;
bool isBME280 = false;
BME280 bme280;

char msgSp = '\t';

void setup() {
  // put your setup code here, to run once:
  pinMode(motorPin_L_DIR, OUTPUT);
  pinMode(motorPin_L_BRK, OUTPUT);
  pinMode(motorPin_L_SPD, OUTPUT);
  pinMode(motorPin_R_DIR, OUTPUT);
  pinMode(motorPin_R_BRK, OUTPUT);
  pinMode(motorPin_R_SPD, OUTPUT);

  if (bme280.init()) {
    lastmillis = millis();
    isBME280 = true;  
  }

  Serial.begin(9600);
}

bool isDrivingMotors = false;
unsigned long lastOrderTime = 0;
unsigned long autoControlDeltaTime = 5000;

int telemtryCycleInMSec = 1000;

void orderDrive(int drivePinDir, int drivePinBlk, int speedPin, bool dir, bool brk, int spd)
{
  if (dir) digitalWrite(drivePinDir, HIGH);
  else digitalWrite(drivePinDir, LOW);
  if (brk) digitalWrite(drivePinBlk, HIGH);
  else digitalWrite(drivePinBlk, LOW);

  if (brk) analogWrite(speedPin, 0);
  else {
    analogWrite(speedPin, spd);
    isDrivingMotors = true;
    lastOrderTime = millis();
  }

  String msg;
  msg += "dirp:" + String(drivePinDir);
  msg += msgSp;
  msg += "dirv:" + String(dir);
  msg += msgSp;
  msg += "blkp:" + String(drivePinBlk);
  msg += msgSp;
  msg += "blkv:" + String(brk);
  msg += msgSp;
  msg += "spdp:" + String(speedPin);
  msg += msgSp;
  msg += "spdv:" + String(spd);
  Serial.println(msg);
}

void executeCommand(String command)
{
  int leftDrive = -1;
  int leftSpeed = 0;
  int rightDrive = -1;
  int rightSpeed = 0;
  int order = -1;
  bool isForwarding = false;
  bool isBreaking = false;
  bool isStopping = false;
  int cmdLen = command.length();
  Serial.println(cmdLen);
  if (cmdLen >= 2) {
    char o = command.charAt(1);
    Serial.println("c1:"+String(o));
    if (o == 'F') {
      order = 1;
      isForwarding = true;
    } else if (o == 'R') {
      order = 2;
      isForwarding = false;
    } else if (o == 'B') {
      order = 3;
      isBreaking = true;
    } else if (o == 'S') {
      order = 0;
      isStopping = true;
    }
    if (order != -1) {
      char s = command.charAt(0);
      Serial.println("c0:"+String(s));
      if (s == 'L') {
        leftDrive = order;
      } else if (s == 'R') {
        rightDrive = order;
      }
      if (order == 1 || order == 2) {
        if (cmdLen >= 5) {
          String sp = command.substring(2);
          char spd[4];
          sp.toCharArray(spd,4);
          Serial.println("Speed:"+sp);
          int speed = 0;
          for (int i=0;i<3;i++) {
            speed = speed * 10;
            speed += (spd[i] - '0');
          }
          if (speed < 256) {
            if (s == 'L') {
              leftSpeed = speed;
            } else {
              rightSpeed = speed;
            }
          }
        }
      }
    }
  }
  if (leftDrive != -1) {
    orderDrive(motorPin_L_DIR, motorPin_L_BRK, motorPin_L_SPD, isForwarding, isBreaking, leftSpeed);
  }
  if (rightDrive != -1) {
    orderDrive(motorPin_R_DIR, motorPin_R_BRK, motorPin_R_SPD, isForwarding, isBreaking, rightSpeed);
  }
}

void serialEvent()
{
  String command = Serial.readString();
  String sensorCommandKey = "sensor:";
  if (command.indexOf(sensorCommandKey) >=0) {
    String sensorSetting = command.substring(sensorCommandKey.length());
    telemtryCycleInMSec = sensorSetting.toInt();
//    Serial.println(telemtryCycleInMSec);
//    delay(2000);
    return;
  }
  int clnIndex = command.indexOf(";");
  executeCommand(command);
  if (clnIndex > 0){
    String command2 = command.substring(clnIndex+1);
    executeCommand(command2);
  }
}

void loop() {
  // put your main code here, to run repeatedly:

  // Serial.println("message waiting...");
//  if (Serial.available()){
//    String command = Serial.readString();
//    Serial.println("received message:");
//    Serial.println(command);
//    int clnIndex = command.indexOf(";");
//    executeCommand(command);
//    if (clnIndex > 0){
//      String command2 = command.substring(clnIndex+1);
//      Serial.println("2nd command:"+command2);
//      executeCommand(command2);
//    }
//  }
  unsigned long currentTick = millis();
  if ( isDrivingMotors && ((currentTick - lastOrderTime) > autoControlDeltaTime)){
      orderDrive(motorPin_L_DIR, motorPin_L_BRK, motorPin_L_SPD, true, true, 0);
      orderDrive(motorPin_R_DIR, motorPin_R_BRK, motorPin_R_SPD, true, true, 0);
    isDrivingMotors = false;
  }
  
  if (isBME280) {
    unsigned long current = millis();
    if (current - lastmillis >= telemtryCycleInMSec) {
      float temperature = bme280.getTemperature();
      uint32_t pressure = bme280.getPressure();
      uint32_t humidity = bme280.getHumidity();
      float altidute = bme280.calcAltitude(pressure);
      String msg = "sensors:temp=" + String(temperature) + ",humi=" + String(humidity) + ",pres=" + String(pressure) + ",alti=" + String(altidute) + ":";
      int msgLen = msg.length();
      if (msgLen < 62){
        for (;msgLen<62;msgLen++) {
          msg += " ";
        }
      }
      Serial.println(msg);      
      lastmillis = current;
    }
  }
}
