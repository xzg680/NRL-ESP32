cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nrl-esp32)

idf_component_get_property(arduino_lib framework-arduinoespressif32 COMPONENT_LIB)
target_link_libraries(
    ${arduino_lib}
    PUBLIC
        idf::bt
        idf::esp_http_client
        idf::esp_https_ota
)
