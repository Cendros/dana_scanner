// Original Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <esp_http_server.h>
#include <esp_timer.h>
#include <esp_camera.h>
#include <esp_int_wdt.h>
#include <esp_task_wdt.h>
#include <Arduino.h>
#include <WiFi.h>

#include "index_other.h"
#include "css.h"
#include "src/favicons.h"
#include "src/logo.h"
#include "storage.h"

// Functions from the main .ino
extern void flashLED(int flashtime);
extern void setLamp(int newVal);
extern void printLocalTime(bool extraData);

// External variables declared in the main .ino
extern char myName[];
extern char myVer[];
extern char baseVersion[];
extern IPAddress ip;
extern IPAddress net;
extern IPAddress gw;
extern bool accesspoint;
extern char apName[];
extern bool captivePortal;
extern int streamPort;
extern char streamURL[];
extern char default_index[];
extern int8_t streamCount;
extern unsigned long streamsServed;
extern unsigned long imagesServed;
extern int minFrameTime;
extern int lampVal;
extern bool autoLamp;
extern bool filesystem;
extern String critERR;
extern bool debugData;
extern bool haveTime;
extern int sketchSize;
extern int sketchSpace;
extern String sketchMD5;
extern bool otaEnabled;
extern char otaPassword[];
extern unsigned long xclk;
extern int sensorPID;

typedef struct {
        httpd_req_t *req;
        size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;

// Flag that can be set to kill all active streams
bool streamKill;

#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();
#ifdef __cplusplus
}
#endif

static esp_err_t stream_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char * part_buf[64];

    streamKill = false;

    Serial.println("Stream requested");
    if (autoLamp && (lampVal != -1)) setLamp(lampVal);
    streamCount = 1;  // at present we only have one stream handler, so values are 0 or 1..
    flashLED(75);     // double flash of status LED
    delay(75);
    flashLED(75);

    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        streamCount = 0;
        if (autoLamp && (lampVal != -1)) setLamp(0);
        Serial.println("STREAM: failed to set HTTP response type");
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if(res == ESP_OK){
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("STREAM: failed to acquire frame");
            res = ESP_FAIL;
        } else {
            if(fb->format != PIXFORMAT_JPEG){
                Serial.println("STREAM: Non-JPEG frame returned by camera module");
                res = ESP_FAIL;
            } else {
                _jpg_buf_len = fb->len;
                _jpg_buf = fb->buf;
            }
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(fb){
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        } else if(_jpg_buf){
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if(res != ESP_OK){
            // This is the error exit point from the stream loop.
            // We end the stream here only if a Hard failure has been encountered or the connection has been interrupted.
            Serial.printf("Stream failed, code = %i : %s\r\n", res, esp_err_to_name(res));
            break;
        }
        if((res != ESP_OK) || streamKill){
            // We end the stream here when a kill is signalled.
            Serial.printf("Stream killed\r\n");
            break;
        }
        int64_t frame_time = esp_timer_get_time() - last_frame;
        frame_time /= 1000;
        int32_t frame_delay = (minFrameTime > frame_time) ? minFrameTime - frame_time : 0;
        delay(frame_delay);

        if (debugData) {
            Serial.printf("MJPG: %uB %ums, delay: %ums, framerate (%.1ffps)\r\n",
                (uint32_t)(_jpg_buf_len),
                (uint32_t)frame_time, frame_delay, 1000.0 / (uint32_t)(frame_time + frame_delay));
        }
        last_frame = esp_timer_get_time();
    }

    streamsServed++;
    streamCount = 0;
    if (autoLamp && (lampVal != -1)) setLamp(0);
    Serial.println("Stream ended");
    last_frame = 0;
    return res;
}

static esp_err_t info_handler(httpd_req_t *req){
    static char json_response[256];
    char * p = json_response;
    *p++ = '{';
    p+=sprintf(p, "\"cam_name\":\"%s\",", myName);
    p+=sprintf(p, "\"stream_url\":\"%s\"", streamURL);
    *p++ = '}';
    *p++ = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}


static esp_err_t error_handler(httpd_req_t *req){
    flashLED(75);
    Serial.println("Sending error page");
    std::string s(error_html);
    size_t index;
    while ((index = s.find("<APPURL>")) != std::string::npos)
    while ((index = s.find("<CAMNAME>")) != std::string::npos)
        s.replace(index, strlen("<CAMNAME>"), myName);
    while ((index = s.find("<ERRORTEXT>")) != std::string::npos)
        s.replace(index, strlen("<ERRORTEXT>"), critERR.c_str());
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "identity");
    return httpd_resp_send(req, (const char *)s.c_str(), s.length());
}

void startCameraServer(int sPort){
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16; // we use more than the default 8 (on port 80)

    httpd_uri_t stream_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };
    httpd_uri_t info_uri = {
        .uri       = "/info",
        .method    = HTTP_GET,
        .handler   = info_handler,
        .user_ctx  = NULL
    };
    httpd_uri_t error_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = error_handler,
        .user_ctx  = NULL
    };

    config.server_port = sPort;
    config.ctrl_port = sPort;
    Serial.printf("Starting stream server on port: '%d'\r\n", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        if (critERR.length() > 0) {
            httpd_register_uri_handler(stream_httpd, &error_uri);
        } else {
            httpd_register_uri_handler(stream_httpd, &stream_uri);
            httpd_register_uri_handler(stream_httpd, &info_uri);
        }
    }
}
