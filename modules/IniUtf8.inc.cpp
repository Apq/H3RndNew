// ========== UTF-8 ini 读写 ==========
// H3RndNew.default.ini / H3RndNew.user.ini 为 UTF-8 文本（写统一带 BOM，读兼容无 BOM）。
// GetPrivateProfileStringA 是 ANSI 接口，读 UTF-8 中文值会按 GBK 误解成
// 乱码，因此手写解析。路径参数为 UTF-8 char*（开文件时转宽字符 _wfopen
// 一致），文件内容按 UTF-8 字节原样返回（调用方内部字符串均为 UTF-8）。
// 行内注释只认 ';'（不认 '#'，避免截断 {#RRGGBB} 颜色标记）；
// 行首 '#' 或 ';' 均为注释行。节/键比较不区分大小写。

#include <cstdio>
#include <cstring>
#include <cstdlib>

// 路径缓冲上限。用户可能把游戏装在很深的目录里，MAX_PATH(260) 不够；
// 4MB 只用于堆上的临时缓冲，静态路径与栈上缓冲仍用 MAX_PATH。
static const int kPathCap_ = 4 * 1024 * 1024;

// UTF-8 路径转宽字符。内部路径一律存 UTF-8，开文件走宽字符接口
// （_wfopen），不再经过系统 ANSI 代码页。
static wchar_t* Utf8ToWide_(const char* utf8, wchar_t* out, int out_chars)
{
    if (!out || out_chars <= 0) return out;
    out[0] = 0;
    if (utf8 && utf8[0])
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, out_chars);
    out[out_chars - 1] = 0;
    return out;
}

// 堆上的宽字符路径（new[]，调用方 delete[]）。游戏线程栈小，
// 长路径缓冲不能放栈上。失败返回 nullptr。
static wchar_t* Utf8ToWideAlloc_(const char* utf8)
{
    const int chars = kPathCap_ / (int)sizeof(wchar_t);
    wchar_t* out = new(std::nothrow) wchar_t[chars];
    if (!out) return nullptr;
    return Utf8ToWide_(utf8, out, chars);
}

// 去掉行首 BOM/空白与行尾空白（原地写 0，返回头指针）。
static char* IniTrim_(char* s)
{
    if (!s) return s;
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB
        && (unsigned char)s[2] == 0xBF)
        s += 3;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'
        || end[-1] == '\r' || end[-1] == '\n'))
        *--end = 0;
    return s;
}

// 读整个文件到堆缓冲（UTF-8 原样字节，追加 '\0'）。
// 返回 new[] 缓冲（调用方 delete[]），失败返回 nullptr。
// 防失控：本插件所有 ini（default/user/lang）正常都在几十 KB 内，超过
// 64KB 视为历史 bug 污染的空行堆积，不读入直接当文件不存在——旧版曾对
// 224MB 的 user.ini 走 new char[234MB]，32 位进程分配失败抛 bad_alloc
// 会直接崩游戏。写入侧（IniWriteKeyUtf8）对超限文件会整体重建。
static char* IniReadFileToBuffer_(const char* path, long* out_size)
{
    static const long kIniReadCap_ = 64 * 1024;
    if (!path || !path[0]) return nullptr;
    FILE* fp = nullptr;
    wchar_t* wpath = Utf8ToWideAlloc_(path);
    if (!wpath) return nullptr;
    if (_wfopen_s(&fp, wpath, L"rb") != 0 || !fp) { delete[] wpath; return nullptr; }
    delete[] wpath;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0 || sz > kIniReadCap_) { fclose(fp); return nullptr; }
    char* buf = new char[sz + 1];
    const size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = 0;
    if (out_size) *out_size = (long)got;
    return buf;
}

// 在缓冲内定位 section/key 的值。返回 true 并给出值区间（已 Trim、
// 已去行内 ';' 注释）。节的键允许乱序；重复键取第一个。
static bool IniFindValue_(char* buf, const char* section, const char* key,
    const char** out_val, int* out_len)
{
    if (!buf || !section || !key) return false;
    bool in_section = false;
    char* p = buf;
    while (*p) {
        char* line = p;
        while (*p && *p != '\r' && *p != '\n') ++p;
        if (*p) {
            // 先存终止符再写 0（同 IniWriteKeyUtf8：写 0 后 p[-1] 读到的是
            // 0 而不是 '\r'，CRLF 的 \n 必须靠 term 判断）。
            const char term = *p;
            *p++ = 0;
            if (term == '\r' && *p == '\n') ++p;
        }
        char* s = IniTrim_(line);
        if (!*s) continue;
        if (*s == '[') {
            char* close = strchr(s, ']');
            if (!close) { in_section = false; continue; }
            *close = 0;
            in_section = _stricmp(IniTrim_(s + 1), section) == 0;
            continue;
        }
        if (!in_section) continue;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char* k = IniTrim_(s);
        if (_stricmp(k, key) != 0) continue;
        char* v = eq + 1;
        char* semi = strchr(v, ';');
        if (semi) *semi = 0;
        v = IniTrim_(v);
        *out_val = v;
        *out_len = (int)strlen(v);
        return true;
    }
    return false;
}

// 读字符串键。命中返回 true；未命中/文件不存在时写 default_value 并
// 返回 false。out 始终以 0 结尾。
static bool IniReadUtf8(const char* path, const char* section, const char* key,
    const char* default_value, char* out, int out_size)
{
    if (!out || out_size <= 0) return false;
    out[0] = 0;
    bool found = false;
    long sz = 0;
    char* buf = IniReadFileToBuffer_(path, &sz);
    if (buf) {
        const char* v = nullptr;
        int vlen = 0;
        if (IniFindValue_(buf, section, key, &v, &vlen) && vlen > 0) {
            const int n = vlen < out_size - 1 ? vlen : out_size - 1;
            memcpy(out, v, n);
            out[n] = 0;
            found = true;
        }
        delete[] buf;
    }
    if (!found) {
        const char* d = default_value ? default_value : "";
        const int n = (int)strlen(d) < out_size - 1 ? (int)strlen(d) : out_size - 1;
        memcpy(out, d, n);
        out[n] = 0;
    }
    return found;
}

// 读整数键（未命中/坏值返回 default_value）。
static int IniReadIntUtf8(const char* path, const char* section,
    const char* key, int default_value)
{
    char buf[32] = {};
    if (!IniReadUtf8(path, section, key, "", buf, sizeof(buf)))
        return default_value;
    if (!buf[0]) return default_value;
    return atoi(buf);
}

// 写/改一个键：保留文件其余行与注释（该键所在行的行内注释会被替换掉），
// 写回统一 UTF-8 带 BOM、CRLF。节不存在则追加新节。文件不存在则新建。
static bool IniWriteKeyUtf8(const char* path, const char* section,
    const char* key, const char* value)
{
    if (!path || !path[0] || !section || !key) return false;
    long sz = 0;
    char* old = IniReadFileToBuffer_(path, &sz);
    // 防失控保险：user.ini 只有几行配置，超 64KB 必然是历史 bug 污染出的
    // 空行堆积（旧版换行判断错误导致每写一次空行翻倍），保留旧内容只会
    // 越滚越大直到 32 位进程写回时分配失败崩溃。超限即整体重建最小文件。
    static const long kIniRebuildThreshold_ = 64 * 1024;
    if (old && sz > kIniRebuildThreshold_) {
        delete[] old;
        old = nullptr;
        sz = 0; // 此处在 LogInfo 声明之前，无法记日志；丢弃即重建
    }
    // 输出缓冲按需扩容。旧实现按「原大小 + 固定余量」一次算死容量，而写回时
    // 开头要补 3 字节 BOM、LF 要换成 CRLF，输出经常比输入长；append 用
    // out_len + n < cap 判断，超了就静默丢弃——文件里只要有注释或空行，
    // 新键那一行就会被截掉，界面却照报「已保存」。日志级别因此永远写不进
    // user.ini，下次启动又读回默认 info。
    long cap = (old ? sz : 0) + 1024;
    char* out = new char[cap];
    long out_len = 0;
    auto append = [&](const char* s, long n) {
        if (n <= 0) return;
        if (out_len + n + 1 > cap) {
            long ncap = cap * 2;
            while (ncap < out_len + n + 1) ncap *= 2;
            char* bigger = new char[ncap];
            memcpy(bigger, out, (size_t)out_len);
            delete[] out;
            out = bigger;
            cap = ncap;
        }
        memcpy(out + out_len, s, (size_t)n);
        out_len += n;
    };
    append("\xEF\xBB\xBF", 3);

    bool written = false;
    bool in_section = false;
    bool section_seen = false;
    if (old) {
        char* p = old;
        bool first_line = true;
        while (*p) {
            char* line = p;
            while (*p && *p != '\r' && *p != '\n') ++p;
            bool had_newline = *p != 0;
            if (*p) {
                // 必须先存终止符再写 0：写完之后 p[-1] 读到的是刚写的 0，
                // 旧版在此判断 p[-1]=='\r' 恒为假，CRLF 里的 \n 不会被消费
                // 而是成为下一行的行首，每写一次文件空行翻倍。
                const char term = *p;
                *p++ = 0;
                if (term == '\r' && *p == '\n') ++p;
            }
            // 跳过旧 BOM
            char* s = line;
            if (first_line) {
                first_line = false;
                if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB
                    && (unsigned char)s[2] == 0xBF)
                    s += 3;
            }
            char* t = IniTrim_(s);
            const bool is_section = *t == '[';
            if (is_section) {
                char* close = strchr(t, ']');
                if (close) {
                    *close = 0;
                    // 离开目标节且键未写：在节末插入
                    if (in_section && !written) {
                        char nl[320];
                        int n = _snprintf(nl, sizeof(nl) - 1, "%s = %s\r\n",
                            key, value ? value : "");
                        append(nl, n);
                        written = true;
                    }
                    in_section = _stricmp(IniTrim_(t + 1), section) == 0;
                    if (in_section) section_seen = true;
                    *close = ']';
                }
            } else if (in_section && !written) {
                char* eq = strchr(t, '=');
                if (eq) {
                    *eq = 0;
                    const bool key_match = _stricmp(IniTrim_(t), key) == 0;
                    *eq = '=';
                    if (key_match) {
                        char nl[320];
                        int n = _snprintf(nl, sizeof(nl) - 1, "%s = %s\r\n",
                            key, value ? value : "");
                        append(nl, n);
                        written = true;
                        continue; // 原行不输出（已被新行替换）
                    }
                }
            }
            // 原样输出该行（保持 CRLF）
            long len = (long)strlen(s);
            append(s, len);
            if (had_newline) append("\r\n", 2);
        }
    }
    if (!written) {
        if (!section_seen) {
            if (out_len > 3) append("\r\n", 2); // BOM 后不产生首空行
            append("[", 1);
            append(section, (long)strlen(section));
            append("]\r\n", 3);
        }
        char nl[320];
        int n = _snprintf(nl, sizeof(nl) - 1, "%s = %s\r\n",
            key, value ? value : "");
        append(nl, n);
    }
    out[out_len] = 0;

    bool ok = false;
    FILE* fp = nullptr;
    wchar_t* wpath = Utf8ToWideAlloc_(path);
    if (wpath && _wfopen_s(&fp, wpath, L"wb") == 0 && fp) {
        ok = fwrite(out, 1, (size_t)out_len, fp) == (size_t)out_len;
        fclose(fp);
    }
    delete[] wpath;
    delete[] out;
    delete[] old;
    return ok;
}
