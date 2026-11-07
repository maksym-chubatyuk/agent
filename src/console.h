// Console output for the interactive frontend.
//
// Rows are "virtual": virtual row = buffer row + the number of lines the buffer has scrolled since
// the last clear, so a row written earlier can still be found (and rewritten in place) after the
// buffer scrolled. Every write is a positioned write of one whole row (WriteConsoleOutputCharacterW
// + FillConsoleOutputAttribute), so conhost never wraps or reflows anything and nothing but the
// rows we touch is repainted. The code page is never changed.
//
// When stdout is not a console, rows are collected and written out as UTF-8 lines once they are
// final (commit), with no in-place rewrites.
#pragma once
#include <windows.h>
#include <string>
#include <map>

namespace ag {

// UTF-8 -> console cells, one wchar_t per cell. Characters the console font can't show become
// ASCII look-alikes (quotes, dashes, ellipsis, bullets, arrows, emoji -> '*', ...). '\n' and '\t'
// are kept; other control characters are dropped.
std::wstring to_cells(const std::string& utf8);

class Screen {
public:
    bool init();                 // false: stdout is not a console (plain UTF-8 mode)
    void restore();              // original cursor and text attribute
    bool console() const { return con_; }
    void refresh();              // re-read the buffer and window size
    int width() const { return w_; }
    int win_height() const { return wh_; }
    WORD norm = 7, inv = 0x70;

    // Write one whole row (padded or cut to the width).
    void row(long long v, const std::wstring& cells, bool inverted);
    // Clear the buffer and put the window at the top; virtual rows restart at 0.
    void clear();
    void cursor(long long v, int col, bool visible);
    void title(const std::wstring& t);
    // Plain mode: write out rows below `upto` (missing rows are empty lines).
    void commit(long long upto);

private:
    bool con_ = false;
    HANDLE out_ = INVALID_HANDLE_VALUE;
    int w_ = 80, h_ = 300, wh_ = 25;
    long long base_ = 0;         // virtual row of buffer row 0
    long long cur_v_ = -1;
    int cur_c_ = -1;
    int cur_vis_ = -1;
    DWORD cur_size_ = 25;
    WORD orig_attr_ = 7;
    std::map<long long, std::wstring> plain_;  // plain mode: rows not written yet
    long long plain_next_ = 0;
    void ensure(long long v);    // scroll the buffer until v is inside it
    void write_plain(const std::wstring& s);
};

}  // namespace ag
