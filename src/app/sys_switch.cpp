// Nintendo Switch (devkitA64 newlib; the SD card is the "sdmc:" device) side of sys.h. Paths are converted to
// UTF-8 for the C library. Everything lives under sdmc:/switch/BustersHiddenTreasure_nx, next to the .nro hbmenu starts.
#include "sys.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>

std::string to_utf8(const std::wstring& w) {
    std::string s;
    for (wchar_t wc : w) {
        uint32_t c = (uint32_t)wc;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    return s;
}

std::wstring from_utf8(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
        if (n < 0) { w += (wchar_t)0xFFFD; i++; continue; }
        uint32_t cp = n ? c & (0x3Fu >> n) : c;
        size_t j = i + 1;
        for (int k = 0; k < n && j < s.size(); k++, j++) cp = (cp << 6) | ((unsigned char)s[j] & 0x3F);
        w += (wchar_t)cp;
        i = j;
    }
    return w;
}

std::wstring path_join(const std::wstring& dir, const std::wstring& name) { return dir + L"/" + name; }

std::wstring file_name(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

FILE* open_file(const std::wstring& path, const char* mode) { return fopen(to_utf8(path).c_str(), mode); }

bool read_file(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = open_file(path, "rb");
    if (!f) {
        // A write that was cut short between "old file removed" and "new file renamed" (see write_file) leaves
        // the complete new file as <path>.tmp: use it.
        const std::wstring tmp = path + L".tmp";
        if (FILE* t = open_file(tmp, "rb")) {
            fclose(t);
            if (rename(to_utf8(tmp).c_str(), to_utf8(path).c_str()) == 0) f = open_file(path, "rb");
        }
    }
    if (!f) return false;
    uint64_t n = file_size(f);
    out.resize((size_t)n);
    bool ok = n == 0 || fread(out.data(), 1, (size_t)n, f) == n;
    fclose(f);
    return ok;
}

bool read_text(const std::wstring& path, std::string& out) {
    std::vector<uint8_t> d;
    if (!read_file(path, d)) return false;
    out.assign(d.begin(), d.end());
    return true;
}

bool write_file(const std::wstring& path, const void* data, size_t n) {
    // Write a temporary file first, so a crash mid-write never leaves half a file, then put it in place. The SD
    // card's FAT driver can't rename a file over an existing one (rename() fails and the .tmp stays, which is how
    // settings and key bindings were lost between runs): take the old file out of the way first.
    const std::string dst = to_utf8(path), tmp = dst + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = n == 0 || fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), dst.c_str()) == 0) return true;
    remove(dst.c_str());
    if (rename(tmp.c_str(), dst.c_str()) == 0) return true;
    // Last resort: write the file where it belongs.
    FILE* g = fopen(dst.c_str(), "wb");
    if (!g) return false;
    ok = n == 0 || fwrite(data, 1, n, g) == n;
    ok = fclose(g) == 0 && ok;
    remove(tmp.c_str());
    return ok;
}

bool file_seek(FILE* f, uint64_t pos) { return fseeko(f, (off_t)pos, SEEK_SET) == 0; }

uint64_t file_size(FILE* f) {
    off_t cur = ftello(f);
    fseeko(f, 0, SEEK_END);
    off_t n = ftello(f);
    fseeko(f, cur, SEEK_SET);
    return n < 0 ? 0 : (uint64_t)n;
}

void make_dir(const std::wstring& path) { mkdir(to_utf8(path).c_str(), 0755); }

bool list_dir(const std::wstring& dir, std::vector<DirEntry>& out) {
    out.clear();
    const std::string d = to_utf8(dir);
    DIR* h = opendir(d.c_str());
    if (!h) return false;
    while (dirent* e = readdir(h)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (stat((d + "/" + e->d_name).c_str(), &st) != 0) continue;
        DirEntry de;
        de.name = from_utf8(e->d_name);
        de.size = (uint64_t)st.st_size;
        de.mtime = (uint64_t)st.st_mtime;
        de.is_dir = S_ISDIR(st.st_mode);
        out.push_back(std::move(de));
    }
    closedir(h);
    return true;
}

std::wstring exe_dir() { return data_dir(); }

// Settings, save states, screenshots, achievements and soundtrack packs.
std::wstring data_dir() { return L"sdmc:/switch/BustersHiddenTreasure_nx"; }

// Searched for ROMs (not recursively; the app lists these every second while the launcher is open, so no big
// folders here): sdmc:/switch/BustersHiddenTreasure_nx/roms, and that folder itself.
std::vector<std::wstring> rom_dirs() { return {path_join(data_dir(), L"roms"), data_dir()}; }

// (the Switch's browser applet isn't available to homebrew started from the album / in applet mode)
void open_url(const char*) {}
