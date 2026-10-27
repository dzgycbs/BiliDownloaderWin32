#include "http.h"
#include <windows.h>
#include <winhttp.h>
#include <vector>

#pragma comment(lib, "winhttp.lib")

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string a(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &a[0], n, nullptr, nullptr);
    return a;
}

std::string TrimAscii(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring Trim(const std::wstring& s)
{
    size_t b = s.find_first_not_of(L" \t\r\n\u3000");
    if (b == std::wstring::npos) return std::wstring();
    size_t e = s.find_last_not_of(L" \t\r\n\u3000");
    return s.substr(b, e - b + 1);
}

// 全局 session，进程退出时自动关闭
namespace {
struct SessionHolder {
    HINTERNET h = nullptr;
    SessionHolder()
    {
        h = WinHttpOpen(L"BiliDownloader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (h) {
            WinHttpSetTimeouts(h, 15000, 15000, 30000, 60000);
        }
    }
    ~SessionHolder()
    {
        if (h) WinHttpCloseHandle(h);
    }
};

HINTERNET Session()
{
    static SessionHolder holder;
    return holder.h;
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool Crack(const std::wstring& url, UrlParts& out)
{
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;
    out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) out.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    out.port = uc.nPort;
    out.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return !out.host.empty();
}

std::wstring PathToLocal(const std::wstring& p)
{
    return p;
}
} // namespace

HttpResponse Http::Once(const std::wstring& url, const std::wstring& verb, const std::string& body,
                        const std::string& contentType)
{
    HttpResponse res;

    UrlParts parts;
    if (!Crack(url, parts)) {
        res.error = L"URL 格式错误";
        return res;
    }

    HINTERNET conn = WinHttpConnect(Session(), parts.host.c_str(), parts.port, 0);
    if (!conn) {
        res.error = L"连接失败 (0x" + std::to_wstring(GetLastError()) + L")";
        return res;
    }

    HINTERNET req = WinHttpOpenRequest(conn, verb.c_str(), parts.path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       parts.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        WinHttpCloseHandle(conn);
        res.error = L"创建请求失败";
        return res;
    }

    // 关闭自动重定向，自己处理（需要读取 Location）
    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));

    // 忽略证书错误（部分镜像/CDN 证书链不全）
    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
    WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

    std::wstring headers = L"Accept: */*\r\n";
    headers += L"Accept-Language: zh-CN,zh;q=0.9\r\n";
    headers += L"User-Agent: " + Utf8ToWide(userAgent) + L"\r\n";
    if (!referer.empty()) headers += L"Referer: " + Utf8ToWide(referer) + L"\r\n";
    if (!cookie.empty()) headers += L"Cookie: " + Utf8ToWide(cookie) + L"\r\n";
    if (!contentType.empty()) headers += L"Content-Type: " + Utf8ToWide(contentType) + L"\r\n";

    BOOL sent = WinHttpSendRequest(req, headers.c_str(), (DWORD)headers.size(),
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                   (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
        res.error = L"网络请求失败 (" + std::to_wstring(GetLastError()) + L")";
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        return res;
    }

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    res.status = (int)status;

    auto queryHeader = [&](DWORD flag, std::string& out) {
        DWORD sz = 0;
        WinHttpQueryHeaders(req, flag, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &sz, WINHTTP_NO_HEADER_INDEX);
        if (sz == 0) return;
        std::wstring buf(sz / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(req, flag, WINHTTP_HEADER_NAME_BY_INDEX, &buf[0], &sz, WINHTTP_NO_HEADER_INDEX)) {
            buf.resize(wcslen(buf.c_str()));
            out = WideToUtf8(buf);
        }
    };

    queryHeader(WINHTTP_QUERY_LOCATION, res.location);
    queryHeader(WINHTTP_QUERY_CONTENT_TYPE, res.contentType);
    queryHeader(WINHTTP_QUERY_SET_COOKIE, res.setCookie);

    // 读取响应体
    std::string data;
    DWORD avail = 0;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) break;
        if (avail == 0) break;
        size_t oldSize = data.size();
        data.resize(oldSize + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, &data[oldSize], avail, &read)) {
            data.resize(oldSize);
            break;
        }
        data.resize(oldSize + read);
    } while (avail > 0);

    res.body = std::move(data);

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return res;
}

HttpResponse Http::Request(const std::wstring& url, bool followRedirect)
{
    std::wstring cur = url;
    for (int i = 0; i < 5; ++i) {
        HttpResponse res = Once(cur, L"GET", std::string(), std::string());
        if (!res.error.empty()) return res;
        if (followRedirect && res.status >= 300 && res.status < 400 && !res.location.empty()) {
            std::wstring next = Utf8ToWide(res.location);
            if (next.rfind(L"http", 0) != 0) {
                // 相对跳转，拼成绝对地址
                UrlParts p;
                if (Crack(cur, p)) {
                    std::wstring base = (p.secure ? L"https://" : L"http://") + p.host;
                    next = base + (next[0] == L'/' ? L"" : L"/") + next;
                }
            }
            cur = next;
            continue;
        }
        res.finalUrl = cur;
        return res;
    }
    HttpResponse res;
    res.error = L"重定向次数过多";
    return res;
}

HttpResponse Http::Get(const std::wstring& url, bool followRedirect)
{
    return Request(url, followRedirect);
}

HttpResponse Http::PostJson(const std::wstring& url, const std::string& json)
{
    return Once(url, L"POST", json, "application/json;charset=UTF-8");
}

DownloadResult Http::Download(const std::wstring& url, const std::wstring& path, const ProgressFn& cb)
{
    bool rangeRejected = false;
    DownloadResult res = DownloadImpl(url, path, cb, true, rangeRejected);

    // 416：本地 .part 与本次资源不匹配（换画质/换 CDN 后长度变短），
    // 直接丢掉残留文件从头下，而不是把错误抛给用户
    if (rangeRejected) {
        DeleteFileW((path + L".part").c_str());
        rangeRejected = false;
        res = DownloadImpl(url, path, cb, false, rangeRejected);
        if (res.ok && res.error.empty()) res.error.clear();
    }
    return res;
}

DownloadResult Http::DownloadImpl(const std::wstring& url, const std::wstring& path, const ProgressFn& cb,
                                  bool allowResume, bool& rangeRejected)
{
    DownloadResult res;
    rangeRejected = false;

    std::wstring partPath = path + L".part";

    // 已下载字节数（断点续传）
    int64_t already = 0;
    if (allowResume) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(partPath.c_str(), GetFileExInfoStandard, &fad)) {
            already = ((int64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        }
    }

    UrlParts parts;
    if (!Crack(url, parts)) {
        res.error = L"下载地址错误";
        return res;
    }

    HINTERNET conn = WinHttpConnect(Session(), parts.host.c_str(), parts.port, 0);
    if (!conn) {
        res.error = L"连接失败";
        return res;
    }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", parts.path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       parts.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        WinHttpCloseHandle(conn);
        res.error = L"创建请求失败";
        return res;
    }

    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
    WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

    std::wstring headers = L"Accept: */*\r\n";
    headers += L"User-Agent: " + Utf8ToWide(userAgent) + L"\r\n";
    headers += L"Referer: " + Utf8ToWide(referer) + L"\r\n";
    if (!cookie.empty()) headers += L"Cookie: " + Utf8ToWide(cookie) + L"\r\n";
    if (already > 0) {
        headers += L"Range: bytes=" + std::to_wstring(already) + L"-\r\n";
    }

    BOOL sent = WinHttpSendRequest(req, headers.c_str(), (DWORD)headers.size(),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
        res.error = L"网络请求失败";
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        return res;
    }

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

    // 206: 续传成功；200: 服务器不支持 Range，从头来；416: 本地片段越界
    bool append = false;
    if (status == 206) {
        append = true;
    } else if (status == 200) {
        already = 0;
    } else if (status == 416 && already > 0) {
        rangeRejected = true;
        res.error = L"续传位置越界";
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        return res;
    } else {
        res.error = L"下载失败, HTTP " + std::to_wstring(status);
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        return res;
    }

    // 总长度
    int64_t total = -1;
    {
        DWORD sz = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        if (sz > 0) {
            wchar_t buf[64]{};
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buf, &sz,
                                    WINHTTP_NO_HEADER_INDEX)) {
                total = _wtoi64(buf);
            }
        }
    }
    int64_t fullTotal = (total > 0 ? total + already : -1);

    HANDLE file = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        res.error = L"无法写入文件，可能被占用或无权限";
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        return res;
    }
    if (append) SetFilePointer(file, 0, nullptr, FILE_END);

    int64_t done = already;
    bool canceled = false;
    bool ioError = false;

    std::vector<char> chunk(64 * 1024);
    DWORD avail = 0;
    for (;;) {
        avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) break;
        if (avail == 0) break;

        DWORD want = avail > chunk.size() ? (DWORD)chunk.size() : avail;
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), want, &read) || read == 0) break;

        DWORD written = 0;
        if (!WriteFile(file, chunk.data(), read, &written, nullptr) || written != read) {
            ioError = true;
            break;
        }
        done += read;

        if (cb && !cb(done, fullTotal)) {
            canceled = true;
            break;
        }
    }

    CloseHandle(file);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);

    if (canceled) {
        res.canceled = true;
        res.total = fullTotal;
        res.error = L"已取消";
        return res;
    }
    if (ioError) {
        res.error = L"写入磁盘失败（磁盘空间不足？）";
        res.total = fullTotal;
        return res;
    }
    if (fullTotal > 0 && done < fullTotal) {
        res.error = L"连接中断，可重新开始继续下载";
        res.total = fullTotal;
        return res;
    }

    DeleteFileW(path.c_str());
    if (!MoveFileW(partPath.c_str(), path.c_str())) {
        res.error = L"重命名文件失败";
        res.total = fullTotal;
        return res;
    }

    res.ok = true;
    res.total = done;
    return res;
}
