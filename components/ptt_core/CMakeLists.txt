# Pure C protocol and audio logic, no ESP-IDF APIs: also built by test/host.
idf_component_register(
    SRCS "ptt_proto.c" "ptt_peers.c" "ptt_floor.c" "ptt_jitter.c" "ptt_adpcm.c"
    INCLUDE_DIRS "include"
)
