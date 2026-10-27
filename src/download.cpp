#include "download.h"
#include <windows.h>
#include <algorithm>

std::wstring SanitizeFileName(const std::wstring& name)
{
    std::wstring out;
    out.reserve(name.size());
    for (wchar_t c : name) {
        switch (c) {
        case L'\\': case L'/': case L':': case L'*': case L'?':
        case L'"': case L'<': case L'>': case L'|':
            out += L'_';
            break;
        case L'\r': case L'\n': case L'\t':
            out += L' ';
            break;
        default:
            out += c;
        }
    }
    // 去掉首尾空白和点
    size_t b = out.find_first_not_of(L" .");
    if (b == std::wstring::npos) return L"未命名";
    size_t e = out.find_last_not_of(L" .");
    out = out.substr(b, e - b + 1);
    if (out.size() > 120) out.resize(120);
    return out;
}

std::wstring FormatSize(int64_t bytes)
{
    if (bytes < 0) return L"未知";
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        ++i;
    }
    wchar_t buf[64];
    if (i == 0) swprintf(buf, 64, L"%lld %s", (long long)bytes, units[i]);
    else swprintf(buf, 64, L"%.1f %s", v, units[i]);
    return buf;
}

std::wstring FormatDuration(int sec)
{
    if (sec <= 0) return L"--:--";
    wchar_t buf[32];
    if (sec >= 3600) swprintf(buf, 32, L"%d:%02d:%02d", sec / 3600, (sec % 3600) / 60, sec % 60);
    else swprintf(buf, 32, L"%02d:%02d", sec / 60, sec % 60);
    return buf;
}

bool EnsureDirectory(const std::wstring& dir)
{
    if (dir.empty()) return false;
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

    // 逐级创建
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        if (dir[i] == L'\\' || dir[i] == L'/') {
            if (cur.size() > 3) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring UniquePath(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;

    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    std::wstring base = path, ext;
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
        base = path.substr(0, dot);
        ext = path.substr(dot);
    }
    for (int i = 1; i < 999; ++i) {
        std::wstring cand = base + L" (" + std::to_wstring(i) + L")" + ext;
        if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) return cand;
    }
    return path;
}

static bool ConcatFiles(const std::vector<std::wstring>& parts, const std::wstring& out, std::wstring& err)
{
    HANDLE hOut = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        err = L"无法创建输出文件";
        return false;
    }

    std::vector<char> buf(1024 * 1024);
    bool ok = true;
    for (const auto& p : parts) {
        HANDLE hIn = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hIn == INVALID_HANDLE_VALUE) {
            err = L"读取分段文件失败";
            ok = false;
            break;
        }
        DWORD read = 0;
        while (ReadFile(hIn, buf.data(), (DWORD)buf.size(), &read, nullptr) && read > 0) {
            DWORD written = 0;
            if (!WriteFile(hOut, buf.data(), read, &written, nullptr) || written != read) {
                err = L"合并写入失败";
                ok = false;
                break;
            }
        }
        CloseHandle(hIn);
        if (!ok) break;
    }
    CloseHandle(hOut);

    if (ok) {
        for (const auto& p : parts) DeleteFileW(p.c_str());
    } else {
        DeleteFileW(out.c_str());
    }
    return ok;
}

// 找 ffmpeg（exe 同目录优先，其次 PATH）
static std::wstring FindFfmpeg()
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos + 1);

    std::wstring local = dir + L"ffmpeg.exe";
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;

    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return std::wstring();
}

static bool MergeWithFfmpeg(const std::wstring& video, const std::wstring& audio, const std::wstring& out,
                            std::wstring& err)
{
    std::wstring ff = FindFfmpeg();
    if (ff.empty()) {
        err = L"未找到 ffmpeg.exe";
        return false;
    }

    std::wstring cmd = L"\"" + ff + L"\" -y -i \"" + video + L"\" -i \"" + audio +
                       L"\" -c copy -map 0:v:0 -map 1:a:0 \"" + out + L"\"";
    std::wstring cmdline = cmd;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = L"无法启动 ffmpeg";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (code != 0 || GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = L"ffmpeg 合并失败";
        return false;
    }
    return true;
}

bool DownloadEpisode(BiliApi& api, const MediaInfo& media, const Episode& ep, int qn,
                     const std::wstring& dir, const std::wstring& fileNameBase,
                     const DownloadCallbacks& cb, std::wstring& outPath, std::wstring& err)
{
    PlayInfo pi;
    if (!api.GetPlayInfo(media, ep.id, qn, pi)) {
        err = pi.error.empty() ? L"获取播放地址失败" : pi.error;
        return false;
    }

    std::wstring fileBase = SanitizeFileName(fileNameBase);
    std::wstring basePath = dir;
    if (!basePath.empty() && basePath.back() != L'\\' && basePath.back() != L'/') basePath += L'\\';
    basePath += fileBase;

    if (cb.log) {
        std::wstring desc;
        for (const auto& q : pi.qualities) {
            if (q.qn == pi.qn) desc = q.desc;
        }
        cb.log(L"实际画质: " + (desc.empty() ? std::to_wstring(pi.qn) : desc));
        if (pi.preview) cb.log(L"注意: 该视频返回的是试看片段");
    }

    // 音视频分离流（DASH）
    if (pi.separated && pi.urls.size() == 2) {
        std::wstring vPart = basePath + L".video.part";
        std::wstring aPart = basePath + L".audio.part";
        DeleteFileW(vPart.c_str());
        DeleteFileW(aPart.c_str());

        int64_t vTotal = 0, aTotal = 0, vDone = 0, aDone = 0;
        auto report = [&]() {
            if (!cb.progress) return true;
            int64_t t = vTotal + aTotal;
            return cb.progress(vDone + aDone, t > 0 ? t : -1);
        };

        if (cb.log) cb.log(L"下载视频流...");
        auto r1 = api.http.Download(pi.urls[0], vPart, [&](int64_t d, int64_t t) {
            vDone = d;
            vTotal = t > 0 ? t : vTotal;
            return report();
        });
        if (!r1.ok) {
            err = r1.error;
            return false;
        }
        if (cb.log) cb.log(L"下载音频流...");
        auto r2 = api.http.Download(pi.urls[1], aPart, [&](int64_t d, int64_t t) {
            aDone = d;
            aTotal = t > 0 ? t : aTotal;
            return report();
        });
        if (!r2.ok) {
            err = r2.error;
            return false;
        }

        std::wstring out = UniquePath(basePath + L".mp4");
        std::wstring mergeErr;
        if (cb.log) cb.log(L"合并音视频...");
        if (MergeWithFfmpeg(vPart, aPart, out, mergeErr)) {
            DeleteFileW(vPart.c_str());
            DeleteFileW(aPart.c_str());
            outPath = out;
            if (cb.log) cb.log(L"已保存: " + out);
            return true;
        }

        // 没有 ffmpeg：保留两路文件
        std::wstring vOut = UniquePath(basePath + L".video.mp4");
        std::wstring aOut = UniquePath(basePath + L".audio.m4a");
        MoveFileW(vPart.c_str(), vOut.c_str());
        MoveFileW(aPart.c_str(), aOut.c_str());
        outPath = vOut;
        if (cb.log) {
            cb.log(L"未找到 ffmpeg，已分别保存视频流与音频流（把 ffmpeg.exe 放到程序目录即可自动合并）");
        }
        return true;
    }

    // 单段或顺序分段
    if (pi.urls.size() == 1) {
        std::wstring out = UniquePath(basePath + L".mp4");
        auto r = api.http.Download(pi.urls[0], out, [&](int64_t d, int64_t t) {
            return cb.progress ? cb.progress(d, t) : true;
        });
        if (!r.ok) {
            err = r.error;
            return false;
        }
        outPath = out;
        if (cb.log) cb.log(L"已保存: " + out);
        return true;
    }

    // 多段：分别下载后按顺序拼接
    std::vector<std::wstring> segs;
    int64_t grandTotal = 0;
    for (size_t i = 0; i < pi.urls.size(); ++i) {
        std::wstring segPath = basePath + L".seg" + std::to_wstring(i) + L".part";
        DeleteFileW(segPath.c_str());
        if (cb.log) cb.log(L"下载第 " + std::to_wstring(i + 1) + L"/" + std::to_wstring(pi.urls.size()) + L" 段...");
        auto r = api.http.Download(pi.urls[i], segPath, [&](int64_t d, int64_t t) {
            if (!cb.progress) return true;
            return cb.progress(grandTotal + d, -1);
        });
        if (!r.ok) {
            err = r.error;
            for (auto& s : segs) DeleteFileW(s.c_str());
            return false;
        }
        grandTotal += r.total;
        segs.push_back(segPath);
    }

    std::wstring out = UniquePath(basePath + L".mp4");
    std::wstring mergeErr;
    if (!ConcatFiles(segs, out, mergeErr)) {
        err = mergeErr;
        return false;
    }
    outPath = out;
    if (cb.log) cb.log(L"已保存: " + out);
    return true;
}
