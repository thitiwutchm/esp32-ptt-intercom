# Phone setup helpers: form parsing/validation, JSON, captive DNS. Plain C,
# no ESP-IDF APIs: also built by test/host.
idf_component_register(
    SRCS "device_config.c" "captive_dns.c"
    INCLUDE_DIRS "include"
)
