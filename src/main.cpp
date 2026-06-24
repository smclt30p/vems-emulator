#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <SDL.h>
#include <SDL_opengl.h>

#include "emu_core.h"
#include "file_browser.h"

#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <utility>

static const uint16_t TCP_PORT = 29000;

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow("VEMS Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 780, 780,
        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (!window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init(glsl_version);

    EmuCore emu;
    emu.init(TCP_PORT);

    FileBrowser browser;

    // Allow firmware path on the command line for convenience (auto-starts).
    if (argc > 1) { emu.open_firmware(argv[1]); emu.run(); }

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) quit = true;
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(window)) quit = true;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);

        bool want_open = false;
        static int pick_mode = 0;   // 0 = firmware, 1 = bootloader
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open firmware (.hex / .bin)...")) { want_open = true; pick_mode = 0; }
                if (ImGui::MenuItem("Open bootloader (.hex)...")) { want_open = true; pick_mode = 1; }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) quit = true;
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        if (want_open) browser.open("", pick_mode == 1 ? "Open bootloader" : "Open firmware");

        std::string picked;
        if (browser.draw(picked)) {
            if (pick_mode == 1) emu.open_bootloader(picked);
            else                emu.open_firmware(picked);
        }

        EmuState st = emu.state();
        const ImGuiChildFlags group_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY;

        if (ImGui::BeginTabBar("tabs")) {

        // ===== Tab: General =====
        if (ImGui::BeginTabItem("General")) {
        ImGui::BeginChild("grp_sim", ImVec2(0, 0), group_flags);
        {
            ImGui::SeparatorText("Simulator config");

            std::string fw = emu.firmware_name();
            ImGui::Text("Image : %s", fw.empty() ? "(none loaded)" : fw.c_str());
            if (!fw.empty())
                ImGui::Text("Flash : 0x%05X - 0x%05X (%u bytes)",
                    emu.fw_lowest(), emu.fw_highest(), emu.fw_highest() - emu.fw_lowest() + 1);

            ImVec4 col = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            if (st == EmuState::Running) col = ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
            else if (st == EmuState::Crashed) col = ImVec4(0.95f, 0.3f, 0.3f, 1.0f);
            else if (st == EmuState::Loaded || st == EmuState::Done) col = ImVec4(0.95f, 0.8f, 0.3f, 1.0f);
            ImGui::Text("State : "); ImGui::SameLine();
            ImGui::TextColored(col, "%s", emu.state_str());

            std::string sig = emu.bridge().signature();
            ImGui::Text("Signature : "); ImGui::SameLine();
            if (!sig.empty())
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%s", sig.c_str());
            else if (st == EmuState::Running)
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "querying...");
            else
                ImGui::TextDisabled("(start to query)");

            std::string err = emu.last_error();
            if (!err.empty()) ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.3f, 1.0f), "%s", err.c_str());

            if (!fw.empty())
                ImGui::Text("EEPROM : %s %s (%u B, %llu saves)", emu.eeprom_path(),
                    emu.eeprom_from_file() ? "[loaded]" : "[new]",
                    emu.eeprom_size(), (unsigned long long)emu.eeprom_saves());

            ImGui::SeparatorText("Bootloader");
            bool boot_on = emu.bootloader_enabled();
            if (ImGui::Checkbox("Boot into v3 bootloader (enable serial firmware updates)", &boot_on))
                emu.set_bootloader(boot_on);
            std::string bpath = emu.bootloader_path();
            if (bpath.empty()) {
                ImGui::TextDisabled("no bootloader loaded");
                ImGui::SameLine();
                if (ImGui::SmallButton("Open bootloader (.hex)...")) { browser.open("", "Open bootloader"); pick_mode = 1; }
            } else {
                size_t s = bpath.find_last_of("/\\");
                ImGui::TextDisabled("source: %s", bpath.substr(s == std::string::npos ? 0 : s + 1).c_str());
            }
            if (boot_on) {
                if (emu.bootloader_loaded()) {
                    ImGui::Text("Boot section : 0x%05X", emu.boot_start());
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "[loaded]");
                } else if (bpath.empty()) {
                    ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f), "[enable needs a bootloader hex]");
                } else if (!fw.empty()) {
                    ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.3f, 1.0f), "[load failed]");
                }
            }

            int ser = emu.serial();
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("ECU serial", &ser)) {
                if (ser < 0) ser = 0;
                if (ser > 999999) ser = 999999;
                emu.set_serial(ser);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("identity: %s", emu.identity().c_str());

            bool locked = emu.report_locked();
            if (ImGui::Checkbox("Report ECU locked", &locked))
                emu.set_report_locked(locked);
            ImGui::SameLine();
            int lb = emu.lock_byte();
            ImGui::SetNextItemWidth(70);
            if (ImGui::InputInt("lock byte (hex)", &lb, 0, 0,
                                ImGuiInputTextFlags_CharsHexadecimal)) {
                emu.set_lock_byte(lb);
            }

            auto ts = [](double s) -> std::string {
                if (s < 0.0) return std::string("never");
                char b[32]; snprintf(b, sizeof(b), "@ %.2f s", s); return std::string(b);
            };
            ImGui::Text("Activity :"); ImGui::SameLine();
            if (emu.bootloader_active())
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "bootloader active");
            else
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "app running / idle");
            ImGui::Text("Lock/fuse reads : %u  (%s)", emu.lock_reads(), ts(emu.last_lock_read_s()).c_str());
            ImGui::Text("Flash written   : %u erased / %u written  (%s)",
                        emu.page_erases(), emu.page_writes(), ts(emu.last_write_s()).c_str());
            ImGui::Text("Flash read-back  : %s", ts(emu.last_download_s()).c_str());

            ImGui::SeparatorText("Control");
            bool can_run = (st == EmuState::Loaded || st == EmuState::Done);
            bool is_running = (st == EmuState::Running);

            ImGui::BeginDisabled(!can_run);
            if (ImGui::Button("Start", ImVec2(90, 0))) emu.run();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!is_running);
            if (ImGui::Button("Stop", ImVec2(90, 0))) emu.stop();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(st == EmuState::Empty);
            if (ImGui::Button("Reset", ImVec2(90, 0))) emu.reset();
            ImGui::EndDisabled();

            ImGui::Text("Sim time : %.3f s", emu.sim_seconds());
            ImGui::Text("Cycles   : %llu", (unsigned long long)emu.cycles());

            ImGui::SeparatorText("Serial bridge");
            using Transport = UartBridge::Transport;
            int tr = (emu.bridge().transport() == Transport::Pty) ? 1 : 0;
            ImGui::TextUnformatted("Transport:"); ImGui::SameLine();
            if (ImGui::RadioButton("TCP", &tr, 0)) emu.bridge().set_transport(Transport::Tcp);
            ImGui::SameLine();
            if (ImGui::RadioButton("Virtual COM (pty)", &tr, 1)) emu.bridge().set_transport(Transport::Pty);

            ImGui::Text("TCP      : 127.0.0.1:%u", emu.bridge().port());
            ImGui::Text("Virtual COM : %s", emu.bridge().pty_path().empty()
                        ? "(opening...)" : emu.bridge().pty_path().c_str());
            ImGui::Text("Client   : "); ImGui::SameLine();
            if (emu.bridge().client_connected())
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%s",
                    tr == 1 ? "pty open" : "connected");
            else
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "waiting...");
            ImGui::Text("Rx (host->ecu): %llu bytes", (unsigned long long)emu.bridge().bytes_from_host());
            ImGui::Text("Tx (ecu->host): %llu bytes", (unsigned long long)emu.bridge().bytes_to_host());
            if (tr == 0)
                ImGui::TextDisabled("VemsTune: Communication -> TCP -> 127.0.0.1 : %u", emu.bridge().port());
            else
                ImGui::TextDisabled("Virtual COM device: %s", emu.bridge().pty_path().c_str());
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
        }

        // ===== Tab: Inputs =====
        if (ImGui::BeginTabItem("Inputs")) {
        // ----- Crank trigger: full width, on top -----
        ImGui::BeginChild("grp_trigger", ImVec2(0, 0), group_flags);
        {
            ImGui::SeparatorText("Crank trigger (60-2, PD4 / ICP1)");

            bool en = emu.trigger_enabled();
            if (ImGui::Checkbox("Enable", &en)) emu.set_trigger_enabled(en);
            ImGui::SameLine();
            ImGui::TextDisabled("%d teeth, %d missing  (LM1815 zero-cross, active-high pulses)",
                EmuCore::trigger_total_teeth(), EmuCore::trigger_missing_teeth());

            float rpm = emu.trigger_rpm();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##rpm", &rpm, 0.0f, 8000.0f, "%.0f RPM"))
                emu.set_trigger_rpm(rpm);

            const int total = EmuCore::trigger_total_teeth();
            const int present = total - EmuCore::trigger_missing_teeth();

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            float h = 90.0f;
            ImGui::Dummy(ImVec2(w, h));
            ImVec2 p1 = ImVec2(p0.x + w, p0.y + h);
            dl->AddRectFilled(p0, p1, IM_COL32(18, 18, 24, 255), 4.0f);
            dl->AddRect(p0, p1, IM_COL32(70, 70, 80, 255), 4.0f);

            const float pad = 10.0f;
            float hi = p0.y + pad;
            float lo = p1.y - pad;
            float x0 = p0.x + pad;
            float ww = w - 2 * pad;
            float sw = ww / total;

            // shade the missing-tooth gap
            for (int i = present; i < total; i++) {
                float gx = x0 + i * sw;
                dl->AddRectFilled(ImVec2(gx, hi), ImVec2(gx + sw, lo), IM_COL32(90, 40, 40, 110));
            }

            std::vector<ImVec2> pts;
            pts.reserve(total * 5 + 1);
            pts.push_back(ImVec2(x0, lo));
            for (int i = 0; i < total; i++) {
                float sx0 = x0 + i * sw, sxm = sx0 + sw * 0.5f, sx1 = sx0 + sw;
                if (i < present) {
                    pts.push_back(ImVec2(sx0, hi));
                    pts.push_back(ImVec2(sxm, hi));
                    pts.push_back(ImVec2(sxm, lo));
                    pts.push_back(ImVec2(sx1, lo));
                } else {
                    pts.push_back(ImVec2(sx1, lo));
                }
            }
            dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(90, 220, 130, 255), 0, 2.0f);

            if (emu.trigger_enabled() && emu.trigger_rpm() >= 1.0f) {
                float cx = x0 + (emu.trigger_slot() + 0.5f) * sw;
                dl->AddLine(ImVec2(cx, p0.y + 2), ImVec2(cx, p1.y - 2), IM_COL32(255, 210, 90, 220), 1.5f);
            }

            ImGui::Text("Slot %2d / %d    line %s", emu.trigger_slot(), total,
                emu.trigger_level() ? "HIGH" : "low");

            // ----- Cam (single pulse per 720 deg) -----
            ImGui::SeparatorText("Cam (single pulse, PE7 / ICP3)");
            bool cam_en = emu.cam_enabled();
            if (ImGui::Checkbox("Enable cam pulse", &cam_en)) emu.set_cam_enabled(cam_en);
            ImGui::SameLine();
            ImGui::TextDisabled("1 pulse / 720 deg (2 crank revs)");

            const int span = total * 2;   // show a full 720 deg cam cycle
            ImVec2 cp0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(w, h));
            ImVec2 cp1 = ImVec2(cp0.x + w, cp0.y + h);
            dl->AddRectFilled(cp0, cp1, IM_COL32(18, 18, 24, 255), 4.0f);
            dl->AddRect(cp0, cp1, IM_COL32(70, 70, 80, 255), 4.0f);

            float chi = cp0.y + pad;
            float clo = cp1.y - pad;
            float cx0 = cp0.x + pad;
            float cww = w - 2 * pad;
            float csw = cww / span;

            // mark the revolution boundary (360 deg)
            float midx = cx0 + total * csw;
            dl->AddLine(ImVec2(midx, cp0.y + 2), ImVec2(midx, cp1.y - 2), IM_COL32(70, 70, 90, 160), 1.0f);

            int cam_start = EmuCore::cam_pulse_start_slot();
            int cam_w = EmuCore::cam_pulse_slots();
            float px0 = cx0 + cam_start * csw;
            float px1 = cx0 + (cam_start + cam_w) * csw;
            ImVec2 cpts[6] = {
                {cx0, clo}, {px0, clo}, {px0, chi}, {px1, chi}, {px1, clo}, {cp1.x - pad, clo}
            };
            dl->AddPolyline(cpts, 6, IM_COL32(120, 180, 255, 255), 0, 2.0f);

            if (emu.trigger_enabled() && emu.trigger_rpm() >= 1.0f) {
                float pos = emu.trigger_rev() * (float)total + emu.trigger_slot();
                float ccx = cx0 + (pos + 0.5f) * csw;
                dl->AddLine(ImVec2(ccx, cp0.y + 2), ImVec2(ccx, cp1.y - 2), IM_COL32(255, 210, 90, 220), 1.5f);
            }

            ImGui::Text("Rev %d / 2    cam %s", emu.trigger_rev() + 1, emu.cam_level() ? "HIGH" : "low");
        }
        ImGui::EndChild();

        ImGui::Spacing();

        float in_half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        // ----- Analog inputs: left half -----
        ImGui::BeginChild("grp_analog", ImVec2(in_half, 0), group_flags);
        {
            ImGui::SeparatorText("Analog inputs (internal ADC)");

            auto adc_slider = [&](const char* label, int ch) {
                float v = emu.adc_mv(ch) / 1000.0f;
                ImGui::SetNextItemWidth(120);
                if (ImGui::SliderFloat(label, &v, 0.0f, 5.0f, "%.2f V"))
                    emu.set_adc_mv(ch, (int)(v * 1000.0f + 0.5f));
            };

            adc_slider("Vbat (ADC0)", 0);
            adc_slider("Nernst 1 (ADC1)", 1);
            adc_slider("TPS (ADC2)", 2);
            adc_slider("CLT (ADC3)", 3);
            adc_slider("IAT (ADC4)", 4);
            adc_slider("Int MAP (ADC5)", 5);
            adc_slider("Knock Vmid (ADC6)", 6);
            adc_slider("Nernst 2 (ADC7)", 7);
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // ----- External ADC (MCP3208): right half -----
        ImGui::BeginChild("grp_mcp", ImVec2(in_half, 0), group_flags);
        {
            ImGui::SeparatorText("External ADC (MCP3208, SPI - CS on PG4)");

            // CS activity indicator: lit while selected, flashes on each transaction.
            static uint64_t prev_txn = 0;
            static float cs_flash = 0.0f;
            uint64_t txn = emu.mcp().cs_transactions();
            if (txn != prev_txn) { cs_flash = 1.0f; prev_txn = txn; }
            cs_flash -= ImGui::GetIO().DeltaTime * 4.0f;
            if (cs_flash < 0.0f) cs_flash = 0.0f;
            bool cs = emu.mcp().cs_active();
            float lvl = cs ? 1.0f : cs_flash;

            ImDrawList* mdl = ImGui::GetWindowDrawList();
            ImVec2 cpos = ImGui::GetCursorScreenPos();
            float r = 7.0f;
            ImVec2 center = ImVec2(cpos.x + r, cpos.y + ImGui::GetTextLineHeight() * 0.5f);
            ImU32 base = IM_COL32(55, 55, 65, 255);
            ImU32 lit = IM_COL32((int)(40 + lvl * 40), (int)(120 + lvl * 135), (int)(60 + lvl * 40), 255);
            mdl->AddCircleFilled(center, r, lvl > 0.05f ? lit : base);
            if (lvl > 0.05f) mdl->AddCircle(center, r + 2.0f + lvl * 2.0f, lit, 0, 1.5f);
            ImGui::Dummy(ImVec2(r * 2 + 6, ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            ImGui::Text("CS %s    %llu txns, %llu conv", cs ? "ACTIVE" : "idle",
                (unsigned long long)emu.mcp().cs_transactions(),
                (unsigned long long)emu.mcp().conversions());

            ImGui::Spacing();
            for (int ch = 0; ch < 8; ch++) {
                int c = emu.mcp().counts(ch);
                char lbl[24];
                snprintf(lbl, sizeof(lbl), "Ch%d##mcp", ch);
                ImGui::SetNextItemWidth(110);
                if (ImGui::SliderInt(lbl, &c, 0, 4095, "%d cnt"))
                    emu.mcp().set_counts(ch, c);
                ImGui::SameLine();
                ImGui::TextDisabled("%.3f V", c / 4096.0f * 5.0f);
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();

        // ----- Knock processor (HIP9011): full width below the ADCs -----
        ImGui::BeginChild("grp_hip", ImVec2(0, 0), group_flags);
        {
            ImGui::SeparatorText("Knock processor (HIP9011, SPI - CS on PC2, INT/HOLD on PC3)");

            ImDrawList* hdl = ImGui::GetWindowDrawList();
            auto led = [&](float lvl, const char* text, ImU32 oncol) {
                ImVec2 pp = ImGui::GetCursorScreenPos();
                float rr = 7.0f;
                ImVec2 cc = ImVec2(pp.x + rr, pp.y + ImGui::GetTextLineHeight() * 0.5f);
                hdl->AddCircleFilled(cc, rr, lvl > 0.05f ? oncol : IM_COL32(55, 55, 65, 255));
                if (lvl > 0.05f) hdl->AddCircle(cc, rr + 2.0f + lvl * 2.0f, oncol, 0, 1.5f);
                ImGui::Dummy(ImVec2(rr * 2 + 6, ImGui::GetTextLineHeight()));
                ImGui::SameLine();
                ImGui::TextUnformatted(text);
            };

            // CS indicator: lit while selected, flashes on each word.
            static uint64_t hprev = 0; static float hflash = 0.0f;
            uint64_t htxn = emu.hip().cs_transactions();
            if (htxn != hprev) { hflash = 1.0f; hprev = htxn; }
            hflash -= ImGui::GetIO().DeltaTime * 4.0f; if (hflash < 0.0f) hflash = 0.0f;
            bool hcs = emu.hip().cs_active();
            char csbuf[64];
            snprintf(csbuf, sizeof(csbuf), "CS %s  (%llu txns)", hcs ? "ACTIVE" : "idle",
                (unsigned long long)htxn);
            led(hcs ? 1.0f : hflash, csbuf, IM_COL32(80, 220, 130, 255));

            ImGui::SameLine(0, 30);

            // INT/HOLD indicator: blue = integrate, amber = hold; dark until driven.
            static uint64_t iprev = 0; static float iflash = 0.0f;
            uint64_t iedg = emu.hip().inthold_edges();
            if (iedg != iprev) { iflash = 1.0f; iprev = iedg; }
            iflash -= ImGui::GetIO().DeltaTime * 4.0f; if (iflash < 0.0f) iflash = 0.0f;
            bool ih = emu.hip().inthold_high();
            char ihbuf[64];
            snprintf(ihbuf, sizeof(ihbuf), "INT/HOLD %s", iedg ? (ih ? "INTEGRATE" : "HOLD") : "not driven");
            float ilvl = iedg ? (ih ? 1.0f : 0.4f + iflash * 0.6f) : 0.0f;
            led(ilvl, ihbuf, ih ? IM_COL32(80, 200, 255, 255) : IM_COL32(230, 180, 60, 255));

            ImGui::Spacing();

            if (!emu.hip().written()) {
                ImGui::TextDisabled("No SPI config words received yet.");
            } else {
                int bp = emu.hip().bandpass(), g = emu.hip().gain(), tc = emu.hip().timeconst();
                int pr = emu.hip().prescaler(), prm = Hip9011::prescaler_mhz(pr);
                ImGui::Text("Channel        : %d", emu.hip().channel());
                ImGui::Text("Bandpass       : %2d  ->  %.2f kHz", bp, Hip9011::bpf_khz(bp));
                ImGui::Text("Gain           : %2d  ->  %.3fx", g, Hip9011::gain_x(g));
                ImGui::Text("Time constant  : %2d  ->  %d us", tc, Hip9011::tc_us(tc));
                if (prm > 0) ImGui::Text("Prescaler      : %2d  ->  %d MHz clock", pr, prm);
                else         ImGui::Text("Prescaler      : %2d  ->  (reserved)", pr);
                ImGui::Text("SO terminal    : %s", emu.hip().so_hiz() ? "high-Z" : "active");
                ImGui::Text("Words received : %llu    last byte 0x%02X",
                    (unsigned long long)emu.hip().words(), emu.hip().last_byte() & 0xFF);
            }
            ImGui::TextDisabled("INTOUT (knock level) is analog -> drive it with the ADC6 (Knock Vmid) slider.");
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
        }

        // ===== Tab: Outputs =====
        if (ImGui::BeginTabItem("Outputs")) {
        Outputs& out = emu.outputs();

        auto out_led = [&](float lvl, ImU32 oncol) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 pp = ImGui::GetCursorScreenPos();
            float rr = 7.0f;
            ImVec2 cc = ImVec2(pp.x + rr, pp.y + ImGui::GetTextLineHeight() * 0.5f);
            dl->AddCircleFilled(cc, rr, lvl > 0.05f ? oncol : IM_COL32(55, 55, 65, 255));
            if (lvl > 0.05f) dl->AddCircle(cc, rr + 2.0f + lvl * 2.0f, oncol, 0, 1.5f);
            ImGui::Dummy(ImVec2(rr * 2 + 6, ImGui::GetTextLineHeight()));
            ImGui::SameLine();
        };

        float out_avail = ImGui::GetContentRegionAvail().x;
        float out_gap = ImGui::GetStyle().ItemSpacing.x;
        float out_col_l = (out_avail - out_gap) * 0.66f;
        float out_col_r = out_avail - out_gap - out_col_l;

        static std::vector<uint64_t> prev_n;
        static std::vector<float> flash;
        if ((int)prev_n.size() < out.count()) { prev_n.resize(out.count(), 0); flash.resize(out.count(), 0.0f); }

        auto out_row = [&](int c) {
            bool act = out.active(c);
            uint64_t n = out.pulses(c);
            if (n != prev_n[c]) { flash[c] = 1.0f; prev_n[c] = n; }
            flash[c] -= ImGui::GetIO().DeltaTime * 4.0f;
            if (flash[c] < 0.0f) flash[c] = 0.0f;

            out_led(act ? 1.0f : flash[c], IM_COL32(90, 220, 130, 255));
            ImGui::Text("%-6s", out.name(c));
            ImGui::SameLine(110);
            ImGui::Text("%6.2f ms", out.pulse_ms(c));
            ImGui::SameLine(210);
            if (out.freq_hz(c) > 0.01f)
                ImGui::Text("%6.1f Hz  %5.1f%% duty", out.freq_hz(c), out.duty(c));
            else
                ImGui::TextDisabled("    -- Hz");
            ImGui::SameLine(420);
            ImGui::TextDisabled("%llu pulses", (unsigned long long)out.pulses(c));
        };

        // One output-list box. width <= 0 => full content width (injectors row).
        auto list_box = [&](const char* group, const char* sub, float width) {
            ImGui::BeginChild((std::string(group) + "##list").c_str(),
                ImVec2(width, 0), group_flags);
            char hdr[96];
            snprintf(hdr, sizeof(hdr), "%s  (%s)", group, sub);
            ImGui::SeparatorText(hdr);
            for (int c = 0; c < out.count(); c++)
                if (std::string(out.group(c)) == group) out_row(c);
            ImGui::EndChild();
        };

        // One 74x259 latch control box, paired to the right of its output list.
        static bool   q_prev[3][8]  = {};
        static float  q_flash[3][8] = {};
        auto latch_box = [&](int slot, const char* title, Hc259& mux, const char* const qn[8]) {
            ImGui::BeginChild(title, ImVec2(out_col_r, 0), group_flags);
            ImGui::SeparatorText(title);
            ImGui::Text("D  (PC7)   : %s", mux.d() ? "HIGH" : "low");
            ImGui::Text("Select     : S2 S1 S0 = %d %d %d  ->  Q%d (%s)",
                mux.s2() ? 1 : 0, mux.s1() ? 1 : 0, mux.s0() ? 1 : 0, mux.addr(), qn[mux.addr()]);
            ImGui::Text("G  (P%c%d)   : %s", mux.g_port(), mux.g_pin(),
                mux.enabled() ? "ENABLED (low)" : "hold (high)");
            ImGui::Text("CLR        : tied VCC (high, no clear)");
            ImGui::Spacing();
            for (int n = 0; n < 8; n++) {
                bool on = mux.q(n);
                if (on != q_prev[slot][n]) { q_flash[slot][n] = 1.0f; q_prev[slot][n] = on; }
                q_flash[slot][n] -= ImGui::GetIO().DeltaTime * 4.0f;
                if (q_flash[slot][n] < 0.0f) q_flash[slot][n] = 0.0f;
                out_led(on ? 1.0f : q_flash[slot][n], IM_COL32(90, 220, 130, 255));
                ImGui::Text("Q%d %-8s", n, qn[n]);
                if (n % 2 == 0) ImGui::SameLine(0, 24);
            }
            ImGui::EndChild();
        };

        static const char* const ign_q[8]  = { "IGN0","IGN1","IGN2","IGN3","IGN4","IGN5","IGN6","IGN7" };
        static const char* const tpic_q[8] = { "DRIVE8","DRIVE9","CL","MISC","IDL","FP","DRIVE10","DRIVE11" };
        static const char* const aux_q[8]  = { "LCD_RS","S259_1","S259_2","STEPA","STEPB","STEPC","STEPD","EN_AB_CD" };

        // Each latched group sits on one row next to its 74x259 control box.
        list_box("Ignition", "74HC259 Q0-Q7, active-high (dwell)", out_col_l);
        ImGui::SameLine();
        latch_box(0, "74HC259 ignition latch (control pins)", emu.ignmux(), ign_q);

        list_box("Power outputs", "TPIC6A259 Q0-Q7, active-high", out_col_l);
        ImGui::SameLine();
        latch_box(1, "TPIC6A259DW power latch (control pins)", emu.outmux(), tpic_q);

        list_box("Aux outputs", "74HC259 Q0-Q7, active-high (LCD/stepper)", out_col_l);
        ImGui::SameLine();
        latch_box(2, "74HC259 LCD/stepper latch (control pins)", emu.auxmux(), aux_q);

        // Injectors are direct on PA0-PA7 (no latch) -> full-width row at the bottom.
        list_box("Injectors", "PA0-PA7, active-low", 0.0f);

        ImGui::EndTabItem();
        }

        // ===== Tab: Scope (channel activity vs 720 deg crank angle) =====
        if (ImGui::BeginTabItem("Scope")) {
        Outputs& sout = emu.outputs();
        int nch = sout.count();

        static int scope_mode = 0;                 // 0 = continuous, 1 = one-shot
        static std::vector<char> scope_on;
        static std::vector<std::vector<std::pair<float, float>>> frozen;
        static bool  has_frozen = false;
        static float frozen_rpm = 0.0f;
        static int   frozen_cycle = 0;
        if ((int)scope_on.size() != nch) scope_on.assign(nch, 1);

        auto group_color = [](const char* g) -> ImU32 {
            std::string s = g;
            if (s == "Injectors")     return IM_COL32( 90, 200, 255, 255);
            if (s == "Ignition")      return IM_COL32(255, 170,  60, 255);
            if (s == "Power outputs") return IM_COL32(120, 230, 140, 255);
            if (s == "Aux outputs")   return IM_COL32(200, 140, 255, 255);
            if (s == "Knock")         return IM_COL32(255,  90,  90, 255);
            return IM_COL32(180, 180, 190, 255);
        };

        // Each output edge carries the crank angle it occurred at (Outputs::Edge.deg).
        static std::vector<Outputs::Edge> es;
        // cap bounds an output still-on at the window end (live cursor in the current cycle).
        auto intervals = [&](int c, double W0, double W1, double cap, std::vector<std::pair<float, float>>& iv) {
            iv.clear();
            sout.edges(c, es);
            bool ah = sout.active_high(c);
            bool active = false; double t_on = 0.0;
            for (const Outputs::Edge& e : es) {
                bool na = (e.level == ah);
                if (na == active) continue;
                double d = e.deg;
                if (na) { active = true; t_on = d; }
                else { active = false; double a = std::max(t_on, W0), b = std::min(d, W1);
                       if (b > a) iv.push_back({ (float)(a - W0), (float)(b - W0) }); }
            }
            if (active) { double a = std::max(t_on, W0), b = std::min(W1, cap);
                          if (b > a) iv.push_back({ (float)(a - W0), (float)(b - W0) }); }
        };

        // ----- controls -----
        ImGui::RadioButton("Continuous", &scope_mode, 0); ImGui::SameLine();
        ImGui::RadioButton("One-shot 720 deg", &scope_mode, 1); ImGui::SameLine();
        if (scope_mode == 1) {
            if (ImGui::Button("Capture cycle")) {
                double cur = emu.crank_total_deg();
                int cyc = (int)std::floor(cur / 720.0) - 1;     // last *completed* cycle
                if (cyc < 0) cyc = 0;
                double W0 = cyc * 720.0, W1 = W0 + 720.0;
                frozen.assign(nch, {});
                for (int c = 0; c < nch; c++) intervals(c, W0, W1, W1, frozen[c]);
                frozen_rpm = emu.trigger_rpm();
                frozen_cycle = cyc;
                has_frozen = true;
            }
            ImGui::SameLine();
        }
        double cur_deg = emu.crank_total_deg();
        if (scope_mode == 1 && has_frozen)
            ImGui::Text("  frozen: cycle %d @ %.0f RPM", frozen_cycle, frozen_rpm);
        else
            ImGui::Text("  %.0f RPM   phase %.0f deg", emu.trigger_rpm(), std::fmod(cur_deg, 720.0));

        if (ImGui::SmallButton("All"))  for (auto& v : scope_on) v = 1;
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) for (auto& v : scope_on) v = 0;
        ImGui::SameLine();
        ImGui::TextDisabled("toggle channels:");

        // grouped channel toggles
        std::string ctgrp;
        int shown = 0;
        for (int c = 0; c < nch; c++) {
            std::string g = sout.group(c);
            if (g != ctgrp) { ctgrp = g; ImGui::NewLine(); ImGui::TextDisabled("%-13s", g.c_str()); ImGui::SameLine(); shown = 0; }
            else if (shown > 0 && (shown % 8) == 0) { ImGui::NewLine(); ImGui::Dummy(ImVec2(96, 0)); ImGui::SameLine(); }
            bool on = scope_on[c] != 0;
            ImGui::PushID(c);
            if (ImGui::Checkbox(sout.name(c), &on)) scope_on[c] = on ? 1 : 0;
            ImGui::PopID();
            ImGui::SameLine();
            shown++;
        }
        ImGui::NewLine();
        ImGui::Separator();

        // ----- the plot -----
        ImGui::BeginChild("scope_plot", ImVec2(0, 0), ImGuiChildFlags_Borders);
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 org = ImGui::GetCursorScreenPos();
            float availw = ImGui::GetContentRegionAvail().x;
            float gutter = 96.0f;
            float x0 = org.x + gutter;
            float x1 = org.x + availw - 8.0f;
            float plotw = x1 - x0 > 10.0f ? x1 - x0 : 10.0f;
            float top = org.y + 20.0f;
            float lane_h = 20.0f;
            auto deg2x = [&](float d) { return x0 + d / 720.0f * plotw; };

            std::vector<int> lanes;
            for (int c = 0; c < nch; c++) if (scope_on[c]) lanes.push_back(c);
            float bottom = top + lanes.size() * lane_h;

            // grid: minor every 30 deg, major + label every 90 deg
            for (int d = 0; d <= 720; d += 30) {
                bool major = (d % 90) == 0;
                float x = deg2x((float)d);
                dl->AddLine(ImVec2(x, top), ImVec2(x, bottom),
                    major ? IM_COL32(70, 70, 85, 255) : IM_COL32(40, 40, 50, 255));
                if (major) {
                    char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", d);
                    dl->AddText(ImVec2(x + 2, org.y + 4), IM_COL32(140, 140, 155, 255), lbl);
                }
            }
            dl->AddText(ImVec2(x1 - 28, org.y + 4), IM_COL32(140, 140, 155, 255), "deg");

            // current crank cursor (continuous mode only)
            double cyc0 = std::floor(cur_deg / 720.0) * 720.0;
            if (scope_mode == 0 && emu.trigger_enabled()) {
                float cx = deg2x((float)(cur_deg - cyc0));
                dl->AddLine(ImVec2(cx, top), ImVec2(cx, bottom), IM_COL32(255, 80, 80, 200), 1.5f);
            }

            std::vector<std::pair<float, float>> iv, ghost;
            for (size_t li = 0; li < lanes.size(); li++) {
                int c = lanes[li];
                float y = top + li * lane_h;
                ImU32 col = group_color(sout.group(c));
                dl->AddText(ImVec2(org.x + 4, y + 3), col, sout.name(c));
                dl->AddLine(ImVec2(x0, y + lane_h - 2), ImVec2(x1, y + lane_h - 2), IM_COL32(45, 45, 55, 255));

                if (scope_mode == 1) {
                    if (has_frozen && c < (int)frozen.size())
                        for (auto& s : frozen[c])
                            dl->AddRectFilled(ImVec2(deg2x(s.first), y + 2), ImVec2(deg2x(s.second), y + lane_h - 3), col);
                } else {
                    intervals(c, cyc0 - 720.0, cyc0, cyc0, ghost);          // previous cycle, faded
                    ImU32 gcol = (col & 0x00FFFFFF) | 0x40000000;
                    for (auto& s : ghost)
                        dl->AddRectFilled(ImVec2(deg2x(s.first), y + 2), ImVec2(deg2x(s.second), y + lane_h - 3), gcol);
                    intervals(c, cyc0, cyc0 + 720.0, cur_deg, iv);          // current cycle, bright to cursor
                    for (auto& s : iv)
                        dl->AddRectFilled(ImVec2(deg2x(s.first), y + 2), ImVec2(deg2x(s.second), y + lane_h - 3), col);
                }
            }

            if (lanes.empty())
                dl->AddText(ImVec2(x0, top + 8), IM_COL32(150, 150, 160, 255),
                    "No channels selected. Enable channels above.");
            else if (!emu.trigger_enabled() && scope_mode == 0)
                dl->AddText(ImVec2(x0, bottom + 26), IM_COL32(150, 150, 160, 255),
                    "Start the engine (Inputs tab: enable crank trigger + set RPM) to see activity.");

            // ----- hover cursor + two click markers (A/B) for measuring -----
            float rpm = (scope_mode == 1 && has_frozen) ? frozen_rpm : (float)emu.trigger_rpm();
            auto deg_to_ms = [&](float d) { return rpm > 0.1f ? d / (6.0f * rpm) * 1000.0f : 0.0f; };
            auto x_to_deg = [&](float x) { float d = (x - x0) / plotw * 720.0f; return d < 0 ? 0.0f : (d > 720 ? 720.0f : d); };

            static bool  mark_set[2] = { false, false };
            static float mark_deg[2] = { 0.0f, 0.0f };
            ImVec2 mp = ImGui::GetIO().MousePos;
            bool hovering = ImGui::IsWindowHovered() && mp.x >= x0 && mp.x <= x1 && mp.y >= top && mp.y <= bottom;

            if (hovering && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                float d = x_to_deg(mp.x);
                if (!mark_set[0] || (mark_set[0] && mark_set[1])) { mark_deg[0] = d; mark_set[0] = true; mark_set[1] = false; }
                else { mark_deg[1] = d; mark_set[1] = true; }
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { mark_set[0] = mark_set[1] = false; }

            const ImU32 colA = IM_COL32(80, 220, 255, 255), colB = IM_COL32(255, 220, 80, 255);
            for (int m = 0; m < 2; m++) {
                if (!mark_set[m]) continue;
                float mx = deg2x(mark_deg[m]);
                ImU32 mc = m == 0 ? colA : colB;
                dl->AddLine(ImVec2(mx, top), ImVec2(mx, bottom), mc, 1.5f);
                char tag[40]; snprintf(tag, sizeof(tag), "%c %.1f deg / %.2f ms", m == 0 ? 'A' : 'B', mark_deg[m], deg_to_ms(mark_deg[m]));
                dl->AddText(ImVec2(mx + 3, top + 1 + m * 14), mc, tag);
            }

            if (hovering) {
                float hd = x_to_deg(mp.x);
                dl->AddLine(ImVec2(deg2x(hd), top), ImVec2(deg2x(hd), bottom), IM_COL32(230, 230, 120, 150));
                ImGui::SetTooltip("%.1f deg\n%.3f ms%s", hd, deg_to_ms(hd),
                    rpm > 0.1f ? "" : "\n(set RPM for time)");
            }

            char rd[200];
            if (mark_set[0] && mark_set[1]) {
                float dd = mark_deg[1] - mark_deg[0];
                snprintf(rd, sizeof(rd), "A %.1f deg (%.2f ms)   B %.1f deg (%.2f ms)   B-A: %.1f deg / %.2f ms   [right-click clears]",
                    mark_deg[0], deg_to_ms(mark_deg[0]), mark_deg[1], deg_to_ms(mark_deg[1]), dd, deg_to_ms(dd));
            } else if (mark_set[0]) {
                snprintf(rd, sizeof(rd), "A %.1f deg (%.2f ms)   click to drop B   [right-click clears]",
                    mark_deg[0], deg_to_ms(mark_deg[0]));
            } else {
                snprintf(rd, sizeof(rd), "click the plot to drop measure markers A then B  (%.0f RPM)", rpm);
            }
            dl->AddText(ImVec2(x0, bottom + 8), IM_COL32(200, 200, 210, 255), rd);

            ImGui::Dummy(ImVec2(availw, (top - org.y) + lanes.size() * lane_h + 48.0f));
        }
        ImGui::EndChild();

        ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
        }

        ImGui::End();

        ImGui::Render();
        ImGuiIO& io = ImGui::GetIO();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
