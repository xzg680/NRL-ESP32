cmake_minimum_required(VERSION 3.16.0)
if(DEFINED NRL_EXTRA_COMPONENT_DIRS)
    foreach(extra_component_dir IN LISTS NRL_EXTRA_COMPONENT_DIRS)
        if(IS_ABSOLUTE "${extra_component_dir}")
            list(APPEND EXTRA_COMPONENT_DIRS "${extra_component_dir}")
        else()
            list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/${extra_component_dir}")
        endif()
    endforeach()
endif()
# The vendored esp-nimble-cpp overrides the registry copy the component manager
# pulls. This matters on S31, where the registry copy rejects the esp32s31 target;
# the vendored copy supports it and registers as an empty component there (see
# vendor/h2zero__esp-nimble-cpp/CMakeLists.txt), since S31 uses the Bluedroid host.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/vendor/h2zero__esp-nimble-cpp/CMakeLists.txt")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/vendor/h2zero__esp-nimble-cpp")
endif()
# Support for NATIVE idf.py builds (the GitHub build-s31 job); pass
# -DNRL_NATIVE_BUILD=1. PlatformIO is NOT involved in these, so we must
# reproduce what platformio.ini normally provides:
#   * src_dir = src -> register src/ as a component. There is no main/ dir, so
#     without this the app (incl. app_main) is never compiled/linked -> the link
#     fails with "undefined reference to app_main". WHOLE_ARCHIVE on the src
#     component (src/CMakeLists.txt) then keeps app_main from being dropped.
#   * [env:s31_korvo] build_flags -> the S31 board identity + feature macros.
#     NRL_BOARD in particular MUST be set or board_pins.h defaults to gezipai.
#     Firmware version via -DNRL_FW_VERSION=... (optional; nrl_version.h default).
# Gated on NRL_NATIVE_BUILD (not IDF_TARGET) so PlatformIO's own S31 build, which
# already registers src as its main component, does not double-register it.
if(NRL_NATIVE_BUILD)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/src")
    add_compile_definitions(
        NRL_BOARD=NRL_BOARD_S31_KORVO
        NRL_ENABLE_AUDIO_AFE=1
        NRL_ENABLE_AEC=1
    )
    if(NRL_FW_VERSION)
        add_compile_definitions(NRL_FIRMWARE_VERSION="${NRL_FW_VERSION}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nrl-esp32)

# S31 Bluetooth (Bluedroid HFP AG): the default Bluedroid security-service-record
# table holds only 3 entries (SDP + AG + device). When the AG *initiates* a
# headset connection it also needs records for SDP discovery + the RFCOMM
# service, overflowing 3 ("BTM_SEC_REG: Out of Service Records (3)" ->
# "MX service not found" -> RFCOMM start fails). Raise the table for the bt
# component so the HFP service-level connection can complete.
if(IDF_TARGET STREQUAL "esp32s31")
    idf_component_get_property(bt_lib bt COMPONENT_LIB)
    if(bt_lib)
        target_compile_definitions(${bt_lib} PRIVATE BTM_SEC_MAX_SERVICE_RECORDS=16)
    endif()
endif()

if(TARGET ${CMAKE_PROJECT_NAME}.elf)
    set(nrl_mbedtls_archive_dir "${CMAKE_BINARY_DIR}/esp-idf/mbedtls/mbedtls")
    target_link_libraries(${CMAKE_PROJECT_NAME}.elf PRIVATE
        "-Wl,--start-group"
        "${nrl_mbedtls_archive_dir}/library/libmbedtls.a"
        "${nrl_mbedtls_archive_dir}/library/libmbedx509.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/core/libtfpsacrypto.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/drivers/builtin/libmbed-builtin.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/platform/libplatform.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/utilities/libutilities.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/extras/libextras.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/drivers/everest/libeverest.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/drivers/p256-m/libp256-m.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/drivers/pqcp/libpqcp.a"
        "-Wl,--end-group")
endif()
