#include "emu_core.h"
#include "firmware.h"

#include <chrono>
#include <thread>

extern "C" {
#include "sim_elf.h"
#include "sim_irq.h"
#include "sim_io.h"
#include "avr_adc.h"
#include "avr_eeprom.h"
#include "avr_ioport.h"
}

#include <cstdio>
#include <cstring>

EmuCore::EmuCore() {
    for (int i = 0; i < 16; i++) adc_mv_[i].store(0);
    char name[8];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "INJ%d", i);
        outputs_.define(Outputs::Def{ 'A', i, name, "Injectors", false });
    }
    int ign_base = outputs_.count();
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "IGN%d", i);
        outputs_.define_virtual(name, "Ignition", true);
    }
    ignmux_.configure(&outputs_, ign_base);

    int out_base = outputs_.count();
    static const char* tpic_names[8] = {
        "DRIVE8", "DRIVE9", "CL", "MISC", "IDL", "FP", "DRIVE10", "DRIVE11"
    };
    for (int i = 0; i < 8; i++)
        outputs_.define_virtual(tpic_names[i], "Power outputs", true);
    outmux_.configure(&outputs_, out_base, 'D', 7);

    int aux_base = outputs_.count();
    static const char* aux_names[8] = {
        "LCD_RS", "S259_1", "S259_2", "STEPA", "STEPB", "STEPC", "STEPD", "EN_AB_CD"
    };
    for (int i = 0; i < 8; i++)
        outputs_.define_virtual(aux_names[i], "Aux outputs", true);
    auxmux_.configure(&outputs_, aux_base, 'D', 6);

    outputs_.define(Outputs::Def{ 'C', 3, "KNK_INT", "Knock", true });   // HIP9011 INT/HOLD (high=integrate)
    outputs_.set_angle_source(&crank_total_deg_);

    lcd_.configure(&auxmux_, 0);   // RS = aux 74HC259 Q0 (LCD_RS)
}
EmuCore::~EmuCore() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    save_eeprom_if_dirty(true);   // final flush after the sim thread has stopped
    bridge_.stop();
    teardown_avr();
}

void EmuCore::init(uint16_t tcp_port) {
    bridge_.start(tcp_port);
    thread_ = std::thread(&EmuCore::sim_loop, this);
}

void EmuCore::open_firmware(const std::string& path) {
    std::lock_guard<std::mutex> lk(req_mtx_);
    pending_path_ = path;
    pending_load_ = true;
}
void EmuCore::set_bootloader(bool enabled) {
    boot_enabled_.store(enabled);
    reapply_if_loaded();
}

void EmuCore::open_bootloader(const std::string& path) {
    { std::lock_guard<std::mutex> lk(info_mtx_); boot_path_ = path; }
    boot_enabled_.store(true);
    reapply_if_loaded();
}

std::string EmuCore::bootloader_path() {
    std::lock_guard<std::mutex> lk(info_mtx_);
    return boot_path_;
}

void EmuCore::set_serial(int serial) {
    serial_.store(serial);
    reapply_if_loaded();
}

void EmuCore::reapply_if_loaded() {
    bool have_fw;
    { std::lock_guard<std::mutex> lk(info_mtx_); have_fw = !loaded_path_.empty(); }
    if (have_fw) {
        std::lock_guard<std::mutex> lk(req_mtx_);
        pending_reset_ = true;
    }
}

std::string EmuCore::identity() const {
    int s = serial_.load();
    if (s < 0) s = 0;
    if (s > 999999) s = 999999;
    char b[16];
    snprintf(b, sizeof(b), "v3.3_%c%06d", s < 135 ? 'k' : 'n', s);
    return std::string(b);
}

void EmuCore::set_report_locked(bool enabled) {
    report_locked_.store(enabled);
    reapply_if_loaded();
}

void EmuCore::set_lock_byte(int v) {
    lock_byte_.store(v & 0xff);
    reapply_if_loaded();
}

void EmuCore::sample_flash_status() {
    if (!avr_) return;
    double t = sim_seconds();
    uint32_t lr = avr_->flash_lockread, pe = avr_->flash_pageerase, pw = avr_->flash_pagewrite;
    bool wrote = (pe != prev_pe_) || (pw != prev_pw_);
    if (lr != prev_lr_) { prev_lr_ = lr; lock_reads_ = lr; last_lock_read_s_ = t; }
    if (pe != prev_pe_) { prev_pe_ = pe; page_erases_ = pe; last_write_s_ = t; }
    if (pw != prev_pw_) { prev_pw_ = pw; page_writes_ = pw; last_write_s_ = t; }
    bool ba = boot_loaded_.load() && avr_->pc >= boot_start_;
    boot_active_ = ba;
    uint64_t tx = bridge_.bytes_to_host();
    if (ba && !wrote && tx > prev_tx_ + 128) last_download_s_ = t;
    prev_tx_ = tx;
}

void EmuCore::set_adc_mv(int ch, int mv) {
    if (ch < 0 || ch >= 16) return;
    if (mv < 0) mv = 0;
    if (mv > 5000) mv = 5000;
    adc_mv_[ch].store(mv);
    adc_dirty_ = true;
}
int EmuCore::adc_mv(int ch) const { return (ch >= 0 && ch < 16) ? adc_mv_[ch].load() : 0; }

void EmuCore::apply_adc() {
    if (!avr_) return;
    for (int ch = 0; ch < 16; ch++) {
        avr_irq_t* irq = avr_io_getirq(avr_, AVR_IOCTL_ADC_GETIRQ, ADC_IRQ_ADC0 + ch);
        if (irq) avr_raise_irq(irq, (uint32_t)adc_mv_[ch].load());
    }
}

void EmuCore::attach_trigger() {
    trig_irq_ = nullptr;
    cam_irq_ = nullptr;
    t_slot_ = 0;
    t_half_ = 0;
    t_rev_ = 0;
    t_cycle_ = 0;
    crank_total_deg_.store(0.0);
    if (!avr_) return;
    trig_irq_ = avr_io_getirq(avr_, AVR_IOCTL_IOPORT_GETIRQ('D'), IOPORT_IRQ_PIN4);
    cam_irq_ = avr_io_getirq(avr_, AVR_IOCTL_IOPORT_GETIRQ('E'), IOPORT_IRQ_PIN7);
    if (trig_irq_) avr_raise_irq(trig_irq_, 0);
    if (cam_irq_) avr_raise_irq(cam_irq_, 0);
    avr_cycle_timer_register(avr_, avr_hz_to_cycles(avr_, 1000), trigger_cb, this);
}

avr_cycle_count_t EmuCore::trigger_cb(avr_t* avr, avr_cycle_count_t when, void* param) {
    return ((EmuCore*)param)->trigger_step(avr, when);
}

avr_cycle_count_t EmuCore::trigger_step(avr_t* avr, avr_cycle_count_t when) {
    const int total = trigger_total_teeth();          // 60
    const int present = total - trigger_missing_teeth(); // 58
    float rpm = trig_rpm_.load();

    if (!trig_enabled_.load() || rpm < 1.0f) {
        if (trig_irq_) avr_raise_irq(trig_irq_, 0);
        if (cam_irq_) avr_raise_irq(cam_irq_, 0);
        trig_level_ = false; cam_level_ = false;
        t_slot_ = 0; t_half_ = 0; t_rev_ = 0; t_cycle_ = 0; trig_slot_ = 0; trig_rev_ = 0;
        crank_total_deg_.store(0.0);
        return when + avr_hz_to_cycles(avr, 100);      // idle: hold low, poll every 10ms
    }

    int level = (t_slot_ < present && t_half_ == 0) ? 1 : 0;
    if (trig_irq_) avr_raise_irq(trig_irq_, level);
    trig_level_ = (level != 0);
    trig_slot_ = t_slot_;
    trig_rev_ = t_rev_;

    int cam_start = cam_pulse_start_slot();
    int cam = (cam_enabled_.load() && t_rev_ == 0 &&
               t_slot_ >= cam_start && t_slot_ < cam_start + cam_pulse_slots()) ? 1 : 0;
    if (cam_irq_) avr_raise_irq(cam_irq_, cam);
    cam_level_ = (cam != 0);

    // Current crank angle at this sim time; Outputs stamps it onto each edge.
    crank_total_deg_.store((double)t_cycle_ * 720.0 + t_rev_ * 360.0 + t_slot_ * 6.0 + t_half_ * 3.0);

    if (++t_half_ > 1) {
        t_half_ = 0;
        if (++t_slot_ >= total) {
            t_slot_ = 0;
            int prev_rev = t_rev_;
            t_rev_ ^= 1;
            if (prev_rev == 1 && t_rev_ == 0) t_cycle_++;
        }
    }

    // one wheel slot = 6 crank deg = (1/RPM) s; we step in half-slots.
    double cyc = (double)avr->frequency * 0.5 / rpm;
    if (cyc < 2.0) cyc = 2.0;
    return when + (avr_cycle_count_t)cyc;
}

void EmuCore::load_eeprom() {
    eeprom_ptr_ = nullptr;
    eeprom_size_ = 0;
    eeprom_loaded_ = false;
    if (!avr_) return;

    uint32_t size = avr_->e2end + 1;
    avr_eeprom_desc_t get{};
    get.ee = nullptr; get.offset = 0; get.size = (uint16_t)size;
    if (avr_ioctl(avr_, AVR_IOCTL_EEPROM_GET, &get) != 0 || !get.ee) return;
    eeprom_ptr_ = get.ee;
    eeprom_size_ = size;

    FILE* f = fopen(eeprom_path_.c_str(), "rb");
    if (f) {
        std::vector<uint8_t> buf(size, 0xFF);
        size_t r = fread(buf.data(), 1, size, f);
        fclose(f);
        if (r > 0) {
            avr_eeprom_desc_t set{};
            set.ee = buf.data(); set.offset = 0; set.size = (uint16_t)size;
            avr_ioctl(avr_, AVR_IOCTL_EEPROM_SET, &set);
            eeprom_loaded_ = true;
        }
    }
    eeprom_saved_.assign(eeprom_ptr_, eeprom_ptr_ + size);
    last_ee_check_ = std::chrono::steady_clock::now();
}

void EmuCore::save_eeprom_if_dirty(bool force) {
    if (!eeprom_ptr_ || eeprom_size_ == 0) return;
    auto now = std::chrono::steady_clock::now();
    if (!force && now - last_ee_check_ < std::chrono::seconds(1)) return;
    last_ee_check_ = now;

    if (eeprom_saved_.size() == eeprom_size_ &&
        memcmp(eeprom_saved_.data(), eeprom_ptr_, eeprom_size_) == 0) return;

    FILE* f = fopen(eeprom_path_.c_str(), "wb");
    if (!f) return;
    fwrite(eeprom_ptr_, 1, eeprom_size_, f);
    fclose(f);
    eeprom_saved_.assign(eeprom_ptr_, eeprom_ptr_ + eeprom_size_);
    eeprom_saves_++;
}

void EmuCore::run() { std::lock_guard<std::mutex> lk(req_mtx_); pending_run_ = true; pending_stop_ = false; }
void EmuCore::stop() { std::lock_guard<std::mutex> lk(req_mtx_); pending_stop_ = true; pending_run_ = false; }
void EmuCore::reset() { std::lock_guard<std::mutex> lk(req_mtx_); pending_reset_ = true; }

const char* EmuCore::state_str() const {
    switch (state_.load()) {
        case EmuState::Empty: return "No firmware";
        case EmuState::Loaded: return "Loaded (stopped)";
        case EmuState::Running: return "Running";
        case EmuState::Done: return "Halted";
        case EmuState::Crashed: return "Crashed";
    }
    return "?";
}

std::string EmuCore::firmware_name() { std::lock_guard<std::mutex> lk(info_mtx_); return fw_name_; }
std::string EmuCore::last_error() { std::lock_guard<std::mutex> lk(info_mtx_); return last_error_; }

void EmuCore::teardown_avr() {
    if (avr_) {
        avr_terminate(avr_);
        avr_ = nullptr;
    }
    eeprom_ptr_ = nullptr;
    eeprom_size_ = 0;
    trig_irq_ = nullptr;
    cam_irq_ = nullptr;
}

void EmuCore::do_load(const std::string& path) {
    save_eeprom_if_dirty(true);   // persist the outgoing image's EEPROM first
    running_ = false;
    teardown_avr();

    avr_ = avr_make_mcu_by_name(mmcu_);
    if (!avr_) {
        std::lock_guard<std::mutex> lk(info_mtx_);
        last_error_ = std::string("unknown mcu: ") + mmcu_;
        state_ = EmuState::Empty;
        return;
    }
    avr_init(avr_);
    avr_->frequency = freq_;
    avr_->vcc = avr_->avcc = avr_->aref = 5000;  // 5V rails so ADC mV -> counts is mv*1023/5000
    avr_->fuse[0] = 0x2f;
    avr_->fuse[1] = 0xc4;
    avr_->fuse[2] = 0xff;
    avr_->lockbits = report_locked_.load() ? (uint8_t)lock_byte_.load() : 0xff;

    prev_lr_ = prev_pe_ = prev_pw_ = 0;
    prev_tx_ = 0;
    lock_reads_ = page_erases_ = page_writes_ = 0;
    last_lock_read_s_ = last_write_s_ = last_download_s_ = -1.0;
    boot_active_ = false;

    size_t cap = (size_t)avr_->flashend + 1;
    FirmwareImage img = load_firmware(path, cap);
    if (!img.ok) {
        std::lock_guard<std::mutex> lk(info_mtx_);
        last_error_ = "load failed: " + img.error;
        teardown_avr();
        state_ = EmuState::Empty;
        return;
    }

    boot_loaded_ = false;
    boot_start_ = 0;
    std::string boot_err;
    if (boot_enabled_.load()) {
        std::string bpath;
        { std::lock_guard<std::mutex> lk(info_mtx_); bpath = boot_path_; }
        if (bpath.empty()) {
            boot_err = "no bootloader loaded (File -> Open bootloader)";
        } else {
            uint32_t blo, bhi;
            if (overlay_intel_hex(bpath, img.flash, kBootOverlayShift, boot_err, blo, bhi)) {
                std::string id = identity();
                for (uint32_t i = 0; i < kBootIdentityLen && i < id.size(); i++)
                    img.flash[kBootIdentityOffset + i] = (uint8_t)id[i];
                boot_loaded_ = true;
                boot_start_ = (avr_->flashend + 1) - 2048;
            }
        }
    }

    memcpy(avr_->flash, img.flash.data(), cap);
    avr_->reset_pc = boot_loaded_ ? boot_start_ : 0;
    avr_->pc = avr_->reset_pc;                            // BOOTRST=0 emulation
    avr_->codeend = avr_->flashend;
    avr_->log = 1;

    bridge_.attach(avr_, '0');
    bridge_.probe_signature();
    prev_tx_ = bridge_.bytes_to_host();
    load_eeprom();
    attach_trigger();
    mcp_.attach(avr_);
    hip_.attach(avr_);
    outputs_.attach(avr_);
    ignmux_.attach(avr_);
    outmux_.attach(avr_);
    auxmux_.attach(avr_);
    lcd_.attach(avr_);
    cycles_ = 0;
    adc_dirty_ = true;

    {
        std::lock_guard<std::mutex> lk(info_mtx_);
        loaded_path_ = path;
        size_t slash = path.find_last_of("/\\");
        fw_name_ = slash == std::string::npos ? path : path.substr(slash + 1);
        last_error_ = (boot_enabled_.load() && !boot_loaded_) ? ("bootloader: " + boot_err) : "";
        fw_lowest_ = img.lowest;
        fw_highest_ = img.highest;
    }
    state_ = EmuState::Loaded;
}

void EmuCore::do_reset() {
    std::string path;
    { std::lock_guard<std::mutex> lk(info_mtx_); path = loaded_path_; }
    if (!path.empty()) do_load(path);
}

void EmuCore::sim_loop() {
    using namespace std::chrono;
    while (!quit_) {
        std::string load_path;
        bool do_load_req = false, do_reset_req = false, run_req = false, stop_req = false;
        {
            std::lock_guard<std::mutex> lk(req_mtx_);
            if (pending_load_) { load_path = pending_path_; do_load_req = true; pending_load_ = false; }
            if (pending_reset_) { do_reset_req = true; pending_reset_ = false; }
            if (pending_run_) { run_req = true; pending_run_ = false; }
            if (pending_stop_) { stop_req = true; pending_stop_ = false; }
        }

        if (do_load_req) do_load(load_path);
        if (do_reset_req) do_reset();
        if (run_req && avr_ && state_ != EmuState::Empty) { running_ = true; state_ = EmuState::Running; }
        if (stop_req && avr_) { running_ = false; if (state_ == EmuState::Running) state_ = EmuState::Loaded; }

        if (avr_ && adc_dirty_.exchange(false)) apply_adc();

        if (avr_ && running_) {
            int s = avr_run(avr_);
            cycles_ = avr_->cycle;
            if (s == cpu_Done) { running_ = false; state_ = EmuState::Done; }
            else if (s == cpu_Crashed) { running_ = false; state_ = EmuState::Crashed; }
        } else {
            std::this_thread::sleep_for(milliseconds(2));
        }

        if (avr_) { sample_flash_status(); save_eeprom_if_dirty(false); }
    }
}
