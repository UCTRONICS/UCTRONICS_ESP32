#include "esp_http_server.h"
#include "esp_timer.h"
#include "ucbot_camera.h"
#include "ucbot_img_converters.h"
#include "Arduino.h"

#include "fb_gfx.h"
#include "dl_lib.h"

#include "Wire.h"
#include "I2C_slave.h"

byte wrSensorReg8_8(uint8_t sensor_addr, int regID, int regDat)
{
    Wire.beginTransmission(sensor_addr);
    Wire.write(regID & 0x00FF);
    Wire.write(regDat & 0x00FF);
    if (Wire.endTransmission())
    {
      return 0;
    }
  return 1;
}
byte rdSensorReg8_8(uint8_t sensor_addr, uint8_t regID, uint8_t* regDat)
{ 
    Wire.beginTransmission(sensor_addr);
    Wire.write(regID & 0x00FF);
    Wire.endTransmission();
  
    Wire.requestFrom((sensor_addr), 1);
    if (Wire.available())
      *regDat = Wire.read();
  return 1;
}typedef struct {
        size_t size; //number of values used for filtering
        size_t index; //current value index
        size_t count; //value count
        int sum;
        int * values; //array to be filled with values
} ra_filter_t;

typedef struct {
        httpd_req_t *req;
        size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static ra_filter_t ra_filter;
httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

uint8_t follow_mode = 0;
uint8_t avoid_mode = 0; 

static ra_filter_t * ra_filter_init(ra_filter_t * filter, size_t sample_size){
    memset(filter, 0, sizeof(ra_filter_t));

    filter->values = (int *)malloc(sample_size * sizeof(int));
    if(!filter->values){
        return NULL;
    }
    memset(filter->values, 0, sample_size * sizeof(int));

    filter->size = sample_size;
    return filter;
}

static int ra_filter_run(ra_filter_t * filter, int value){
    if(!filter->values){
        return value;
    }
    filter->sum -= filter->values[filter->index];
    filter->values[filter->index] = value;
    filter->sum += filter->values[filter->index];
    filter->index++;
    filter->index = filter->index % filter->size;
    if (filter->count < filter->size) {
        filter->count++;
    }
    return filter->sum / filter->count;
}



static size_t jpg_encode_stream(void * arg, size_t index, const void* data, size_t len){
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if(!index){
        j->len = 0;
    }
    if(httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK){
        return 0;
    }
    j->len += len;
    return len;
}

static esp_err_t capture_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    int64_t fr_start = esp_timer_get_time();

    fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

  size_t fb_len = 0;
  if(fb->format == PIXFORMAT_JPEG){
    fb_len = fb->len;
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  } else {
    jpg_chunking_t jchunk = {req, 0};
    res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk)?ESP_OK:ESP_FAIL;
    httpd_resp_send_chunk(req, NULL, 0);
    fb_len = jchunk.len;
  }
  esp_camera_fb_return(fb);
  int64_t fr_end = esp_timer_get_time();
  Serial.printf("JPG: %uB %ums\n", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start)/1000));
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char * part_buf[64];

    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Camera capture failed");
            res = ESP_FAIL;
        } else {
                if(fb->format != PIXFORMAT_JPEG){
                    bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                    esp_camera_fb_return(fb);
                    fb = NULL;
                    if(!jpeg_converted){
                        Serial.println("JPEG compression failed");
                        res = ESP_FAIL;
                    }
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
            break;
        }
        int64_t fr_end = esp_timer_get_time();
       
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
    }
    last_frame = 0;
    return res;
}

static esp_err_t cmd_handler(httpd_req_t *req){
    char*  buf;
    size_t buf_len;
    char variable[32] = {0,};
    char value[32] = {0,};

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char*)malloc(buf_len);
        if(!buf){
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
                httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
            } else {
                free(buf);
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
        } else {
            free(buf);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        free(buf);
    } else {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    int val = atoi(value);
    sensor_t * s = esp_camera_sensor_get();
    int res = 0;
    Serial.println(variable);
    Serial.println(val, DEC);
    if(!strcmp(variable, "motor_speed")) res = wrSensorReg8_8(STM32_1_SLAVE_ADDR, SPEED_REG, val);
    else if(!strcmp(variable, "motor_direction")) res = wrSensorReg8_8(STM32_1_SLAVE_ADDR, MOTOR_REG, val);
    else if(!strcmp(variable, "motor_calibration")) res = wrSensorReg8_8(STM32_1_SLAVE_ADDR, MOTOR_CAL_REG, val);
    else if(!strcmp(variable, "led_r")) {
      res = wrSensorReg8_8(STM32_2_SLAVE_ADDR, LED_R_REG, val);
      res = wrSensorReg8_8(STM32_2_SLAVE_ADDR, LED_SET_REG, 1);
    }
    else if(!strcmp(variable, "led_g")){
      res = wrSensorReg8_8(STM32_2_SLAVE_ADDR, LED_G_REG, val);
      res = wrSensorReg8_8(STM32_2_SLAVE_ADDR, LED_SET_REG, 1);
    }
    else if(!strcmp(variable, "led_b")){
      res = wrSensorReg8_8(STM32_2_SLAVE_ADDR, LED_B_REG, val);
      res = wrSensorReg8_8(STM32_2_SLAVE_ADDR, LED_SET_REG, 1);
    }
    else if(!strcmp(variable, "note")) res = wrSensorReg8_8(STM32_2_SLAVE_ADDR, NOTE_REG, val);
    else if(!strcmp(variable, "music")) res = wrSensorReg8_8(STM32_2_SLAVE_ADDR, SONG_REG, val);
    else if(!strcmp(variable, "avoid_mode")) {
      avoid_mode = val;
      wrSensorReg8_8(STM32_2_SLAVE_ADDR, AVOID_REG, val);
      wrSensorReg8_8(STM32_1_SLAVE_ADDR, AVOID_REG, val);  
    }
    else if(!strcmp(variable, "follow_mode")) {
      wrSensorReg8_8(STM32_2_SLAVE_ADDR, FOLLOW_REG, val);
      wrSensorReg8_8(STM32_1_SLAVE_ADDR, FOLLOW_REG, val);
      follow_mode = val;
      if(val){
          wrSensorReg8_8(STM32_2_SLAVE_ADDR, ULTRA_DIS_REG, 9);  
      }
      else{
          wrSensorReg8_8(STM32_2_SLAVE_ADDR, ULTRA_DIS_REG, 15);  
      }
    }
    else if(!strcmp(variable, "trace_mode")){
      res = wrSensorReg8_8(STM32_1_SLAVE_ADDR, TRACE_REG, val);
      if(val){
          res = wrSensorReg8_8(STM32_2_SLAVE_ADDR, TCS_LED_SWITCH, val);
        }
      else{
           res = wrSensorReg8_8(STM32_2_SLAVE_ADDR, TCS_LED_SWITCH, 2);  
          }
      }
    else {
        res = -1;
    }

    if(res){
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t index_handler(httpd_req_t *req){
    char req_data[] = "Access!";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, (const char *)req_data, sizeof(req_data)+1);
}

void startCameraServer(){
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    Wire.begin();
    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t cmd_uri = {
        .uri       = "/control",
        .method    = HTTP_GET,
        .handler   = cmd_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t capture_uri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };

   httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

    ra_filter_init(&ra_filter, 20);
    
    Serial.printf("Starting web server on port: '%d'\n", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
    }

    config.server_port = 2001;
    config.ctrl_port += 1;
    Serial.printf("Starting stream server on port: '%d'\n", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
    wrSensorReg8_8(STM32_2_SLAVE_ADDR, TCS_STATUS, 1);
}
