// 扫码登录：passport 的 qrcode/generate + qrcode/poll，二维码由本项目的 qr.cpp 现场生成
#define NOMINMAX
#include "login.h"
#include "http.h"
#include "json.h"
#include "qr.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using mini::Json;

namespace {

#define WM_QR_DATA (WM_APP + 101)
#define WM_QR_STATUS (WM_APP + 102)
#define WM_QR_DONE (WM_APP + 103)

const wchar_t* kQrClass = L"BiliQrLoginWnd";

struct QrData {
    int size = 0;
    std::vector<unsigned char> cells;
};

struct QrDone {
    bool ok = false;
    std::string cookie;
    std::wstring err;
};

std::atomic<bool> g_cancel{false};
QrData* g_qr = nullptr;               // 仅 UI 线程访问
HFONT g_font = nullptr;
HFONT g_fontSmall = nullptr;
std::wstring g_status = L"正在获取二维码...";
std::wstring g_hint = L"请用「哔哩哔哩」手机 App 扫码";
bool g_finished = false;
QrDone g_result;
HWND g_btnClose = nullptr;
int g_dpi = 96;
RECT g_qrBox{};

inline int S(int v) { return MulDiv(v, g_dpi, 96); }

int DpiOf(HWND h)
{
    int dpi = 96;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef UINT(WINAPI * Fn)(HWND);
    auto fn = (Fn)GetProcAddress(u32, "GetDpiForWindow");
    if (fn && h) {
        UINT d = fn(h);
        if (d >= 96) dpi = (int)d;
    }
    return dpi;
}

int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string UrlDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = HexVal(s[i + 1]), l = HexVal(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out += (char)(h * 16 + l);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// 从 "a=1&b=2" 里取某个键的值
std::string QueryValue(const std::string& query, const std::string& key)
{
    std::string k = key + "=";
    size_t pos = 0;
    while ((pos = query.find(k, pos)) != std::string::npos) {
        if (pos == 0 || query[pos - 1] == '&' || query[pos - 1] == '?') {
            size_t v = pos + k.size();
            size_t end = query.find('&', v);
            return UrlDecode(query.substr(v, end == std::string::npos ? std::string::npos : end - v));
        }
        pos += k.size();
    }
    return std::string();
}

// 从 Set-Cookie 文本里取某个 Cookie 的值
std::string CookieValue(const std::string& blob, const std::string& name)
{
    std::string k = name + "=";
    size_t pos = 0;
    while ((pos = blob.find(k, pos)) != std::string::npos) {
        bool boundary = (pos == 0) || blob[pos - 1] == ';' || blob[pos - 1] == ' ' || blob[pos - 1] == ',';
        if (boundary) {
            size_t v = pos + k.size();
            size_t end = blob.find_first_of(";,", v);
            return blob.substr(v, end == std::string::npos ? std::string::npos : end - v);
        }
        pos += k.size();
    }
    return std::string();
}

std::string BuildCookie(const std::string& redirectUrl, const std::string& setCookie)
{
    std::string query = redirectUrl;
    size_t q = query.find('?');
    if (q != std::string::npos) query = query.substr(q + 1);

    auto pick = [&](const char* name) {
        std::string v = QueryValue(query, name);
        if (v.empty()) v = CookieValue(setCookie, name);
        return v;
    };

    const char* names[] = {"SESSDATA", "bili_jct", "DedeUserID", "DedeUserID__ckMd5"};
    std::string cookie;
    for (const char* n : names) {
        std::string v = pick(n);
        if (v.empty()) continue;
        if (!cookie.empty()) cookie += "; ";
        cookie += std::string(n) + "=" + v;
    }
    return cookie;
}

void PostStatus(HWND hwnd, const std::wstring& s)
{
    auto* p = new std::wstring(s);
    if (!PostMessageW(hwnd, WM_QR_STATUS, 0, (LPARAM)p)) delete p;
}

void PostDone(HWND hwnd, bool ok, const std::string& cookie, const std::wstring& err)
{
    auto* d = new QrDone{ok, cookie, err};
    if (!PostMessageW(hwnd, WM_QR_DONE, 0, (LPARAM)d)) delete d;
}

void LoginWorker(HWND hwnd)
{
    Http http;   // 独立会话：不带上主界面里已有的 Cookie

    HttpResponse r = http.Get(L"https://passport.bilibili.com/x/passport-login/web/qrcode/generate");
    if (g_cancel) return;
    if (!r.error.empty()) {
        PostDone(hwnd, false, std::string(), L"网络请求失败：" + r.error);
        return;
    }
    Json root;
    if (!Json::Parse(r.body, root)) {
        PostDone(hwnd, false, std::string(), L"二维码接口返回解析失败");
        return;
    }
    std::string url = root["data"]["url"].asString();
    std::string key = root["data"]["qrcode_key"].asString();
    if (url.empty() || key.empty()) {
        PostDone(hwnd, false, std::string(), L"接口未返回二维码（code=" +
                                                std::to_wstring(root["code"].asInt()) + L"）");
        return;
    }

    qr::Matrix m;
    if (!qr::Encode(url, m)) {
        PostDone(hwnd, false, std::string(), L"二维码生成失败");
        return;
    }

    auto* data = new QrData();
    data->size = m.size;
    data->cells = m.cells;
    if (!PostMessageW(hwnd, WM_QR_DATA, 0, (LPARAM)data)) delete data;

    PostStatus(hwnd, L"等待扫码...");

    // 轮询：86101 未扫码 / 86090 已扫码未确认 / 86038 已失效 / 0 成功
    for (int i = 0; i < 180; ++i) {
        for (int k = 0; k < 10 && !g_cancel; ++k) Sleep(100);
        if (g_cancel) return;

        std::wstring pu = L"https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=" +
                          Utf8ToWide(key);
        HttpResponse pr = http.Get(pu);
        if (g_cancel) return;
        if (!pr.error.empty()) {
            PostStatus(hwnd, L"网络异常，正在重试...");
            continue;
        }
        Json j;
        if (!Json::Parse(pr.body, j)) continue;
        if (j["code"].asInt() != 0) {
            PostStatus(hwnd, L"接口返回异常（code=" + std::to_wstring(j["code"].asInt()) + L"）");
            continue;
        }

        int code = j["data"]["code"].asInt();
        if (code == 86101) {
            PostStatus(hwnd, L"等待扫码...");
        } else if (code == 86090) {
            PostStatus(hwnd, L"已扫码，请在手机上点击「确认登录」");
        } else if (code == 86038) {
            PostDone(hwnd, false, std::string(), L"二维码已失效，请重新打开登录窗口");
            return;
        } else if (code == 0) {
            std::string redirect = j["data"]["url"].asString();
            std::string cookie = BuildCookie(redirect, pr.setCookie);
            if (cookie.find("SESSDATA=") == std::string::npos) {
                PostDone(hwnd, false, std::string(), L"登录成功但未能取到 SESSDATA");
                return;
            }
            PostDone(hwnd, true, cookie, std::wstring());
            return;
        } else {
            PostStatus(hwnd, L"等待确认...（code=" + std::to_wstring(code) + L"）");
        }
    }

    if (!g_cancel) PostDone(hwnd, false, std::string(), L"二维码已过期，请重新打开登录窗口");
}

void PaintQr(HDC dc, const RECT& client)
{
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &client, white);

    if (g_qr && g_qr->size > 0) {
        int avail = std::min((int)(client.right - client.left) - S(40), (int)(client.bottom - client.top) - S(150));
        if (avail < S(120)) avail = S(120);
        int scale = avail / g_qr->size;
        if (scale < 1) scale = 1;
        int px = scale * g_qr->size;
        int left = (client.right - px) / 2;
        int top = S(18);

        RECT frame{left - S(10), top - S(10), left + px + S(10), top + px + S(10)};
        HBRUSH border = CreateSolidBrush(RGB(225, 228, 235));
        FillRect(dc, &frame, border);
        RECT inner{left - S(8), top - S(8), left + px + S(8), top + px + S(8)};
        FillRect(dc, &inner, white);
        DeleteObject(border);

        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        for (int y = 0; y < g_qr->size; ++y) {
            for (int x = 0; x < g_qr->size; ++x) {
                if (!g_qr->cells[(size_t)y * g_qr->size + x]) continue;
                RECT r{left + x * scale, top + y * scale, left + (x + 1) * scale, top + (y + 1) * scale};
                FillRect(dc, &r, black);
            }
        }
        g_qrBox = frame;
    }
    DeleteObject(white);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(32, 36, 44));
    SelectObject(dc, g_font);
    RECT tr{S(20), g_qrBox.bottom + S(14), client.right - S(20), g_qrBox.bottom + S(46)};
    DrawTextW(dc, g_status.c_str(), -1, &tr, DT_CENTER | DT_WORDBREAK);

    SelectObject(dc, g_fontSmall);
    SetTextColor(dc, RGB(120, 126, 138));
    RECT hr{S(20), g_qrBox.bottom + S(48), client.right - S(20), client.bottom - S(52)};
    DrawTextW(dc, g_hint.c_str(), -1, &hr, DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL);
}

LRESULT CALLBACK QrWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_dpi = DpiOf(GetParent(hwnd));
        g_font = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        g_fontSmall = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        RECT rc;
        GetClientRect(hwnd, &rc);
        g_btnClose = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     rc.right / 2 - S(40), rc.bottom - S(44), S(80), S(28), hwnd,
                                     (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_btnClose, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
        SetFocus(g_btnClose);
        g_qrBox = RECT{S(20), S(8), rc.right - S(20), S(160)};
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        PaintQr(dc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_QR_DATA: {
        QrData* d = (QrData*)lp;
        if (g_qr != d) {
            delete g_qr;
            g_qr = d;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_QR_STATUS: {
        std::wstring* s = (std::wstring*)lp;
        g_status = *s;
        delete s;
        if (g_finished) g_hint = L"";
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_QR_DONE: {
        QrDone* d = (QrDone*)lp;
        g_result = *d;
        delete d;
        g_finished = true;
        if (g_result.ok) {
            g_status = L"登录成功";
            g_hint = L"Cookie 已自动填入";
        } else {
            g_status = g_result.err;
            g_hint = L"";
        }
        EnableWindow(g_btnClose, TRUE);
        InvalidateRect(hwnd, nullptr, FALSE);
        SetTimer(hwnd, 1, 700, nullptr);
        return 0;
    }

    case WM_TIMER:
        KillTimer(hwnd, 1);
        DestroyWindow(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) {
            g_cancel = true;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            g_cancel = true;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_CLOSE:
        g_cancel = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_cancel = true;
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        if (g_fontSmall) { DeleteObject(g_fontSmall); g_fontSmall = nullptr; }
        delete g_qr;
        g_qr = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool QrLogin(HWND parent, std::string& cookieOut, std::wstring& err)
{
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = QrWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.lpszClassName = kQrClass;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        registered = true;
    }

    g_cancel = false;
    g_finished = false;
    g_result = QrDone();
    g_status = L"正在获取二维码...";
    g_hint = L"请用「哔哩哔哩」手机 App 扫码";
    g_dpi = DpiOf(parent);

    int w = S(380), h = S(500);
    RECT pr{};
    if (parent) GetWindowRect(parent, &pr);
    else {
        pr.right = GetSystemMetrics(SM_CXSCREEN);
        pr.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    if (parent) EnableWindow(parent, FALSE);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kQrClass, L"扫码登录哔哩哔哩",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, parent, nullptr, inst, nullptr);
    if (!hwnd) {
        if (parent) EnableWindow(parent, TRUE);
        err = L"无法创建登录窗口";
        return false;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    std::thread(LoginWorker, hwnd).detach();

    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_cancel = true;
    if (parent) {
        EnableWindow(parent, TRUE);
        SetActiveWindow(parent);
    }

    if (g_result.ok) {
        cookieOut = g_result.cookie;
        return true;
    }
    err = g_result.err.empty() ? L"已取消登录" : g_result.err;
    return false;
}
