#include "uart_bridge.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <util.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

static long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

UartBridge::~UartBridge() { stop(); }

void UartBridge::trace_byte(char dir, uint8_t b) {
    if (!trace_) return;
    std::lock_guard<std::mutex> lk(trace_mtx_);
    fprintf(trace_, "%c %02X\n", dir, b);
    fflush(trace_);
}

bool UartBridge::start(uint16_t port) {
    if (const char* tp = getenv("VEMS_UART_TRACE")) {
        trace_ = fopen(tp, "w");
        if (trace_) fprintf(stderr, "[uart] tracing to %s\n", tp);
    }
    port_ = port;
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { perror("socket"); return false; }
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port_);
    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(listen_fd_); listen_fd_ = -1; return false; }
    if (listen(listen_fd_, 1) < 0) { perror("listen"); close(listen_fd_); listen_fd_ = -1; return false; }
    fcntl(listen_fd_, F_SETFL, O_NONBLOCK);
    quit_ = false;
    thread_ = std::thread(&UartBridge::service_loop, this);
    return true;
}

void UartBridge::stop() {
    quit_ = true;
    if (listen_fd_ >= 0) { shutdown(listen_fd_, SHUT_RDWR); close(listen_fd_); listen_fd_ = -1; }
    close_tcp_client();
    int m = pty_master_.exchange(-1);
    if (m >= 0) close(m);
    if (thread_.joinable()) thread_.join();
    if (!pty_link_.empty()) unlink(pty_link_.c_str());
}

void UartBridge::close_tcp_client() {
    int c = client_fd_.exchange(-1);
    if (c >= 0) { shutdown(c, SHUT_RDWR); close(c); }
}

bool UartBridge::open_pty() {
    int m = -1, s = -1;
    char name[128] = {0};
    if (openpty(&m, &s, name, nullptr, nullptr) < 0) { perror("openpty"); return false; }

    struct termios t;
    if (tcgetattr(m, &t) == 0) { cfmakeraw(&t); tcsetattr(m, TCSANOW, &t); }
    fcntl(m, F_SETFL, O_NONBLOCK);
    close(s);   // we only keep the master; readers open the slave via the symlink

    pty_link_ = "/tmp/vems-uart0";
    unlink(pty_link_.c_str());
    if (symlink(name, pty_link_.c_str()) != 0) {
        // fall back to advertising the raw slave path if the symlink can't be made
        pty_link_ = name;
    }
    pty_master_.store(m);
    fprintf(stderr, "[uart] pty ready: %s -> %s\n", pty_link_.c_str(), name);
    return true;
}

void UartBridge::attach(avr_t* avr, char uart) {
    avr_ = avr;
    uart_ = uart;
    xon_ = true;

    uint32_t f = 0;
    avr_ioctl(avr, AVR_IOCTL_UART_GET_FLAGS(uart), &f);
    f &= ~AVR_UART_FLAG_STDIO;
    avr_ioctl(avr, AVR_IOCTL_UART_SET_FLAGS(uart), &f);

    avr_irq_t* src  = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUTPUT);
    irq_input_      = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_INPUT);
    avr_irq_t* xon  = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XON);
    avr_irq_t* xoff = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XOFF);

    if (src) avr_irq_register_notify(src, byte_from_avr_cb, this);
    if (xon) avr_irq_register_notify(xon, xon_cb, this);
    if (xoff) avr_irq_register_notify(xoff, xoff_cb, this);

    last_probe_when_ = 0;
    sig_active_ = false;

    // Persistent ~1ms poll that drains host->AVR bytes whenever the UART has room.
    avr_cycle_timer_register(avr, avr_hz_to_cycles(avr, 1000), flush_cb, this);
}

void UartBridge::probe_signature() {
    { std::lock_guard<std::mutex> lk(sig_mtx_); signature_.clear(); }
    sig_wanted_ = true;
}

std::string UartBridge::signature() {
    std::lock_guard<std::mutex> lk(sig_mtx_);
    return signature_;
}

void UartBridge::byte_from_avr_cb(avr_irq_t*, uint32_t value, void* param) {
    UartBridge* p = (UartBridge*)param;
    uint8_t b = (uint8_t)value;

    if (p->sig_active_) {
        if (b == 0 || p->sig_buf_.size() >= 96) {
            bool has_alpha = false;
            for (char c : p->sig_buf_) if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') { has_alpha = true; break; }
            if (p->sig_buf_.size() >= 4 && has_alpha) {
                std::lock_guard<std::mutex> lk(p->sig_mtx_);
                p->signature_ = p->sig_buf_;
                p->sig_wanted_ = false;     // got a valid signature, stop probing
            }
            // else: junk/early-boot reply — discard and let flush_cb retry
            p->sig_active_ = false;
        } else if (b >= 0x20 && b < 0x7f) {
            p->sig_buf_.push_back((char)b);
        }
        return; // don't leak probe bytes into the host fifo
    }

    p->trace_byte('T', b);
    std::lock_guard<std::mutex> lk(p->tx_mtx_);
    p->tx_.push_back(b);
    p->tx_total_++;
}

void UartBridge::xon_cb(avr_irq_t*, uint32_t, void* param) {
    UartBridge* p = (UartBridge*)param;
    p->xon_ = true;
    p->flush_to_avr();
}

void UartBridge::xoff_cb(avr_irq_t*, uint32_t, void* param) {
    UartBridge* p = (UartBridge*)param;
    p->xon_ = false;
}

avr_cycle_count_t UartBridge::flush_cb(avr_t* avr, avr_cycle_count_t when, void* param) {
    UartBridge* p = (UartBridge*)param;
    p->flush_to_avr();

    // Self-query the signature while idle: send 'S', retrying every ~500ms until
    // the reply is captured. Stay silent whenever a client is actively talking
    // (TCP connected, or any host->AVR byte in the last 1.5s on either transport),
    // so the probe never injects 'S' into or steals bytes from a live session.
    bool host_active = p->client_fd_.load() >= 0 ||
                       (now_ms() - p->last_rx_ms_.load()) < 1500;
    if (p->sig_wanted_ && !p->sig_active_ && !host_active && p->irq_input_) {
        if (p->last_probe_when_ == 0 || (when - p->last_probe_when_) > avr_hz_to_cycles(avr, 2)) {
            p->last_probe_when_ = when;
            p->sig_buf_.clear();
            p->sig_active_ = true;
            avr_raise_irq(p->irq_input_, 'S');
        }
    }

    return when + avr_hz_to_cycles(avr, 1000);
}

void UartBridge::flush_to_avr() {
    if (!irq_input_) return;
    std::lock_guard<std::mutex> lk(rx_mtx_);
    while (xon_ && !rx_.empty()) {
        uint8_t b = rx_.front();
        rx_.pop_front();
        avr_raise_irq(irq_input_, b);
    }
}

// Move bytes between one host fd and the fifos. Returns false if the fd died
// (only meaningful for the TCP client; a pty master is kept open across EOF).
void UartBridge::pump_fd(int fd) {
    // host -> AVR
    uint8_t buf[4096];
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n == 0) {                          // EOF
        if (fd == client_fd_.load()) close_tcp_client();
    } else if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && fd == client_fd_.load()) close_tcp_client();
    } else {
        std::lock_guard<std::mutex> lk(rx_mtx_);
        for (ssize_t i = 0; i < n; i++) { rx_.push_back(buf[i]); trace_byte('R', buf[i]); }
        rx_total_ += (uint64_t)n;
        last_rx_ms_.store(now_ms());
    }

    // AVR -> host
    size_t len = 0;
    {
        std::lock_guard<std::mutex> lk(tx_mtx_);
        while (len < sizeof(buf) && !tx_.empty()) { buf[len++] = tx_.front(); tx_.pop_front(); }
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = ::write(fd, buf + off, len - off);
        if (w <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // re-queue the unsent tail at the front, preserving order
                std::lock_guard<std::mutex> lk(tx_mtx_);
                for (size_t i = len; i > off; i--) tx_.push_front(buf[i - 1]);
                break;
            }
            if (fd == client_fd_.load()) close_tcp_client();
            break;
        }
        off += (size_t)w;
    }
}

void UartBridge::service_loop() {
    open_pty();

    while (!quit_) {
        Transport tr = transport_.load();

        // Drop a TCP client if we've switched to the pty.
        if (tr != Transport::Tcp && client_fd_.load() >= 0) close_tcp_client();

        int afd = (tr == Transport::Tcp) ? client_fd_.load() : pty_master_.load();

        fd_set rs, ws;
        FD_ZERO(&rs); FD_ZERO(&ws);
        int maxfd = -1;
        if (listen_fd_ >= 0) { FD_SET(listen_fd_, &rs); maxfd = listen_fd_; }
        if (afd >= 0) {
            FD_SET(afd, &rs);
            bool have_tx;
            { std::lock_guard<std::mutex> lk(tx_mtx_); have_tx = !tx_.empty(); }
            if (have_tx) FD_SET(afd, &ws);
            if (afd > maxfd) maxfd = afd;
        }
        timeval tv{0, 2000};
        int r = select(maxfd + 1, &rs, &ws, nullptr, &tv);
        if (r < 0) { if (errno == EINTR) continue; break; }

        if (listen_fd_ >= 0 && FD_ISSET(listen_fd_, &rs)) {
            sockaddr_in caddr{}; socklen_t clen = sizeof(caddr);
            int cfd = accept(listen_fd_, (sockaddr*)&caddr, &clen);
            if (cfd >= 0) {
                if (transport_.load() != Transport::Tcp) {
                    close(cfd);   // pty mode: refuse TCP clients
                } else {
                    int one = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    fcntl(cfd, F_SETFL, O_NONBLOCK);
                    close_tcp_client();   // one client at a time
                    { std::lock_guard<std::mutex> lk(tx_mtx_); tx_.clear(); }
                    { std::lock_guard<std::mutex> lk(rx_mtx_); rx_.clear(); }
                    client_fd_.store(cfd);
                    fprintf(stderr, "[uart] tcp client %s:%d\n",
                            inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));
                }
            }
        }

        if (afd >= 0 && (FD_ISSET(afd, &rs) || FD_ISSET(afd, &ws))) pump_fd(afd);
    }
}
