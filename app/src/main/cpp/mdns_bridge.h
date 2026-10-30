/* MirrorTV - native -> Java bridge for mDNS (NsdManager) registration. GPL-3.0-or-later */
#ifndef MIRRORTV_MDNS_BRIDGE_H
#define MIRRORTV_MDNS_BRIDGE_H

#define MDNS_SERVICE_RAOP    0   /* _raop._tcp    */
#define MDNS_SERVICE_AIRPLAY 1   /* _airplay._tcp */

#ifdef __cplusplus
extern "C" {
#endif

void mdns_bridge_register(int service, const char *name, unsigned short port,
                          int count, const char **keys, const char **values);
void mdns_bridge_unregister(int service);

#ifdef __cplusplus
}
#endif
#endif
