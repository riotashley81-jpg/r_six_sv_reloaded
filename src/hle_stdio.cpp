// stdio, POSIX file I/O and the printf/scanf engine for guest varargs.
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <unistd.h>
#define _O_BINARY 0
#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_CREAT O_CREAT
#define _O_EXCL O_EXCL
#define _O_TRUNC O_TRUNC
#define _O_APPEND O_APPEND
#define _S_IREAD 0400
#define _S_IWRITE 0200
#define _S_IFDIR S_IFDIR
#define _open open
#define _read read
#define _write write
#define _lseek lseek
#define _close close
#define _stat64 stat
#define _fstat64 fstat
#else
#include <io.h>
#endif

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "hle.h"
#include "os.h"
#include "texture_decode.h"

namespace hle {

// ---------------------------------------------------------------------------
// Paths: /sdcard/gameloft/games/GloftR6HP/... -> <data root>/...

static std::string data_root = "gamedata/GloftR6HP";
static const char* const ANDROID_PREFIXES[] = {"/sdcard/gameloft/games/GloftR6HP", "/mnt/sdcard/gameloft/games/GloftR6HP",
                                               "sdcard/gameloft/games/GloftR6HP"};

void set_data_root(const std::string& r) { data_root = r; }

#ifdef __vita__
// The Vita can't open files more than 9 folders below ux0:, and the game's
// data goes 9 deep on its own. So on the Vita each file's folder path is
// stored as one folder: data/3d/x/y.bdae -> <root>/data~3d~x/y.bdae
// (vita/flatten_data.py builds that layout). The game only opens files by
// path, never lists folders, so it doesn't notice.
static std::string flatten(const char* rel) {
    std::string dir, out;
    const char* last = strrchr(rel, '/');
    for (const char* q = rel; last && q < last; q++) {
        if (*q == '/') {
            if (!dir.empty() && dir.back() != '~') dir += '~';
        } else {
            dir += *q;
        }
    }
    if (!dir.empty() && dir.back() == '~') dir.pop_back();
    out = "/";
    if (!dir.empty()) out += dir + "/";
    return out + (last ? last + 1 : rel);
}
#endif

std::string host_path(const char* p) {
    for (const char* pre : ANDROID_PREFIXES) {
        size_t n = strlen(pre);
#ifdef __vita__
        if (os::strnicmp(p, pre, n) == 0) return data_root + flatten(p + n);
#else
        if (os::strnicmp(p, pre, n) == 0) return data_root + (p + n);
#endif
    }
    if (p[0] == '/') LOGW("unmapped absolute path %s", p);
    return p;
}

// ---------------------------------------------------------------------------
// printf engine. Format strings are read as sequences of code units so the
// same code serves char and (32-bit) wchar_t formats.

template <class CharT>
static std::u32string guest_format(const CharT* fmt, ArgReader& va) {
    std::u32string out;
    auto emit_narrow = [&](const char* s) { while (*s) out.push_back((u8)*s++); };
    for (const CharT* p = fmt; *p; p++) {
        if (*p != '%') {
            out.push_back((char32_t)*p);
            continue;
        }
        // collect the spec
        std::string spec = "%";
        p++;
        while (*p && strchr("-+ #0", (char)*p)) spec += (char)*p++;
        auto num = [&]() {
            if (*p == '*') {
                spec += std::to_string((s32)va.w32());
                p++;
            } else {
                while (*p >= '0' && *p <= '9') spec += (char)*p++;
            }
        };
        num();
        if (*p == '.') {
            spec += '.';
            p++;
            num();
        }
        int longs = 0;
        bool shrt = false;
        while (*p && strchr("hlLqjzt", (char)*p)) {
            if (*p == 'l') longs++;
            if (*p == 'q' || *p == 'L') longs = 2;
            if (*p == 'h') shrt = true;
            p++;
        }
        char conv = (char)*p;
        if (!conv) break;
        char buf[512];
        switch (conv) {
        case '%': out.push_back('%'); break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (conv == 'c' && (longs || sizeof(CharT) == 4)) {
                out.push_back((char32_t)va.w32());
                break;
            }
            if (longs >= 2) snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (long long)va.w64());
            else if (shrt && conv != 'c') snprintf(buf, sizeof buf, (spec + "h" + conv).c_str(), (int)va.w32());
            else snprintf(buf, sizeof buf, (spec + conv).c_str(), (int)va.w32());
            emit_narrow(buf);
            break;
        case 'p':
            snprintf(buf, sizeof buf, "0x%x", va.w32());
            emit_narrow(buf);
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            snprintf(buf, sizeof buf, (spec + conv).c_str(), bitcast<double>(va.w64()));
            emit_narrow(buf);
            break;
        case 's': case 'S': {
            u32 a = va.w32();
            bool wide = conv == 'S' || longs > 0;
            std::u32string s;
            if (!a) {
                for (const char* n = "(null)"; *n; n++) s.push_back((u8)*n);
            } else if (wide) {
                for (const char32_t* w = (const char32_t*)mem::H(a); *w; w++) s.push_back(*w);
            } else {
                for (const char* n = mem::str(a); *n; n++) s.push_back((u8)*n);
            }
            // apply width / precision from the spec
            int width = 0, prec = -1;
            bool left = spec.find('-') != std::string::npos;
            sscanf(spec.c_str() + 1 + strspn(spec.c_str() + 1, "-+ #0"), "%d", &width);
            size_t dot = spec.find('.');
            if (dot != std::string::npos) prec = atoi(spec.c_str() + dot + 1);
            if (prec >= 0 && (int)s.size() > prec) s.resize(prec);
            if (!left) while ((int)s.size() < width) s.insert(s.begin(), U' ');
            else while ((int)s.size() < width) s.push_back(U' ');
            out += s;
            break;
        }
        case 'n': {
            u32 a = va.w32();
            if (a) mem::wr32(a, (u32)out.size());
            break;
        }
        default: out.push_back((char32_t)conv); break;
        }
    }
    return out;
}

// Narrow output keeps bytes as-is (the game's strings are byte strings).
static std::string bytes(const std::u32string& s) {
    std::string r;
    r.reserve(s.size());
    for (char32_t c : s) r += (char)(c < 0x100 ? c : '?');
    return r;
}

std::string format_guest(const char* fmt, ArgReader& va) { return bytes(guest_format(fmt, va)); }

static u32 write_out(u32 buf, u32 size, const std::string& s) {
    if (size) {
        u32 n = (u32)std::min<size_t>(s.size(), size - 1);
        memcpy(mem::H(buf), s.data(), n);
        mem::ref<char>(buf + n) = 0;
    }
    return (u32)s.size();
}

static void h_sprintf(Cpu& c) {
    ArgReader va(c, 2);
    c.ret(write_out(c.r(0), 0x7FFFFFFF, format_guest(mem::str(c.r(1)), va)));
}
static void h_snprintf(Cpu& c) {
    ArgReader va(c, 3);
    c.ret(write_out(c.r(0), c.r(1), format_guest(mem::str(c.r(2)), va)));
}
static void h_vsprintf(Cpu& c) {
    auto va = ArgReader::from_va_list(c, c.r(2));
    c.ret(write_out(c.r(0), 0x7FFFFFFF, format_guest(mem::str(c.r(1)), va)));
}
static void h_vsnprintf(Cpu& c) {
    auto va = ArgReader::from_va_list(c, c.r(3));
    c.ret(write_out(c.r(0), c.r(1), format_guest(mem::str(c.r(2)), va)));
}
static void h_swprintf(Cpu& c) {
    // int swprintf(wchar_t* buf, size_t n, const wchar_t* fmt, ...)
    ArgReader va(c, 3);
    auto s = guest_format((const char32_t*)mem::H(c.r(2)), va);
    u32 n = c.r(1), buf = c.r(0);
    if (!n) { c.ret((u32)-1); return; }
    u32 cnt = (u32)std::min<size_t>(s.size(), n - 1);
    memcpy(mem::H(buf), s.data(), cnt * 4);
    mem::wr32(buf + cnt * 4, 0);
    c.ret(s.size() < n ? (u32)s.size() : (u32)-1);
}
static void h_printf(Cpu& c) {
    ArgReader va(c, 1);
    auto s = format_guest(mem::str(c.r(0)), va);
    fputs(s.c_str(), stdout);
    c.ret((u32)s.size());
}
static void h_android_log_print(Cpu& c) {
    ArgReader va(c, 3);
    int prio = (int)c.r(0);
    auto s = format_guest(mem::str(c.r(2)), va);
    while (!s.empty() && s.back() == '\n') s.pop_back();
    log_line(prio >= 6 ? "E" : prio == 5 ? "W" : "I", "[%s] %s", mem::str(c.r(1)), s.c_str());
    c.ret(1);
}

// sscanf: each conversion is handed to the host sscanf, which writes straight
// into guest memory. Sizes match because long is 32-bit on both hosts.
static void h_sscanf(Cpu& c) {
    const char* in = mem::str(c.r(0));
    const char* f = mem::str(c.r(1));
    ArgReader va(c, 2);
    int assigned = 0;
    size_t pos = 0;
    bool any_input = false;
    while (*f) {
        if (isspace((u8)*f)) {
            while (isspace((u8)in[pos])) pos++;
            f++;
            continue;
        }
        if (*f != '%' || f[1] == '%') {
            char want = *f == '%' ? (f++, '%') : *f;
            if (in[pos] != want) break;
            pos++;
            f++;
            continue;
        }
        // copy one conversion spec
        std::string spec = "%";
        const char* s = f + 1;
        bool suppress = *s == '*';
        if (suppress) spec += *s++;
        while (isdigit((u8)*s)) spec += *s++;
        std::string len;
        while (*s && strchr("hlLqjzt", *s)) len += *s++;
        char conv = *s;
        if (conv == '[') {
            std::string set = "[";
            s++;
            if (*s == '^') set += *s++;
            if (*s == ']') set += *s++;
            while (*s && *s != ']') set += *s++;
            set += ']';
            spec += set;
        } else {
            if (len == "z" || len == "t" || len == "j") len = "";
            if (conv == 'p') { conv = 'x'; len = ""; }
            spec += len + conv;
        }
        f = *s ? s + 1 : s;
        if (!in[pos]) break;
        int used = 0, got;
        spec += "%n";
        if (conv == 'n') {
            if (!suppress) mem::wr32(va.w32(), (u32)pos);
            continue;
        }
        if (suppress) got = sscanf(in + pos, spec.c_str(), &used);
        else got = sscanf(in + pos, spec.c_str(), mem::H(va.w32()), &used);
        if (got < 1 && !suppress) break;
        if (!suppress) assigned++;
        any_input = true;
        pos += used;
    }
    c.ret(!any_input && !in[pos] ? (u32)-1 : (u32)assigned);
}

// ---------------------------------------------------------------------------
// FILE emulation: guest FILE objects are opaque blocks mapped to host FILEs.
//
// The game never closes many of the files it reads (about 70 are open by the
// first mission). The Vita can't keep that many host handles open, so
// read-only files are "parked" (closed, position remembered) when too many
// are open, and reopened transparently on their next use.

static constexpr u32 GFILE_SIZE = 84;  // sizeof(bionic FILE)
// Vita: 24. PC: no limit, unless R6SV_MAXFILES sets one (to test parking).
#ifdef __vita__
static const u32 MAX_HOST_FILES = 24;
#else
static const u32 MAX_HOST_FILES = getenv("R6SV_MAXFILES") ? (u32)atoi(getenv("R6SV_MAXFILES")) : 0;
#endif

struct GFile {
    FILE* f = nullptr;  // null while parked
    std::string path, mode;
    long pos = 0;           // where to resume after reopening
    bool parkable = false;  // read-only, so it can be closed and reopened
    bool dxt5_as_atc = false;  // a DXT5 texture served as ATC (see dxt5_texture)
    u64 used = 0;
};
static std::mutex file_lock;  // held for every file operation
static std::unordered_map<u32, GFile> files;
static u64 use_clock = 0;
static u32 host_open = 0;  // host handles held by parkable/writable guest files
static u32 g_sF = 0;

// Recent file operations, printed in crash reports. Written without a lock
// (a torn entry in a crash report is harmless).
struct FileOp {
    char op;  // o(pen) r(ead) s(eek) t(ell) c(lose) p(ark) R(eopen)
    u32 g, a, b;
    s32 ret;
    char name[44];
};
static FileOp file_ops[48];
static u32 file_op_n = 0;
static void trace(char op, u32 g, u32 a, u32 b, s32 ret, const char* path = nullptr) {
    FileOp& e = file_ops[file_op_n++ % 48];
    e.op = op, e.g = g, e.a = a, e.b = b, e.ret = ret;
    e.name[0] = 0;
    if (path) {
        size_t l = strlen(path);
        snprintf(e.name, sizeof e.name, "%s", l >= sizeof e.name ? path + l - (sizeof e.name - 1) : path);
    }
}
void dump_file_ops(void (*out)(const char*)) {
    char s[128];
    u32 n = file_op_n, first = n > 48 ? n - 48 : 0;
    for (u32 i = first; i < n; i++) {
        const FileOp& e = file_ops[i % 48];
        snprintf(s, sizeof s, "%c %08x %u %u -> %d %s", e.op, e.g, e.a, e.b, e.ret, e.name);
        out(s);
    }
}

// Close the least recently used parkable file. Lock held.
static bool park_one() {
    GFile* lru = nullptr;
    for (auto& [g, gf] : files)
        if (gf.f && gf.parkable && (!lru || gf.used < lru->used)) lru = &gf;
    if (!lru) return false;
    lru->pos = ftell(lru->f);
    trace('p', 0, 0, 0, (s32)lru->pos, lru->path.c_str());
    fclose(lru->f);
    lru->f = nullptr;
    host_open--;
    return true;
}

// fopen, making room first when the host limit is reached. Lock held.
static FILE* open_host(const std::string& path, const std::string& mode) {
    if (MAX_HOST_FILES)
        while (host_open >= MAX_HOST_FILES && park_one()) {}
    FILE* f = fopen(path.c_str(), mode.c_str());
    if (!f && MAX_HOST_FILES && errno != ENOENT) {
        int e = errno, parked = 0;
        while (parked < 8 && park_one()) parked++;
        if (parked && (f = fopen(path.c_str(), mode.c_str())))
            LOGW("fopen(%s) only worked after closing %d idle files (errno was %d)", path.c_str(), parked, e);
        if (!f) errno = e;
    }
    if (f) host_open++;
    return f;
}

// Guest FILE -> host FILE, reopening a parked file. Lock held.
static FILE* hf_locked(u32 g) {
    auto it = files.find(g);
    if (it == files.end()) return nullptr;
    GFile& gf = it->second;
    gf.used = ++use_clock;
    if (!gf.f) {
        gf.f = open_host(gf.path, gf.mode);
        if (!gf.f) {
            LOGW("cannot reopen %s (errno %d)", gf.path.c_str(), errno);
            return nullptr;
        }
        int r = fseek(gf.f, gf.pos, SEEK_SET);
        trace('R', g, (u32)gf.pos, 0, r, gf.path.c_str());
    }
    return gf.f;
}

template <class R, class Fn> static R with_file(u32 g, R fail, Fn fn) {
    std::lock_guard lk(file_lock);
    FILE* f = hf_locked(g);
    return f ? fn(f) : fail;
}

// The LG Optimus 2X data has DXT5 textures (DDS files named .tga), which the
// other builds skip, leaving them untextured. Such files are read as ATC
// instead, which every build loads: fread rewrites the header's format tag and
// converts the 16-byte blocks after the 128-byte header (tex::dxt5_to_atc).
static const u32 DDS_HEADER = 128, DDS_FOURCC = 84;
static bool dxt5_texture(FILE* f, const std::string& path) {
    size_t n = path.size();
    if (n < 4) return false;
    std::string ext = path.substr(n - 4);
    for (char& ch : ext) ch = (char)tolower((unsigned char)ch);
    if (ext != ".tga" && ext != ".dds") return false;
    u8 h[DDS_HEADER];
    bool dxt5 = fread(h, 1, DDS_HEADER, f) == DDS_HEADER && memcmp(h, "DDS ", 4) == 0 && (h[80] & 4) &&
                memcmp(h + DDS_FOURCC, "DXT5", 4) == 0;
    if (dxt5) {
        fseek(f, 0, SEEK_END);
        dxt5 = (ftell(f) - DDS_HEADER) % 16 == 0;
    }
    fseek(f, 0, SEEK_SET);
    return dxt5;
}
// buf holds len bytes read from file offset pos; blocks cut by the ends of the
// read are fetched whole from the file to convert them.
static void dxt5_read_as_atc(FILE* f, u8* buf, long pos, size_t len) {
    long end = pos + (long)len;
    for (u32 i = 0; i < 4; i++)
        if (DDS_FOURCC + i >= (u32)pos && DDS_FOURCC + i < (u32)end) buf[DDS_FOURCC + i - pos] = (u8)"ATCI"[i];
    if (end <= (long)DDS_HEADER) return;
    long first = (std::max(pos, (long)DDS_HEADER) - DDS_HEADER) / 16, last = (end - DDS_HEADER + 15) / 16;
    long resume = -1;
    for (long b = first; b < last; b++) {
        long at = DDS_HEADER + b * 16;
        if (at >= pos && at + 16 <= end) {
            tex::dxt5_to_atc(buf + (at - pos));
            continue;
        }
        if (resume < 0) resume = ftell(f);
        u8 blk[16] = {};
        fseek(f, at, SEEK_SET);
        if (fread(blk, 1, 16, f) != 16) continue;
        tex::dxt5_to_atc(blk);
        long from = std::max(at, pos), to = std::min(at + 16, end);
        memcpy(buf + (from - pos), blk + (from - at), (size_t)(to - from));
    }
    if (resume >= 0) fseek(f, resume, SEEK_SET);
}

static u32 h_fopen(const char* path, const char* mode) {
    std::string m;
    for (const char* p = mode; *p; p++) if (*p != 'b' && *p != 't') m += *p;
    m += 'b';
    std::string hp = host_path(path);
    // The game probes for a pack file named "data", which is our data folder.
    // Opening a folder fails on PC but succeeds on the Vita, and the game then
    // reads garbage from it; make it fail everywhere.
    struct stat st;
    if (stat(hp.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
        set_errno(21);  // EISDIR
        return 0;
    }
    std::lock_guard lk(file_lock);
    FILE* f = open_host(hp, m);
    if (!f) {
        int e = errno;
#ifdef __vita__
        int raw = sceIoOpen(hp.c_str(), SCE_O_RDONLY, 0);  // the Vita's own error code, before newlib translates it
        if (raw >= 0) sceIoClose(raw);
        LOGI("fopen(%s, %s) -> not found (errno %d, sceIoOpen 0x%08x, %u files open)", path, mode, e, (u32)raw, host_open);
#else
        LOGI("fopen(%s, %s) -> not found (errno %d, %u files open)", path, mode, e, host_open);
#endif
        set_errno(2);
        return 0;
    }
    static const bool file_log = getenv("R6SV_FILELOG") != nullptr;
    if (file_log) LOGI("fopen(%s)", path);
    u32 g = mem::alloc(GFILE_SIZE);
    memset(mem::H(g), 0, GFILE_SIZE);
    GFile& gf = files[g];
    gf.f = f;
    gf.path = hp;
    gf.mode = m;
    gf.parkable = m[0] == 'r' && m.find('+') == std::string::npos;
    gf.dxt5_as_atc = gf.parkable && dxt5_texture(f, hp);
    if (gf.dxt5_as_atc) {
        static u32 converted = 0;
        if (converted++ == 0) LOGI("textures: DXT5 data found, loading it as ATC (%s)", path);
    }
    gf.used = ++use_clock;
    trace('o', g, 0, 0, 0, hp.c_str());
    static u32 peak = 0;
    if (files.size() > peak && files.size() % 8 == 0) LOGI("%u files open", (u32)files.size());
    peak = std::max(peak, (u32)files.size());
    return g;
}
static int h_fclose(u32 g) {
    if (g >= g_sF && g < g_sF + 3 * GFILE_SIZE) return 0;
    FILE* f;
    {
        std::lock_guard lk(file_lock);
        auto it = files.find(g);
        if (it == files.end()) return -1;
        f = it->second.f;
        trace('c', g, 0, 0, 0);
        files.erase(it);
        if (f) host_open--;
    }
    mem::free(g);
    return f ? fclose(f) : 0;
}
static u32 h_fread(void* buf, u32 size, u32 n, u32 g) {
    bool as_atc;
    {
        std::lock_guard lk(file_lock);
        auto it = files.find(g);
        as_atc = it != files.end() && it->second.dxt5_as_atc;
    }
    return with_file(g, 0u, [&](FILE* f) {
        long pos = as_atc ? ftell(f) : 0;
        u32 r = (u32)fread(buf, size, n, f);
        if (as_atc && r) dxt5_read_as_atc(f, (u8*)buf, pos, (size_t)r * size);
        trace('r', g, size, n, (s32)r);
        return r;
    });
}
static u32 h_fwrite(const void* buf, u32 size, u32 n, u32 g) { return with_file(g, 0u, [&](FILE* f) { return (u32)fwrite(buf, size, n, f); }); }
static int h_fseek(u32 g, s32 off, int whence) {
    return with_file(g, -1, [&](FILE* f) {
        int r = fseek(f, off, whence);
        trace('s', g, (u32)off, (u32)whence, r);
        return r;
    });
}
static s32 h_ftell(u32 g) {
    return with_file(g, (s32)-1, [&](FILE* f) {
        s32 r = (s32)ftell(f);
        trace('t', g, 0, 0, r);
        return r;
    });
}
static int h_fgetpos(u32 g, s32* pos) { return with_file(g, -1, [&](FILE* f) { *pos = (s32)ftell(f); return 0; }); }
static int h_fsetpos(u32 g, const s32* pos) { return with_file(g, -1, [&](FILE* f) { return fseek(f, *pos, SEEK_SET); }); }
static int h_feof(u32 g) { return with_file(g, 1, [&](FILE* f) { return feof(f); }); }
static void h_clearerr(u32 g) { with_file(g, 0, [&](FILE* f) { clearerr(f); return 0; }); }
static int h_fflush(u32 g) { return g ? with_file(g, -1, [&](FILE* f) { return fflush(f); }) : fflush(nullptr); }
static int h_getc(u32 g) { return with_file(g, -1, [&](FILE* f) { return getc(f); }); }
static int h_ungetc(int ch, u32 g) { return with_file(g, -1, [&](FILE* f) { return ungetc(ch, f); }); }
static int h_putc(int ch, u32 g) { return with_file(g, -1, [&](FILE* f) { return putc(ch, f); }); }
static int h_fputs(const char* s, u32 g) { return with_file(g, -1, [&](FILE* f) { return fputs(s, f); }); }
static int h_puts(const char* s) { return puts(s); }
static char* h_fgets(char* s, int n, u32 g) { return with_file(g, (char*)nullptr, [&](FILE* f) { return fgets(s, n, f) ? s : nullptr; }); }
static int h_setvbuf(u32, u32, int, u32) { return 0; }
static int h_remove(const char* p) {
    int r = remove(host_path(p).c_str());
    LOGI("remove(%s) -> %d", p, r);
    return r;
}
static void h_fprintf(Cpu& c) {
    ArgReader va(c, 2);
    auto s = format_guest(mem::str(c.r(1)), va);
    c.ret(with_file(c.r(0), (u32)-1, [&](FILE* f) { return (u32)fwrite(s.data(), 1, s.size(), f); }));
}

// --- POSIX fds ----------------------------------------------------------------
static int h_open(const char* path, int flags, int mode) {
    int f = _O_BINARY;
    switch (flags & 3) {
    case 0: f |= _O_RDONLY; break;
    case 1: f |= _O_WRONLY; break;
    default: f |= _O_RDWR; break;
    }
    if (flags & 0x40) f |= _O_CREAT;
    if (flags & 0x80) f |= _O_EXCL;
    if (flags & 0x200) f |= _O_TRUNC;
    if (flags & 0x400) f |= _O_APPEND;
    int fd = _open(host_path(path).c_str(), f, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        LOGI("open(%s) -> not found", path);
        set_errno(2);
    }
    return fd;
}
static s32 h_read(int fd, void* buf, u32 n) { return _read(fd, buf, n); }
static s32 h_write(int fd, const void* buf, u32 n) { return _write(fd, buf, n); }
static s32 h_lseek(int fd, s32 off, int whence) { return _lseek(fd, off, whence); }
static int h_close(int fd) { return _close(fd); }

// bionic ARM struct stat (== kernel stat64), 104 bytes
static int h_fstat(int fd, u8* st) {
    struct _stat64 s;
    if (_fstat64(fd, &s) != 0) return -1;
    memset(st, 0, 104);
    u32 mode = (s.st_mode & _S_IFDIR) ? 0040000 : 0100000;
    mode |= 0666;
    memcpy(st + 16, &mode, 4);
    s64 size = s.st_size;
    memcpy(st + 48, &size, 8);
    u32 t = (u32)s.st_mtime;
    memcpy(st + 72, &t, 4);
    memcpy(st + 80, &t, 4);
    memcpy(st + 88, &t, 4);
    return 0;
}

// mmap: anonymous or private file mappings, emulated by copying
static u32 h_mmap(u32 addr, u32 len, int prot, int flags, int fd, s32 off) {
    u32 a = mem::map(len);
    if (!a) return ~0u;
    if (!(flags & 0x20) && fd >= 0) {  // not MAP_ANONYMOUS
        s32 cur = _lseek(fd, 0, SEEK_CUR);
        _lseek(fd, off, SEEK_SET);
        _read(fd, mem::H(a), len);
        _lseek(fd, cur, SEEK_SET);
    }
    return a;
}
static int h_munmap(u32 addr, u32 len) {
    mem::unmap(addr, len);
    return 0;
}

void init_stdio() {
    g_sF = mem::data_alloc(3 * GFILE_SIZE);
    files[g_sF].f = stdin;  // never parked: not counted in host_open
    files[g_sF + GFILE_SIZE].f = stdout;
    files[g_sF + 2 * GFILE_SIZE].f = stderr;
    reg_data("__sF", g_sF);

    reg("sprintf", h_sprintf); reg("snprintf", h_snprintf); reg("vsprintf", h_vsprintf);
    reg("vsnprintf", h_vsnprintf); reg("swprintf", h_swprintf); reg("printf", h_printf);
    reg("fprintf", h_fprintf); reg("sscanf", h_sscanf); reg("__android_log_print", h_android_log_print);
#define R(name, fn) reg(name, W<fn>)
    R("fopen", h_fopen); R("fclose", h_fclose); R("fread", h_fread); R("fwrite", h_fwrite);
    R("fseek", h_fseek); R("ftell", h_ftell); R("fgetpos", h_fgetpos); R("fsetpos", h_fsetpos);
    R("feof", h_feof); R("clearerr", h_clearerr); R("fflush", h_fflush); R("getc", h_getc);
    R("ungetc", h_ungetc); R("putc", h_putc); R("fputc", h_putc); R("fputs", h_fputs); R("puts", h_puts);
    R("fgets", h_fgets); R("setvbuf", h_setvbuf); R("remove", h_remove);
    R("open", h_open); R("read", h_read); R("write", h_write); R("lseek", h_lseek); R("close", h_close);
    R("fstat", h_fstat); R("mmap", h_mmap); R("munmap", h_munmap);
#undef R
}

}  // namespace hle
