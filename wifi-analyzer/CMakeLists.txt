# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi-analyzer)

# The registry wolfSSL/wolfSSH versions predate C23 and ESP-IDF 6.1: a port file
# uses `thread_local` as an enum member, which GCC 15 (C23 default) treats as a
# reserved keyword. Compile just these two components as C17 so it builds, and
# don't let their internal warnings fail the build.
foreach(_wc wolfssl__wolfssl wolfssl__wolfssh)
    idf_component_get_property(_lib ${_wc} COMPONENT_LIB)
    if(_lib)
        target_compile_options(${_lib} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-std=gnu17>
            -Wno-error)
        # Force all-software crypto: wolfSSL 5.x's ESP32 hardware-crypto port uses
        # periph/HAL APIs removed in ESP-IDF 6.x. Several headers check the granular
        # guards independently of the master NO_ESP32_CRYPT, so define them all.
        target_compile_definitions(${_lib} PRIVATE
            NO_ESP32_CRYPT
            NO_WOLFSSL_ESP32_CRYPT_HASH
            NO_WOLFSSL_ESP32_CRYPT_AES
            NO_WOLFSSL_ESP32_CRYPT_RSA_PRI)
    endif()
endforeach()
