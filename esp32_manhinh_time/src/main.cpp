#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <TFT_eSPI.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>

// ================== WiFi AP (ESP32 tự phát) ==================
const char* ap_ssid = "ESP32_TFT_WIFI";
const char* ap_password = "12345678";

// Múi giờ Việt Nam (UTC+7)
const char* TZ_VN = "<+07>-7";
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.google.com";

// ================== Nút BOOT ==================
#define BOOT_PIN 0   // nút BOOT trên board ESP32 (nhấn = LOW)

enum Mode { MODE_IMAGE, MODE_CLOCK };
Mode currentMode = MODE_IMAGE;

int lastSec = -1;
bool lastTimeValid = false;

WebServer server(80);
TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
File uploadFile;

const char* html_page = R"=====(
<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 ST7789 Display</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; background: #121212; color: #fff; margin: 0; padding: 20px; }
    .container { max-width: 400px; margin: auto; background: #1e1e1e; padding: 20px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); }
    h2 { margin-top: 0; color: #4CAF50; }
    canvas { background: #000; border: 2px solid #333; border-radius: 8px; margin: 15px 0; max-width: 100%; box-shadow: 0 0 10px rgba(0,0,0,0.3); }
    input[type="file"] { display: none; }
    input[type="text"], input[type="password"] { width: 100%; box-sizing: border-box; padding: 10px; margin-bottom: 10px; border-radius: 6px; border: 1px solid #444; background: #121212; color: #fff; font-size: 15px; }
    .btn { display: inline-block; background: #007bff; color: white; padding: 12px 20px; border-radius: 6px; cursor: pointer; font-weight: bold; transition: 0.3s; width: 100%; box-sizing: border-box; margin-bottom: 10px; border: none; font-size: 16px; }
    .btn:hover { background: #0056b3; }
    .btn-upload { background: #28a745; }
    .btn-upload:hover { background: #218838; }
    .btn-clock { background: #e67e22; }
    .btn-clock:hover { background: #ca6f1e; }
    #status, #wifiStatus { font-size: 14px; color: #aaa; margin-top: 10px; }
    hr { border: 0; border-top: 1px solid #333; margin: 20px 0; }
  </style>
</head>
<body>
  <div class="container">
    <h2>Gửi Ảnh Lên ESP32</h2>
    <label class="btn" for="fileInput">1. Chọn hình ảnh</label>
    <input type="file" id="fileInput" accept="image/*">
    <br>
    <canvas id="canvas" width="240" height="240"></canvas>
    <br>
    <button class="btn btn-upload" onclick="uploadImage()">2. Truyền lên màn hình</button>
    <div id="status">Chưa chọn ảnh. (Tối ưu cho 240x240)</div>

    <hr>
    <h2 style="color:#e67e22">Đồng hồ (NTP)</h2>
    <button class="btn btn-clock" onclick="setMode('clock')">Hiện đồng hồ trên màn hình</button>
    <button class="btn" onclick="setMode('image')">Quay lại hiện ảnh</button>
    <div style="font-size:12px;color:#777;margin-bottom:12px">Hoặc nhấn nút BOOT trên board để chuyển chế độ.</div>

    <input type="text" id="wifiSsid" placeholder="Tên WiFi nhà (SSID)">
    <input type="password" id="wifiPass" placeholder="Mật khẩu WiFi">
    <button class="btn" onclick="saveWifi()">Lưu WiFi &amp; lấy giờ NTP</button>
    <div id="wifiStatus">Đang kiểm tra...</div>
  </div>

  <script>
    const canvas = document.getElementById('canvas');
    const ctx = canvas.getContext('2d');
    const status = document.getElementById('status');
    let img = new Image();

    document.getElementById('fileInput').addEventListener('change', (e) => {
      const file = e.target.files[0];
      if(!file) return;
      const reader = new FileReader();

      reader.onload = (event) => {
        img.onload = () => {
          ctx.clearRect(0, 0, 240, 240);
          let scale = Math.max(240 / img.width, 240 / img.height);
          let w = img.width * scale;
          let h = img.height * scale;
          let x = (240 - w) / 2;
          let y = (240 - h) / 2;

          ctx.drawImage(img, x, y, w, h);
          status.innerText = "Ảnh đã sẵn sàng!";
        }
        img.src = event.target.result;
      };
      reader.readAsDataURL(file);
    });

    function uploadImage() {
      if(!img.src) {
        alert("Vui lòng chọn ảnh trước!");
        return;
      }
      status.innerText = "Đang chuyển đổi và gửi dữ liệu...";

      const imageData = ctx.getImageData(0, 0, 240, 240).data;
      const rgb565 = new Uint16Array(240 * 240);

      for (let i = 0, j = 0; i < imageData.length; i += 4, j++) {
        let r = imageData[i] >> 3;
        let g = imageData[i+1] >> 2;
        let b = imageData[i+2] >> 3;

        let color = (r << 11) | (g << 5) | b;
        rgb565[j] = (color >> 8) | ((color & 0xFF) << 8);
      }

      const blob = new Blob([rgb565], {type: 'application/octet-stream'});
      const formData = new FormData();
      formData.append("image", blob, "image.bin");

      fetch('/upload', { method: 'POST', body: formData })
        .then(response => {
          if(response.ok) status.innerText = "Gửi thành công!";
          else status.innerText = "Lỗi khi gửi.";
        })
        .catch(err => status.innerText = "Lỗi kết nối tới ESP32.");
    }

    function setMode(m) {
      fetch('/mode?m=' + m).catch(() => {});
    }

    function saveWifi() {
      const ssid = document.getElementById('wifiSsid').value.trim();
      const pass = document.getElementById('wifiPass').value;
      if (!ssid) { alert('Nhập tên WiFi!'); return; }
      document.getElementById('wifiStatus').innerText = 'Đang kết nối...';
      fetch('/wifi', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: new URLSearchParams({ssid: ssid, pass: pass})
      }).catch(() => {});
    }

    function pollWifi() {
      fetch('/wifistatus')
        .then(r => r.json())
        .then(d => {
          let s;
          if (!d.ssid) s = 'Chưa cài WiFi nhà. Nhập WiFi để lấy giờ NTP.';
          else if (!d.connected) s = 'Đang kết nối tới "' + d.ssid + '"...';
          else s = 'Đã kết nối "' + d.ssid + '"';
          if (d.time) s += ' | Giờ ESP32: ' + d.time;
          else if (d.connected) s += ' | Đang chờ giờ NTP...';
          document.getElementById('wifiStatus').innerText = s;
        })
        .catch(() => {});
    }
    setInterval(pollWifi, 2000);
    pollWifi();
  </script>
</body>
</html>
)=====";

// ======================================================
//                    THỜI GIAN / WIFI NHÀ
// ======================================================
bool getLocalTimeNow(struct tm &t) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // chưa có giờ hợp lệ
  localtime_r(&now, &t);
  return true;
}

void startSta(const String& ssid, const String& pass) {
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.c_str());
  configTzTime(TZ_VN, NTP_SERVER1, NTP_SERVER2);  // tự đồng bộ NTP khi có mạng
}

// ======================================================
//                    ĐỒNG HỒ SỐ
// ======================================================
void drawClockScreen() {
  tft.fillScreen(TFT_BLACK);
  lastSec = -1;
  lastTimeValid = false;
}

void updateClock() {
  struct tm t;
  bool valid = getLocalTimeNow(t);

  if (!valid) {
    if (lastSec != -2) {
      tft.fillScreen(TFT_BLACK);
      tft.setTextSize(1);
      tft.setTextDatum(MC_DATUM);
      tft.setTextPadding(0);
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.drawString("Cho gio NTP...", 120, 100, 4);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("Mo web de cai WiFi nha", 120, 140, 2);
      lastSec = -2;
      lastTimeValid = false;
    }
    return;
  }

  // Vừa có giờ lần đầu: xoá thông báo
  if (!lastTimeValid) {
    tft.fillScreen(TFT_BLACK);
    lastTimeValid = true;
    lastSec = -1;
  }

  if (t.tm_sec == lastSec) return;
  lastSec = t.tm_sec;

  char buf[24];

  // Giờ:phút:giây (font 4 phóng to x2)
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.setTextPadding(236);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(buf, 120, 105, 4);


}

// ======================================================
//                    ẢNH / MÀN HÌNH CHỜ
// ======================================================
void drawImageFromFS() {
  if (!LittleFS.exists("/image.bin")) {
    Serial.println("Chưa có ảnh lưu trong bộ nhớ.");
    return;
  }

  File file = LittleFS.open("/image.bin", "r");
  if (!file) {
    Serial.println("Không thể mở file ảnh.");
    return;
  }

  tft.setAddrWindow(0, 0, 240, 240);

  const size_t CHUNK_SIZE = 512;
  uint8_t buffer[CHUNK_SIZE];

  tft.startWrite();
  while (file.available()) {
    size_t bytesRead = file.read(buffer, CHUNK_SIZE);
    if (bytesRead > 0) {
      tft.pushColors((uint16_t*)buffer, bytesRead / 2, false);
    }
  }
  tft.endWrite();
  file.close();
}

void showInfoScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("Ket noi WiFi:", 10, 20);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(ap_ssid, 10, 50);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Mat khau:", 10, 90);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(ap_password, 10, 120);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Truy cap IP:", 10, 160);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(WiFi.softAPIP().toString(), 10, 190);
  tft.setTextSize(1);
}

void showImageOrInfo() {
  if (LittleFS.exists("/image.bin")) {
    drawImageFromFS();
  } else {
    showInfoScreen();
  }
}

// ======================================================
//                    CHUYỂN CHẾ ĐỘ
// ======================================================
void setMode(Mode m) {
  currentMode = m;
  if (m == MODE_CLOCK) {
    drawClockScreen();
    updateClock();
  } else {
    showImageOrInfo();
  }
}

void checkBootButton() {
  static bool lastState = HIGH;
  static unsigned long lastChangeMs = 0;

  bool state = digitalRead(BOOT_PIN);
  if (state != lastState && millis() - lastChangeMs > 50) {  // chống rung
    lastChangeMs = millis();
    lastState = state;
    if (state == LOW) {  // vừa nhấn
      setMode(currentMode == MODE_IMAGE ? MODE_CLOCK : MODE_IMAGE);
    }
  }
}

// ======================================================
//                    WEB HANDLERS
// ======================================================
void handleUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Bat dau nhan anh...\n");
    uploadFile = LittleFS.open("/image.bin", FILE_WRITE);

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
    }
    Serial.printf("Hoan tat nhan anh! Dung luong: %u bytes\n", upload.totalSize);

    currentMode = MODE_IMAGE;  // nhận ảnh mới thì chuyển về chế độ ảnh
    drawImageFromFS();
  }
}

void handleMode() {
  setMode(server.arg("m") == "clock" ? MODE_CLOCK : MODE_IMAGE);
  server.send(200, "text/plain", "OK");
}

void handleWifiSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() == 0) {
    server.send(400, "text/plain", "Missing ssid");
    return;
  }

  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  Serial.printf("Luu WiFi nha: %s\n", ssid.c_str());
  server.send(200, "text/plain", "OK");
  startSta(ssid, pass);
}

String jsonEscape(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

void handleWifiStatus() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  prefs.end();

  String timeStr = "";
  struct tm t;
  if (getLocalTimeNow(t)) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d %02d/%02d/%04d",
             t.tm_hour, t.tm_min, t.tm_sec, t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    timeStr = buf;
  }

  String json = "{\"ssid\":\"" + jsonEscape(ssid) + "\",";
  json += "\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"time\":\"" + timeStr + "\"}";
  server.send(200, "application/json", json);
}

// ======================================================
//                    SETUP / LOOP
// ======================================================
void setup() {
  Serial.begin(115200);
  pinMode(BOOT_PIN, INPUT_PULLUP);

  if (!LittleFS.begin(true)) {
    Serial.println("Lỗi khởi tạo LittleFS. Đang format...");
  }

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  setenv("TZ", TZ_VN, 1);
  tzset();

  Serial.println("Dang khoi tao WiFi...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.softAP(ap_ssid, ap_password);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  // Nếu đã lưu WiFi nhà từ lần trước thì tự kết nối + lấy giờ NTP
  prefs.begin("wifi", true);
  String savedSsid = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");
  prefs.end();
  if (savedSsid.length() > 0) {
    Serial.printf("Ket noi WiFi nha: %s\n", savedSsid.c_str());
    startSta(savedSsid, savedPass);
  }

  showImageOrInfo();

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", html_page); });
  server.on("/upload", HTTP_POST, []() { server.send(200, "text/plain", "Upload OK"); }, handleUpload);
  server.on("/mode", HTTP_GET, handleMode);
  server.on("/wifi", HTTP_POST, handleWifiSave);
  server.on("/wifistatus", HTTP_GET, handleWifiStatus);

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  checkBootButton();

  if (currentMode == MODE_CLOCK) {
    updateClock();
  }
}