set(WEBCAM_MAIN "${CMAKE_CURRENT_LIST_DIR}/../../esp_webcam/main")
idf_component_register(SRCS "tab5_main.c" "webrtc_h264.c" "camera_h264.c" "status_display.c"
    "${WEBCAM_MAIN}/network_settings.c" "${WEBCAM_MAIN}/network_settings_values.c"
    "${WEBCAM_MAIN}/wifi_auth.c" "${WEBCAM_MAIN}/network_diagnostics.c" "${WEBCAM_MAIN}/network_probes.c"
    "${WEBCAM_MAIN}/cloud_settings.c" "${WEBCAM_MAIN}/cloud_settings_values.c" "${WEBCAM_MAIN}/cloud_auth_logic.c"
    "${WEBCAM_MAIN}/device_auth.c" "${WEBCAM_MAIN}/device_auth_values.c" "${WEBCAM_MAIN}/peer_authorization.c"
    "${WEBCAM_MAIN}/firebase_client.c" "${WEBCAM_MAIN}/cloud_signaling.c"
    "${WEBCAM_MAIN}/nvs_boot.c" "${WEBCAM_MAIN}/nvs_boot_values.c"
    INCLUDE_DIRS "." "${WEBCAM_MAIN}"
    REQUIRES esp_wifi esp_netif nvs_flash esp_http_client esp_peer esp_timer mbedtls esp_system
        esp_hw_support esp_app_format esp_driver_uart esp_driver_usb_serial_jtag wpa_supplicant lwip
        espressif__cjson espressif__esp_hosted m5stack_tab5)
