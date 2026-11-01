idf_component_register(SRCS "mjpeg_main.c" "main.c" "camera.c" "jpeg_frame.c" "udp_fast.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../vendor/esp-webrtc-solution/components/esp_peer/src"
    REQUIRES esp_wifi esp_netif nvs_flash esp_http_server esp_http_client esp_peer usb_host_uvc esp_timer
    EMBED_TXTFILES "web/mjpeg.html"
    # Include the strong UDP hook even though esp_peer already supplies a weak
    # definition; otherwise the linker need not extract udp_fast.c from main.a.
    WHOLE_ARCHIVE)
