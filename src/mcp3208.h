#pragma once
#include <atomic>
#include <cstdint>

extern "C" {
#include "sim_avr.h"
#include "sim_irq.h"
#include "sim_io.h"
#include "avr_spi.h"
#include "avr_ioport.h"
}

// MCP3208: 8-channel 12-bit SPI ADC, modeled as a bit-level SPI slave so it works
// regardless of how the firmware frames the transfer. CS is on PG4 (VEMS schematic).
class Mcp3208 {
public:
    Mcp3208() { for (int i = 0; i < 8; i++) ch_[i].store(0); }

    void attach(avr_t* avr);                 // sim thread, after avr init
    void set_counts(int ch, int counts);     // 0..4095
    int counts(int ch) const;

    bool cs_active() const { return cs_active_.load(); }
    uint64_t cs_transactions() const { return cs_txn_.load(); }
    uint64_t conversions() const { return conv_.load(); }

private:
    static void spi_out_cb(avr_irq_t*, uint32_t value, void* param);
    static void cs_cb(avr_irq_t*, uint32_t value, void* param);
    int clock_bit(int din);                  // one SCK; returns the MISO bit
    void reset_xfer();

    avr_t* avr_ = nullptr;
    avr_irq_t* spi_in_ = nullptr;            // raise to push a MISO byte into SPDR
    std::atomic<int> ch_[8];
    std::atomic<bool> cs_active_{false};
    std::atomic<uint64_t> cs_txn_{0};
    std::atomic<uint64_t> conv_{0};

    enum St { WAIT_START, CFG, DATA, DONE };
    St st_ = WAIT_START;                      // bit machine state (sim thread only)
    int cfg_ = 0, cfg_n_ = 0;
    int out_bits_[13];                        // null + 12 data bits, MSB first
    int out_n_ = 0, out_pos_ = 0;
};
