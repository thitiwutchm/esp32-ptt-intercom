# BLE presence logger: scan for enrolled phones by their IRK, log arrivals and
# departures to flash, serve the log over HTTP. flog.c and irk_resolver.c are
# plain C (no ESP APIs) and are also built by test/host. The BLE, web, time and
# store code is active only when CONFIG_PTT_PRESENCE_ENABLE is set; otherwise
# the files compile to nothing and presence.c provides no-op stubs.
set(srcs
    "presence.c"
    "presence_db.c"
    "flog.c"
    "irk_resolver.c"
    "presence_time.c"
    "ble_presence.c"
    "presence_web.c")

set(reqs setup_core esp_http_server esp_partition esp_timer esp_netif lwip nvs_flash bt)

if(CONFIG_PTT_PRESENCE_ENABLE)
    idf_component_register(
        SRCS ${srcs}
        INCLUDE_DIRS "include"
        EMBED_TXTFILES "presence_page.html"
        REQUIRES ${reqs})
else()
    idf_component_register(
        SRCS ${srcs}
        INCLUDE_DIRS "include"
        REQUIRES ${reqs})
endif()
