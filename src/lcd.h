#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

extern "C" {
#include "sim_avr.h"
#include "sim_irq.h"
#include "sim_io.h"
#include "avr_ioport.h"
}

class Hc259;

// HD44780 20x4 character LCD wired to the VEMS v3 in 4-bit mode.
//   RS   = LCD_RS (74HC259 aux latch Q0)     E (enable) = PC1     DDIR = PC0
//   D4..D7 = PC4..PC7 — shared with the 74HC259 address/data bus, but the LCD only
//   captures them on the falling edge of its own E strobe, so the bus multiplexes.
// Decodes the 4-bit nibble protocol, keeps DDRAM, and exposes the visible 20x4 text
// plus raw pin state for the UI.
class Hd44780 {
public:
    static constexpr int COLS = 20;
    static constexpr int ROWS = 4;

    void configure(Hc259* rs_latch, int rs_q);   // RS = rs_latch->q(rs_q)
    void attach(avr_t* avr);

    // UI accessors (thread-safe).
    void snapshot(char out[ROWS][COLS]) const;
    bool rs() const;
    bool en() const { return en_.load(); }
    bool ddir() const { return ddir_.load(); }
    bool data(int bit) const { return d_[bit & 7].load(); }   // bit 4..7
    bool display_on() const { return disp_pub_.load(); }
    bool four_bit() const { return fourbit_pub_.load(); }
    int  cursor() const { return addr_pub_.load(); }
    uint64_t strobes() const { return strobes_.load(); }
    uint64_t chars() const { return chars_.load(); }

private:
    enum Sig { SIG_D4, SIG_D5, SIG_D6, SIG_D7, SIG_EN, SIG_DDIR, SIG_N };
    struct Pin { Hd44780* self; int sig; };
    static void pin_cb(avr_irq_t*, uint32_t value, void* param);
    void on_pin(int sig, uint32_t value);
    void strobe();
    void process(uint8_t byte, bool rs);
    void clear_ram();

    Hc259* rs_latch_ = nullptr;
    int rs_q_ = 0;
    avr_t* avr_ = nullptr;

    Pin pins_[SIG_N];
    std::atomic<bool> d_[8]{};
    std::atomic<bool> en_{false}, ddir_{false};

    // protocol state (sim thread only)
    bool fourbit_ = false;
    bool hi_pending_ = false;
    uint8_t hi_nibble_ = 0;
    bool inc_ = true;
    bool disp_on_ = false;
    bool cgram_ = false;
    uint8_t addr_ = 0;

    mutable std::mutex mtx_;
    uint8_t ram_[128];

    std::atomic<uint8_t> addr_pub_{0};
    std::atomic<bool> disp_pub_{false};
    std::atomic<bool> fourbit_pub_{false};
    std::atomic<uint64_t> strobes_{0}, chars_{0};
};
