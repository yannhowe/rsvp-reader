#pragma once

#include <Arduino.h>

// Initialize WiFi subsystem (call in setup). Does NOT start the radio.
void wifi_init();

// Start WiFi AP + mDNS + web server.
void wifi_start();

// Stop everything and power down the radio.
void wifi_stop();

// Returns true if WiFi AP is currently running.
bool wifi_is_active();

// Service web server clients. Call every loop() iteration.
// No-op when WiFi is inactive (~0 overhead).
void wifi_tick();

// Returns the SSID of the access point ("RSVP-Reader").
const char* wifi_ssid();

// Returns the IP address string when active ("192.168.4.1"), or "" when off.
const char* wifi_ip();
