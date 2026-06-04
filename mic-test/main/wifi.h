#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Init NVS + WiFi stack. Call once at boot, before anything else.
void wifi_init(void);

// Try the credentials in NVS. Returns true if connected within timeout.
bool wifi_try_connect_from_nvs(int timeout_ms);

// Start SoftAP + HTTP provisioning page. Blocks while AP runs.
// When user submits creds: saves to NVS, schedules esp_restart() in ~1s.
void wifi_start_provisioning(void);

bool wifi_is_connected(void);

// Read the user-configured Mac hostname (set via SoftAP) into `buf`.
// Returns true if a host was saved. Falls back to compile-time default in main.cpp.
bool wifi_get_mac_host(char* buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
