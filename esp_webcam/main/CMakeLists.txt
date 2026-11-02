idf_component_register(SRCS "mjpeg_main.c" "main.c" "camera.c" "jpeg_frame.c" "udp_fast.c" "perf.c"
    "network_settings.c" "network_settings_values.c" "wifi_auth.c" "network_diagnostics.c" "network_probes.c"
    "cloud_settings.c" "firebase_client.c" "cloud_signaling.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../vendor/esp-webrtc-solution/components/esp_peer/src"
    REQUIRES esp_wifi esp_netif nvs_flash esp_http_server esp_http_client esp_peer usb_host_uvc esp_timer mbedtls esp_system esp_hw_support esp_app_format esp_driver_uart wpa_supplicant lwip json
    EMBED_TXTFILES "web/mjpeg.html"
    # Include the strong UDP hook even though esp_peer already supplies a weak
    # definition; otherwise the linker need not extract udp_fast.c from main.a.
    WHOLE_ARCHIVE)
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=mbedtls_ssl_write" "-Wl,--wrap=mbedtls_ssl_read"
    "-Wl,--wrap=udp_socket_recvfrom_nowait")
