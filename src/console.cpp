#include "console.h"
#include "util.h"
#include <vector>

namespace ag {

// ---------------------------------------------------------------- character mapping

static UINT g_cp = 0;
static unsigned char g_known[65536 / 8], g_ok[65536 / 8];

// Can the console show this BMP character? ASCII and Latin-1 always; others only if the console's
// output code page has it (no best-fit guesses).
static bool showable(unsigned c) {
    if ((c >= 0x20 && c < 0x7F) || (c >= 0xA1 && c <= 0xFF)) return true;
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0xA0) || c > 0xFFFF) return false;
    if (g_cp == 0) g_cp = GetConsoleOutputCP();
    if (g_cp == 0 || g_cp == 65001 || g_cp == 65000) return false;
    unsigned i = c >> 3, bit = 1u << (c & 7);
    if (!(g_known[i] & bit)) {
        g_known[i] |= bit;
        wchar_t w = (wchar_t)c;
        char buf[8];
        BOOL used = FALSE;
        int n = WideCharToMultiByte(g_cp, WC_NO_BEST_FIT_CHARS, &w, 1, buf, sizeof buf, nullptr, &used);
        if (n == 1 && !used) g_ok[i] |= bit;
    }
    return (g_ok[i] & bit) != 0;
}

static bool is_pictograph(unsigned c) {
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2B00 && c <= 0x2BFF) ||
           c == 0x231A || c == 0x231B || c == 0x23F0 || c == 0x23F3 || (c >= 0x23E9 && c <= 0x23FA);
}

// Look-alike for a character the font can't show; nullptr = no special mapping.
static const char* lookalike(unsigned c) {
    switch (c) {
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x2035: case 0x02BC: case 0x02B9:
        return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: case 0x2036: case 0x02BA:
        return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: case 0x2043:
    case 0x2E3A: case 0x2E3B: case 0xFE58: case 0xFE63: case 0xFF0D:
        return "-";
    case 0x2026: return "...";
    case 0x2022: case 0x2023: case 0x2027: case 0x25CF: case 0x25E6: case 0x25AA: case 0x25AB: case 0x25B8:
    case 0x25BA: case 0x25B6: case 0x25C6: case 0x25C7: case 0x25CB: case 0x25A1: case 0x2605: case 0x2606:
    case 0x2756: case 0x2219: case 0x22C5:
        return "*";
    case 0x2190: case 0x27F5: return "<-";
    case 0x2192: case 0x27F6: case 0x2794: case 0x279C: case 0x27A1: case 0x21A6: case 0x21AA: case 0x2B95: return "->";
    case 0x2191: return "^";
    case 0x2193: return "v";
    case 0x2194: case 0x27F7: return "<->";
    case 0x21D2: case 0x27F9: return "=>";
    case 0x21D0: case 0x27F8: return "<=";
    case 0x21D4: case 0x27FA: return "<=>";
    case 0x2713: case 0x2714: case 0x2705: case 0x2611: return "v";
    case 0x2717: case 0x2718: case 0x274C: case 0x2612: case 0x2715: case 0x2716: return "x";
    case 0x2260: return "!=";
    case 0x2122: return "TM";
    case 0x20AC: return "EUR";
    case 0x2039: return "<";
    case 0x203A: return ">";
    case 0x00A0: case 0x2002: case 0x2003: case 0x2004: case 0x2005: case 0x2006: case 0x2007: case 0x2008:
    case 0x2009: case 0x200A: case 0x202F: case 0x205F: case 0x3000:
        return " ";
    case 0xFF0C: return ",";
    case 0xFF1A: return ":";
    case 0x3002: return ".";
    }
    return nullptr;
}

static bool dropped(unsigned c) {
    return (c >= 0x200B && c <= 0x200F) || c == 0x2060 || c == 0xFEFF || (c >= 0xFE00 && c <= 0xFE0F) ||
           (c >= 0x0300 && c <= 0x036F) || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0x1F3FB && c <= 0x1F3FF) ||
           (c >= 0xE0020 && c <= 0xE007F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

std::wstring to_cells(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    bool after_zwj = false;
    bool prev_flag = false;
    for (size_t i = 0; i < s.size();) {
        unsigned char b = (unsigned char)s[i];
        unsigned c;
        int n;
        if (b < 0x80) { c = b; n = 1; }
        else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; n = 2; }
        else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; n = 3; }
        else if ((b & 0xF8) == 0xF0) { c = b & 0x07; n = 4; }
        else { out += L'?'; i++; continue; }
        if (i + n > s.size()) { out += L'?'; break; }
        bool bad = false;
        for (int k = 1; k < n; k++) {
            unsigned char cb = (unsigned char)s[i + k];
            if ((cb & 0xC0) != 0x80) { bad = true; break; }
            c = (c << 6) | (cb & 0x3F);
        }
        if (bad) { out += L'?'; i++; continue; }
        i += n;

        if (c == 0x200D) { after_zwj = true; continue; }   // emoji joiner: the next pictograph merges
        if (after_zwj) {
            after_zwj = false;
            if (is_pictograph(c)) continue;
        }
        bool flag = c >= 0x1F1E6 && c <= 0x1F1FF;          // regional indicators come in pairs
        if (flag && prev_flag) { prev_flag = false; continue; }
        prev_flag = flag;
        if (c == '\n' || c == '\t') { out += (wchar_t)c; continue; }
        if (dropped(c)) continue;
        if (const char* l = lookalike(c)) { for (; *l; l++) out += (wchar_t)*l; continue; }
        if (showable(c)) { out += (wchar_t)c; continue; }
        if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0)) continue;
        if (is_pictograph(c)) { out += L'*'; continue; }
        if (c >= 0x2500 && c <= 0x257F) { out += L'+'; continue; }  // box drawing missing from the code page
        if (c >= 0x2580 && c <= 0x259F) { out += L'#'; continue; }
        out += L'?';
    }
    return out;
}

// ---------------------------------------------------------------- Screen

bool Screen::init() {
    out_ = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO bi;
    con_ = GetConsoleScreenBufferInfo(out_, &bi) != 0;
    if (!con_) return false;
    orig_attr_ = bi.wAttributes;
    norm = bi.wAttributes & 0xFF;
    inv = (WORD)(((norm & 0x0F) << 4) | ((norm & 0xF0) >> 4));
    if (inv == norm) inv = 0x70;
    CONSOLE_CURSOR_INFO ci;
    if (GetConsoleCursorInfo(out_, &ci)) cur_size_ = ci.dwSize;
    SetConsoleTextAttribute(out_, norm);
    refresh();
    return true;
}

void Screen::restore() {
    if (!con_) return;
    SetConsoleTextAttribute(out_, orig_attr_);
    CONSOLE_CURSOR_INFO ci = {cur_size_, TRUE};
    SetConsoleCursorInfo(out_, &ci);
}

void Screen::refresh() {
    if (!con_) { w_ = 80; return; }
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (!GetConsoleScreenBufferInfo(out_, &bi)) return;
    w_ = bi.dwSize.X > 8 ? bi.dwSize.X : 8;
    h_ = bi.dwSize.Y > 2 ? bi.dwSize.Y : 2;
    wh_ = bi.srWindow.Bottom - bi.srWindow.Top + 1;
    if (wh_ < 2) wh_ = 2;
}

void Screen::ensure(long long v) {
    long long b = v - base_;
    if (b < h_) return;
    // Scroll the buffer the native way: line feeds written with the cursor on the last row.
    long long k = b - h_ + 1;
    COORD c = {0, (SHORT)(h_ - 1)};
    SetConsoleCursorPosition(out_, c);
    std::wstring nl((size_t)k, L'\n');
    DWORD n;
    WriteConsoleW(out_, nl.data(), (DWORD)nl.size(), &n, nullptr);
    base_ += k;
    cur_v_ = -1;
}

void Screen::row(long long v, const std::wstring& cells, bool inverted) {
    if (!con_) { plain_[v] = cells; return; }
    if (v < base_) return;  // scrolled out of the buffer
    ensure(v);
    std::wstring s = cells;
    s.resize((size_t)w_, L' ');
    COORD c = {0, (SHORT)(v - base_)};
    DWORD n;
    WriteConsoleOutputCharacterW(out_, s.data(), (DWORD)s.size(), c, &n);
    FillConsoleOutputAttribute(out_, inverted ? inv : norm, (DWORD)s.size(), c, &n);
}

void Screen::clear() {
    if (!con_) {
        commit(plain_.empty() ? plain_next_ : plain_.rbegin()->first + 1);
        plain_.clear();
        plain_next_ = 0;
        return;
    }
    refresh();
    COORD o = {0, 0};
    DWORD n, total = (DWORD)w_ * (DWORD)h_;
    FillConsoleOutputCharacterW(out_, L' ', total, o, &n);
    FillConsoleOutputAttribute(out_, norm, total, o, &n);
    SMALL_RECT r = {0, 0, 0, 0};
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (GetConsoleScreenBufferInfo(out_, &bi)) {
        r.Left = bi.srWindow.Left;
        r.Right = bi.srWindow.Right;
        r.Top = 0;
        r.Bottom = (SHORT)(bi.srWindow.Bottom - bi.srWindow.Top);
        SetConsoleWindowInfo(out_, TRUE, &r);
    }
    SetConsoleCursorPosition(out_, o);
    base_ = 0;
    cur_v_ = 0;
    cur_c_ = 0;
}

void Screen::cursor(long long v, int col, bool visible) {
    if (!con_) return;
    if (v < base_) v = base_;
    ensure(v);
    if (col >= w_) col = w_ - 1;
    if (v != cur_v_ || col != cur_c_) {
        COORD c = {(SHORT)col, (SHORT)(v - base_)};
        SetConsoleCursorPosition(out_, c);
        cur_v_ = v;
        cur_c_ = col;
    }
    if ((int)visible != cur_vis_) {
        CONSOLE_CURSOR_INFO ci = {cur_size_, visible ? TRUE : FALSE};
        SetConsoleCursorInfo(out_, &ci);
        cur_vis_ = visible;
    }
}

void Screen::title(const std::wstring& t) { SetConsoleTitleW(t.c_str()); }

void Screen::write_plain(const std::wstring& s) {
    std::string u = narrow(s);
    DWORD n;
    WriteFile(out_, u.data(), (DWORD)u.size(), &n, nullptr);
}

void Screen::commit(long long upto) {
    if (con_) return;
    std::wstring buf;
    for (; plain_next_ < upto; plain_next_++) {
        auto it = plain_.find(plain_next_);
        if (it != plain_.end()) {
            std::wstring l = it->second;
            while (!l.empty() && l.back() == L' ') l.pop_back();
            buf += l;
            plain_.erase(it);
        }
        buf += L"\r\n";
    }
    if (!buf.empty()) write_plain(buf);
}

}  // namespace ag
