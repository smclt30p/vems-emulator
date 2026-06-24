#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

#include "uart_bridge.h"
#include "mcp3208.h"
#include "hip9011.h"
#include "outputs.h"
#include "hc259.h"
#include "lcd.h"

extern "C" {
#include "sim_avr.h"
}

enum class EmuState { Empty, Loaded, Running, Done, Crashed };

class EmuCore {
public:
    EmuCore();
    ~EmuCore();

    void init(uint16_t tcp_port);       // start sim thread + tcp bridge

    void open_firmware(const std::string& path);  // queued, loads + stays stopped
    void open_bootloader(const std::string& path); // load v3 bootloader hex from disk, enable + reset
    std::string bootloader_path();                 // "" = none loaded
    void run();

    // Real v3 bootloader: overlay boot/main.hex at the atmega128 boot section and
    // start the core there (emulates BOOTRST=0), so prog.pl can reflash over serial.
    void set_bootloader(bool enabled);
    bool bootloader_enabled() const { return boot_enabled_.load(); }
    bool bootloader_loaded() const { return boot_loaded_.load(); }
    uint32_t boot_start() const { return boot_start_; }

    void set_serial(int serial);
    int serial() const { return serial_.load(); }
    std::string identity() const;

    void set_report_locked(bool enabled);
    bool report_locked() const { return report_locked_.load(); }
    void set_lock_byte(int v);
    int lock_byte() const { return lock_byte_.load(); }

    bool bootloader_active() const { return boot_active_.load(); }
    uint32_t lock_reads() const { return lock_reads_.load(); }
    uint32_t page_erases() const { return page_erases_.load(); }
    uint32_t page_writes() const { return page_writes_.load(); }
    double last_lock_read_s() const { return last_lock_read_s_.load(); }
    double last_write_s() const { return last_write_s_.load(); }
    double last_download_s() const { return last_download_s_.load(); }

    void stop();
    void reset();

    // Analog input injection. ch = ADC channel (0..15), value in millivolts (0..5000).
    void set_adc_mv(int ch, int mv);
    int adc_mv(int ch) const;

    // Crank trigger: 60-2 missing-tooth wheel on PD4 (Timer1 ICP1).
    void set_trigger_enabled(bool e) { trig_enabled_ = e; }
    bool trigger_enabled() const { return trig_enabled_.load(); }
    void set_trigger_rpm(float rpm) { trig_rpm_ = rpm; }
    float trigger_rpm() const { return trig_rpm_.load(); }
    int trigger_slot() const { return trig_slot_.load(); }
    bool trigger_level() const { return trig_level_.load(); }
    int trigger_rev() const { return trig_rev_.load(); }
    static int trigger_total_teeth() { return 60; }
    static int trigger_missing_teeth() { return 2; }

    // Current total crank degrees (monotonic); stamped onto each output edge so the
    // scope can plot channel activity against 0..720 deg crank angle.
    double crank_total_deg() const { return crank_total_deg_.load(); }

    // Cam: single pulse per 720 deg (2 crank revs) on PE7 (Timer3 ICP3).
    void set_cam_enabled(bool e) { cam_enabled_ = e; }
    bool cam_enabled() const { return cam_enabled_.load(); }
    bool cam_level() const { return cam_level_.load(); }
    static int cam_pulse_start_slot() { return 0; }   // slot in rev 0 where the pulse starts
    static int cam_pulse_slots() { return 3; }        // pulse width in wheel slots

    EmuState state() const { return state_.load(); }
    const char* state_str() const;
    uint64_t cycles() const { return cycles_.load(); }
    double sim_seconds() const { return (double)cycles_.load() / (double)freq_; }
    std::string firmware_name();
    std::string last_error();
    uint32_t fw_lowest() const { return fw_lowest_; }
    uint32_t fw_highest() const { return fw_highest_; }

    // EEPROM persistence (eeprom.bin in the working directory).
    bool eeprom_from_file() const { return eeprom_loaded_.load(); }
    uint32_t eeprom_size() const { return eeprom_size_; }
    uint64_t eeprom_saves() const { return eeprom_saves_.load(); }
    const char* eeprom_path() const { return eeprom_path_.c_str(); }

    UartBridge& bridge() { return bridge_; }
    Mcp3208& mcp() { return mcp_; }
    Hip9011& hip() { return hip_; }
    Outputs& outputs() { return outputs_; }
    Hc259& ignmux() { return ignmux_; }
    Hc259& outmux() { return outmux_; }
    Hc259& auxmux() { return auxmux_; }
    Hd44780& lcd() { return lcd_; }

private:
    void sim_loop();
    void do_load(const std::string& path);
    void do_reset();
    void teardown_avr();
    void apply_adc();
    void load_eeprom();
    void save_eeprom_if_dirty(bool force);
    void attach_trigger();
    static avr_cycle_count_t trigger_cb(avr_t*, avr_cycle_count_t when, void* param);
    avr_cycle_count_t trigger_step(avr_t*, avr_cycle_count_t when);

    avr_t* avr_ = nullptr;
    uint32_t freq_ = 16000000;
    const char* mmcu_ = "atmega128";

    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> running_{false};
    std::atomic<EmuState> state_{EmuState::Empty};
    std::atomic<uint64_t> cycles_{0};

    std::atomic<int> adc_mv_[16];
    std::atomic<bool> adc_dirty_{true};

    avr_irq_t* trig_irq_ = nullptr;            // PD4 pin (sim thread only)
    avr_irq_t* cam_irq_ = nullptr;             // PE7 pin (sim thread only)
    std::atomic<bool> trig_enabled_{false};
    std::atomic<float> trig_rpm_{0.0f};
    std::atomic<int> trig_slot_{0};            // current wheel slot, for the UI graph cursor
    std::atomic<int> trig_rev_{0};             // current crank rev (0/1) within the 720 deg cycle
    std::atomic<bool> trig_level_{false};
    std::atomic<bool> cam_enabled_{false};
    std::atomic<bool> cam_level_{false};
    int t_slot_ = 0;                           // stepping state (sim thread only)
    int t_half_ = 0;
    int t_rev_ = 0;
    int t_cycle_ = 0;                          // completed 720 deg cycles (sim thread only)
    std::atomic<double> crank_total_deg_{0.0};

    uint8_t* eeprom_ptr_ = nullptr;            // live simavr buffer (sim thread only)
    uint32_t eeprom_size_ = 0;
    std::vector<uint8_t> eeprom_saved_;        // last value written to disk
    std::string eeprom_path_ = "eeprom.bin";
    std::chrono::steady_clock::time_point last_ee_check_;
    std::atomic<bool> eeprom_loaded_{false};
    std::atomic<uint64_t> eeprom_saves_{0};

    std::mutex req_mtx_;
    std::string pending_path_;
    bool pending_load_ = false;
    bool pending_reset_ = false;
    bool pending_run_ = false;
    bool pending_stop_ = false;

    std::atomic<bool> boot_enabled_{false};
    std::atomic<bool> boot_loaded_{false};
    std::atomic<int> serial_{10443};
    uint32_t boot_start_ = 0;

    std::atomic<bool> report_locked_{true};
    std::atomic<int> lock_byte_{0xfc};
    std::atomic<bool> boot_active_{false};
    std::atomic<uint32_t> lock_reads_{0}, page_erases_{0}, page_writes_{0};
    std::atomic<double> last_lock_read_s_{-1.0}, last_write_s_{-1.0}, last_download_s_{-1.0};
    uint32_t prev_lr_ = 0, prev_pe_ = 0, prev_pw_ = 0;
    uint64_t prev_tx_ = 0;
    void sample_flash_status();
    void reapply_if_loaded();

    std::mutex info_mtx_;
    std::string fw_name_;
    std::string last_error_;
    std::string loaded_path_;
    std::string boot_path_;                       // user-loaded v3 bootloader hex ("" = none)
    uint32_t fw_lowest_ = 0, fw_highest_ = 0;

    UartBridge bridge_;
    Mcp3208 mcp_;
    Hip9011 hip_;
    Outputs outputs_;
    Hc259 ignmux_;
    Hc259 outmux_;
    Hc259 auxmux_;
    Hd44780 lcd_;
};
