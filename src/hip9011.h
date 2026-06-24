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

// HIP9011 knock signal processor. SPI is config-only (write-only: SO is high-Z in run
// mode), one self-addressing 8-bit word per CS frame. CS on PC2, INT/HOLD on PC3.
// The knock magnitude (INTOUT) is analog -> injected separately on ADC6.
class Hip9011 {
public:
    void attach(avr_t* avr);

    // Decoded registers (raw field values).
    int bandpass() const { return bandpass_.load(); }   // 0..63
    int gain() const { return gain_.load(); }            // 0..63
    int timeconst() const { return tc_.load(); }         // 0..31
    int channel() const { return channel_.load(); }      // 0/1
    int prescaler() const { return presc_.load(); }      // 0..15 (P4..P1)
    bool so_hiz() const { return so_hiz_.load(); }
    uint64_t words() const { return words_.load(); }
    int last_byte() const { return last_byte_.load(); }
    bool written() const { return words_.load() > 0; }

    // Line indicators.
    bool cs_active() const { return cs_active_.load(); }
    uint64_t cs_transactions() const { return cs_txn_.load(); }
    bool inthold_high() const { return inthold_high_.load(); }   // high = integrate, low = hold
    uint64_t inthold_edges() const { return inthold_edges_.load(); }

    // Datasheet lookups (Table 3 / Table 4) for display.
    static float bpf_khz(int v);
    static float gain_x(int v);
    static int tc_us(int v);
    static int prescaler_mhz(int v);

private:
    static void spi_out_cb(avr_irq_t*, uint32_t value, void* param);
    static void cs_cb(avr_irq_t*, uint32_t value, void* param);
    static void inthold_cb(avr_irq_t*, uint32_t value, void* param);
    void decode(uint8_t b);

    avr_t* avr_ = nullptr;
    std::atomic<bool> cs_active_{false};
    std::atomic<uint64_t> cs_txn_{0};
    std::atomic<bool> inthold_high_{false};
    std::atomic<uint64_t> inthold_edges_{0};

    std::atomic<int> bandpass_{0}, gain_{0}, tc_{0}, channel_{0}, presc_{0};
    std::atomic<bool> so_hiz_{false};
    std::atomic<uint64_t> words_{0};
    std::atomic<int> last_byte_{-1};

    bool have_byte_ = false;     // sim thread only
    uint8_t pending_ = 0;
};
