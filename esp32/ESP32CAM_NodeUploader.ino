/*********
  ESP32-CAM (AI Thinker)
  Routes:
    /                 → HTML (ปุ่ม Capture พรีวิว + Upload ไป Node backend)
    /stream           → MJPEG stream (อัปเดตแคชเฟรมล่าสุด)
    /capture          → ส่ง JPEG เดี่ยว (ดึงจากแคช → เร็วมาก; ถ้าไม่มีแคชค่อยถ่ายใหม่)
    /capture-upload   → ESP จับภาพแล้วอัปขึ้น Node backend: /api/upload?filename=...
    /health           → JSON ok:true (เช็คเร็ว)
*********/
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_http_server.h"
#include <memory>   // std::unique_ptr
#include <cstring>  // memcpy

// ===== FreeRTOS (สำหรับ mutex แคชเฟรม) =====
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ===== Wi-Fi =====
const char* ssid     = "satulong";
const char* password = "shitangme";

// ===== Node backend base (พีซีที่รัน server.js) =====
const char* API_BASE = "http://10.48.11.71:3000";  // <-- แก้เป็น IP พีซีคุณถ้าไม่ตรง

// ===== Camera model: AI Thinker =====
#define CAMERA_MODEL_AI_THINKER
#if defined(CAMERA_MODEL_AI_THINKER)
  #define PWDN_GPIO_NUM     32
  #define RESET_GPIO_NUM    -1
  #define XCLK_GPIO_NUM      0
  #define SIOD_GPIO_NUM     26
  #define SIOC_GPIO_NUM     27
  #define Y9_GPIO_NUM       35
  #define Y8_GPIO_NUM       34
  #define Y7_GPIO_NUM       39
  #define Y6_GPIO_NUM       36
  #define Y5_GPIO_NUM       21
  #define Y4_GPIO_NUM       19
  #define Y3_GPIO_NUM       18
  #define Y2_GPIO_NUM        5
  #define VSYNC_GPIO_NUM    25
  #define HREF_GPIO_NUM     23
  #define PCLK_GPIO_NUM     22
#else
  #error "Select CAMERA_MODEL_AI_THINKER"
#endif

// ===== Stream constants =====
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* CT_STREAM = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* BOUNDARY  = "\r\n--" PART_BOUNDARY "\r\n";
static const char* PART_HDR  = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t httpd = NULL;

// ====== แคชเฟรมล่าสุดจากสตรีม (สำหรับ /capture ให้เร็ว) ======
static uint8_t* g_last_jpg = NULL;
static size_t   g_last_len = 0;
static SemaphoreHandle_t g_last_mtx = NULL;

// ---------- Helpers ----------
static inline void add_cors(httpd_req_t* req){
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}
static esp_err_t options_handler(httpd_req_t* req){
  add_cors(req);
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, NULL, 0);
}
static String ipStr(){ return WiFi.localIP().toString(); }

// ---------- Index (HTML) ----------
static esp_err_t index_handler(httpd_req_t* req) {
  add_cors(req);
  String html;
  html.reserve(6000);
  html += F("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>");
  html += F("<title>ESP32-CAM</title><style>body{margin:0;background:#111;color:#fff;font:14px system-ui}"
            "header{padding:12px 16px;background:#000;font-weight:700}"
            ".c{padding:16px}.row{display:flex;gap:8px;flex-wrap:wrap;margin:8px 0}"
            ".btn{border:1px solid #333;background:#222;color:#fff;border-radius:10px;padding:8px 12px;cursor:pointer}"
            "img{width:100%;max-height:70vh;object-fit:contain;background:#000;border-radius:12px}"
            "code{background:#000;padding:2px 6px;border-radius:6px;border:1px solid #222}"
            "small{color:#9aa}"
            "</style>");
  html += "<header>ESP32-CAM</header><div class=c>";
  html += "IP: <code>" + ipStr() + "</code> • Stream: <code>/stream</code> • API_BASE: <code>";
  html += API_BASE; html += "</code><br><br>";
  html += F("<div class=row>"
            "<button class=btn id=cap>Capture (Preview)</button>"
            "<button class=btn id=up>Upload to Server</button>"
            "<a class=btn id=dl href='#' download='snapshot.jpg' style='display:none'>Download last</a>"
            "</div>");
  html += F("<img id=s alt='stream' src='/stream'>"
            "<div style='margin-top:10px'><img id=shot style='max-height:40vh'></div>"
            "<small>* /capture จะดึงเฟรมล่าสุดจากสตรีม จึงเร็วขึ้นมาก ถ้ายังไม่ได้เปิดสตรีมจะถ่ายใหม่ให้อัตโนมัติ</small>"
            "<script>"
            "const $=s=>document.querySelector(s);"
            "cap.onclick=async()=>{"
              "try{const r=await fetch('/capture?ts='+Date.now(),{cache:'no-store'});"
              "if(!r.ok) throw new Error('capture failed');"
              "const b=await r.blob();const u=URL.createObjectURL(b);"
              "shot.src=u;dl.href=u;dl.style.display='inline-block';}catch(e){alert(e.message);} "
            "};"
            "up.onclick=async()=>{"
              "try{const name='shot_'+Date.now()+'.jpg';"
              "const r=await fetch('/capture-upload?filename='+encodeURIComponent(name),{method:'POST'});"
              "const j=await r.json(); if(!j.ok) throw new Error(j.err||'upload failed');"
              "alert('Uploaded: '+(j.url||name)); if(j.url) window.open(j.url,'_blank'); }catch(e){alert(e.message);} "
            "};"
            "</script></div>");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html.c_str(), HTTPD_RESP_USE_STRLEN);
}

// ---------- Stream: /stream (อัปเดตแคชเฟรมล่าสุด) ----------
static esp_err_t stream_handler(httpd_req_t *req){
  add_cors(req);
  camera_fb_t * fb = NULL;
  esp_err_t res = httpd_resp_set_type(req, CT_STREAM);
  if(res != ESP_OK) return res;

  size_t jpg_len = 0;
  uint8_t *jpg = NULL;
  char part[64];

  while(true){
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else {
      if (fb->format != PIXFORMAT_JPEG){
        bool ok = frame2jpg(fb, 80, &jpg, &jpg_len);
        esp_camera_fb_return(fb); fb = NULL;
        if(!ok) res = ESP_FAIL;
      } else {
        jpg = fb->buf; jpg_len = fb->len;
      }
    }

    // อัปเดตแคชสำหรับ /capture ให้ไว
    if (res == ESP_OK && jpg && jpg_len > 0 && g_last_mtx && xSemaphoreTake(g_last_mtx, 0) == pdTRUE) {
      uint8_t* tmp = (uint8_t*)malloc(jpg_len);
      if (tmp) {
        memcpy(tmp, jpg, jpg_len);
        free(g_last_jpg);
        g_last_jpg = tmp;
        g_last_len = jpg_len;
      }
      xSemaphoreGive(g_last_mtx);
    }

    if(res == ESP_OK){
      size_t hlen = snprintf(part, sizeof(part), PART_HDR, (unsigned)jpg_len);
      if (httpd_resp_send_chunk(req, part, hlen) != ESP_OK) res = ESP_FAIL;
    }
    if(res == ESP_OK){
      if (httpd_resp_send_chunk(req, (const char*)jpg, jpg_len) != ESP_OK) res = ESP_FAIL;
    }
    if(res == ESP_OK){
      if (httpd_resp_send_chunk(req, BOUNDARY, strlen(BOUNDARY)) != ESP_OK) res = ESP_FAIL;
    }

    if (fb){ esp_camera_fb_return(fb); fb = NULL; jpg = NULL; }
    else if (jpg && fb==NULL){ free(jpg); jpg = NULL; }

    if(res != ESP_OK) break;
  }
  return res;
}

// ---------- Single JPEG: /capture (ดึงจากแคชก่อน) ----------
static esp_err_t capture_handler(httpd_req_t *req){
  add_cors(req);

  // 1) ส่งเฟรมล่าสุดจากแคชก่อน (เร็วมาก)
  if (g_last_mtx && xSemaphoreTake(g_last_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (g_last_jpg && g_last_len > 0) {
      size_t len = g_last_len;
      uint8_t* copy = (uint8_t*)malloc(len);
      if (copy) {
        memcpy(copy, g_last_jpg, len);
        xSemaphoreGive(g_last_mtx);

        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t r = httpd_resp_send(req, (const char*)copy, len);
        free(copy);
        return r;
      }
    }
    xSemaphoreGive(g_last_mtx);
  }

  // 2) ถ้าไม่มีแคช (ยังไม่เปิดสตรีม) ค่อยถ่ายใหม่
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }

  uint8_t* jpg = nullptr; size_t jpg_len = 0;
  if (fb->format != PIXFORMAT_JPEG) {
    if(!frame2jpg(fb, 80, &jpg, &jpg_len)){ esp_camera_fb_return(fb); httpd_resp_send_500(req); return ESP_FAIL; }
  } else { jpg = fb->buf; jpg_len = fb->len; }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t res = httpd_resp_send(req, (const char*)jpg, jpg_len);

  if (fb->format != PIXFORMAT_JPEG && jpg) free(jpg);
  esp_camera_fb_return(fb);
  return res;
}

// ---------- Upload helper: POST raw JPEG to API_BASE/api/upload?filename=... ----------
static bool upload_to_api(const uint8_t* buf, size_t len, String filename, String &respOut){
  if (!buf || !len) { Serial.println("[UP] no buffer"); return false; }
  if (filename.length() == 0) filename = String("shot_") + String((uint32_t)millis()) + ".jpg";
  String url = String(API_BASE) + "/api/upload?filename=" + filename;

  HTTPClient http;
  WiFiClient client;
  http.setTimeout(15000);
  http.setReuse(false);                    // ไม่ reuse ซ็อกเก็ต (กันค้าง)

  Serial.printf("[UP] POST %s (len=%u)\n", url.c_str(), (unsigned)len);
  if (!http.begin(client, url)) { Serial.println("[UP] begin() failed"); return false; }

  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("Connection", "close");   // ปิด keep-alive

  int code = http.POST((uint8_t*)buf, len);
  respOut = http.getString();
  http.end();

  Serial.printf("[UP] HTTP %d, resp=%s\n", code, respOut.c_str());
  return (code == 200 || code == 201);
}

// ---------- Health: /health ----------
static esp_err_t health_handler(httpd_req_t* req){
  add_cors(req);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// ---------- /capture-upload : ESP จับภาพแล้วอัปขึ้น Node backend ----------
static esp_err_t capture_upload_handler(httpd_req_t *req){
  add_cors(req);
  Serial.println("[CU] /capture-upload requested");

  // ดึง filename จาก query
  char name[64] = {0};
  String filename;
  int qlen = httpd_req_get_url_query_len(req);
  if (qlen > 0){
    std::unique_ptr<char[]> q(new char[qlen+1]);
    if (httpd_req_get_url_query_str(req, q.get(), qlen+1) == ESP_OK){
      if (httpd_query_key_value(q.get(), "filename", name, sizeof(name)) == ESP_OK){
        filename = String(name);
      }
    }
  }

  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[CU] no_frame");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"no_frame\"}");
  }

  uint8_t* jpg = nullptr; size_t jpg_len = 0;
  bool need_free = false;
  if (fb->format != PIXFORMAT_JPEG) {
    if(!frame2jpg(fb, 80, &jpg, &jpg_len)){
      esp_camera_fb_return(fb);
      httpd_resp_set_type(req,"application/json");
      return httpd_resp_sendstr(req,"{\"ok\":false,\"err\":\"jpeg_fail\"}");
    }
    need_free = true;
  } else { jpg = fb->buf; jpg_len = fb->len; }

  String resp; bool ok = upload_to_api(jpg, jpg_len, filename, resp);
  Serial.printf("[CU] upload_to_api ok=%d, resp=%s\n", ok?1:0, resp.c_str());

  if (need_free && jpg) free(jpg);
  esp_camera_fb_return(fb);

  httpd_resp_set_type(req, "application/json");
  if (!ok) return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"post_fail\"}");
  if (resp.length()) return httpd_resp_send(req, resp.c_str(), resp.length());
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// ---------- Server start ----------
static void startServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  if (httpd_start(&httpd, &config) == ESP_OK) {
    httpd_uri_t u_index    = { .uri="/",               .method=HTTP_GET,     .handler=index_handler,           .user_ctx=NULL };
    httpd_uri_t u_stream   = { .uri="/stream",         .method=HTTP_GET,     .handler=stream_handler,          .user_ctx=NULL };
    httpd_uri_t u_capture  = { .uri="/capture",        .method=HTTP_GET,     .handler=capture_handler,         .user_ctx=NULL };
    httpd_uri_t u_cup_post = { .uri="/capture-upload", .method=HTTP_POST,    .handler=capture_upload_handler,  .user_ctx=NULL };
    httpd_uri_t u_cup_get  = { .uri="/capture-upload", .method=HTTP_GET,     .handler=capture_upload_handler,  .user_ctx=NULL }; // ไว้เทสง่าย (GET ก็ได้)
    httpd_uri_t u_health   = { .uri="/health",         .method=HTTP_GET,     .handler=health_handler,          .user_ctx=NULL };
    httpd_uri_t opt1       = { .uri="/capture",        .method=HTTP_OPTIONS, .handler=options_handler,         .user_ctx=NULL };
    httpd_uri_t opt2       = { .uri="/capture-upload", .method=HTTP_OPTIONS, .handler=options_handler,         .user_ctx=NULL };

    httpd_register_uri_handler(httpd, &u_health);
    httpd_register_uri_handler(httpd, &u_index);
    httpd_register_uri_handler(httpd, &u_stream);
    httpd_register_uri_handler(httpd, &u_capture);
    httpd_register_uri_handler(httpd, &u_cup_post);
    httpd_register_uri_handler(httpd, &u_cup_get);
    httpd_register_uri_handler(httpd, &opt1);
    httpd_register_uri_handler(httpd, &opt2);
  }
}

// ---------- Camera init (เริ่มแบบเบาให้ติดง่าย) ----------
static bool initCamera(){
  camera_config_t c;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;    c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()){
    c.frame_size = FRAMESIZE_QVGA;    // ถ้าจะให้ไวขึ้นอีก: เปลี่ยนเป็น FRAMESIZE_QQVGA
    c.jpeg_quality = 20;              // ถ้าจะให้ไวขึ้นอีก: เพิ่มเป็น 25-30
    c.fb_count = 2;
  } else {
    c.frame_size = FRAMESIZE_QQVGA;
    c.jpeg_quality = 30;
    c.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    // ถ้ายังไม่ผ่าน ลองลด XCLK ลง 10 MHz
    c.xclk_freq_hz = 10000000;
    err = esp_camera_init(&c);
  }
  return (err == ESP_OK);
}

void setup(){
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // ปิด brownout
  Serial.begin(115200);

  WiFi.begin(ssid, password);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED){ delay(300); Serial.print("."); }
  Serial.println(); Serial.print("IP: "); Serial.println(ipStr());
  Serial.print("API_BASE: "); Serial.println(API_BASE);

  // สร้าง mutex สำหรับแคชเฟรม
  g_last_mtx = xSemaphoreCreateMutex();

  if (!initCamera()){
    Serial.println("Camera init failed, restarting...");
    delay(2000); ESP.restart();
  }

  startServer();
  Serial.println("Open: http://<above IP>/  (Stream=/stream, Capture=/capture, Upload=/capture-upload)");
}

void loop(){ delay(1); }
