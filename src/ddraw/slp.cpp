#include "slp.h"
#include <string.h>

namespace swgbm {

static uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int16_t S16(const uint8_t* p) { int16_t v; memcpy(&v, p, 2); return v; }

bool SlpDecode(const uint8_t* d, size_t size, std::vector<SlpFrame>& out) {
    if (size < 32) return false;
    int n = (int)U32(d + 4);
    if (n <= 0 || n > 4096 || 32 + (size_t)n * 32 > size) return false;
    out.resize(n);
    for (int k = 0; k < n; k++) {
        const uint8_t* fi = d + 32 + k * 32;
        uint32_t cmdo = U32(fi), outo = U32(fi + 4);
        SlpFrame& f = out[k];
        f.w = (int)U32(fi + 16); f.h = (int)U32(fi + 20);
        f.hx = (int)U32(fi + 24); f.hy = (int)U32(fi + 28);
        if (f.w <= 0 || f.h <= 0 || f.w > 8192 || f.h > 8192) return false;
        f.px.assign((size_t)f.w * f.h, -1);
        for (int y = 0; y < f.h; y++) {
            if (outo + y * 4 + 4 > size || cmdo + y * 4 + 4 > size) return false;
            int16_t l = S16(d + outo + y * 4), r = S16(d + outo + y * 4 + 2);
            if (l == -32768 || r == -32768) continue;
            size_t p = U32(d + cmdo + y * 4);
            int x = l;
            auto put = [&](int v) { if (x >= 0 && x < f.w) f.at(x, y) = (int16_t)v; x++; };
            while (p < size) {
                uint8_t c = d[p++];
                if (c == 0x0F) break;
                int lo2 = c & 3, lo4 = c & 0xF;
                if (lo2 == 0) { int cnt = c >> 2; for (int i = 0; i < cnt; i++) put(d[p++]); }
                else if (lo2 == 1) { x += c >> 2; }
                else if (lo4 == 2) { int cnt = ((c & 0xF0) << 4) + d[p++]; for (int i = 0; i < cnt; i++) put(d[p++]); }
                else if (lo4 == 3) { x += ((c & 0xF0) << 4) + d[p++]; }
                else if (lo4 == 6 || lo4 == 7 || lo4 == 0xA || lo4 == 0xB) {
                    int cnt = c >> 4;
                    if (!cnt) cnt = d[p++];
                    if (lo4 == 6) for (int i = 0; i < cnt; i++) put(d[p++] + 16);
                    else if (lo4 == 7) { int v = d[p++]; for (int i = 0; i < cnt; i++) put(v); }
                    else if (lo4 == 0xA) { int v = d[p++] + 16; for (int i = 0; i < cnt; i++) put(v); }
                    else x += cnt; // shadow: treat as transparent
                } else if (lo4 == 0xE) {
                    if (c == 0x4E || c == 0x6E) x++;
                    else if (c == 0x5E || c == 0x7E) x += d[p++];
                } else {
                    return false;
                }
            }
        }
    }
    return true;
}

std::string SlpEncode(const std::vector<SlpFrame>& frames) {
    std::string hdr(32, '\0');
    memcpy(&hdr[0], "2.0N", 4);
    uint32_t n = (uint32_t)frames.size();
    memcpy(&hdr[4], &n, 4);
    memcpy(&hdr[8], "SWGB Modern", 11);
    std::string info(32 * n, '\0');
    std::string body;
    size_t base = hdr.size() + info.size();
    for (uint32_t k = 0; k < n; k++) {
        const SlpFrame& f = frames[k];
        std::string outline, cmdTable(4 * f.h, '\0'), cmds;
        for (int y = 0; y < f.h; y++) {
            const int16_t* row = f.px.data() + (size_t)y * f.w;
            int l = 0, r = f.w - 1;
            while (l < f.w && row[l] < 0) l++;
            while (r >= l && row[r] < 0) r--;
            int16_t ol, orr;
            if (l >= f.w) { ol = orr = -32768; }
            else { ol = (int16_t)l; orr = (int16_t)(f.w - 1 - r); }
            outline.append((const char*)&ol, 2);
            outline.append((const char*)&orr, 2);
            uint32_t rowOff = (uint32_t)cmds.size(); // relative, fixed up below
            memcpy(&cmdTable[y * 4], &rowOff, 4);
            if (l < f.w) {
                int x = l;
                while (x <= r) {
                    int s = x;
                    if (row[x] < 0) {
                        while (x <= r && row[x] < 0) x++;
                        int cnt = x - s;
                        while (cnt > 0) {
                            int c = cnt > 4095 ? 4095 : cnt;
                            if (c < 64) cmds += (char)((c << 2) | 1);
                            else { cmds += (char)(((c >> 8) << 4) | 3); cmds += (char)(c & 0xFF); }
                            cnt -= c;
                        }
                    } else {
                        while (x <= r && row[x] >= 0) x++;
                        int pos = s, cnt = x - s;
                        while (cnt > 0) {
                            int c = cnt > 4095 ? 4095 : cnt;
                            if (c < 64) cmds += (char)(c << 2);
                            else { cmds += (char)(((c >> 8) << 4) | 2); cmds += (char)(c & 0xFF); }
                            for (int i = 0; i < c; i++) cmds += (char)(uint8_t)row[pos + i];
                            pos += c; cnt -= c;
                        }
                    }
                }
            }
            cmds += (char)0x0F;
        }
        uint32_t outlineOff = (uint32_t)(base + body.size());
        uint32_t cmdTableOff = outlineOff + (uint32_t)outline.size();
        uint32_t cmdsOff = cmdTableOff + (uint32_t)cmdTable.size();
        for (int y = 0; y < f.h; y++) {
            uint32_t v;
            memcpy(&v, &cmdTable[y * 4], 4);
            v += cmdsOff;
            memcpy(&cmdTable[y * 4], &v, 4);
        }
        uint32_t fi[8] = {cmdTableOff, outlineOff, 0, 0, (uint32_t)f.w, (uint32_t)f.h, (uint32_t)f.hx, (uint32_t)f.hy};
        memcpy(&info[k * 32], fi, 32);
        body += outline + cmdTable + cmds;
    }
    return hdr + info + body;
}

} // namespace swgbm
