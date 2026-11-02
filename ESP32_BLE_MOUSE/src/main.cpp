#include <Arduino.h>
#include <BleMouse.h>
#include <Wire.h>
#include <MPU6050_light.h>

// ===== CẤU HÌNH  =====
#define BUTTON_LEFT    17
#define BUTTON_RIGHT   18
#define BUTTON_SCROLL  19

#define SENSITIVITY    3.0    // Giảm độ nhạy (cũ: 15.0)
#define DEAD_ZONE      5.0    // Tăng vùng chết (cũ: 2.0)
#define SMOOTHING      0.92   // Tăng độ mượt (cũ: 0.7)
#define MAX_SPEED      20     // Giới hạn tốc độ tối đa (pixel/frame)

BleMouse bleMouse("AirMouse-ESP32", "DIY", 100);
MPU6050 mpu(Wire);

float prevX = 0, prevY = 0;

// ===== KHAI BÁO PROTOTYPE =====
void handleButtons();

void setup() {
  Serial.begin(115200);
  
  Wire.begin(21, 22);
  
  byte status = mpu.begin();
  Serial.print("MPU6050 status: ");
  Serial.println(status);
  
  if (status != 0) {
    Serial.println("Lỗi kết nối MPU6050!");
    while (1);
  }
  
  Serial.println("Đang hiệu chuẩn... Giữ yên thiết bị!");
  delay(1000);
  mpu.calcOffsets(true, true);
  Serial.println("Hiệu chuẩn xong!");
  
  pinMode(BUTTON_LEFT, INPUT_PULLUP);
  pinMode(BUTTON_RIGHT, INPUT_PULLUP);
  pinMode(BUTTON_SCROLL, INPUT_PULLUP);
  
  bleMouse.begin();
  Serial.println("Bluetooth đã sẵn sàng!");
}

void loop() {
  if (bleMouse.isConnected()) {
    mpu.update();
    
    float angleX = mpu.getAngleX();
    float angleY = mpu.getAngleY();
    
    float moveX = 0, moveY = 0;
    
    // Áp dụng vùng chết lớn hơn
    if (abs(angleY) > DEAD_ZONE) {
      // Trừ đi DEAD_ZONE để chuyển động bắt đầu từ 0
      moveX = (angleY - (angleY > 0 ? DEAD_ZONE : -DEAD_ZONE)) * SENSITIVITY / 10.0;
    }
    if (abs(angleX) > DEAD_ZONE) {
      moveY = -(angleX - (angleX > 0 ? DEAD_ZONE : -DEAD_ZONE)) * SENSITIVITY / 10.0;
    }
    
    // Giới hạn tốc độ tối đa
    moveX = constrain(moveX, -MAX_SPEED, MAX_SPEED);
    moveY = constrain(moveY, -MAX_SPEED, MAX_SPEED);
    
    // Bộ lọc làm mượt mạnh hơn
    moveX = SMOOTHING * prevX + (1.0 - SMOOTHING) * moveX;
    moveY = SMOOTHING * prevY + (1.0 - SMOOTHING) * moveY;
    prevX = moveX;
    prevY = moveY;
    
    // Di chuyển chuột
    if (abs(moveX) > 0.5 || abs(moveY) > 0.5) {
      bleMouse.move((int8_t)moveX, (int8_t)moveY);
    }
    
    handleButtons();
    delay(10);
  } else {
    Serial.println("Đang chờ kết nối Bluetooth...");
    delay(1000);
  }
}

void handleButtons() {
  // NÚT CHUỘT TRÁI - GPIO 17
  static bool lastLeft = HIGH;
  bool currentLeft = digitalRead(BUTTON_LEFT);
  if (currentLeft == LOW && lastLeft == HIGH) {
    bleMouse.click(MOUSE_LEFT);
    Serial.println("Left Click!");
    delay(50);
  }
  lastLeft = currentLeft;
  
  // NÚT CHUỘT PHẢI - GPIO 18
  static bool lastRight = HIGH;
  bool currentRight = digitalRead(BUTTON_RIGHT);
  if (currentRight == LOW && lastRight == HIGH) {
    bleMouse.click(MOUSE_RIGHT);
    Serial.println("Right Click!");
    delay(50);
  }
  lastRight = currentRight;
  
  // NÚT SCROLL - GPIO 19
  static bool lastScroll = HIGH;
  static bool scrollMode = false;
  bool currentScroll = digitalRead(BUTTON_SCROLL);
  
  if (currentScroll == LOW && lastScroll == HIGH) {
    scrollMode = !scrollMode;
    Serial.println(scrollMode ? "Scroll Mode: ON" : "Scroll Mode: OFF");
    delay(50);
  }
  lastScroll = currentScroll;
  
  if (scrollMode) {
    float scrollAngle = mpu.getAngleX();
    if (abs(scrollAngle) > DEAD_ZONE) {
      int8_t scrollVal = (int8_t)constrain(-scrollAngle / 8.0, -3, 3);
      bleMouse.move(0, 0, scrollVal);
    }
  }
}
