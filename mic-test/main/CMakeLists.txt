idf_component_register(
    SRCS "main.cpp" "wifi.cpp" "ws_client.cpp" "buttons.cpp" "display.cpp"
         "cst820.cpp" "codex_pet_assets.cpp" "claude_pet_assets.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_codec_dev M5IOE1 M5PM1 M5GFX lvgl json
             nvs_flash esp_wifi esp_netif esp_event esp_http_server
             esp_websocket_client
)
