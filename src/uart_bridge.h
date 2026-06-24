#pragma once
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>

extern "C" {
#include "sim_avr.h"
#include "sim_irq.h"
#include "sim_io.h"
#include "sim_cycle_timers.h"
#include "sim_time.h"
#include "avr_uart.h"
}

// Bridges an AVR UART to the host. Two interchangeable transports share the same
// simavr-side plumbing and fifos: a TCP server (127.0.0.1:port, for VemsTune) and
// a POSIX pty/virtual-COM-port (a /tmp symlink, for prog.pl / avrdude / minicom).
// One service thread owns both backends; the active one is selectable at runtime.
// All avr_* access happens on the simavr thread; the service thread only touches
// the mutex-guarded fifos, mirroring simavr's uart_pty concurrency model.
class UartBridge {
public:
    enum class Transport { Tcp, Pty };

    UartBridge() = default;
    ~UartBridge();

    bool start(uint16_t port);          // open TCP listener + pty + service thread
    void stop();

    // Called on the sim thread after each (re)load of the avr.
    void attach(avr_t* avr, char uart = '0');

    void set_transport(Transport t) { transport_.store(t); }
    Transport transport() const { return transport_.load(); }

    uint16_t port() const { return port_; }
    bool tcp_client_connected() const { return client_fd_.load() >= 0; }
    std::string pty_path() const { return pty_link_; }
    bool pty_open() const { return pty_master_.load() >= 0; }
    // "connected" w.r.t. the active transport, for the existing UI text.
    bool client_connected() const {
        return transport_.load() == Transport::Tcp ? tcp_client_connected() : pty_open();
    }
    uint64_t bytes_to_host() const { return tx_total_.load(); }
    uint64_t bytes_from_host() const { return rx_total_.load(); }

    // One-shot 'S' signature query (only runs while no client is connected).
    void probe_signature();
    std::string signature();

private:
    static void byte_from_avr_cb(avr_irq_t*, uint32_t value, void* param);
    static void xon_cb(avr_irq_t*, uint32_t value, void* param);
    static void xoff_cb(avr_irq_t*, uint32_t value, void* param);
    static avr_cycle_count_t flush_cb(avr_t*, avr_cycle_count_t when, void* param);

    void flush_to_avr();                // sim thread: rx fifo -> UART_IRQ_INPUT
    void service_loop();                // service thread: routes fifos <-> active fd
    bool open_pty();                    // service thread
    void close_tcp_client();
    void pump_fd(int fd);               // move rx<-fd and tx->fd for one fd

    avr_t* avr_ = nullptr;
    char uart_ = '0';
    avr_irq_t* irq_input_ = nullptr;
    bool xon_ = true;

    std::atomic<Transport> transport_{Transport::Tcp};

    uint16_t port_ = 0;
    int listen_fd_ = -1;
    std::atomic<int> client_fd_{-1};

    std::atomic<int> pty_master_{-1};
    std::string pty_link_;              // e.g. /tmp/vems-uart0 (service thread sets it)

    std::thread thread_;
    std::atomic<bool> quit_{false};

    std::mutex tx_mtx_;                  // AVR -> host
    std::deque<uint8_t> tx_;
    std::mutex rx_mtx_;                  // host -> AVR
    std::deque<uint8_t> rx_;

    std::atomic<uint64_t> tx_total_{0};
    std::atomic<uint64_t> rx_total_{0};
    std::atomic<long long> last_rx_ms_{0};    // steady_clock ms of last host->AVR byte

    std::atomic<bool> sig_wanted_{false};
    bool sig_active_ = false;                 // sim thread only
    avr_cycle_count_t last_probe_when_ = 0;   // sim thread only
    std::string sig_buf_;                     // sim thread only
    std::mutex sig_mtx_;
    std::string signature_;

    FILE* trace_ = nullptr;
    std::mutex trace_mtx_;
    void trace_byte(char dir, uint8_t b);
};
