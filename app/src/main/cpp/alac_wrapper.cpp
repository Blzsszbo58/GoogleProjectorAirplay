/* MirrorTV - C wrapper around Apple's open-source ALAC decoder. GPL-3.0-or-later */
#include "alac_wrapper.h"

#include <string.h>
#include <new>

#include "ALACBitUtilities.h"
#include "ALACDecoder.h"

struct alac_wrapper {
    ALACDecoder decoder;
};

/* ALACSpecificConfig (big endian) for AirPlay's fixed format:
 * frameLength 352, compatibleVersion 0, bitDepth 16, pb 40, mb 10, kb 14,
 * numChannels 2, maxRun 255, maxFrameBytes 0, avgBitRate 0, sampleRate 44100 */
static const uint8_t kAirPlayCookie[24] = {
    0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44,
};

extern "C" alac_wrapper_t *alac_wrapper_create(void) {
    alac_wrapper_t *w = new (std::nothrow) alac_wrapper_t;
    if (!w) return nullptr;
    uint8_t cookie[sizeof(kAirPlayCookie)];
    memcpy(cookie, kAirPlayCookie, sizeof(cookie));
    if (w->decoder.Init(cookie, sizeof(cookie)) != 0) {
        delete w;
        return nullptr;
    }
    return w;
}

extern "C" int alac_wrapper_decode(alac_wrapper_t *w, const uint8_t *in, int in_len, int16_t *out, int out_max_frames) {
    if (!w || !in || in_len <= 0) return -1;
    uint32_t frames_per_packet = w->decoder.mConfig.frameLength;
    if ((int) frames_per_packet > out_max_frames) return -1;

    BitBuffer bits;
    BitBufferInit(&bits, const_cast<uint8_t *>(in), (uint32_t) in_len);
    uint32_t out_frames = 0;
    int32_t status = w->decoder.Decode(&bits, reinterpret_cast<uint8_t *>(out), frames_per_packet, 2, &out_frames);
    if (status != 0) return -1;
    return (int) out_frames;
}

extern "C" void alac_wrapper_destroy(alac_wrapper_t *w) {
    delete w;
}
