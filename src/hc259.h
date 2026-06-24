#pragma once
#include <atomic>
#include <cstdint>

#include "outputs.h"

extern "C" {
#include "sim_avr.h"
#include "sim_irq.h"
#include "sim_io.h"
#include "avr_ioport.h"
}

// 74x259 8-bit addressable latch. Shared by two board parts:
//   - 74HC259 ignition latch: outputs IGN0..IGN7, G (latch enable) = PG2.
//   - TPIC6A259DW power latch: outputs DRIVE8/DRIVE9/CL/MISC/IDL/FP/DRIVE10/DRIVE11, G = PD7.
// Both share D = PC7 and address S0/S1/S2 = PC4/PC5/PC6; only the G pin differs.
// CLR (master reset, active low) tied to VCC (never clears). With CLR high:
//   G low  -> addressable-latch mode: Q[addr] follows D, others hold.
//   G high -> all latches hold.
// Each Q transition is forwarded to the Outputs monitor (virtual channels) so the
// latched outputs get the same pulse/period/duty/history as the injectors.
class Hc259 {
public:
    // base = channel index of Q0; g_port/g_pin = the latch-enable pin (default PG2).
    void configure(Outputs* out, int base_channel, char g_port = 'G', int g_pin = 2);
    void attach(avr_t* avr);

    bool d() const { return d_.load(); }
    bool s0() const { return s0_.load(); }
    bool s1() const { return s1_.load(); }
    bool s2() const { return s2_.load(); }
    bool g() const { return g_.load(); }              // raw G pin level
    bool enabled() const { return !g_.load(); }       // latch transparent when G low
    char g_port() const { return g_port_; }
    int g_pin() const { return g_pin_; }
    int addr() const { return (s2_.load() << 2) | (s1_.load() << 1) | s0_.load(); }
    bool q(int n) const { return (q_bits_.load() >> n) & 1; }

private:
    enum Sig { SIG_D, SIG_S0, SIG_S1, SIG_S2, SIG_G };
    struct Pin { Hc259* self; int sig; };
    static void pin_cb(avr_irq_t*, uint32_t value, void* param);
    void on_pin(int sig, uint32_t value);
    void recompute();

    Outputs* out_ = nullptr;
    int base_ = 0;
    char g_port_ = 'G';
    int g_pin_ = 2;
    avr_t* avr_ = nullptr;

    Pin pins_[5];
    std::atomic<bool> d_{false}, s0_{false}, s1_{false}, s2_{false}, g_{false};
    std::atomic<int> q_bits_{0};
    bool q_[8] = {};        // sim thread only
};
