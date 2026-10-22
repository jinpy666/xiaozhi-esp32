message("78/esp_wifi_connect: IDF_VERSION_MAJOR is ${IDF_VERSION_MAJOR}")

if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    # v6
    message("78/esp_wifi_connect: idf_component_register for v6 used")

    idf_component_register(
        SRCS
            "wifi_manager.cc"
            "wifi_configuration_ap.cc"
            "wifi_station.cc"
            "ssid_manager.cc"
            "dns_server.cc"
        INCLUDE_DIRS
            "include"
        REQUIRES "esp_timer" "esp_http_server" "esp_wifi" "nvs_flash"
    )
else()
    # v5
    message("78/esp_wifi_connect: idf_component_register for v5 used")

    idf_component_register(
        SRCS
            "wifi_manager.cc"
            "wifi_configuration_ap.cc"
            "wifi_station.cc"
            "ssid_manager.cc"
            "dns_server.cc"
        INCLUDE_DIRS
            "include"
        REQUIRES "esp_timer" "esp_http_server" "esp_wifi" "nvs_flash" "json"
    )
endif()

# ---------------------------------------------------------------------------
# Portal pages: stock pages by default, replaceable by an OEM brand pack.
#
# The product build pipeline (scripts/build.py --brand <tenant>) stages a pack
# to <project>/product/brand_assets/portal/ using the stock file basenames, so
# the embedded symbol names stay stable. Each page falls back to the stock copy
# independently, and is embedded pre-gzip'd when a ".gz" sibling exists.
# ---------------------------------------------------------------------------
set(STOCK_PORTAL_DIR "${CMAKE_CURRENT_LIST_DIR}/assets")
set(OEM_PORTAL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../product/brand_assets/portal")

set(PORTAL_INDEX "${STOCK_PORTAL_DIR}/wifi_configuration.html")
if(EXISTS "${OEM_PORTAL_DIR}/wifi_configuration.html")
    set(PORTAL_INDEX "${OEM_PORTAL_DIR}/wifi_configuration.html")
    message(STATUS "78/esp_wifi_connect: using OEM portal index ${PORTAL_INDEX}")
endif()

set(PORTAL_DONE "${STOCK_PORTAL_DIR}/wifi_configuration_done.html")
if(EXISTS "${OEM_PORTAL_DIR}/wifi_configuration_done.html")
    set(PORTAL_DONE "${OEM_PORTAL_DIR}/wifi_configuration_done.html")
endif()

set(PORTAL_TEXT_FILES)
set(PORTAL_BINARY_FILES)

if(EXISTS "${PORTAL_INDEX}.gz")
    list(APPEND PORTAL_BINARY_FILES "${PORTAL_INDEX}.gz")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PORTAL_HTML_GZIPPED=1)
else()
    list(APPEND PORTAL_TEXT_FILES "${PORTAL_INDEX}")
endif()

if(EXISTS "${PORTAL_DONE}.gz")
    list(APPEND PORTAL_BINARY_FILES "${PORTAL_DONE}.gz")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PORTAL_DONE_GZIPPED=1)
else()
    list(APPEND PORTAL_TEXT_FILES "${PORTAL_DONE}")
endif()

# target_add_binary_data accepts exactly one file per call.
foreach(portal_text ${PORTAL_TEXT_FILES})
    target_add_binary_data(${COMPONENT_LIB} "${portal_text}")
endforeach()
foreach(portal_binary ${PORTAL_BINARY_FILES})
    target_add_binary_data(${COMPONENT_LIB} "${portal_binary}" BINARY)
endforeach()
