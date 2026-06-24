#include "mcp3208.h"

void Mcp3208::set_counts(int ch, int counts) {
    if (ch < 0 || ch >= 8) return;
    if (counts < 0) counts = 0;
    if (counts > 4095) counts = 4095;
    ch_[ch].store(counts);
}
int Mcp3208::counts(int ch) const { return (ch >= 0 && ch < 8) ? ch_[ch].load() : 0; }

void Mcp3208::reset_xfer() {
    st_ = WAIT_START;
    cfg_ = 0; cfg_n_ = 0;
    out_n_ = 0; out_pos_ = 0;
}

void Mcp3208::attach(avr_t* avr) {
    avr_ = avr;
    reset_xfer();
    cs_active_ = false;

    spi_in_ = avr_io_getirq(avr, AVR_IOCTL_SPI_GETIRQ(0), SPI_IRQ_INPUT);
    avr_irq_t* spi_out = avr_io_getirq(avr, AVR_IOCTL_SPI_GETIRQ(0), SPI_IRQ_OUTPUT);
    if (spi_out) avr_irq_register_notify(spi_out, spi_out_cb, this);

    avr_irq_t* cs = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('G'), IOPORT_IRQ_PIN4);
    if (cs) avr_irq_register_notify(cs, cs_cb, this);
}

// CS is active-low on PG4: assert resets the transaction, deassert ends it.
void Mcp3208::cs_cb(avr_irq_t*, uint32_t value, void* param) {
    Mcp3208* p = (Mcp3208*)param;
    bool low = (value == 0);
    bool was = p->cs_active_.load();
    if (low && !was) {
        p->reset_xfer();
        p->cs_active_ = true;
        p->cs_txn_++;
    } else if (!low && was) {
        p->cs_active_ = false;
    }
}

void Mcp3208::spi_out_cb(avr_irq_t*, uint32_t value, void* param) {
    Mcp3208* p = (Mcp3208*)param;
    if (!p->cs_active_.load()) return;     // not selected: don't drive MISO
    uint8_t din = (uint8_t)value;
    uint8_t dout = 0;
    for (int i = 7; i >= 0; i--)
        dout = (uint8_t)((dout << 1) | p->clock_bit((din >> i) & 1));
    if (p->spi_in_) avr_raise_irq(p->spi_in_, dout);
}

// Real MCP3208 bit timing: ignore leading bits until the START (1), then sample
// SGL/DIFF + D2 D1 D0, emit a null bit, then B11..B0. "Output then sample" matches
// SPI mode 0 (DOUT changes on falling edge, master samples on rising edge).
int Mcp3208::clock_bit(int din) {
    int dout = 0;
    switch (st_) {
        case WAIT_START:
            if (din) { st_ = CFG; cfg_ = 0; cfg_n_ = 0; }
            break;
        case CFG:
            cfg_ = (cfg_ << 1) | din;
            if (++cfg_n_ == 4) {            // SGL/DIFF, D2, D1, D0
                int ch = cfg_ & 0x7;
                int val = ch_[ch].load() & 0xFFF;
                out_bits_[0] = 0;          // null bit
                for (int i = 0; i < 12; i++) out_bits_[1 + i] = (val >> (11 - i)) & 1;
                out_n_ = 13; out_pos_ = 0;
                st_ = DATA;
                conv_++;
            }
            break;
        case DATA:
            dout = (out_pos_ < out_n_) ? out_bits_[out_pos_] : 0;
            if (++out_pos_ >= out_n_) st_ = DONE;
            break;
        case DONE:
            break;
    }
    return dout;
}
