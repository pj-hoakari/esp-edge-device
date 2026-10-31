idf_component_register(SRCS "mjpeg_main.c" "main.c" "camera.c" "jpeg_frame.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash esp_http_server esp_http_client esp_peer usb_host_uvc esp_timer
    EMBED_TXTFILES "web/mjpeg.html")
