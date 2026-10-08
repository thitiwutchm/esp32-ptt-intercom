/* The log web server (port 80), up while the station has an address and not
 * during phone setup (which needs the port). Internal to presence_core. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void presence_web_start(void);
void presence_web_stop(void);

#ifdef __cplusplus
}
#endif
