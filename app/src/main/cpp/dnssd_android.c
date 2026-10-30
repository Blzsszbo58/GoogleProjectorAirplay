/*
 * MirrorTV - Android service-discovery backend for the UxPlay AirPlay library.
 *
 * UxPlay normally advertises itself through Avahi/Bonjour or its own mDNS
 * responder bound to UDP 5353.  On Android the system already owns that port,
 * so this backend builds the same _raop._tcp / _airplay._tcp TXT records and
 * hands them to Java, which registers them with android.net.nsd.NsdManager.
 *
 * Implements the dnssd_private_* / dnssd_register_* half of lib/dnssd.h
 * (the other half lives in UxPlay's lib/dnssd.c).
 *
 * License: GPL-3.0-or-later (same as UxPlay).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnssd.h"
#include "dnssdint.h"
#include "global.h"
#include "utils.h"
#include "mdns_bridge.h"

#define TXT_MAX_ITEMS 32
#define TXT_MAX_BYTES 1024
#define SERVICE_NAME_MAX 256

typedef struct {
    int count;
    char keys[TXT_MAX_ITEMS][32];
    char values[TXT_MAX_ITEMS][160];
    unsigned char raw[TXT_MAX_BYTES];   /* RFC 6763 TXT wire format */
    int raw_len;
} txt_record_t;

typedef struct dnssd_private_s {
    txt_record_t raop_txt;
    txt_record_t airplay_txt;
    char raop_name[SERVICE_NAME_MAX];
    char airplay_name[SERVICE_NAME_MAX];
    int raop_registered;
    int airplay_registered;
} dnssd_private_t;

static int txt_add(txt_record_t *txt, const char *key, const char *value) {
    if (!value) value = "";
    size_t klen = strlen(key);
    size_t vlen = strlen(value);
    size_t item = klen + 1 + vlen;
    if (txt->count >= TXT_MAX_ITEMS || klen >= sizeof(txt->keys[0]) ||
        vlen >= sizeof(txt->values[0]) || item > 255 ||
        txt->raw_len + 1 + (int) item > TXT_MAX_BYTES) {
        return -1;
    }
    snprintf(txt->keys[txt->count], sizeof(txt->keys[0]), "%s", key);
    snprintf(txt->values[txt->count], sizeof(txt->values[0]), "%s", value);
    txt->count++;

    unsigned char *p = txt->raw + txt->raw_len;
    *p++ = (unsigned char) item;
    memcpy(p, key, klen);
    p += klen;
    *p++ = '=';
    memcpy(p, value, vlen);
    txt->raw_len += 1 + (int) item;
    return 0;
}

static void txt_reset(txt_record_t *txt) {
    memset(txt, 0, sizeof(*txt));
}

static void txt_publish(int service, const char *name, unsigned short port, txt_record_t *txt) {
    const char *keys[TXT_MAX_ITEMS];
    const char *values[TXT_MAX_ITEMS];
    for (int i = 0; i < txt->count; i++) {
        keys[i] = txt->keys[i];
        values[i] = txt->values[i];
    }
    mdns_bridge_register(service, name, port, txt->count, keys, values);
}

void *dnssd_private_init(dnssd_t *dnssd_public, int *error) {
    if (error) *error = DNSSD_ERROR_NOERROR;
    dnssd_private_t *dnssd = (dnssd_private_t *) calloc(1, sizeof(dnssd_private_t));
    if (!dnssd) {
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }

    char hwaddr[2 * MAX_HWADDR_LEN + 1];
    if (utils_hwaddr_raop(hwaddr, sizeof(hwaddr), dnssd_public->hw_addr, dnssd_public->hw_addr_len) < 0) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_HWADDRLEN;
        return NULL;
    }
    /* NsdManager wants the bare instance name; the service type is passed separately */
    snprintf(dnssd->raop_name, sizeof(dnssd->raop_name), "%s@%s", hwaddr, dnssd_public->name);
    snprintf(dnssd->airplay_name, sizeof(dnssd->airplay_name), "%s", dnssd_public->name);
    return dnssd;
}

void dnssd_private_destroy(void *private_data) {
    free(private_data);
}

int dnssd_register_raop(dnssd_t *dnssd_public, unsigned short port) {
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;
    txt_record_t *txt = &dnssd->raop_txt;
    char features[32];
    const char *pw = "false";
    const char *sf = RAOP_SF;

    switch (dnssd_public->pin_pw) {
    case 1:
        pw = "true";
        sf = "0x8c";
        break;
    case 2:
    case 3:
        pw = "true";
        sf = "0x84";
        break;
    default:
        break;
    }
    snprintf(features, sizeof(features), "0x%X,0x%X", dnssd_public->features1, dnssd_public->features2);

    txt_reset(txt);
    if (txt_add(txt, "ch", RAOP_CH) || txt_add(txt, "cn", RAOP_CN) ||
        txt_add(txt, "da", RAOP_DA) || txt_add(txt, "et", RAOP_ET) ||
        txt_add(txt, "vv", RAOP_VV) || txt_add(txt, "ft", features) ||
        txt_add(txt, "am", GLOBAL_MODEL) || txt_add(txt, "md", RAOP_MD) ||
        txt_add(txt, "rhd", RAOP_RHD) || txt_add(txt, "pw", pw) ||
        txt_add(txt, "sr", RAOP_SR) || txt_add(txt, "ss", RAOP_SS) ||
        txt_add(txt, "sv", RAOP_SV) || txt_add(txt, "tp", RAOP_TP) ||
        txt_add(txt, "txtvers", RAOP_TXTVERS) || txt_add(txt, "sf", sf) ||
        txt_add(txt, "vs", RAOP_VS) || txt_add(txt, "vn", RAOP_VN) ||
        txt_add(txt, "pk", dnssd_public->pk)) {
        return -1;
    }
    txt_publish(MDNS_SERVICE_RAOP, dnssd->raop_name, port, txt);
    dnssd->raop_registered = 1;
    return 0;
}

int dnssd_register_airplay(dnssd_t *dnssd_public, unsigned short port) {
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;
    txt_record_t *txt = &dnssd->airplay_txt;
    char device_id[3 * MAX_HWADDR_LEN];
    char features[32];

    if (utils_hwaddr_airplay(device_id, sizeof(device_id), dnssd_public->hw_addr, dnssd_public->hw_addr_len) < 0) {
        return -1;
    }
    snprintf(features, sizeof(features), "0x%X,0x%X", dnssd_public->features1, dnssd_public->features2);

    txt_reset(txt);
    if (txt_add(txt, "deviceid", device_id) || txt_add(txt, "features", features) ||
        txt_add(txt, "pw", dnssd_public->pin_pw ? "true" : "false") ||
        txt_add(txt, "flags", "0x4") || txt_add(txt, "model", GLOBAL_MODEL) ||
        txt_add(txt, "pk", dnssd_public->pk) || txt_add(txt, "pi", AIRPLAY_PI) ||
        txt_add(txt, "srcvers", AIRPLAY_SRCVERS) || txt_add(txt, "vv", AIRPLAY_VV)) {
        return -1;
    }
    txt_publish(MDNS_SERVICE_AIRPLAY, dnssd->airplay_name, port, txt);
    dnssd->airplay_registered = 1;
    return 0;
}

const char *dnssd_get_raop_txt(dnssd_t *dnssd_public, int *length) {
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;
    *length = dnssd->raop_txt.raw_len;
    return (const char *) dnssd->raop_txt.raw;
}

const char *dnssd_get_airplay_txt(dnssd_t *dnssd_public, int *length) {
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;
    *length = dnssd->airplay_txt.raw_len;
    return (const char *) dnssd->airplay_txt.raw;
}

void dnssd_unregister_raop(dnssd_t *dnssd_public) {
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;
    if (dnssd->raop_registered) {
        mdns_bridge_unregister(MDNS_SERVICE_RAOP);
        dnssd->raop_registered = 0;
    }
}

void dnssd_unregister_airplay(dnssd_t *dnssd_public) {
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;
    if (dnssd->airplay_registered) {
        mdns_bridge_unregister(MDNS_SERVICE_AIRPLAY);
        dnssd->airplay_registered = 0;
    }
}

int dnssd_get_service_fd(dnssd_t *dnssd, int service) {
    (void) dnssd;
    (void) service;
    return -1;
}

int dnssd_process_service(dnssd_t *dnssd, int service) {
    (void) dnssd;
    (void) service;
    return 0;
}

void dnssd_error_text(int *dnssd_error, const char *appname) {
    (void) appname;
    fprintf(stderr, "dnssd (Android NsdManager backend) error %d\n", dnssd_error ? *dnssd_error : 0);
}
