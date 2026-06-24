#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "sim_avr.h"
#include "sim_irq.h"
#include "sim_io.h"
#include "avr_ioport.h"
}

// Universal monitor for the firmware's pulsed digital outputs (injectors, ignition
// coils, IAC/boost/PWM solenoids, ...). Each channel is one MCU port pin; we observe
// edges on the sim thread, time the active pulse / period, and keep a rolling edge
// history (sim-time + level) that a spectrogram view can consume later.
class Outputs {
public:
    static constexpr size_t kHistory = 4096;   // edges retained per channel

    struct Def { char port; int pin; const char* name; const char* group; bool active_high; };
    struct Edge { double t; bool level; double deg; };   // deg = crank angle at the edge

    // Source of the current total crank angle, stamped onto each edge (sim thread).
    void set_angle_source(std::atomic<double>* p) { angle_src_ = p; }
    void define(const Def& d);          // build a pin-backed channel (before attach)
    int define_virtual(const char* name, const char* group, bool active_high); // code-driven channel; returns its index
    void attach(avr_t* avr);            // (re)wire IRQs to a fresh avr
    void drive(int ch, bool level);     // sim thread: feed a virtual channel an edge

    int count() const { return (int)ch_.size(); }
    const char* name(int c) const { return ch_[c]->name.c_str(); }
    const char* group(int c) const { return ch_[c]->group.c_str(); }

    bool level(int c) const { return ch_[c]->level.load(); }
    bool active(int c) const { return ch_[c]->level.load() == ch_[c]->active_high; }
    bool active_high(int c) const { return ch_[c]->active_high; }
    double pulse_ms(int c) const { return ch_[c]->pulse_ms.load(); }
    double period_ms(int c) const { return ch_[c]->period_ms.load(); }
    uint64_t pulses(int c) const { return ch_[c]->pulses.load(); }
    double freq_hz(int c) const { double p = ch_[c]->period_ms.load(); return p > 0.0 ? 1000.0 / p : 0.0; }
    double duty(int c) const { double p = ch_[c]->period_ms.load(); return p > 0.0 ? ch_[c]->pulse_ms.load() / p * 100.0 : 0.0; }

    void edges(int c, std::vector<Edge>& out) const;   // snapshot, oldest -> newest

private:
    struct Chan {
        Outputs* self; int idx;
        char port; int pin; bool active_high;   // port == 0 => virtual (code-driven)
        std::string name, group;
        std::atomic<bool> level{false};
        std::atomic<double> pulse_ms{0.0};
        std::atomic<double> period_ms{0.0};
        std::atomic<uint64_t> pulses{0};
        avr_cycle_count_t t_start = 0, t_prev = 0;   // sim thread only
        bool started = false, had_prev = false;
        mutable std::mutex hist_mtx;
        std::vector<Edge> hist;
        size_t hist_head = 0;
    };

    static void pin_cb(avr_irq_t*, uint32_t value, void* param);
    void on_edge(Chan& c, uint32_t value);

    avr_t* avr_ = nullptr;
    std::atomic<double>* angle_src_ = nullptr;
    std::vector<std::unique_ptr<Chan>> ch_;
};
