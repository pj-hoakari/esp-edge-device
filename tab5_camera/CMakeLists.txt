cmake_minimum_required(VERSION 3.16)

set(WEBCAM_DIR "${CMAKE_CURRENT_LIST_DIR}/../esp_webcam")
set(EXTRA_COMPONENT_DIRS
    "${WEBCAM_DIR}/vendor/esp-webrtc-solution/components/esp_peer")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_camera)

if(CONFIG_COMPILER_OPTIMIZATION_PERF)
    foreach(crypto_target mbedtls mbedx509 tfpsacrypto builtin)
        if(TARGET ${crypto_target})
            target_compile_options(${crypto_target} PRIVATE -O3)
        endif()
    endforeach()
endif()

idf_component_get_property(peer_lib esp_peer COMPONENT_LIB)
foreach(link_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(peer_links ${peer_lib} ${link_property})
    list(FILTER peer_links EXCLUDE REGEX "-L ")
    set_property(TARGET ${peer_lib} PROPERTY ${link_property} "${peer_links}")
endforeach()
target_link_directories(${peer_lib} PUBLIC
    "${WEBCAM_DIR}/vendor/esp-webrtc-solution/components/esp_peer/libs/${IDF_TARGET}")
