/*
 * Resolve a Bluetooth LE Resolvable Private Address (RPA) against an Identity
 * Resolving Key (IRK). Modern phones rotate a random RPA every ~15 minutes to
 * stop being tracked; a peer that bonded with them holds their IRK and can
 * tell that two different RPAs are the same phone. That is exactly what lets
 * this device recognise an enrolled phone despite its changing address.
 *
 * Addresses and the IRK are in the little-endian byte order NimBLE uses
 * (val[0] is the least significant octet). The AES-128 used by the hash is
 * self-contained here, so this file also builds and is tested on the host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True if addr is a resolvable private address (top two bits of the MSB = 0b01). */
bool irk_addr_is_rpa(const uint8_t addr[6]);

/* True if addr is an RPA that resolves with this IRK, i.e. belongs to that phone. */
bool irk_resolve(const uint8_t irk[16], const uint8_t addr[6]);

/* The BLE random-address hash ah(irk, prand). Exposed for testing. prand is
 * the 24-bit value in bytes prand[0..2], prand[0] the least significant. The
 * 24-bit result goes to out[0..2], out[0] the least significant. */
void irk_ah(const uint8_t irk[16], const uint8_t prand[3], uint8_t out[3]);

#ifdef __cplusplus
}
#endif
