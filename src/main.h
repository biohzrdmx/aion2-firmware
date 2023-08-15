#ifndef MAIN_h
#define MAIN_h

#include <Arduino.h>

void setup();
void setup_ap();
void setup_client();
void callback_xhr_scan();
void callback_xhr_connect();
void callback_xhr_ping();
void callback_xhr_reset();
void callback_xhr_rpc();
void read_eeprom();
void write_eeprom(String ssid, String password, String cloud_uid);
void clear_eeprom();
void on_hold_reset();
void on_pressed_reset();
void update_sensor_data();
float convert_cto_f(float c);
float convert_fto_c(float f);
float compute_heat_index(float temperature, float percentHumidity, bool isFahrenheit);
void loop();

#endif
