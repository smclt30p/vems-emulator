#include "outputs.h"

void Outputs::define(const Def& d) {
    auto c = std::make_unique<Chan>();
    c->self = this;
    c->idx = (int)ch_.size();
    c->port = d.port;
    c->pin = d.pin;
    c->active_high = d.active_high;
    c->name = d.name;
    c->group = d.group;
    c->hist.reserve(kHistory);
    ch_.push_back(std::move(c));
}

int Outputs::define_virtual(const char* name, const char* group, bool active_high) {
    define(Def{ 0, 0, name, group, active_high });
    return (int)ch_.size() - 1;
}

void Outputs::drive(int ch, bool level) {
    if (ch < 0 || ch >= (int)ch_.size()) return;
    on_edge(*ch_[ch], level ? 1u : 0u);
}

void Outputs::attach(avr_t* avr) {
    avr_ = avr;
    if (!avr) return;
    for (auto& c : ch_) {
        c->level = false;
        c->started = false;
        c->had_prev = false;
        if (c->port == 0) continue;   // virtual channel: driven via drive()
        avr_irq_t* irq = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(c->port), IOPORT_IRQ_PIN0 + c->pin);
        if (irq) avr_irq_register_notify(irq, pin_cb, c.get());
    }
}

void Outputs::pin_cb(avr_irq_t*, uint32_t value, void* param) {
    Chan* c = (Chan*)param;
    c->self->on_edge(*c, value);
}

void Outputs::on_edge(Chan& c, uint32_t value) {
    bool lvl = (value != 0);
    if (lvl == c.level.load()) return;
    c.level.store(lvl);

    avr_cycle_count_t now = avr_ ? avr_->cycle : 0;
    double freq = avr_ ? (double)avr_->frequency : 16000000.0;

    bool now_active = (lvl == c.active_high);
    if (now_active) {
        if (c.had_prev)
            c.period_ms.store((double)(now - c.t_prev) / freq * 1000.0);
        c.t_prev = now;
        c.had_prev = true;
        c.t_start = now;
        c.started = true;
    } else if (c.started) {
        c.pulse_ms.store((double)(now - c.t_start) / freq * 1000.0);
        c.pulses++;
    }

    {
        std::lock_guard<std::mutex> lk(c.hist_mtx);
        Edge e{ (double)now / freq, lvl, angle_src_ ? angle_src_->load() : 0.0 };
        if (c.hist.size() < kHistory) {
            c.hist.push_back(e);
        } else {
            c.hist[c.hist_head] = e;
            c.hist_head = (c.hist_head + 1) % kHistory;
        }
    }
}

void Outputs::edges(int c, std::vector<Edge>& out) const {
    out.clear();
    const Chan& ch = *ch_[c];
    std::lock_guard<std::mutex> lk(ch.hist_mtx);
    if (ch.hist.size() < kHistory) {
        out = ch.hist;
    } else {
        out.reserve(kHistory);
        for (size_t i = 0; i < kHistory; i++)
            out.push_back(ch.hist[(ch.hist_head + i) % kHistory]);
    }
}
