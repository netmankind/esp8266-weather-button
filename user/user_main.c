/*
ESP8266 weather display firmware
Copyright (C) 2017 Sakari Kapanen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <osapi.h>
#include <ets_sys.h>
#include <user_interface.h>
#include <espmissingincludes.h>
#include <driver/uart.h>

#include "u8g2.h"
#include "u8g2_esp8266_hal.h"

#include "ntp.h"
#include "jsmn.h"
#include "httpclient.h"
#include "wifi_station.h"

#include "images/icons_32.h"
#include "util.h"
#include "credentials.h"
#include "my_config.h"

#define CONNECTION_TIMEOUT 10000
#define DATA_FETCH_TIMEOUT 10000

#define SLEEP_INTERVAL 5000

#define FORECAST_MAX_COUNT 8

os_timer_t timeout_timer;

u8g2_t u8g2;
jsmn_parser parser;

typedef struct {
    time_t time;
    int temp;
    weather_icon_t icon;
} weather_t;

void oled_draw_forecast(int x, int y, const weather_t *forecast,
    bool draw_weekday) {
    int dx;

    u8g2_SetFontPosTop(&u8g2);

    struct tm *dt = gmtime(&forecast->time);
    char buf[32];
    if (draw_weekday) {
        os_sprintf(buf, "%s %02d", WEEKDAYS[dt->tm_wday], dt->tm_hour);
    } else {
        os_sprintf(buf, "%02d", dt->tm_hour);
    }
    dx = (32 - u8g2_GetUTF8Width(&u8g2, buf)) / 2;
    u8g2_DrawUTF8(&u8g2, x + dx, y, buf);

    const uint8_t *bitmap = get_icon_bitmap(forecast->icon);
    if (bitmap != NULL) {
        u8g2_DrawXBMP(&u8g2, x, y + 14, 32, 32, bitmap);
    }

    os_sprintf(buf, "%d°C", forecast->temp);
    dx = (32 - u8g2_GetUTF8Width(&u8g2, buf)) / 2;
    u8g2_DrawUTF8(&u8g2, x + dx, y + 52, buf);
}

void oled_draw_forecasts(const weather_t *forecasts, int n_forecasts) {
    u8g2_ClearBuffer(&u8g2);

    int j;
    for (j = 0; j < n_forecasts; ++j) {
        struct tm *dt = gmtime(&forecasts[j].time);
        if (dt->tm_hour < 15 && dt->tm_hour > 6) {
            break;
        }
    }
    if (n_forecasts - j < 3) return;

    const weather_t *shift_forecasts = &forecasts[j];
    for (int i = 0; i < 3; ++i) {
        oled_draw_forecast(2 + i*46, 0, &shift_forecasts[i], i == 0);
    }

    u8g2_SendBuffer(&u8g2);
}

void oled_init(void) {
    u8g2_Setup_ssd1306_128x64_noname_f(&u8g2, U8G2_R0,
        u8x8_byte_esp8266_hw_spi,
        u8x8_gpio_and_delay_esp8266);  // init u8g2 structure
    u8g2_InitDisplay(&u8g2); // send init sequence to the display, display is in sleep mode after this,
    u8g2_SetPowerSave(&u8g2, 0); // wake up display
    u8g2_SetFont(&u8g2, u8g2_font_profont12_tf);
    u8g2_ClearDisplay(&u8g2);
}

char current_key[64];
int array_level;
int object_level;
bool in_list;
long dt;
int temp;
weather_icon_t icon;
weather_t forecasts[FORECAST_MAX_COUNT];
int forecast_count;

void start_arr(void) {
    if (os_strcmp(current_key, "list") == 0) {
        in_list = true;
        array_level = 0;
        object_level = 0;
    }

    array_level += 1;
}

void end_arr(void) {
    if (in_list && (--array_level) == 0) {
        in_list = false;
    }
}

void start_obj(void) {
    if (!in_list) return;

    if (object_level == 0) {
        dt = 0;
        temp = 0.0;
        icon = ICON_NONE;
    }

    ++object_level;
}

void end_obj(void) {
    if (!in_list) return;

    if ((--object_level) == 0 && forecast_count < FORECAST_MAX_COUNT) {
        forecasts[forecast_count].time = dt;
        forecasts[forecast_count].temp = temp;
        forecasts[forecast_count].icon = icon;
        forecast_count += 1;
    }
}

void obj_key(const char *key, size_t key_len) {
    os_strcpy(current_key, key);
}

void str(const char *value, size_t len) {
    if (os_strcmp(current_key, "icon") == 0 && icon == ICON_NONE) {
        icon = atoi(value);
    }
}

void primitive(const char *value, size_t len) {
    if (os_strcmp(current_key, "dt") == 0 && dt == 0) {
        dt = atoi(value);
    } else if (os_strcmp(current_key, "temp") == 0 && temp == 0) {
        temp = atoi(value);
        char *point = os_strchr(value, '.');
        if (point != NULL && point - value > len && *(point + 1) >= '5') {
            if (value[0] == '-') temp -= 1;
            else temp += 1;
        }
    }
}

void init_weather_parser(void) {
    current_key[0] = '\0';
    array_level = 0;
    object_level = 0;
    in_list = false;
    forecast_count = 0;
}

jsmn_callbacks_t cbs = {
    start_arr,
    end_arr,
    start_obj,
    end_obj,
    obj_key,
    str,
    primitive
};

void go_to_sleep(void) {
    os_printf("Going to sleep\n");
    u8g2_SetPowerSave(&u8g2, 1); // put display to sleep
    system_deep_sleep(1000 * SLEEP_INTERVAL);
}

void http_get_callback(char * response_body, int http_status,
    char * response_headers, int body_size) {
    static int current_status = 0;
    if (response_headers != NULL) {
        current_status = http_status;
    }
    if (current_status == 200 && response_body != NULL) {
        char ch;
        while ((ch = *(response_body++)) != '\0') {
            jsmn_parse(&parser, ch);
        }
    }

    if (http_status == HTTP_STATUS_DISCONNECT && forecast_count >= 3) {
        oled_draw_forecasts(forecasts, forecast_count);
    }
}

char owmap_query[128];
void ntp_cb(time_t timestamp, struct tm *dt) {
    apply_tz(dt, TIMEZONE_OFFSET);
    os_printf("Date %d.%d, time %d.%d\n",
        dt->tm_mday, dt->tm_mon + 1, dt->tm_hour, dt->tm_min);

    os_sprintf(owmap_query,
        "http://api.openweathermap.org/data/2.5/forecast?id=%s&appid=%s&units=metric",
        OWMAP_CITY_ID, OWMAP_API_KEY);

    http_get_streaming(owmap_query, "", http_get_callback);  // Example domain for testing for now - this sends chunked responses

}

void ntp_dns_cb(uint8_t *addr) {
    if (addr == NULL) {
        os_printf("Error resolving NTP server address\n");
        return;
    }

    ntp_get_time(addr, ntp_cb);
}

void wifi_connect_cb(bool connected) {
    if (connected) {
        os_timer_disarm(&timeout_timer);
        os_timer_setfn(&timeout_timer, (os_timer_func_t *)go_to_sleep, NULL);
        /* os_timer_arm(&timeout_timer, DATA_FETCH_TIMEOUT, false); */

        dns_resolve("time.nist.gov", ntp_dns_cb);
    } else {
        os_printf("Connection failed\n");
        go_to_sleep();
    }
}

void user_init(void) {
    system_update_cpu_freq(80);
    uart_init(BIT_RATE_115200, BIT_RATE_115200);

    oled_init();

    jsmn_init(&parser, &cbs);

    wifi_station_init(WIFI_SSID, WIFI_PWD, wifi_connect_cb, CONNECTION_TIMEOUT);
}

