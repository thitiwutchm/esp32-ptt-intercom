#include "ptt_proto.h"

#include <string.h>

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t ptt_proto_build(uint8_t *out, size_t out_cap, const ptt_hdr_t *hdr, const void *payload)
{
    size_t total = PTT_HDR_LEN + hdr->payload_len;
    if (hdr->payload_len > PTT_MAX_PAYLOAD || total > out_cap) {
        return 0;
    }
    out[0] = 'P';
    out[1] = 'T';
    out[2] = PTT_PROTO_VERSION;
    out[3] = hdr->type;
    put_u32(out + 4, hdr->device_id);
    put_u32(out + 8, hdr->talk_id);
    put_u16(out + 12, hdr->seq);
    out[14] = hdr->channel;
    out[15] = hdr->codec;
    put_u16(out + 16, hdr->payload_len);
    if (hdr->payload_len) {
        memcpy(out + PTT_HDR_LEN, payload, hdr->payload_len);
    }
    return total;
}

bool ptt_proto_parse(const uint8_t *buf, size_t len, ptt_hdr_t *hdr, const uint8_t **payload)
{
    if (len < PTT_HDR_LEN || buf[0] != 'P' || buf[1] != 'T' || buf[2] != PTT_PROTO_VERSION) {
        return false;
    }
    hdr->type = buf[3];
    hdr->device_id = get_u32(buf + 4);
    hdr->talk_id = get_u32(buf + 8);
    hdr->seq = get_u16(buf + 12);
    hdr->channel = buf[14];
    hdr->codec = buf[15];
    hdr->payload_len = get_u16(buf + 16);
    if (hdr->payload_len > PTT_MAX_PAYLOAD || PTT_HDR_LEN + (size_t)hdr->payload_len > len) {
        return false;
    }
    if (hdr->type < PTT_MSG_HELLO || hdr->type > PTT_MSG_TALK_END) {
        return false;
    }
    if (hdr->channel < 1 || hdr->channel > PTT_MAX_CHANNEL) {
        return false;
    }
    *payload = buf + PTT_HDR_LEN;
    return true;
}

size_t ptt_hello_encode(uint8_t *out, size_t out_cap, const ptt_hello_t *hello)
{
    if (out_cap < PTT_HELLO_LEN) {
        return 0;
    }
    memset(out, 0, PTT_NAME_LEN);
    size_t n = 0;
    while (n < PTT_NAME_LEN && hello->name[n]) {
        n++;
    }
    memcpy(out, hello->name, n);
    out[PTT_NAME_LEN] = hello->battery;
    out[PTT_NAME_LEN + 1] = hello->state;
    return PTT_HELLO_LEN;
}

bool ptt_hello_decode(const uint8_t *buf, size_t len, ptt_hello_t *hello)
{
    if (len < PTT_HELLO_LEN) {
        return false;
    }
    memcpy(hello->name, buf, PTT_NAME_LEN);
    hello->name[PTT_NAME_LEN] = '\0';
    /* Names are shown on screen: replace control characters. */
    for (int i = 0; i < PTT_NAME_LEN && hello->name[i]; i++) {
        if ((unsigned char)hello->name[i] < 0x20) {
            hello->name[i] = '?';
        }
    }
    hello->battery = buf[PTT_NAME_LEN];
    hello->state = buf[PTT_NAME_LEN + 1];
    return true;
}
