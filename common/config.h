#ifndef AVSYNC_CONFIG_H
#define AVSYNC_CONFIG_H

// Shared-memory configuration block for avsync_hook.dll.
// Created and held open by the injector/configurator (section disappears when
// the owner exits -> the hook falls back to delay 0, i.e. pass-through).

#define AVSYNC_SHM_NAME  L"Local\\AVSyncDelayConfig"
#define AVSYNC_MAGIC     0x41565344u   /* 'AVSD' */
#define AVSYNC_VERSION   2u
#define AVSYNC_MAX_DEVICES 32
#define AVSYNC_ID_LEN    256
#define AVSYNC_MAX_BROWSERS 16
#define AVSYNC_EXE_LEN   32

typedef struct _AVSYNC_DEVICE {
    wchar_t id[AVSYNC_ID_LEN];   /* IMMDevice::GetId() string */
    long    delayMs;             /* per-device delay (phase 3) */
} AVSYNC_DEVICE;

/* Per-browser delay override, keyed by executable base name (lowercase).
 * Different browsers have different native output buffering, so the extra
 * delay needed to sync the same video differs per browser. */
typedef struct _AVSYNC_BROWSER {
    wchar_t exe[AVSYNC_EXE_LEN]; /* e.g. "firefox.exe", "browser.exe" (Yandex) */
    long    delayMs;
} AVSYNC_BROWSER;

typedef struct _AVSYNC_CONFIG {
    unsigned      magic;         /* AVSYNC_MAGIC */
    unsigned      version;       /* AVSYNC_VERSION */
    unsigned      enabled;       /* master switch: 1 = apply delays */
    long          globalDelayMs; /* fallback for browsers without an override */
    unsigned      browserCount;  /* entries used in browsers[] */
    AVSYNC_BROWSER browsers[AVSYNC_MAX_BROWSERS];
    unsigned      deviceCount;   /* entries used in devices[] (phase 3) */
    AVSYNC_DEVICE devices[AVSYNC_MAX_DEVICES];
} AVSYNC_CONFIG;

#endif /* AVSYNC_CONFIG_H */
