#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct FirmwareImage {
    std::vector<uint8_t> flash;
    uint32_t lowest = 0xFFFFFFFF;
    uint32_t highest = 0;
    std::string error;
    bool ok = false;
};

static inline int hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline bool hex_byte(const char* s, uint8_t& out) {
    int hi = hex_nib(s[0]), lo = hex_nib(s[1]);
    if (hi < 0 || lo < 0) return false;
    out = (uint8_t)((hi << 4) | lo);
    return true;
}

static inline FirmwareImage parse_intel_hex(FILE* f, size_t cap) {
    FirmwareImage img;
    img.flash.assign(cap, 0xFF);
    uint32_t ext_base = 0;
    char line[1024];
    int lineno = 0;
    bool saw_eof = false;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char* p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == 0) continue;
        if (*p != ':') { img.error = "line " + std::to_string(lineno) + ": missing ':'"; return img; }
        p++;
        uint8_t count, ah, al, type;
        if (!hex_byte(p, count) || !hex_byte(p + 2, ah) || !hex_byte(p + 4, al) || !hex_byte(p + 6, type)) {
            img.error = "line " + std::to_string(lineno) + ": bad header"; return img;
        }
        uint16_t addr = (uint16_t)((ah << 8) | al);
        const char* dp = p + 8;
        uint8_t sum = count + ah + al + type;
        std::vector<uint8_t> data(count);
        for (int i = 0; i < count; i++) {
            if (!hex_byte(dp + i * 2, data[i])) { img.error = "line " + std::to_string(lineno) + ": bad data"; return img; }
            sum += data[i];
        }
        uint8_t cksum;
        if (!hex_byte(dp + count * 2, cksum)) { img.error = "line " + std::to_string(lineno) + ": bad checksum"; return img; }
        if ((uint8_t)(sum + cksum) != 0) { img.error = "line " + std::to_string(lineno) + ": checksum mismatch"; return img; }

        switch (type) {
            case 0x00: {
                uint32_t abs = ext_base + addr;
                for (int i = 0; i < count; i++) {
                    uint32_t a = abs + i;
                    if (a >= cap) continue;
                    img.flash[a] = data[i];
                    if (a < img.lowest) img.lowest = a;
                    if (a > img.highest) img.highest = a;
                }
                break;
            }
            case 0x01: saw_eof = true; break;
            case 0x02: ext_base = ((uint32_t)data[0] << 8 | data[1]) << 4; break;
            case 0x04: ext_base = ((uint32_t)data[0] << 8 | data[1]) << 16; break;
            case 0x03: // start segment address — ignored (this is what breaks simavr's loader)
            case 0x05: // start linear address — ignored
                break;
            default: break;
        }
        if (saw_eof) break;
    }
    if (img.highest == 0 && img.lowest == 0xFFFFFFFF) { img.error = "no data records"; return img; }
    img.ok = true;
    return img;
}

// VEMS v3 bootloader layout (offsets only — the bootloader binary itself is NOT
// bundled; load it at runtime via File -> Open bootloader).
static const uint32_t kBootOverlayShift   = 0x10000;   // 16-bit boot hex -> atmega128 boot section
static const uint32_t kBootIdentityOffset = 0x1FFF0;   // 12-byte identity string ("v3.3_nNNNNNN")
static const uint32_t kBootIdentityLen    = 12;

// Overlays an Intel HEX (e.g. the v3 bootloader) into an existing flash buffer,
// shifting every record by base_off. The VEMS boot hex carries 16-bit addresses
// (0xF7FE..0xFFFF) with no extended-address record, so base_off=0x10000 places it
// at the real atmega128 boot section (0x1F800), matching the reset vector's jmp.
static inline bool overlay_intel_hex(const std::string& path, std::vector<uint8_t>& flash,
                                     uint32_t base_off, std::string& err,
                                     uint32_t& lo_out, uint32_t& hi_out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = std::string("cannot open: ") + strerror(errno); return false; }
    FirmwareImage tmp = parse_intel_hex(f, flash.size());
    fclose(f);
    if (!tmp.ok) { err = tmp.error; return false; }

    uint32_t lo = base_off + tmp.lowest, hi = base_off + tmp.highest;
    if (hi >= flash.size()) { err = "bootloader does not fit in flash"; return false; }
    for (uint32_t a = tmp.lowest; a <= tmp.highest; a++)
        flash[base_off + a] = tmp.flash[a];
    lo_out = lo; hi_out = hi;
    return true;
}

static inline FirmwareImage load_firmware(const std::string& path, size_t cap) {
    FirmwareImage img;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { img.error = std::string("cannot open: ") + strerror(errno); return img; }

    int c = fgetc(f);
    bool is_hex = (c == ':');
    rewind(f);

    if (is_hex) {
        img = parse_intel_hex(f, cap);
        fclose(f);
        return img;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); img.error = "empty file"; return img; }
    size_t n = (size_t)sz > cap ? cap : (size_t)sz;
    img.flash.assign(cap, 0xFF);
    size_t r = fread(img.flash.data(), 1, n, f);
    fclose(f);
    if (r != n) { img.error = "short read"; return img; }
    img.lowest = 0;
    img.highest = (uint32_t)n - 1;
    img.ok = true;
    return img;
}
