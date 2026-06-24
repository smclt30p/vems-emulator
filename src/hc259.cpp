#include "hc259.h"

void Hc259::configure(Outputs* out, int base_channel, char g_port, int g_pin) {
    out_ = out;
    base_ = base_channel;
    g_port_ = g_port;
    g_pin_ = g_pin;
}

void Hc259::attach(avr_t* avr) {
    avr_ = avr;
    for (int i = 0; i < 8; i++) q_[i] = false;
    q_bits_ = 0;
    if (!avr) return;

    struct { char port; int pin; int sig; } map[5] = {
        { 'C', 7, SIG_D },
        { 'C', 4, SIG_S0 },
        { 'C', 5, SIG_S1 },
        { 'C', 6, SIG_S2 },
        { g_port_, g_pin_, SIG_G },
    };
    for (int i = 0; i < 5; i++) {
        pins_[i] = Pin{ this, map[i].sig };
        avr_irq_t* irq = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(map[i].port), IOPORT_IRQ_PIN0 + map[i].pin);
        if (irq) avr_irq_register_notify(irq, pin_cb, &pins_[i]);
    }
}

void Hc259::pin_cb(avr_irq_t*, uint32_t value, void* param) {
    Pin* p = (Pin*)param;
    p->self->on_pin(p->sig, value);
}

void Hc259::on_pin(int sig, uint32_t value) {
    bool v = (value != 0);
    switch (sig) {
        case SIG_D:  d_ = v; break;
        case SIG_S0: s0_ = v; break;
        case SIG_S1: s1_ = v; break;
        case SIG_S2: s2_ = v; break;
        case SIG_G:  g_ = v; break;
    }
    recompute();
}

void Hc259::recompute() {
    if (g_.load()) return;                 // G high: all latches hold
    int a = addr();
    bool nd = d_.load();
    if (q_[a] == nd) return;
    q_[a] = nd;
    int bits = 0;
    for (int i = 0; i < 8; i++) if (q_[i]) bits |= (1 << i);
    q_bits_ = bits;
    if (out_) out_->drive(base_ + a, nd);
}
