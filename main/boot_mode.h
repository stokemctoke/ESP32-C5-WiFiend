#ifndef BOOT_MODE_H
#define BOOT_MODE_H

#include <stdint.h>

// RTC-persisted boot destination (survives esp_restart, cleared on cold power-on).
#define BOOT_MAGIC       0x57464542u   /* 'WFEB' */
#define BOOT_DEST_MENU   0u
#define BOOT_DEST_WEBUI  1u
#define BOOT_DEST_OTA    2u

// Read-and-clear the destination requested by the previous reboot.
uint32_t boot_mode_consume(void);

// Persist dest and reboot (short delay so HTTP/WS can flush).
void boot_mode_reboot(uint32_t dest);

#endif
