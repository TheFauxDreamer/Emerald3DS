// I2S audio output driver -- see i2s_audio.h for the wiring and contract.
//
// Sample path is DMA-only: one channel streams a ping-pong pair of buffers into
// the PIO TX FIFO, paced by the SM's TX DREQ. The DMA IRQ (DMA_IRQ_1; the HSTX
// scanout owns DMA_IRQ_0) re-arms the other, already-filled buffer and refills
// the one that just finished via the fill callback. Buffers are 16 ms each, so
// a late IRQ is at worst an audible click, never signal loss -- this is the
// opposite trade-off from the display, where the scanout deliberately keeps the
// CPU out of the re-arm loop (a late display IRQ would corrupt the HDMI signal).

#include "i2s_audio.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#include "i2s_audio.pio.h"   // generated from i2s_audio.pio by pioasm (CMake)

// In the full game EWRAM and the SDK RAM region are both full, so the DMA
// buffers are parked in IWRAM's slack (.iwram_top). The standalone i2s_test
// build leaves AUDIO_BUFS_IN_IWRAM undefined -> ordinary .bss.
#ifdef AUDIO_BUFS_IN_IWRAM
#define I2S_BUF_ATTR __attribute__((section(".iwram_top"), aligned(8)))
#else
#define I2S_BUF_ATTR
#endif

static uint32_t i2s_buf[2][I2S_FRAMES_PER_BUF] I2S_BUF_ATTR;
static volatile int i2s_cur;            // buffer index DMA is currently playing
static int i2s_dma_chan = -1;
static PIO i2s_pio;
static uint i2s_sm;
static uint i2s_offset;
static i2s_fill_fn i2s_fill;
static void *i2s_ctx;
static volatile uint32_t i2s_played;

static void i2s_dma_isr(void) {
    if (!(dma_hw->ints1 & (1u << i2s_dma_chan))) return;
    dma_hw->ints1 = 1u << i2s_dma_chan;       // ack

    int done = i2s_cur;
    int next = done ^ 1;
    i2s_cur = next;

    // Re-arm on the buffer prepared last cycle (or at init) and let it run.
    dma_channel_set_read_addr(i2s_dma_chan, i2s_buf[next], true);
    i2s_played++;

    // Refill the buffer that just finished, ready for the cycle after next.
    if (i2s_fill) {
        i2s_fill(i2s_ctx, i2s_buf[done], I2S_FRAMES_PER_BUF);
    } else {
        memset(i2s_buf[done], 0, sizeof i2s_buf[done]);
    }
}

int i2s_audio_init(uint32_t sample_rate, i2s_fill_fn fill, void *ctx) {
    i2s_fill = fill;
    i2s_ctx = ctx;
    i2s_played = 0;
    i2s_cur = 0;

    // Prime both buffers before anything streams.
    if (fill) {
        fill(ctx, i2s_buf[0], I2S_FRAMES_PER_BUF);
        fill(ctx, i2s_buf[1], I2S_FRAMES_PER_BUF);
    } else {
        memset(i2s_buf, 0, sizeof i2s_buf);
    }

    // Claim a PIO + SM. PIO0/PIO1 are otherwise unused here.
    if (!pio_claim_free_sm_and_add_program(&i2s_audio_program, &i2s_pio,
                                           &i2s_sm, &i2s_offset)) {
        return 0;
    }
    i2s_audio_program_init(i2s_pio, i2s_sm, i2s_offset,
                           I2S_DIN_PIN, I2S_BCK_PIN, sample_rate);

    i2s_dma_chan = dma_claim_unused_channel(false);
    if (i2s_dma_chan < 0) {
        pio_remove_program_and_unclaim_sm(&i2s_audio_program, i2s_pio, i2s_sm,
                                          i2s_offset);
        return 0;
    }

    dma_channel_config c = dma_channel_get_default_config(i2s_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(i2s_pio, i2s_sm, true));
    dma_channel_configure(i2s_dma_chan, &c,
                          &i2s_pio->txf[i2s_sm],   // write: PIO TX FIFO
                          i2s_buf[0],              // read:  buffer 0
                          I2S_FRAMES_PER_BUF,
                          false);                  // don't start yet

    // IRQ on completion via DMA_IRQ_1 (DMA_IRQ_0 belongs to the scanout). Bound
    // to the calling core's NVIC.
    dma_channel_set_irq1_enabled(i2s_dma_chan, true);
    irq_add_shared_handler(DMA_IRQ_1, i2s_dma_isr,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);

    dma_channel_start(i2s_dma_chan);
    return 1;
}

void i2s_audio_deinit(void) {
    if (i2s_dma_chan < 0) return;
    dma_channel_abort(i2s_dma_chan);
    dma_channel_set_irq1_enabled(i2s_dma_chan, false);
    irq_remove_handler(DMA_IRQ_1, i2s_dma_isr);
    dma_channel_unclaim(i2s_dma_chan);
    i2s_dma_chan = -1;
    pio_sm_set_enabled(i2s_pio, i2s_sm, false);
    pio_remove_program_and_unclaim_sm(&i2s_audio_program, i2s_pio, i2s_sm,
                                      i2s_offset);
    i2s_fill = NULL;
}

uint32_t i2s_audio_buffers_played(void) { return i2s_played; }

int i2s_audio_dma_chan(void) { return i2s_dma_chan; }
int i2s_audio_pio_index(void) {
    if (i2s_dma_chan < 0) return -1;
    return pio_get_index(i2s_pio);
}
int i2s_audio_sm(void) { return i2s_dma_chan < 0 ? -1 : (int)i2s_sm; }

uint32_t i2s_audio_fifo_level(void) {
    if (i2s_dma_chan < 0) return 0;
    return pio_sm_get_tx_fifo_level(i2s_pio, i2s_sm);
}
