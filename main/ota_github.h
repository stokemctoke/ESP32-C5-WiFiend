#ifndef OTA_GITHUB_H
#define OTA_GITHUB_H

#include <stdbool.h>

// Dedicated boot mode: join saved home Wi-Fi, check GitHub releases/latest,
// download WiFiend.bin if newer, flash inactive OTA slot. Never returns
// (always reboots — MENU on success/up-to-date, WEBUI on failure).
void ota_github_run(void);

// Current firmware version from the app descriptor (version.txt / PROJECT_VER).
const char *ota_github_fw_version(void);

#endif
