#include "rtp.h"

#include <string.h>

size_t rtp_build(uint8_t *out, size_t cap, const rtp_hdr_t *h, const uint8_t *payload, size_t len)
{
    if (cap < RTP_HDR_LEN + len) {
        return 0;
    }
    out[0] = 0x80; /* V=2 */
    out[1] = (uint8_t)((h->marker ? 0x80 : 0) | (h->pt & 0x7F));
    out[2] = (uint8_t)(h->seq >> 8);
    out[3] = (uint8_t)h->seq;
    out[4] = (uint8_t)(h->ts >> 24);
    out[5] = (uint8_t)(h->ts >> 16);
    out[6] = (uint8_t)(h->ts >> 8);
    out[7] = (uint8_t)h->ts;
    out[8] = (uint8_t)(h->ssrc >> 24);
    out[9] = (uint8_t)(h->ssrc >> 16);
    out[10] = (uint8_t)(h->ssrc >> 8);
    out[11] = (uint8_t)h->ssrc;
    memcpy(out + RTP_HDR_LEN, payload, len);
    return RTP_HDR_LEN + len;
}

bool rtp_parse(const uint8_t *buf, size_t len, rtp_hdr_t *h, const uint8_t **payload, size_t *payload_len)
{
    if (len < RTP_HDR_LEN || (buf[0] >> 6) != 2) {
        return false;
    }
    size_t off = RTP_HDR_LEN + 4u * (buf[0] & 0x0F);
    if (buf[0] & 0x10) { /* header extension */
        if (len < off + 4) {
            return false;
        }
        off += 4 + 4u * ((buf[off + 2] << 8) | buf[off + 3]);
    }
    size_t end = len;
    if (buf[0] & 0x20) { /* padding */
        uint8_t pad = buf[len - 1];
        if (pad == 0 || pad > len) {
            return false;
        }
        end = len - pad;
    }
    if (off > end) {
        return false;
    }
    h->marker = (buf[1] & 0x80) != 0;
    h->pt = buf[1] & 0x7F;
    h->seq = (uint16_t)((buf[2] << 8) | buf[3]);
    h->ts = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
    h->ssrc = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) | ((uint32_t)buf[10] << 8) | buf[11];
    *payload = buf + off;
    *payload_len = end - off;
    return true;
}
