/* One UDP socket for HELLO broadcasts and unicast voice. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ptt_proto.h"

/* Called from the receive task for every valid datagram. src_ip is in network byte order. */
typedef void (*ptt_net_rx_cb_t)(const ptt_hdr_t *hdr, const uint8_t *payload, uint32_t src_ip);

esp_err_t ptt_net_start(uint16_t port, ptt_net_rx_cb_t cb);

/* Send to one address (network byte order). Safe from any task. */
void ptt_net_send(uint32_t ip, const uint8_t *buf, size_t len);

/* Send to the subnet broadcast address. No-op while Wi-Fi is down. */
void ptt_net_broadcast(const uint8_t *buf, size_t len);
