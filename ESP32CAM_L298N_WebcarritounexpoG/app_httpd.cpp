#include <esp32-hal-ledc.h>
#include "fb_gfx.h"

int speed = 255;  
int noStop = 0;

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "Arduino.h"

extern int ENR;
extern int ENL;
extern int gpLb;
extern int gpLf;
extern int gpRb;
extern int gpRf;
extern int gpLed;

extern int distanciaActual; 
extern int leerDistancia();

void WheelAct(int speed_R, int speed_L, int nLf, int nLb, int nRf, int nRb);

typedef struct {
        httpd_req_t *req;
        size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

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

    size_t out_len, out_width, out_height;
    uint8_t * out_buf;
    bool s;
    {
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

    bool image_matrix = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
    if (!image_matrix) {
        esp_camera_fb_return(fb);
        Serial.println("dl_matrix3du_alloc failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    out_len = fb->width * fb->height * 3;
    out_width = fb->width;
    out_height = fb->height;
    out_buf = (uint8_t*)malloc(out_len);

    s = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
    esp_camera_fb_return(fb);
    if(!s){
        free(out_buf);
        Serial.println("to rgb888 failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    jpg_chunking_t jchunk = {req, 0};
    s = fmt2jpg_cb(out_buf, out_len, out_width, out_height, PIXFORMAT_RGB888, 90, jpg_encode_stream, &jchunk);
    free(out_buf);
    if(!s){
        Serial.println("JPEG compression failed");
        return ESP_FAIL;
    }

    int64_t fr_end = esp_timer_get_time();
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
             {
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
        Serial.printf("MJPG: %uB %ums (%.1ffps)\r\n",
            (uint32_t)(_jpg_buf_len),
            (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time           
        );
    }

    last_frame = 0;
    return res;
}

enum state {fwd,rev,stp};
state actstate = stp;

static esp_err_t cmd_handler(httpd_req_t *req)
{
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
    //Serial.println(variable);
    if(!strcmp(variable, "framesize")) 
    {
        Serial.println("framesize");
        if(s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)val);
    }
    else if(!strcmp(variable, "quality")) 
    {
      Serial.println("quality");
      res = s->set_quality(s, val);
    }
    //Remote Control Car 
    //Don't use channel 1 and channel 2
    else if(!strcmp(variable, "flash")) 
    {
      ledcWrite(7,val);
    }  
    else if(!strcmp(variable, "speed")) 
    {
      if      (val > 255) val = 255;
      else if (val <   0) val = 0;       
      speed = val;
    }     
    else if(!strcmp(variable, "nostop")) 
    {
      noStop = val;
      Serial.println(noStop);
    }             
    else if(!strcmp(variable, "servo")) // 3250, 4875, 6500
    {
      if      (val > 650) val = 650; //650
      else if (val < 225) val = 325; //325      
      //ledcWrite(8,10*val);
    }     
    else if(!strcmp(variable, "car")) 
    {  
      if (val==1) 
      {

        int dist = leerDistancia();
    
    if (dist > 15) { // Si hay más de 15cm de espacio
        WheelAct(speed, speed, 1, 0, 1, 0); 
        //Serial.printf("Avanzando... Distancia: %d cm\n", dist);
    } else {
        WheelAct(0, 0, 0, 0, 0, 0); // Freno automático
        //Serial.println("¡ALERTA! Obstáculo detectado. Avance bloqueado.");
    }
        /*
        Serial.println("Forward");
        actstate = fwd;
        WheelAct(speed, speed, HIGH, LOW, HIGH, LOW);
        //httpd_resp_set_type(req, "text/html");
        //return httpd_resp_send(req, "OK", 2);
        */
      }
      else if (val==2) 
      {   
        Serial.println("TurnRight");
        WheelAct(speed, speed, LOW, HIGH, HIGH, LOW);
        //httpd_resp_set_type(req, "text/html");
        //return httpd_resp_send(req, "OK", 2);
      }
      else if (val==3) 
      {
        Serial.println("Stop"); 
        actstate = stp;       
        WheelAct(0, 0, LOW, LOW, LOW, LOW);
        //httpd_resp_set_type(req, "text/html");
        //return httpd_resp_send(req, "OK", 2); 
      }
      else if (val==4) 
      {
        Serial.println("TurnLeft");
        WheelAct(speed, speed, HIGH, LOW, LOW, HIGH);
        //httpd_resp_set_type(req, "text/html");
        //return httpd_resp_send(req, "OK", 2);        
      }
      else if (val==5) 
      {
        Serial.println("Backward");  
        actstate = rev;      
        WheelAct(speed, speed, LOW, HIGH, LOW, HIGH);
        //httpd_resp_set_type(req, "text/html");
        //return httpd_resp_send(req, "OK", 2);              
      }
    }        
    else 
    { 
      Serial.println("variable");
      res = -1; 
    }

    if(res){ return httpd_resp_send_500(req); }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req){
    static char json_response[1024]; // Mantenemos tu tamaño original

    // 1. Obtenemos el estado actual de la cámara
    sensor_t * s = esp_camera_sensor_get();
    
    // 2. Leemos la distancia del HC-SR04
    distanciaActual = leerDistancia(); 

    char * p = json_response;
    *p++ = '{';

    // 3. Concatenamos framesize y quality (esenciales para el video)
    p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p += sprintf(p, "\"quality\":%u,", s->status.quality);
    
    // 4. Añadimos la distancia al final del JSON
    p += sprintf(p, "\"distancia\":%d", distanciaActual); 
    
    *p++ = '}';
    *p++ = 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}


static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!doctype html>
<html>
    <head>
        <meta charset="utf-8">
        <meta name="viewport" content="width=device-width,initial-scale=1">
        <title>UNEXPO ROBOT ESPCAM</title>
        <style>
            *{
                padding: 0; margin: 0;
                font-family:monospace;
            }

            *{  
                -webkit-touch-callout:none;  
                -webkit-user-select:none;  
                -khtml-user-select:none;  
                -moz-user-select:none;  
                -ms-user-select:none;  
                user-select:none;  
            }
        .header-container {
        display: flex;         /* Activa el modo alineación flexible */
        justify-content: center; /* Centra horizontalmente */
        align-items: center;     /* Alinea verticalmente el logo con el texto */
        padding: 15px 0;
        gap: 15px;               /* Espacio de separación entre logo y texto */
        }

        .logo-unexpo {
        width: 60px;             /* Tamaño del logo */
        height: 60px;
        display: inline-block;
        vertical-align: middle;
        }

        canvas {
        margin: auto;
        display: block;

        }
        .tITULO{
            text-align: center;
            color: rgb(97, 97, 97);
            
        }
        .LINK{
            color: red;
            width: 60px;
            margin: auto;
            display: block;
            font-size: 14px;
        }
        .cont_stream{
            width: 90%;
            max-width: 700px;///////////////

            border: 1px solid red;
            margin: auto;
            display:block;
        }
        .cont_flex_Screen{
            margin: 20px auto 20px;
            width: 70%;
            max-width: 400px;
            display: flex;
            flex-wrap: wrap;
            justify-content: space-around;
            
        }
        .cont_flex_Screen button{
            width: 70px;
            height: 30px;
            border: none;
            background-color: #64c2ed;
            border-radius: 10px;
            color: white;

        }
        .cont_flex_Screen button:active{
            background-color: green;
        }
        .cont_flex{
            margin: 20px auto 20px;
            width: 90%;
            max-width: 400px;
            display: flex;
            flex-wrap: wrap;
            justify-content: space-around;
        }
        .cont_flex button{
            width: 70px;
            height: 30px;
            border: none;
            background-color: #007BFF;
            border-radius: 10px;
            color: white;

        }
        .cont_flex button:active{
            background-color: green;
        }

        input{-webkit-user-select:auto;} 
        input[type=range]{-webkit-appearance:none;width:300px;height:25px;background:#cecece;cursor:pointer;margin:0}
        input[type=range]:focus{outline:0}
        input[type=range]::-webkit-slider-runnable-track{width:100%;height:2px;cursor:pointer;background:#EFEFEF;border-radius:0;border:0 solid #EFEFEF}
        input[type=range]::-webkit-slider-thumb{border:1px solid rgba(0,0,30,0);height:22px;width:22px;border-radius:50px;background:#64c2ed;cursor:pointer;-webkit-appearance:none;margin-top:-10px}

        </style>
    </head>
    <body>
         <div class="header-container">
        <img src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAGQAAABkCAIAAAD/gAIDAAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAAGYktHRAD/AP8A/6C9p5MAAAAHdElNRQfqAwQPLCstGKoQAAAp9ElEQVR42tVdd3hWxdKf3dPent4hoYaE0BI6oRfpRcWCgiLWa8VyRVEuKnZsWOhWRARFFKSIQBICBBJCCTUJaZBe37z9tN3vj00iei3gBcw3Tx4ect73nLPnd2ZmZ34zu0GUUri2QikgBA0Or8UkCjx3je9+CcOjCKGLh9oi+BoPhVCKEJSU2x1On8Bz1/xN/YUwpI6frVi8Kr2m3s2Q+nHPmeVfZ+o6uaZgEUIxQiVlDYXn62KiAgB+9d5agxBCAWDf4ZKnX9pSVtXIDn75/dHn39ulavq1A4sQijGyO3x7DxX079kWAFqbWgEAIAAAs1H0C7G0uAg/qyEiyIIQukZgUUoxRoTQNZsO90+MMRlFQmhrUysAAAoAQChVNdL0C4BOiKYTAHpNzfD9z9OjIwNi24cwLfungblsuRZgaTpBCK3ZlG1v9E0dk8C07J9+8L8jVx0sTSc8h1MyCr7beXreAyP+6ee9DNFJkxmi5ono6oKl64Tn8LmS2n+/uX3hI6OMBkEnv0QxrVYEHgNtcVlAgSKEMcZXESxCKMdhr1e54dEN99zSr32bAIfLx/1/MECzSVQ1vabezX6tqfeYTaLAc1cLrBbHdNPcDUkJkTeMjk85WGCzGK59wnBZwrS+fRt/QeDe/exA5vELX246nJpZHN8hCAD4q4MUsIjlqde2Fl+o2/Thra8uS5k4Ih7+K4FobcJecGLXqBkTun383dH0rGKfrNks4uzrEwEAXY1XzSKDFesOzXtzx4END1TWul5ZmrJ7zT0AQJvivtYrLOOxO7yfbTxyrqTOIPFjh3QaMziWss+urGi6TindlpZr7vHCks/2UUq7jnvv8Ze3UEo1nVzx210NIeT3x3mFfZauEw7jY6fL7n5m47ghnR69M3nRh7vLqx33z+gHrV6nWgQhRCkllCkTkGbju5Jg6YRwHC6vapw9b6Mk8ctenJpbUP36yvT7Z/Tr0iH0/1fUjhDCCAEAQoCbvewVc/CEUA5jt0eZ88zGU+dqvv1gRkiQZfa8jQF+xifnJP/Tz355Qillr5ZNjoRQCsBhdGV8VouRz356A8Q8/dSrWymlK77KgHbzlq49RAg5mVf5J76gVcnFYyTkV79eAbAIoTohlNIF7+6Ejs8Mu22lqumFF+rDB77ac+oHlNJPvs1+Y0Uqbfb9rVnY6zxbWPP+mkOllY3s4P7s4jWbjur6leCzCKUYodUbst79dH9kuN878yfyHP7Pez9X1jhfeGgkACz54gBqShVau89qIv+yzz/66Lrlaw+yg+u35jzx2vYrQP7pOuEw2pF2duGSXTohz94/LCkh8usfj637MWfqqLhpY7p+99Op82UNk0fG/T+AqlkMEmdpY/0x9Wx5VSMAWExiWIjpfyX/dJ1wHD52qmzuK9tqGjzTxyY8PGtgVZ3r1eV7rWbxqXuGAMA7nx7o26NNXIcQSgH9P5kNdZ0ijAvO13+/6wwAaDrRNAL/S+igE8pxuKyy8aEXN5eU23vEhr765DgAePnDPSfOVNw8vtvgPu1/3H16/9Hi2yb3BACdkP8fUAFQSkWBCwowfbXlOAAIPGaR1t8EixDKYeTxKv9auDknt8rfanhx7pg2EX7bUs+u+f5ou7aBj94xCACWfnUwMtQ2YWgsAFy9pP2Ki64Ti0m868be2afKUzPyzUaR+bK/8wC0mVF4/OUfUw8VYAz33dJ34vC4Rqf3teWpDrdy68QeCbHh21PP7NxfOGVkXEiQBQAOHbuQfbIMmp1oaxaEkNurTRvVtX3bwJUbsmVF4zhE6eWD1UKKLfpwz7qtORyHB/duP+/+YQDw3qcHsk6Ux3cI+tft/QFgxddZgNCN4xLYiRu25iz5bD800yCtWRBCsqKFh1jn3Jj48/6C1INFJoNA/kZuSHSCEfr028PvfLrPaOBtFsNrT401GcWMIyUff5NFKJ05LSk60n97Wu6uA4V9ukUO6hUNAF6feuDYhZTMovPldoR+ybZapyAECCFF1W+f0stiEQ+fKuV5Di4XLDb97dyXP2/xT0aJ93jVFx4Z2SMu3CerLy/dU13v7h4b9uDtAwBg5bpDbrc8bXS8ySgCwJGT5TlnK2JjgkMCzQC/MLatUxACgcc+WYsIsd04uqus6CKP4bJ8FkPqZF7lgy9sxog63fKtE7vfNb0PAKz8OnNf9nlR4B6dNdDPatiacjYlszgqwm/80C7s3O3peR633Cs+zMj0GSMAoK1Vv2RFb3D4mPrPuj7JaBCqGjyXARYLFKprnfc//319g1snNLZ9CIsVTp+r+mDNQV0nyUkxM69PBIDV3xx2uHwDe0V37xIOAF6fuiujQJD47rFhACDL2o69uYRQhFBrs0fmT7t1DnlsVv9APyMA9IgLf+nRkbdNSOB57pJYB0Iph5GsaA8s/CEnt9LPKqkqffuZCSFBFkLpy0tTq2qdRoMwd3Yyh/H2tNy9WcVWszRheCzToIyj58+cqw4NsnSNDQcAjGDBkt2HcsoWPjwSI9SqiGaMEQUYmBgzMDGmBb65c4Y2ffqX51NKGaHz1Ovbd6bnB/oZ7E758TnJwwd0AIB1W47vSMtFABOGxY4dGgsAK9ZnebxydITf6EGd2BV27M1zueXoCL/2bQIAQFZ1SugbK9JefH+XT9ZQE/PxT+PULIgRfhfFN4Q0EYGXAhYAwGvLUz7bmB3kb2hw+MYN7vzEXckAUFZpf31FKkLgZzM8PGsgw2VvZpHAc8m9Y9pG+gOAy62kZRUJHOoUExQSaAEAp0vx+DSrSXhz1d4PPt/H3h5CvxQ1r5S08C2XjReCi3lKjJuIwL8wQ+bU120++uaqdJtF9Pi0tuH+rz89VhR5AFj0UWpJmR0Apo9N6NujLQCsWJepa7ok8pNHxLErZB4/X1hSL4p8t9gwdqS6zulweAQeY4xYba7e7jEZBYMkUAC4ElZJKRBCOK6J47zcKgljrn4D318k0gypPRkF8xb/JPIYAWg6vDR3VKeYYADYmpq7ftsJo4GPCrU+OHMgAOxMz9uXXSQIXIe2gcl92rGL/JSe71NUo8QzZw8ANfUer6wihBBCNosBAE7lV93x1IYD2cWsUE6bW1kuVyloM2uOEHAcVlXtk2+y1mw6gi4nbaDNmnXxD0KI0j/WLIbU2YLqR17a4vYqFpNY2+C555a+N47rDgCNTt+L7+/iOfB61TtnJzP4Vm84rGlEJ3TkgHb+NiMAOFy+9KxijFGAzRjfMYRd2e7wqhploIgSDwAGg/DzgYJ92RfGD+v8+r/HhQSaKW1ivgml6NdBfwuAFysgm1tRM2teU+/euS//6y1H07Mv2MxSt9iwxIQondC/rIczoKtqXUUX6iWRp5QihDSdREf5hwVZfh8sVnmvqXfdPf+70spGf6vkcMmJXSNeemwM+8Lry1NPF9SYDHyn9iF339wPAHYfOJdyqNBo4AmBKaO6sq9l5ZTmn6/DCLVvGxgV7sfuXV7tYFEpRshkEAFAErCfRfLK6totOTeMTZg4PE7T9YKSujbhfhazBL8uzf7GSJmJMRdjb/QYjZIkcmcLau5bsJnH1GYWG13ep9/cuXnFTKNBoPQvOi2Y8W5Py3385a2iyKuqzvO4pt7z+rxx8+4d8jtmyIJGRdUfWPD9sTMVfhbRp2hmo/jOs5P8rAYA2J9d8snGI34WUZa1h2/vFxJoppSuWp+lqJpP1nvGRfTvFcOmj5378mVFI4QmdA4VeE7XKQCUVjYyo6AAZqMIAKLAI4Q4jA0CrqhyAoDAc8vXZQ66bfW9z313/Ew5Qk12pGq616doqubxKrpOAAAB1NS73/t0/8ynvkmeseq7HTkAMKRvu3/d2pt9wWoW9x8pfm15GlxyryGlIGtacp92U69LmDAibtrYhM7RgfDfDr4lUHj6je3b9+YF+RkJoW6P8soTY/v1aksplRVt0Yd7VFXTNOjXs+1tU5IAYG9m4Z6DhVaz1NDonTSyC3vPDqd3b1axJHCKRpIS2rTcor7R2/J/g4FnYIkC9slU00lljYN9lJQQuWxd5olTpQLHLX1pKqEUA6qudd2/8EefQtxu3/Rx8U/OGQwAksit3XL86Olyg8S/8+mBUcmdQ4Mszz4wYndG0fnyeqPE+1nEpWsPDu3bbnRyJ+Ze/gIthBDgVx4fFd8p7OLDvz2NYf/mqrTV3xwOsBkAoNElTxkZ/9jsQcwvfLTmwL7sYotJ0HT66J2DDBJPKV25Pssnq4SQQH/TuOYUJzOn7FxJHc9ji0lM6BwKAJhDlNKGRh/HIeZHJYEDAEHAPM8xjb5QaW8BKzLEEhxgOnKqtNHh5TlMKUSF+2mamnYo/0Ru6Wcbs6tqnQBgsxheeGSE1SwE+xtOF1Sv+DoTAIICTC8+NlrXgVDgMCKEPPvWT7X1Lo7Dl5g2+HwaAHh9qiyr9L/jLF0nGKP1W3NeXpZmNQkIgcendmwb+Oa88QAIY3Qyt2Lp2kybRbQ75TGDYyePjAeAfYeLf95fYLOITrfSt1dM53bBTP93H8j3yZqmk6gwW8eYQADACDlcvuo6J8dhSgFjJIk8AEgib5AERiiWV7uZxbVrExgVatUJLS6zHz1dwWwQAKaMiDPwKCTAVFJm37L7DBv2xBHxowd1rm/0+VnEjzccPpFbAQBTRsXNnNrT4fIBQmajcOpc1YL391ySHQIAok++sX3qfV/c9PBXkx5Ye7aw5ldgMf3cn1389Js7DDziMNJ1ihB+5Ynr2kT4s+d/ZVlqTYMbI2Q0CP++u6l0ump9lten8BhrhN54XQIAcBxudHjTMguNEueVtU4dQiwmiV3B7VEbnTKHEaWUw4jnMQCIAsemHo5DdQ1ut0cGAItJ7NYlXCfEK2tpWUUMawBI7hNjNBsUVecwbNh+UlY0ZvVPzBlslASMUZ3ds3hVOhvb/AdHdIwO8vlUQmmAzbD2h6Nfb83BCLHB/BlWAGVVjnPn6wtL64tK69WLOXiGVH5xzYMLNztdPkHgAIHTrTx4W7+JI+NVVec4/Pl3R35MzQuwGRocvpsn9OjfKxoA0rOKtu/Ns1kkt1ft2CZo3JCmFCfrRGlecYMk8USnSXERAE0cltMte2SNpYQcRgaRY2AZRI4Q4Dhcb/fU2T1Nltg1khAqcCg9q8gnqwzZbp3DunUMcXtVs0nMPlm2J6MAIaRpZGBi9PXXdW1wyAE2acueM0zpIkNtzz4wXCWUkUISj19Y8nNxaT3H4T+PvAiFpf+ZlLr2nm2r79zz2ewu7YObwGKBQp3dff9zm4rLGkxGHgE4XEpyUvT8B0cAgCBw58vtry5LMUqcrGhhwdbH7hjELrp8XaZXVnkOu33q6OSOQf4mZiw/7z/n9SkIQBC43t2jWgZRWeP0eBSMEaVUEDiDxLPriyJPKOU5bHfJtQ3NYHWLMhlFnudyC2vOnKtmlshxeFj/9rKi8xgpqrbmh2MATXWjx+5MDvA3aRpBCL2yNNXllgHg1kk9rx+d0OiSEQJJ4suqnPPe3A5AMUZ/4rsQoLZRASFBlnZtAqOjApm7wMytEp088sLmzJPlVrNICCgaCbAZF88bb5AEVgVa+P7u0iqHycA73cqcG5M6tQsCgAPZxTvS82xmSdOJKPBTR8cDgMBzTpeccqjYKPGyooeH2OI7hLLbA0BNvUtWVNxkhlgUOADgOSwJHCUUI+ST1YrmpQ2d2wWHB9sopXannJ5VCNDEGo4a1EmSeFUjFpOYerDw6OlyDiNVI/GdQudcn2h3yTaLdPxs+Ydrmqqk/3l4ZGSYn6zohFA/i7g1Ne/DLw7AH4f1CAHGUF3rcruV6lpXda3L7VEAALNneOy1rd/tOhNglXSdYARur/r8QyN6xEfKisbz+PudpzbuOBloM7g8Smz7kHtv7ccu+s7nB2VZ5Tns8ao9YsMGJcawd3Xg2Pm8ohqjQfDJWoe2AeEhFmgOJhscsqZTlj1wHBZFAQAQQgaJJ5QihHSdXKh0AICqET+roXuXUJ+s8RzafaiYUsrzHAD0ToiM6xDqlVWBxw0O76ffHYHm7O9ftw/o0DbQ7VX8bIalXx3MK6oBgA7RgU/fM0RWdIyAUDCbhNdWph/OucBx6Hezd0Ulbq8669lN3W9YNmjmJ3GTP3rho5Qmp/neZwdWfp0V5Gdgbdj1Dt8t47vfe0s/Qqgk8vV2z4sf7OZ5BACyoj94e/+wYCsA7Msu2bU/32aRKKWyok8Z3VWSeE3TAWBbWp6iahyHZFXvnRDJfCILnVmNFwFQSiWRNxkENj5J5FiYTgiUVzuhOdHr16OtohGjgT+RW3WupA4h0DRiNAhD+7TzKToFsJqFbSlnSysbeR5rGokItf1rRj+vT5MErt7ueWNFKgBQCvfc3Oe65I4Ot4IRCBx2uuWnF+/w+hQ21VykUwgAQgJNyYltO0faYoKN7UNNHcLMAVYJAPjPNh5+ZVlKoFViBS6XV+nSLviNp8c1Dxe9viItv6Qu0N/odMmDEqNvn9KLXXfZ2oO6qnFGg6Lq4cGWicO7MO/jcPr2Hio0SryuU0Hg+vZoc/E4KppznSYHLzVFxQaJZ8cRgvLqRmjOYPp0jzJKPIdxbZ0r40hJ53bB7NlGDuywakOmrhNJ4MuqHOu3nXhyzmD20Hdcn7Rh28kTeZX+VmnznrO7958bldwJAL04d0zWiXKvT+F5zmYWDx0vfXVZ6qLHr7uYkmA3nTIybtyQzhihpk8ocIyD35tV4vEqLOQjhBol4Z35E0KCLMyV7kzP+/L7o1azSHRCKdw1vQ8rQKQdKty575zFLBJKdUKtFik0yMzutzerqOhCvUHidUIEgQ+wGZscJgIAqGnwMtQoUI7HjOoBAJNRpIAopTyHy6scqqazomx8x5CoMJui6gRg96FCAGDxd59uUe2iAmRFpwBGid+w9bjT5RN4rGnEajHMviFR1XSMkaqSN1aluTwypZDQOfzp+4YpKkEICCE2s7hy/eGf0vNbcqkW4ThsNAiSxEsiL4m8JPFsMPjtZ8cP6Nm20SXzHGZ5pp/V2HLa3qziBqeP5zAFoJSeL7ez46qmASWM5BQFXFrpOHK6gn10prDGp+gYIQ5jr1c5ea4amhMDSqmq0aa3RYHHuKWpzmyUmgeKauq9LrfCHHCAn6lblwivrJmN/KGcssoaJ8ZIJzQ40Ny/ZxtZ0QGo0cDnFtYeOn6hZdinC2oQQjqhFpOQcezCrv0F7D7DB3QwGARCKKXAc9jlkd9cvY+Z1G9mRkp/9dOkdwF+pnfmTwjyN8mKJvCc0y0/+/ZORdHZArK7pid1ig70yhoLAj7fdPRChR0ARifHjhnc2elWMEYYY69P2ZtZyK6YGB9pNPAtjvNsQXWLDSKEBAE1930Dz6OWRQvMc1MKHMYOl7fO7m6e+oDoBCiVBK6isjHjaEnL8zQ0ejFGCJBP0dpEBnTvEgEAPI+zci58teW40cAjBG6f2qNLxIBebdkpr360x+tTGT+l6sRiEu+/pTerm/yK8KGUAkWIcX5AoZlW1nXSMz5y0dzRikp0Qm0WaV928eLVewFA1fSO0cEPzRzgU3QAMIjc+fKGdVuOsys+emeyIPJEp5RSUeT3ZZ+XFQ0AesZHMMMBAFHApwpqAIBrfnUCh3+X0nO6fc1eA2RFY0E8h1Gd3XMqv0oUeUIBKGWRN4dRztnK/UfOmww8IPB4tTum9QoLtmg6AYAPv8hwuHwCzxFCOcw996/h4SFWAFj2ZcYPu89YjAIhFCNwe9Rn7ht284QeLdzZL56ruaH0N79izGFK6cxpSXdN793okjECq0lYuvbgwaMlAs9RSudM7zswMdrlURFCksB99WNOZY0TAAb0ajthRFyjW8YIGUX+TEH16YJqAAgJNPfoEu5TNAAQee58RSNLdxlGHPfLIHT9F9ycLh8Cyg6aTVJwgIUdzy+qrahxSiInK1p4uF9y73bs+OY9ZxoaPYLAybLWJsJ/1tReAMBzeNf+/G178ywmEQE0uuTbJvecMLwLABw/U/bm6nSTUaBAeQ7XOXwzJvd8fM7gJgd+kRBCz5XUFZXWt9hjcWlDwfk6Sukv1OErT4wd2Kut3SlLIu/2KM+/u8snqwghUeSee3AEwUgnxGAQ8otrv/zhGDvlqdnJJqOo6YTnscPp25dVzI4PTIwhBCiAKOCqWldBqb0FLIFrYsURgNZcNQEAj5cRzaAREhxgDvQ3se/n5FZ6farAYY9PG9AzOiLUBgAer7J1zxlR5DBCDo86c0qviFAbIVTT9Hc+2aeoGs9jr6zGRAY+88AwAPDJ6nNv76xvZMQ/tjvl5MTod+ZPgGaK9SJXRTFG736SPmLm6lN5lQBw5lzV8NtXfvTlwSYOHiGkE2IyCu8vmBwSaPF4VZtFzDh2/s2VacwYh/drf/uUxAaHj0PIKHKfbjpSXecCgF5dI2+Z0KPRJWOMOA6lHGxyW/16RFlNoq4TjLGmanmFtS0+3mAQm8aFgBDK6EAAYKw806zIUIvRIDCnl32yDCEgABxGY5M7sC+nZRafKqg1GwWvT40Ks915QxIAYIzWbz2xN6vYahIpoV6fvuDhEZGhNgBYvDo9JbPYahIBwOtT20b4rVg0zWKSdEJ+023ObnrzxJ4lpfaPv80GgE+/yz5f4bhpfDdoSaQ5jDWddO0ctnjeOI1QXac2s/jhlwfTDhUwT//CQyM6tA1y+1STUSgsqVu7uclzPTxrQHCAWVY0s1E4fLqiuMwOAHEdQ9u1CZAVnSVSx3MrWnTdIAksekGAdJ2oqgYAuk58so4QAEK6TqLC/ZmnUFT9ZH61JPKyrEWF+Q/u0wTW+m05mqbxGLu96m0Te7SL8geARqf3rc/2iQLHcbjB4btpfPcZk3oCQNqhwvc/z7CaRQqgaUSShKUvTOkYE8zWN/zGdXIYUYBh/dpPHNFl087T+w8Xrd9+6obrug5MjPlVyxHPYULoDWO7PTprYINTFgVOUfXn393ldMkAEBZseea+YbKiUwpmI//FpiMNjV4AiOsQetvkHk6PahD5mjonm638rIakrhGKqlMKooDPFdexcTCwWpY8UtpUK5QVzeNTMEJAKUIoMsQKAAhBaYW9tKLBIPFeWe3TPSo60g8ASsrsezOLbGbRK2sRIU1qBQCrN2Sfya+2mgS3V42JCnhp7igAaGj0zlv8k6ZpPIeBgkfWXp47ZviAjn/ClxKdAMCDtw+oa/Q8umiL2+W775Z+wNj2i7/HjHfBI6PGJneqb/T5W6Ujp8tfW5HGbHvmtF7jhnRudMlmo5hbWPPFpiPsrHtv7hsZalU0wmO0J6PZEhNj2KIOUeBKSuvr7R6m8JKAGF2CACghiqIDgE/WPD4NY0Qo8DyKifJjFzmVX2l3+DgOAaAR/Ztc+/a03MpapyTyHp96/diETu2CAaC0snHpukybSdApqBqZ/8Dw6MgAAHjpo5QTeZUWkwiU2p2+R2YOnHNTH0Ip/mNmmX00bmjsqIEdj56pHD2o46hBHZmm/wYsRAjlObxkwaQO0YFOtxpglVZ8nbkzPZ/FJs8+MMzfZlQ13WYRP990xOHyAUDHmOCZU3o2On2SyGUcL7U7fADQu2tEkL+JECIKXJ3dU1LWwG5hMIjMIBFCqqYrms40y+tVMUaUUIHnI8OawMrJrdIJ1TQSFmQZ3KcdAGg62bznLNP6oADzHdMS2Tc/XHOwrLLBZBQanb4pI2Jvn9oTAL776dSn32b5WSQAsLuUqaPjFz0+hr2nP6nwIACdUIxR23A/SmjnmCCOwzqhCP0XB48x0nUSHRXw1rxxosCiG/L8e7vq7R4ASOrWZvYNiTUNXlkjR09XrFyfxc66+6a+3buEW8zG8sqGzJzzANAxOigi1Frb6MMIebxK4YUmsEwGASHELFGnlJFfPllTFBUjpBNis0jBAaYWsHge+2QtKSGic0ww8/fHTpdZjILLo44fGssKtyfzKr/afDTAavD4tKgw2/wHRyGEL1TYF7z3M4cRzyGnR0mMj3jv+UmM8/urahjlMDpbWPPDnrNms/RjytnSikYOo9/v/GNXHDM49sk5yQ63YjWLZwsqX/qwib1+ZNagSSO6jBrU6fkHh/eKC2fG3CbC//PFN21YcuvGD25jT2U2iTMm90zqGiVIot0hHzp2np0uiQJCCIAiBESniqIxzVI0HWGkaiTIzxgaZAGAugZ3flGNKHA6ocP7d2D03taUXIdLpgA2izTnxt7smh98caC+0SsInKxoT9yVHN8pFAAWLtlVUtZgMYkenxoabP3oxamhQVb9EhZbMS+xesPh+kbP7Ot7FVyoX/PDUQCghP5+kZVB/+Q9Q3NyqzZsz+EF7tPvj906sceAxOjgQMu6d28Vea7F7DFCFKBL+5Bf7kcBIXj0jkF3TE0sKmtIPVjIZj0AoJRqOsEIQdNsqAOAV9ZUjWAELOmzmiUAKC5tqKhxIQQBNuPQvu0BoN7u2bkvz2jgXW5l+rgefXu2BYD0rKLvd53xt0p2h29Mcue7pvcFgE++yfp2x0l/m0FRdYTwW/PG9egSfilFMLZcq7yq8avNxwYnxiyeN/742cqPvzn8wIy+AX7mPwILsWVRrz01pqza0SE66M5pvZqbFahBEqA5yGxK+tgiT2BtHb+oub+fMdHPmNg1EpqXtw7uEx0ZYq21u60mQSeg6hQAPF5F0wmPka6TqDAryxNP5ld7ZRUh6NutTZeOoQCw73BRfnGdQeIA0Owbk9g1P/jigE/WOCNvsxjmPzBMELhTeZWvLEuVRB4hcLiVRY+NnjKqK6PO4dJk2brDFaX2d56dYDRKd93Q++4nNqzekP3ve4f+8aSAEaU0Ktx/8/JZq16eNrhPO7NJhKaFi0x30MXGzxboYfwrh0CbF1axCxJCu3eJ+OT1G8NCA+ocqq7rbJ52uhVdp+wNtQ1v9u5nywmhRKcjBnRkDMkPu3M1nbi96vD+HYf0bQcAm3ef/nn/OT+LZHfKj8wa0Lt7G0XRnn1rR3Wd22zkaxu8t0/u+cTdQwAuqTOHhe91De592efHj+s+eVQcAFw/NmH0mISUzPMut/xnLUds7jebRLaas6Wr4tJbgn6NZxNeQ/u13/PZnW+sSl/7wzGPTwMAp0vWNAIihzkcGe4PALKincqvxghMRmnEgA4AkF9Um5ZVZJR4j6zdc0tvAHB7lPc/z8AYOd3yoKR2c+8aAgDvfpq+K6MwNNBUZ/cN6dNuyYLJLUp9KaMFAJvVsPGDWywmURR5SmmAn2nripkerypJAv+X5zd18FyhVkbGRkWF+72/YNKgXm1sZhEAPD5ZJ4QCSALXNsIPAEor7CXldp3Q+I4h3ePCAWDn/vyaOhdGMLRPu+uSOwPAV1uOHj5ZajWLlMLLc0dJIrc/u3jJ5xkBVoPTrcZE+X/82o1Gg3BJ9fqLROC5QH8TNHleRCmIIs9Iyr/uKb3iDZ8cRoRQhNGtk3sxUsXlltnKPoMkRITYACC/uLbO7tF0OrRfe4HnfLL6Y0ouh4FSdP+Mfgih6jrX8nVZBolzuJR/3zN4QGJMo8M7/+2ffbImiZzJKC5/aWrbCD9WVbjcETI+nT14c78YvXZbQv1GmFGwvccAQBB4Xac6oX5WKcjfCAA5uVVen2o2S6OSOwNA1omy42fKdUIHJkWPGxoLAKs2HM4rrtV16NM9au5dgwHglWWp2SdLzUZBVsirT44Z3Ke9/reQgmacmmjS5rY/uPbb2P0yIICW2eDWSb2mjYmva/BEhFiCAkwAcDK/yitrcR2C+yREAMC2lLNOt8xz3JzpfQSeO1dS9/nGbKPISSL3n4dG2CyGLbtPr/4mO8BmsLvkx+4cMGtaEv3TnOYS8Gr+uai8eFV2DLmcMSFCqc0iLV90fV5RvSjwBklocHgLSuoIoSP6dzRIQk29e+f+czqhSQmRE4bFAsAHXxyoqHFyHLr7pr4jBnYqr3YsXLKb58Dhkm+8LuE/j4xpeR9/T9gMjjGwdpiWyeEfBgsAWJuGzWL44D+Tt6ScAYDiC3Ul5XZ/q2H0wA4AsCfjXMH5OkngZ12faDKKWTml3+44wXMovlPYk/cMAYAX3t9dcKFe4FCvrpHvPjeJVWv+3rYI7MSd+84tW5thMvA6oQaJnzg87pZJPf+sp/RaCutAGpgUHd8pBADyi+tr6ty9u0f16d4GADbvPuv2KIN7t79hTAIAvPPJPqdbMRqEZ+4bGhxg/nzj4W+3n5BEzmIWP1o4OcDPeLnT38XCXPuFCvvmPWcjQ62Eojq7e0tKbnRUwMDE6NayYJJNOqxt9/jZSsWnJvdpbzaJZwqq0w8XSyJ/66TuFrO0Y2/e7oxzhNBpo+OnjO5acL7uleVpPIe8Knn3ucndukRo/wNSLSIInMUkffzqDSd+fGTpwklur7ol5Sz8gw7+j0TV9MMnSyWTOGZQRwD4YdeZCxWNPbqEz5jUAwDe/yLD6VY6xQS9NHcMADz9+vaqWpdP0Rc9MnLqqPi/Pf39RhgrGRMZEBxovmt63/ZR/mVVjtYIFs9hBKhjdFByUgyldFtqLsfh2yb3tFmNazcfO3i0RBK55x8cERZsffeT9F0HzgHAnBt7z71rMKVwJfdOo9TjUwAgPauwotYV5GeC1uDgWwQh0AnhMJ4wPDYnt9JsElMyzmWfKuveJfz2qb18srrq60yHW7nz+qTp47tnn7iw5PMDOqFjh3R64+nxAECB4isXQCME89/eaZCEI6fLZEVjTXqtCCxoboS8bnCnnnHhAPDDrjMel2/GhIQAP9PSLzMO5ZTGdQh5ae5onZBn3tpZUePqnRC55PlJksRf8V2BEIKi0nqMUWig5bE7Bl43JLbVgcWC1Nj2oXEdw1h4FRMTNOemvvV278r1hwHg6XsHR4XZnn9nZ/rh4rbhtrfnT2T9rv+7U/+NEAofLJiYmBCFMQ4ObKr4tjqfBc1MWUpGwZn86hmTegUHWj5cc+D46fKbxibccX3vn/fnL1+XaTaJi+aOHpgYrV8OUXXpggC1jQwKDbYFB1poU0dqqwSLpSnf/XTK5m967I6BpRWNK9dntY8OePmJ61xu38L3dtmdvidmJ8+Ykkiv2v6wFEBprmm29Gm1OrAIoQjgdH7V1vS8WdMSw0Osry5PKat0zH9gRHRUwIJ3d2YcLr5lUo/nHhrJvn81oKKUaKSpqfe3ZGarErYv4KafTlgTXyytbDx2upyPe37KfV9QSr/76aQQ/3zvG5c2NHoopfpV2EGQLeYsrbTv2p/PiKOL989qXQ6+RexO+ebx3aLCbHOe3Wg28m8/O6623v3Ea9uiQm1fLZ7ub/ufcpo/EZZIRIX5RTXXLi9WrFYHFmr+9/HZySkZBTtTc99fOLlTu5AZc7+urnX/uHJmbPuQv0fpXQ5elK2W+e1n/7TZ/Z4tUHoit8LjlZNvWpZ883JK6cffZEHn+SvXHaTXdv9OQsjFW++1RrA0TaeUfvrtYWPCgpO5lefL6gN6L3ri1a30vzbhu5ow/bJDYQtkV2UH3P9FKKUIIYfT12/6spvGJSx6/Loht66wWaTvl98h8E0twld/DE2uyunyaToJ8GP1i9bn4AkFDsFHXx2SRG7R49ctXPJzdZ3r2w9vE3h8bXY6ZUipmr5s7cHtaXmySgYltnlsdnJIoKXVgcWqk3sziz74z5SsnNKlX2VuW3VHWLD1Kk1/vwMWUATo5Q/3LFqaGhlmkwxSytLU3KK6NW/d3Lp8FnMNp/Iq3/l4ryyr3Scs+fibLEqpql2jPTtZ7HYit8La84URt63ILaiqrXc+tmgz6jx/5deZrRGs3MJql1t+YMGmJ1/dRq/t9MfmlsWr0rguz6UeKmQH7Q5P51GLR92xunWlO2wThdj2IZt+Pt3g8L317HgAuJZbULLZ42R+dVSola3r1nTiZzX2io88c66qdYHF+g5zC2u2pZxe+sIU+K/W62sDlsstW60GtmKNUWyB/kaHS25FYLGgWdPJ6g1ZT907NNDfxBa4X+NRAADPYV3VdUKguclaUTSeu5p/8OPyh0kBYN3mo8P6tU9KaHPNpr9fjYECAIQFmavr3XUN3pajxWX2iFBrawGL9T0cOVkm8NykkfGUXhVK7xLAogAwIDG6vtG7cccJAOAwSjtUmHWibEBSu1YRZ1FKMUaNTl9xWf308T0A4J/ah5lZ/fhhXbrFhr2yLE1R9QCbYem6TJ7n7pneuxWlO/uzi7t2CgvwM9JrktP8kbA8IeVg4X3Pf3+uqBYA/PwMb/x77P0z+v/zYNHmHScEHocGWQi9khWtvzkkShFCJWUNKQcLvbLev0dkUrc2cJX+lMzliq4Tj0+1mqXWs1nib/JQBt//Afk5CVIcxjf/AAAAJXRFWHRkYXRlOmNyZWF0ZQAyMDI2LTAzLTA0VDE1OjQ0OjI2KzAwOjAwEe1FhgAAACV0RVh0ZGF0ZTptb2RpZnkAMjAyNi0wMy0wNFQxNTo0NDoyNiswMDowMGCw/ToAAAAodEVYdGRhdGU6dGltZXN0YW1wADIwMjYtMDMtMDRUMTU6NDQ6NDMrMDA6MDCj8vrFAAAAAElFTkSuQmCC" 
             alt="Logo UNEXPO" 
             class="logo-unexpo">
        
        <h1 class="tITULO">UNEXPO ROBOT ESPCAM</h1>
        </div>

        <img id="stream" src="" class="cont_stream">
        
           

        <div class="cont_flex_Screen">     
            <button type="button" id="toggle-stream">Iniciar Video</button>
            <button type="button" id="get-still">Pausar Video</button>
            <button type="button" id="close-stream">Cerrar Video</button>
        </div>
        <div style="text-align:center; font-weight:bold; color: #00FF00; margin: 10px;">DISTANCIA: <span id="val_dist">0</span> cm</div>

        <!--
        <div class="cont_flex"><div><input type="checkbox" style="margin-right: 5px;" id="nostop" onclick="var noStop=0;if (this.checked) noStop=1;fetch(document.location.origin+'/control?var=nostop&val='+noStop);">No Stop</div></div>
        -->
        
        <div class="cont_flex">     
            <button type="button" id="forward" ontouchstart="fetch(document.location.origin+'/control?var=car&val=1');"ontouchend="fetch(document.location.origin+'/control?var=car&val=3');">Avance</button>
        </div>

        <div class="cont_flex">     
            <button type="button" id="turnleft" ontouchstart="fetch(document.location.origin+'/control?var=car&val=4');"ontouchend="fetch(document.location.origin+'/control?var=car&val=3');">Izquierda</button>
            <!--  <button type="button" id="stop" ontouchstart="fetch(document.location.origin+'/control?var=car&val=3');">Stop</button>    -->
            <button type="button" id="turnright" ontouchstart="fetch(document.location.origin+'/control?var=car&val=2');"ontouchend="fetch(document.location.origin+'/control?var=car&val=3');">Derecha</button>  
        </div>

        <div class="cont_flex">     
            <button type="button" id="backward" ontouchstart="fetch(document.location.origin+'/control?var=car&val=5');"ontouchend="fetch(document.location.origin+'/control?var=car&val=3');">Retroceso</button>
        </div>

        <!--
        <div class="cont_flex">  
            <div style="display: flex;align-items: center;">Servo <input type="range" id="servo" min="325" max="650" value="487" onchange="try{fetch(document.location.origin+'/control?var=servo&val='+this.value);}catch(e){}"></div>
        </div>
        -->
        <div class="cont_flex">  
            <div style="display: flex;align-items: center;">Speed <input type="range" id="speed" min="150" max="255" value="220" onchange="try{fetch(document.location.origin+'/control?var=speed&val='+this.value);}catch(e){}"></div>
        </div>
        <div class="cont_flex">  
            <div style="display: flex;align-items: center;">L E D<input type="range" id="flash" min="0" max="255" value="0" onchange="try{fetch(document.location.origin+'/control?var=flash&val='+this.value);}catch(e){}"></div>
        </div>
        <!--
        <div class="cont_flex">  
            <div style="display: flex;align-items: center;">Qual. <input type="range" id="quality" min="10" max="63" value="10" onchange="try{fetch(document.location.origin+'/control?var=quality&val='+this.value);}catch(e){}"></div>
        </div>
        <div class="cont_flex">  
            <div style="display: flex;align-items: center;">Frame <input type="range" id="framesize" min="0" max="5" value="5" onchange="try{fetch(document.location.origin+'/control?var=framesize&val='+this.value);}catch(e){}"></div>
        </div>
        -->

        <script>
            window.onload = function(){
                var canvas = document.getElementById("canvas");
                var ctx = canvas.getContext("2d");

                ctx.fillStyle = "rgb(255,0,0)";
                ctx.fillRect(73,25,60,35);
                ctx.clearRect(78,30,50,25);

                ctx.fillRect(93,20,20,5);
                ctx.fillRect(68,35,5,15);
                ctx.fillRect(133,35,5,15);

                ctx.beginPath();
                ctx.arc(92,42,6,0,2*Math.PI,true);
                ctx.fill();

                ctx.beginPath();
                ctx.arc(117,42,6,0,2*Math.PI,true);
                ctx.fill();

                ctx.beginPath();
                ctx.arc(104,100,35,0,Math.PI,true);
                ctx.fill();

                ctx.clearRect(50,85,100,20);

            }
        
            document.addEventListener(
            'DOMContentLoaded',function(){
                function b(B){let C;switch(B.type){case'checkbox':C=B.checked?1:0;break;case'range':case'select-one':C=B.value;break;case'button':case'submit':C='1';break;default:return;}const D=`${c}/control?var=${B.id}&val=${C}`;fetch(D).then(E=>{console.log(`request to ${D} finished, status: ${E.status}`)})}var c=document.location.origin;const e=B=>{B.classList.add('hidden')},f=B=>{B.classList.remove('hidden')},g=B=>{B.classList.add('disabled'),B.disabled=!0},h=B=>{B.classList.remove('disabled'),B.disabled=!1},i=(B,C,D)=>{D=!(null!=D)||D;let E;'checkbox'===B.type?(E=B.checked,C=!!C,B.checked=C):(E=B.value,B.value=C),D&&E!==C?b(B):!D&&('aec'===B.id?C?e(v):f(v):'agc'===B.id?C?(f(t),e(s)):(e(t),f(s)):'awb_gain'===B.id?C?f(x):e(x):'face_recognize'===B.id&&(C?h(n):g(n)))};document.querySelectorAll('.close').forEach(B=>{B.onclick=()=>{e(B.parentNode)}}),fetch(`${c}/status`).then(function(B){return B.json()}).then(function(B){document.querySelectorAll('.default-action').forEach(C=>{i(C,B[C.id],!1)})});const j=document.getElementById('stream'),k=document.getElementById('stream-container'),l=document.getElementById('get-still'),m=document.getElementById('toggle-stream'),n=document.getElementById('face_enroll'),o=document.getElementById('close-stream'),p=()=>{window.stop(),m.innerHTML='Iniciar Video'},q=()=>{j.src=`${c+':81'}/stream`,f(k),m.innerHTML='Stop Stream'};l.onclick=()=>{p(),j.src=`${c}/capture?_cb=${Date.now()}`,f(k)},o.onclick=()=>{p(),e(k)},m.onclick=()=>{const B='Stop Stream'===m.innerHTML;B?p():q()},n.onclick=()=>{b(n)},document.querySelectorAll('.default-action').forEach(B=>{B.onchange=()=>b(B)});const r=document.getElementById('agc'),s=document.getElementById('agc_gain-group'),t=document.getElementById('gainceiling-group');r.onchange=()=>{b(r),r.checked?(f(t),e(s)):(e(t),f(s))};const u=document.getElementById('aec'),v=document.getElementById('aec_value-group');u.onchange=()=>{b(u),u.checked?e(v):f(v)};const w=document.getElementById('awb_gain'),x=document.getElementById('wb_mode-group');w.onchange=()=>{b(w),w.checked?f(x):e(x)};const y=document.getElementById('face_detect'),z=document.getElementById('face_recognize'),A=document.getElementById('framesize');A.onchange=()=>{b(A),5<A.value&&(i(y,!1),i(z,!1))},y.onchange=()=>{return 5<A.value?(alert('Please select CIF or lower resolution before enabling this feature!'),void i(y,!1)):void(b(y),!y.checked&&(g(n),i(z,!1)))},z.onchange=()=>{return 5<A.value?(alert('Please select CIF or lower resolution before enabling this feature!'),void i(z,!1)):void(b(z),z.checked?(h(n),i(y,!0)):g(n))}});
        setInterval(function() {
    fetch(document.location.origin + '/status')
        .then(response => response.json())
        .then(data => {
            const span = document.getElementById('val_dist');
            span.innerHTML = data.distancia;
            
            // Si está muy cerca, ponlo en rojo
            if(data.distancia < 15) span.style.color = "red";
            else span.style.color = "#00FF00";
        });
}, 1000); // Se actualiza cada 1 segundo
        </script>
    </body>
</html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req){
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t status_uri = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_handler,
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
    
    Serial.printf("Starting web server on port: '%d'\n", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &status_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
    }

    config.server_port += 1;
    config.ctrl_port += 1;
    Serial.printf("Starting stream server on port: '%d'\n", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
}

void WheelAct(int speed_R, int speed_L, int nLf, int nLb, int nRf, int nRb)
{
    ledcWrite(ENR, speed_R);
    ledcWrite(ENL, speed_L);
    digitalWrite(gpLf, nLf);
    digitalWrite(gpLb, nLb);
    digitalWrite(gpRf, nRf);
    digitalWrite(gpRb, nRb);
}
