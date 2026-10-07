# SIP, RTP and audio helpers for calling through a PBX. Plain C, no ESP-IDF
# APIs: also built by test/host and run against a real Asterisk there.
idf_component_register(
    SRCS "g711.c" "resample.c" "rtp.c" "md5.c" "sip_msg.c" "sdp.c" "sip_ua.c" "echo_ref.c"
    INCLUDE_DIRS "include"
)
