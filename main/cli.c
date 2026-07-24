#include "cli.h"
#include "wifi_scan.h"
#include "battery.h"
#include "radio_mgr.h"
#include "deauth_engine.h"
#include "esp_console.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_log.h"
#include "linenoise/linenoise.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "cli";

#define LFS_ROOT     "/lfs"
#define CLI_VERSION  "WiFiend v1.0"

static int cmd_help(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("Commands:\n");
    printf("  help          List commands\n");
    printf("  scan          Run WiFi scan\n");
    printf("  battery       Show battery mV / %%\n");
    printf("  heap          Free heap bytes\n");
    printf("  mode          Current radio_mgr mode\n");
    printf("  ls            List /lfs files\n");
    printf("  cat <path>    Print file (under /lfs)\n");
    printf("  deauth stop   Stop deauth engine\n");
    printf("  version       Firmware version\n");
    return 0;
}

static int cmd_scan(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uint16_t n = wifi_scan_start();
    printf("Scan done: %u AP(s)\n", (unsigned)n);
    return 0;
}

static int cmd_battery(int argc, char **argv) {
    (void)argc;
    (void)argv;
    battery_tick();
    uint16_t mv = battery_read_mv();
    uint8_t pct = battery_get_percentage();
    if (pct == BATTERY_INVALID) {
        printf("Battery: %u mV (USB / no LiPo)\n", (unsigned)mv);
    } else {
        printf("Battery: %u mV (%u%%)\n", (unsigned)mv, (unsigned)pct);
    }
    return 0;
}

static int cmd_heap(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("Free heap: %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    return 0;
}

static int cmd_mode(int argc, char **argv) {
    (void)argc;
    (void)argv;
    radio_mode_t m = radio_mgr_current();
    printf("Radio mode: %s (%d)\n", radio_mgr_mode_name(m), (int)m);
    return 0;
}

static int cmd_ls(int argc, char **argv) {
    (void)argc;
    (void)argv;
    DIR *dir = opendir(LFS_ROOT);
    if (!dir) {
        printf("Cannot open %s\n", LFS_ROOT);
        return 1;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char path[64];
        snprintf(path, sizeof(path), "%s/%.48s", LFS_ROOT, de->d_name);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            printf("%-20s %ld\n", de->d_name, (long)st.st_size);
        }
    }
    closedir(dir);
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: cat <filename>\n");
        return 1;
    }

    char path[64];
    if (argv[1][0] == '/') {
        strncpy(path, argv[1], sizeof(path) - 1);
    } else {
        snprintf(path, sizeof(path), "%s/%s", LFS_ROOT, argv[1]);
    }
    path[sizeof(path) - 1] = '\0';

    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("Cannot open %s\n", path);
        return 1;
    }

    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) {
        printf("%s", buf);
    }
    fclose(fp);
    return 0;
}

static int cmd_deauth(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "stop") != 0) {
        printf("Usage: deauth stop\n");
        return 1;
    }
    if (deauth_engine_is_running()) {
        deauth_engine_stop();
        printf("Deauth stopped\n");
    } else {
        printf("Deauth not running\n");
    }
    return 0;
}

static int cmd_version(int argc, char **argv) {
    (void)argc;
    (void)argv;
    esp_chip_info_t info;
    esp_chip_info(&info);
    printf("%s — ESP32-C5\n", CLI_VERSION);
    printf("Cores: %d  Rev: %d\n", info.cores, info.revision);
    return 0;
}

static void register_commands(void) {
    const esp_console_cmd_t cmds[] = {
        { .command = "help",        .help = "List commands",           .func = &cmd_help },
        { .command = "scan",        .help = "Run WiFi scan",           .func = &cmd_scan },
        { .command = "battery",     .help = "Battery mV/percent",      .func = &cmd_battery },
        { .command = "heap",        .help = "Free heap",               .func = &cmd_heap },
        { .command = "mode",        .help = "radio_mgr mode",          .func = &cmd_mode },
        { .command = "ls",          .help = "List /lfs",               .func = &cmd_ls },
        { .command = "cat",         .help = "Print file",              .func = &cmd_cat },
        { .command = "deauth",      .help = "deauth stop",             .func = &cmd_deauth },
        { .command = "version",     .help = "Firmware version",        .func = &cmd_version },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

void cli_init(void) {
    // Register commands on the existing USB/UART console without starting a
    // second linenoise REPL (that fought the primary console and ate RAM).
    esp_console_config_t cfg = ESP_CONSOLE_CONFIG_DEFAULT();
    esp_err_t err = esp_console_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_console_init: %s", esp_err_to_name(err));
        return;
    }

    esp_console_register_help_command();
    register_commands();
    ESP_LOGI(TAG, "CLI commands registered (%s) — use idf.py monitor", CLI_VERSION);
}
