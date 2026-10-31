# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/vendor/esp-webrtc-solution/components/esp_peer"
    "${CMAKE_CURRENT_LIST_DIR}/vendor/esp-usb/host/class/uvc/usb_host_uvc"
    "${CMAKE_CURRENT_LIST_DIR}/vendor/esp-usb/host/usb")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_webcam)

# esp_peer 1.5.7 builds a raw '-L <path>' string, which splits paths containing
# spaces at the final link. Keep vendor sources intact and let CMake quote it.
idf_component_get_property(peer_lib esp_peer COMPONENT_LIB)
foreach(link_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(peer_links ${peer_lib} ${link_property})
    list(FILTER peer_links EXCLUDE REGEX "-L ")
    set_property(TARGET ${peer_lib} PROPERTY ${link_property} "${peer_links}")
endforeach()
target_link_directories(${peer_lib} PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/vendor/esp-webrtc-solution/components/esp_peer/libs/${IDF_TARGET}")
