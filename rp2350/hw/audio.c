// m4a -> I2S audio integration layer. See audio.h for the architecture.
//
// The decoupling ring is single-producer (Rp2350AudioFrame on the core-0 game
// thread) / single-consumer (the I2S DMA IRQ, also on core 0). 32-bit aligned
// index loads/stores are atomic on the M33 and the producer publishes data
// before advancing the head, so no lock is needed. Samples are stored mono
// (int16); the fill callback duplicates to stereo on the way out -- half the
// RAM of a stereo ring, which matters because this all lives in EWRAM slack.

#include "audio.h"

#include <string.h>

#include "pico/stdlib.h"

#include "i2s_audio.h"

// ---- ring (mono int16) -----------------------------------------------------
// ~3 game frames of slack (3 * 224 = 672 samples) rounded up to a power of two
// for cheap masked wrap. 1024 samples @ 13440 Hz = ~76 ms.
#define AUDIO_RING_LEN 1024u
#define AUDIO_RING_MASK (AUDIO_RING_LEN - 1u)

#ifdef AUDIO_BUFS_IN_IWRAM
#define AUDIO_BUF_ATTR __attribute__((section(".iwram_top"), aligned(8)))
#else
#define AUDIO_BUF_ATTR
#endif

static int16_t audio_ring[AUDIO_RING_LEN] AUDIO_BUF_ATTR;
static volatile uint32_t audio_head;    // producer writes, next free slot
static volatile uint32_t audio_tail;    // consumer reads, next unread slot

static volatile uint32_t audio_underruns;
static int16_t audio_last;              // held through underruns to avoid clicks

static inline uint32_t ring_fill(void) { return audio_head - audio_tail; }

// ---- I2S DMA fill (IRQ context) --------------------------------------------
// Pull mono samples from the ring, duplicate L=R into the packed stereo DMA
// word. On underrun, hold the last sample (DC, click-free) rather than snapping
// to zero.
static void audio_fill(void *ctx, uint32_t *dst, uint32_t nframes) {
    (void)ctx;
    uint32_t tail = audio_tail;
    uint32_t head = audio_head;
    int16_t last = audio_last;
    for (uint32_t i = 0; i < nframes; i++) {
        if (tail != head) {
            last = audio_ring[tail & AUDIO_RING_MASK];
            tail++;
        } else {
            audio_underruns++;
        }
        dst[i] = i2s_pack(last, last);
    }
    audio_tail = tail;
    audio_last = last;
}

// ---- mixer seam ------------------------------------------------------------
#ifdef AUDIO_TEST_TONE
// End-to-end pipeline check: a ~440 Hz sawtooth straight from the seam, so the
// whole game build (frame hook -> ring -> DMA -> PIO -> PCM5102A) can be heard
// before the real mixer exists. Enable with -DAUDIO_TEST_TONE.
int Rp2350MixFrame(int8_t *out, int n) {
    static uint8_t phase;
    for (int i = 0; i < n; i++) {
        phase += (uint8_t)(256 * 440 / AUDIO_SAMPLE_RATE);
        out[i] = (int8_t)(phase - 128) / 4;   // modest amplitude
    }
    return n;
}
#else
// Weak default: silence. The audio milestone overrides this with the C port of
// SoundMainRAM (or an m4aSoundMain() wrapper) -- a strong definition wins over
// this weak one at link time with no change here.
__attribute__((weak)) int Rp2350MixFrame(int8_t *out, int n) {
    (void)out;
    (void)n;
    return 0;
}
#endif

// ---- public API ------------------------------------------------------------
void Rp2350AudioInit(void) {
    audio_head = audio_tail = 0;
    audio_underruns = 0;
    audio_last = 0;
    memset(audio_ring, 0, sizeof audio_ring);
    i2s_audio_init(AUDIO_SAMPLE_RATE, audio_fill, NULL);
}

void Rp2350AudioFrame(void) {
    int8_t mono[AUDIO_SAMPLES_PER_FRAME];
    int n = Rp2350MixFrame(mono, AUDIO_SAMPLES_PER_FRAME);
    if (n <= 0) return;                         // seam produced silence
    if (n > AUDIO_SAMPLES_PER_FRAME) n = AUDIO_SAMPLES_PER_FRAME;

    uint32_t head = audio_head;
    uint32_t space = AUDIO_RING_LEN - (head - audio_tail);
    if ((uint32_t)n > space) n = (int)space;    // ring full: drop the tail end
    for (int i = 0; i < n; i++) {
        // s8 -> s16: scale up by 256 to fill the 16-bit range.
        audio_ring[head & AUDIO_RING_MASK] = (int16_t)(mono[i] << 8);
        head++;
    }
    audio_head = head;                          // publish after the writes
}

void Rp2350AudioStats(uint32_t *played, uint32_t *underruns, uint32_t *ring_samples) {
    if (played) *played = i2s_audio_buffers_played();
    if (underruns) *underruns = audio_underruns;
    if (ring_samples) *ring_samples = ring_fill();
}
