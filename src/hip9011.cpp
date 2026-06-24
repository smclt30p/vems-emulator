#include "hip9011.h"

// Table 3: bandpass centre frequency (kHz) by 6-bit value.
static const float kBpf[64] = {
    1.22f,1.26f,1.31f,1.35f,1.40f,1.45f,1.51f,1.57f,1.63f,1.71f,1.78f,1.87f,1.96f,2.07f,2.18f,2.31f,
    2.46f,2.54f,2.62f,2.71f,2.81f,2.92f,3.03f,3.15f,3.28f,3.43f,3.59f,3.76f,3.95f,4.16f,4.39f,4.66f,
    4.95f,5.12f,5.29f,5.48f,5.68f,5.90f,6.12f,6.37f,6.64f,6.94f,7.27f,7.63f,8.02f,8.46f,8.95f,9.50f,
    10.12f,10.46f,10.83f,11.22f,11.65f,12.10f,12.60f,13.14f,13.72f,14.36f,15.07f,15.84f,16.71f,17.67f,18.76f,19.98f
};
// Table 3: programmable gain by 6-bit value.
static const float kGain[64] = {
    2.000f,1.882f,1.778f,1.684f,1.600f,1.523f,1.455f,1.391f,1.333f,1.280f,1.231f,1.185f,1.143f,1.063f,1.000f,0.944f,
    0.895f,0.850f,0.810f,0.773f,0.739f,0.708f,0.680f,0.654f,0.630f,0.607f,0.586f,0.567f,0.548f,0.500f,0.471f,0.444f,
    0.421f,0.400f,0.381f,0.364f,0.348f,0.333f,0.320f,0.308f,0.296f,0.286f,0.276f,0.267f,0.258f,0.250f,0.236f,0.222f,
    0.211f,0.200f,0.190f,0.182f,0.174f,0.167f,0.160f,0.154f,0.148f,0.143f,0.138f,0.133f,0.129f,0.125f,0.118f,0.111f
};
// Table 3: integrator time constant (us) by 5-bit value.
static const int kTc[32] = {
    40,45,50,55,60,65,70,75,80,90,100,110,120,130,140,150,
    160,180,200,220,240,260,280,300,320,360,400,440,480,520,560,600
};
// Table 4: prescaler P4..P1 -> external clock (MHz); -1 = reserved.
static const int kPresc[16] = { 4,5,6,8,10,12,16,20,24,-1,-1,-1,-1,-1,-1,-1 };

float Hip9011::bpf_khz(int v) { return (v >= 0 && v < 64) ? kBpf[v] : 0.0f; }
float Hip9011::gain_x(int v) { return (v >= 0 && v < 64) ? kGain[v] : 0.0f; }
int Hip9011::tc_us(int v) { return (v >= 0 && v < 32) ? kTc[v] : 0; }
int Hip9011::prescaler_mhz(int v) { return (v >= 0 && v < 16) ? kPresc[v] : -1; }

void Hip9011::attach(avr_t* avr) {
    avr_ = avr;
    cs_active_ = false;
    have_byte_ = false;

    avr_irq_t* spi_out = avr_io_getirq(avr, AVR_IOCTL_SPI_GETIRQ(0), SPI_IRQ_OUTPUT);
    if (spi_out) avr_irq_register_notify(spi_out, spi_out_cb, this);

    avr_irq_t* cs = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('C'), IOPORT_IRQ_PIN2);
    if (cs) avr_irq_register_notify(cs, cs_cb, this);

    avr_irq_t* ih = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('C'), IOPORT_IRQ_PIN3);
    if (ih) avr_irq_register_notify(ih, inthold_cb, this);
}

// Self-addressing word: address in the leading bits, value in the rest.
void Hip9011::decode(uint8_t b) {
    last_byte_ = b;
    switch (b & 0xC0) {
        case 0x00:                       // 00xxxxxx : bandpass frequency
            bandpass_ = b & 0x3F;
            break;
        case 0x40:                       // 01 P5 P4 P3 P2 P1 Z : prescaler / SO status
            presc_ = (b >> 1) & 0x0F;    // P4..P1
            so_hiz_ = (b & 0x01) != 0;   // Z: 1 = high impedance
            break;
        case 0x80:                       // 10xxxxxx : gain
            gain_ = b & 0x3F;
            break;
        default:                         // 11xxxxxx
            if ((b & 0xE0) == 0xC0)      // 110xxxxx : integrator time constant
                tc_ = b & 0x1F;
            else                         // 111 B4..B0 : test / channel select
                channel_ = b & 0x01;     // B0 = channel
            break;
    }
    words_++;
}

void Hip9011::cs_cb(avr_irq_t*, uint32_t value, void* param) {
    Hip9011* p = (Hip9011*)param;
    bool low = (value == 0);
    bool was = p->cs_active_.load();
    if (low && !was) {                   // CS asserted: start of word
        p->cs_active_ = true;
        p->have_byte_ = false;
        p->cs_txn_++;
    } else if (!low && was) {            // CS released: latch the word (datasheet: loads on CS rising)
        p->cs_active_ = false;
        if (p->have_byte_) p->decode(p->pending_);
    }
}

void Hip9011::spi_out_cb(avr_irq_t*, uint32_t value, void* param) {
    Hip9011* p = (Hip9011*)param;
    if (!p->cs_active_.load()) return;   // not selected
    p->pending_ = (uint8_t)value;
    p->have_byte_ = true;
}

void Hip9011::inthold_cb(avr_irq_t*, uint32_t value, void* param) {
    Hip9011* p = (Hip9011*)param;
    bool high = (value != 0);
    if (high != p->inthold_high_.load()) {
        p->inthold_high_ = high;
        p->inthold_edges_++;
    }
}
