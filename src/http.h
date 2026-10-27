#pragma once
#include <string>
#include <functional>
#include <cstdint>

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
std::string TrimAscii(const std::string& s);
std::wstring Trim(const std::wstring& s);

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string location;
    std::string contentType;
    std::string setCookie;   // 响应里的 Set-Cookie（多个时由 WinHTTP 拼接）
    std::wstring finalUrl;   // 跟随重定向后的最终地址
    std::wstring error;      // 网络层错误信息（非 HTTP 状态码错误）

    bool ok() const { return status >= 200 && status < 300; }
    bool isJson() const { return contentType.find("json") != std::string::npos; }
};

struct DownloadResult {
    bool ok = false;
    bool canceled = false;
    int64_t total = 0;
    std::wstring error;
};

// 进度回调：已下载/总字节（total 可能为 -1），返回 false 表示取消
using ProgressFn = std::function<bool(int64_t done, int64_t total)>;

class Http {
public:
    std::string userAgent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/120.0.0.0 Safari/537.36";
    std::string referer = "https://www.bilibili.com";
    std::string cookie;

    HttpResponse Get(const std::wstring& url, bool followRedirect = false);
    HttpResponse PostJson(const std::wstring& url, const std::string& json);

    // 下载到文件（支持 .part 断点续传）
    DownloadResult Download(const std::wstring& url, const std::wstring& path, const ProgressFn& cb);

private:
    HttpResponse Request(const std::wstring& url, bool followRedirect);
    HttpResponse Once(const std::wstring& url, const std::wstring& verb, const std::string& body,
                      const std::string& contentType);
    // allowResume=false 时忽略已存在的 .part；rangeRejected 标记服务端返回 416
    DownloadResult DownloadImpl(const std::wstring& url, const std::wstring& path, const ProgressFn& cb,
                                bool allowResume, bool& rangeRejected);
};
