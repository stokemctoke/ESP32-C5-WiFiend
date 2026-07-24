#ifndef CLI_H
#define CLI_H

// Lightweight USB Serial/JTAG console (esp_console + linenoise).
// Call cli_init() once after NVS/LittleFS are ready.

void cli_init(void);

#endif
