#include "oui_lookup.h"
#include <stddef.h>

#define OUI24(a, b, c) ((uint32_t)(a) << 16 | (uint32_t)(b) << 8 | (c))

typedef struct {
    uint32_t    oui;
    const char *name;
} oui_entry_t;

// Sorted by oui for binary search (~50 common vendors).
static const oui_entry_t oui_table[] = {
    { OUI24(0x00, 0x03, 0x93), "Apple"      },
    { OUI24(0x00, 0x14, 0x51), "Apple"      },
    { OUI24(0x00, 0x15, 0x17), "Intel"      },
    { OUI24(0x00, 0x17, 0xF2), "Apple"      },
    { OUI24(0x00, 0x19, 0xE3), "Apple"      },
    { OUI24(0x00, 0x1A, 0x11), "Google"     },
    { OUI24(0x00, 0x1B, 0x63), "Apple"      },
    { OUI24(0x00, 0x1C, 0xB3), "Apple"      },
    { OUI24(0x00, 0x1D, 0x0F), "TP-Link"    },
    { OUI24(0x00, 0x1D, 0xD8), "Microsoft"  },
    { OUI24(0x00, 0x1D, 0xE0), "Intel"      },
    { OUI24(0x00, 0x1E, 0xC2), "Apple"      },
    { OUI24(0x00, 0x1F, 0x5B), "Apple"      },
    { OUI24(0x00, 0x24, 0x54), "Samsung"    },
    { OUI24(0x00, 0x25, 0x9E), "Huawei"     },
    { OUI24(0x00, 0x27, 0x22), "Ubiquiti"   },
    { OUI24(0x00, 0x50, 0x56), "VMware"     },
    { OUI24(0x00, 0x50, 0xF2), "Microsoft"  },
    { OUI24(0x00, 0x90, 0x4C), "Intel"      },
    { OUI24(0x00, 0xE0, 0x4C), "Realtek"    },
    { OUI24(0x04, 0x18, 0xD6), "Ubiquiti"   },
    { OUI24(0x04, 0xD4, 0xC4), "ASUS"       },
    { OUI24(0x0C, 0x47, 0xC9), "Amazon"     },
    { OUI24(0x10, 0x2C, 0x83), "Xiaomi"     },
    { OUI24(0x14, 0xDD, 0xA9), "ASUS"       },
    { OUI24(0x18, 0xB4, 0x30), "Nest"       },
    { OUI24(0x1C, 0x61, 0xB4), "TP-Link"    },
    { OUI24(0x24, 0x6F, 0x28), "Espressif"  },
    { OUI24(0x28, 0x6A, 0xBA), "Apple"      },
    { OUI24(0x30, 0xAE, 0xA4), "Espressif"  },
    { OUI24(0x34, 0xCE, 0x00), "Xiaomi"     },
    { OUI24(0x38, 0x43, 0x7D), "Sonos"      },
    { OUI24(0x3C, 0x52, 0x82), "HP"         },
    { OUI24(0x44, 0x04, 0xA8), "Amazon"     },
    { OUI24(0x48, 0x5A, 0xB6), "Huawei"     },
    { OUI24(0x48, 0xDB, 0x50), "Huawei"     },
    { OUI24(0x54, 0x60, 0x09), "Google"     },
    { OUI24(0x5C, 0x62, 0x8B), "TP-Link"    },
    { OUI24(0x60, 0x32, 0xB1), "TP-Link"    },
    { OUI24(0x64, 0x09, 0x80), "Xiaomi"     },
    { OUI24(0x6C, 0xAD, 0xF8), "Apple"      },
    { OUI24(0x7C, 0x1E, 0x52), "Microsoft"  },
    { OUI24(0x84, 0xF3, 0xEB), "Espressif"  },
    { OUI24(0x88, 0x66, 0x5A), "Apple"      },
    { OUI24(0x8C, 0x77, 0x12), "Samsung"    },
    { OUI24(0xA0, 0x76, 0x4E), "Espressif"  },
    { OUI24(0xA4, 0x77, 0x33), "Google"     },
    { OUI24(0xAC, 0x84, 0xC6), "Samsung"    },
    { OUI24(0xB0, 0xBE, 0x76), "Apple"      },
    { OUI24(0xB8, 0x27, 0xEB), "Raspberry"  },
    { OUI24(0xB8, 0xE9, 0x37), "Sonos"      },
    { OUI24(0xC0, 0x25, 0xE7), "Apple"      },
    { OUI24(0xC8, 0x3A, 0x35), "Tesla"      },
    { OUI24(0xCC, 0x6E, 0xA4), "Roku"       },
    { OUI24(0xD0, 0x50, 0x99), "ASUS"       },
    { OUI24(0xDC, 0x26, 0x2A), "Raspberry"  },
    { OUI24(0xE0, 0x46, 0x9A), "Netgear"    },
    { OUI24(0xE4, 0x5F, 0x01), "Raspberry"  },
    { OUI24(0xF4, 0xF5, 0xD8), "Google"     },
    { OUI24(0xFC, 0xAA, 0x14), "Google"     },
};

#define OUI_COUNT (sizeof(oui_table) / sizeof(oui_table[0]))

static uint32_t mac_to_oui24(const uint8_t mac[6]) {
    return ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
}

const char *oui_lookup(const uint8_t mac[6]) {
    if (!mac) return NULL;
    if (mac[0] & 0x02) return "Local";

    uint32_t key = mac_to_oui24(mac);
    int lo = 0;
    int hi = (int)OUI_COUNT - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (oui_table[mid].oui == key) return oui_table[mid].name;
        if (oui_table[mid].oui < key) lo = mid + 1;
        else                          hi = mid - 1;
    }
    return NULL;
}
