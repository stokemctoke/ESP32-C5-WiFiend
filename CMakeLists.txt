# WiFiend ESP32-C5 Interactive WiFi Hacking Handheld
# Requires: ESP-IDF v6.0+ (or v5.5.1 minimum)
# Note: patched libnet80211.a in ./patched_libnet/ for deauth support

cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(WiFiend)
