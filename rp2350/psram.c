// APS6404 PSRAM init for RP2350 QMI window 1 (CS1). See psram.h.
//
// Sequence: enter QMI direct serial mode, read the JEDEC ID (0x9F) to confirm an
// APS6404 (KGD 0x5D) and decode density, enable quad mode (0x35), then program
// the M1 window for quad fast read (0xEB, 6 dummy cycles) and quad write (0x38).
//
// The direct-mode section suspends XIP, so this function lives in RAM and calls
// no flash-resident code while direct mode is enabled (clock/gpio reads happen
// before, register writes after).

#include "psram.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/regs/qmi.h"
#include "hardware/regs/xip.h"

// GPIO0/47 funcsel value selecting the XIP second chip select (CS1).
#define FUNC_XIP_CS1 9u

size_t __no_inline_not_in_flash_func(psram_init)(uint32_t cs_pin) {
    gpio_set_function(cs_pin, (gpio_function_t)FUNC_XIP_CS1);

    uint32_t clock_hz = clock_get_hz(clk_sys);
    uint32_t intr_stash = save_and_disable_interrupts();

    // --- direct serial mode: ID read + quad enable (no flash calls below) -----
    qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}

    // Exit a possible quad/QPI continuous mode left over from a prior init.
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS
                      | (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB)
                      | 0xf5u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    // Read JEDEC ID (0x9F): byte 5 = KGD, byte 6 = EID (density).
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0, eid = 0;
    for (int i = 0; i < 7; i++) {
        qmi_hw->direct_tx = (i == 0) ? 0x9fu : 0xffu;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {}
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
        uint8_t rx = (uint8_t)qmi_hw->direct_rx;
        if (i == 5) kgd = rx;
        else if (i == 6) eid = rx;
    }
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    if (kgd != 0x5d) {
        restore_interrupts(intr_stash);
        return 0;
    }

    // Decode density from EID bits [7:5].
    size_t psram_size = 0;
    uint8_t size_id = eid >> 5;
    if (size_id == 0b000) psram_size = 2u * 1024 * 1024;
    else if (size_id == 0b001) psram_size = 4u * 1024 * 1024;
    else psram_size = 8u * 1024 * 1024; // 0b010 and the common APS6404L-3SQR

    // Stay in SPI mode and use the SPI quad commands (1-4-4): single-line command
    // prefix, quad address + quad data. Confirmed on this board: full QPI (quad
    // command prefix via 0x35) read back garbled; SPI-quad with a 6-cycle read
    // dummy round-trips cleanly. So do NOT enter QPI; ensure we're out of it.
    qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS
                      | (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB)
                      | 0xf5u; // exit QPI (quad), harmless if already SPI
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    // --- XIP window 1 timing & quad read/write formats ------------------------
    // PSRAM tops out ~133 MHz; clk_div 1 keeps it == clk_sys at our <=150 MHz.
    int clk_div = (int)((clock_hz + 132999999u) / 133000000u);
    if (clk_div < 1) clk_div = 1;
    // APS6404 needs CS asserted < 8 us (refresh); MAX_SELECT is in 64-clock units.
    uint8_t max_select = (uint8_t)((clock_hz / 1000000u) * 8u / 64u);
    // ~50 ns minimum CS-high between bursts; MIN_DESELECT in 1-clock units.
    uint8_t min_deselect = (uint8_t)((clock_hz / 1000000u) * 50u / 1000u + 1u);

    qmi_hw->m[1].timing =
        (QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB) |
        ((uint32_t)max_select << QMI_M1_TIMING_MAX_SELECT_LSB) |
        ((uint32_t)min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB) |
        (1u << QMI_M1_TIMING_RXDELAY_LSB) |
        ((uint32_t)clk_div << QMI_M1_TIMING_CLKDIV_LSB);

    // SPI quad read 0xEB (1-4-4): single command, quad address+data, 6 dummy
    // cycles. PREFIX_WIDTH defaults to single (0); leave it unset.
    qmi_hw->m[1].rfmt =
        (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M1_RFMT_ADDR_WIDTH_LSB) |
        (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M1_RFMT_DUMMY_WIDTH_LSB) |
        (6u                               << QMI_M1_RFMT_DUMMY_LEN_LSB) |
        (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M1_RFMT_DATA_WIDTH_LSB) |
        (QMI_M1_RFMT_PREFIX_LEN_VALUE_8   << QMI_M1_RFMT_PREFIX_LEN_LSB);
    qmi_hw->m[1].rcmd = 0xebu << QMI_M1_RCMD_PREFIX_LSB;

    // SPI quad write 0x38 (1-4-4): single command, quad address+data, no dummy.
    qmi_hw->m[1].wfmt =
        (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M1_WFMT_ADDR_WIDTH_LSB) |
        (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M1_WFMT_DATA_WIDTH_LSB) |
        (QMI_M1_WFMT_PREFIX_LEN_VALUE_8   << QMI_M1_WFMT_PREFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = 0x38u << QMI_M1_WCMD_PREFIX_LSB;

    // Enable writes to XIP window 1. This is OFF by default (XIP windows assume a
    // read-only flash); without it every write to PSRAM is silently dropped.
    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

    restore_interrupts(intr_stash);
    return psram_size;
}
