#include "wbi.h"
#include "json.h"
#include "md5.h"
#include <windows.h>
#include <algorithm>
#include <ctime>

using mini::Json;

// wbi 混淆表（官方前端写死）
static const int kMixinKeyEncTab[64] = {
    46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35, 27, 43, 5,  49, 33, 9,  42, 19,
    29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  1,  60, 51, 30, 4,
    22, 25, 54, 21, 56, 59, 6,  63, 57, 62, 11, 36, 20, 34, 44, 52};

// RFC 3986 百分号编码：仅保留 A-Za-z0-9-_.~
static std::string UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// wbi 要求过滤掉 '!()* 这几个字符
static std::string FilterValue(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '!' || c == '\'' || c == '(' || c == ')' || c == '*') continue;
        out += c;
    }
    return out;
}

bool WbiSigner::EnsureKeys(Http& http, std::wstring& err)
{
    unsigned long long now = GetTickCount64();
    if (!mixin_.empty() && now - fetched_ < 30ull * 60 * 1000) return true;  // 30 分钟内复用

    HttpResponse r = http.Get(L"https://api.bilibili.com/x/web-interface/nav");
    if (!r.error.empty()) {
        err = r.error;
        return false;
    }
    Json root;
    if (!Json::Parse(r.body, root)) {
        err = L"nav 接口返回解析失败";
        return false;
    }
    // 未登录时 code = -101，但 wbi_img 依然会返回，可正常使用
    const Json& wbi = root["data"]["wbi_img"];
    std::string img = wbi["img_url"].asString();
    std::string sub = wbi["sub_url"].asString();
    if (img.empty() || sub.empty()) {
        err = L"未能取得 wbi 密钥（nav 接口未返回 wbi_img）";
        return false;
    }

    auto keyOf = [](std::string url) {
        size_t slash = url.find_last_of('/');
        if (slash != std::string::npos) url = url.substr(slash + 1);
        size_t dot = url.find('.');
        if (dot != std::string::npos) url = url.substr(0, dot);
        return url;
    };
    std::string raw = keyOf(img) + keyOf(sub);
    if (raw.size() < 64) {
        err = L"wbi 密钥长度异常";
        return false;
    }
    std::string mixin;
    mixin.reserve(32);
    for (int i = 0; i < 32; ++i) mixin += raw[kMixinKeyEncTab[i]];

    mixin_ = mixin;
    fetched_ = now;
    return true;
}

bool WbiSigner::Sign(Http& http, std::vector<std::pair<std::string, std::string>> params, std::string& query,
                     std::wstring& err)
{
    if (!EnsureKeys(http, err)) return false;

    params.emplace_back("wts", std::to_string((long long)time(nullptr)));
    std::sort(params.begin(), params.end(),
              [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
                  return a.first < b.first;
              });

    std::string q;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) q += '&';
        q += UrlEncode(params[i].first);
        q += '=';
        q += UrlEncode(FilterValue(params[i].second));
    }

    query = q + "&w_rid=" + Md5Hex(q + mixin_);
    return true;
}
