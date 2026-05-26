#pragma once
#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
