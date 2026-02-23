set(srcs "Misc.cpp"
         "Listener.cpp"
         "SocketServer.cpp"
         "Connection.cpp"
         "DNSServer.cpp"
         "WirelessConfigurationMgr.cpp")
set(include_dirs ".")

if(IDF_TARGET STREQUAL "esp8266")
     list(APPEND srcs "esp8266/gpio.c" "esp8266/partition.c" "esp8266/HSPI.cpp")
     list(APPEND include_dirs "esp8266")
else()
     list(APPEND srcs "esp32/HSPI.cpp")
     list(APPEND include_dirs "esp32")
endif()

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "${include_dirs}"
                       REQUIRES spi_flash mdns
                       PRIV_REQUIRES indicator nvs_flash wpa_supplicant spiffs)

target_compile_options(${COMPONENT_LIB} PUBLIC -std=gnu++17)

if(DEBUG_BUILD)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC "DEBUG")
endif()

idf_build_get_property(python PYTHON)
idf_build_get_property(build_dir BUILD_DIR)

if(IDF_TARGET STREQUAL "esp8266")
    set(bin "DuetWiFiServer.bin")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC "ESP8266")
    target_linker_script(${COMPONENT_LIB} INTERFACE "esp8266/scratch.ld")
else()
    string(REPLACE "esp" "" chip "${IDF_TARGET}")
    string(TOUPPER "${chip}" chip)
    set(bin "DuetWiFiModule_${chip}.bin")
endif()

add_custom_target(image ALL DEPENDS app bootloader
    COMMAND ${python} "${CMAKE_CURRENT_LIST_DIR}/create_image.py"
                "cmake" "${build_dir}" "${build_dir}/${bin}")
