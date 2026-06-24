#include "lcd.h"
#include "hc259.h"

void Hd44780::configure(Hc259* rs_latch, int rs_q) {
    rs_latch_ = rs_latch;
    rs_q_ = rs_q;
}

bool Hd44780::rs() const {
    return rs_latch_ ? rs_latch_->q(rs_q_) : false;
}

void Hd44780::clear_ram() {
    for (int i = 0; i < 128; i++) ram_[i] = ' ';
}

void Hd44780::attach(avr_t* avr) {
    avr_ = avr;
    { std::lock_guard<std::mutex> lk(mtx_); clear_ram(); }
    fourbit_ = false; hi_pending_ = false; hi_nibble_ = 0;
    inc_ = true; disp_on_ = false; cgram_ = false; addr_ = 0;
    for (int i = 0; i < 8; i++) d_[i] = false;
    en_ = false; ddir_ = false;
    addr_pub_ = 0; disp_pub_ = false; fourbit_pub_ = false; strobes_ = 0; chars_ = 0;
    if (!avr) return;

    struct { char port; int pin; int sig; } map[SIG_N] = {
        { 'C', 4, SIG_D4 }, { 'C', 5, SIG_D5 }, { 'C', 6, SIG_D6 }, { 'C', 7, SIG_D7 },
        { 'C', 1, SIG_EN }, { 'C', 0, SIG_DDIR },
    };
    for (int i = 0; i < SIG_N; i++) {
        pins_[i] = Pin{ this, map[i].sig };
        avr_irq_t* irq = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(map[i].port), IOPORT_IRQ_PIN0 + map[i].pin);
        if (irq) avr_irq_register_notify(irq, pin_cb, &pins_[i]);
    }
}

void Hd44780::pin_cb(avr_irq_t*, uint32_t value, void* param) {
    Pin* p = (Pin*)param;
    p->self->on_pin(p->sig, value);
}

void Hd44780::on_pin(int sig, uint32_t value) {
    bool v = (value != 0);
    switch (sig) {
        case SIG_D4: d_[4] = v; break;
        case SIG_D5: d_[5] = v; break;
        case SIG_D6: d_[6] = v; break;
        case SIG_D7: d_[7] = v; break;
        case SIG_DDIR: ddir_ = v; break;
        case SIG_EN: {
            bool was = en_.load();
            en_ = v;
            if (was && !v) strobe();          // HD44780 latches on E falling edge
            break;
        }
    }
}

void Hd44780::strobe() {
    uint8_t nib = (d_[7].load() << 3) | (d_[6].load() << 2) | (d_[5].load() << 1) | d_[4].load();
    bool rs = rs_latch_ ? rs_latch_->q(rs_q_) : false;
    strobes_++;

    if (!fourbit_) {
        // 8-bit mode: D4..D7 carry the high nibble, low nibble unwired (0).
        uint8_t byte = nib << 4;
        process(byte, rs);
        // Function set (001x'xxxx) with DL=0 switches the controller to 4-bit.
        if (!rs && (byte & 0xE0) == 0x20 && !(byte & 0x10)) {
            fourbit_ = true; hi_pending_ = false; fourbit_pub_ = true;
        }
        return;
    }

    if (!hi_pending_) { hi_nibble_ = nib; hi_pending_ = true; return; }
    uint8_t byte = (uint8_t)((hi_nibble_ << 4) | nib);
    hi_pending_ = false;
    process(byte, rs);
}

void Hd44780::process(uint8_t byte, bool rs) {
    if (rs) {                                  // data write
        if (!cgram_) {
            { std::lock_guard<std::mutex> lk(mtx_); ram_[addr_ & 0x7f] = byte; }
            chars_++;
        }
        addr_ = (uint8_t)((addr_ + (inc_ ? 1 : -1)) & 0x7f);
        addr_pub_ = addr_;
        return;
    }
    // command
    if (byte & 0x80)      { cgram_ = false; addr_ = byte & 0x7f; addr_pub_ = addr_; }   // set DDRAM addr
    else if (byte & 0x40) { cgram_ = true;  addr_ = byte & 0x3f; }                       // set CGRAM addr
    else if (byte & 0x20) { /* function set (DL/N/F) — no visible effect */ }
    else if (byte & 0x10) { /* cursor/display shift — ignored */ }
    else if (byte & 0x08) { disp_on_ = (byte & 0x04); disp_pub_ = disp_on_; }            // display on/off
    else if (byte & 0x04) { inc_ = (byte & 0x02); }                                      // entry mode set
    else if (byte & 0x02) { addr_ = 0; cgram_ = false; addr_pub_ = 0; }                  // return home
    else if (byte & 0x01) { std::lock_guard<std::mutex> lk(mtx_); clear_ram();           // clear display
                            addr_ = 0; cgram_ = false; addr_pub_ = 0; }
}

void Hd44780::snapshot(char out[ROWS][COLS]) const {
    static const uint8_t base[ROWS] = { 0x00, 0x40, 0x14, 0x54 };   // HD44780 20x4 line map
    std::lock_guard<std::mutex> lk(mtx_);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            uint8_t ch = ram_[(base[r] + c) & 0x7f];
            out[r][c] = (ch >= 0x20 && ch < 0x80) ? (char)ch : ' ';
        }
}
