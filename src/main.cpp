// BiliDownloader - Win32 原生界面版
// 依赖：仅 Windows 系统库（WinHTTP / Common Controls）

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <cstdio>

#include "bili.h"
#include "download.h"
#include "login.h"
#include "queue.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------- 控件 ID ----------------
enum : int {
    IDC_URL = 1001,
    IDC_PARSE,
    IDC_COOKIE,
    IDC_DIR,
    IDC_BROWSE,
    IDC_LIST,
    IDC_SELALL,
    IDC_QN,
    IDC_DOWNLOAD,
    IDC_STOP,
    IDC_PROGRESS,
    IDC_STATUS,
    IDC_LOG,
    IDC_LOGIN,
    IDC_CONC,
};

// ---------------- 自定义消息 ----------------
#define WM_PARSE_DONE (WM_APP + 1)
#define WM_LOG_MSG (WM_APP + 2)

// 界面刷新定时器：轮询队列状态，避免每块数据都跨线程发消息
#define TIMER_QUEUE 1

struct ParseMsg {
    bool ok = false;
    std::wstring error;
    MediaInfo media;
    std::vector<Quality> qualities;
};

struct LogMsg {
    std::wstring text;
};

// ---------------- 全局 ----------------
static HWND g_hwnd = nullptr;
static HWND g_url = nullptr, g_login = nullptr, g_parse = nullptr, g_cookie = nullptr, g_dir = nullptr,
            g_browse = nullptr;
static HWND g_list = nullptr, g_selAll = nullptr, g_qn = nullptr, g_download = nullptr, g_stop = nullptr;
static HWND g_progress = nullptr, g_status = nullptr, g_log = nullptr;
static HWND g_lblUrl = nullptr, g_lblCookie = nullptr, g_lblDir = nullptr, g_lblQn = nullptr,
            g_lblConc = nullptr;
static HWND g_conc = nullptr;
static HINSTANCE g_hInst = nullptr;
static HFONT g_font = nullptr;
static HBRUSH g_bgBrush = nullptr;

static MediaInfo g_media;
static std::atomic<bool> g_parsing{false};
static DownloadQueue g_queue;
static bool g_downloading = false;
static std::vector<std::shared_ptr<DownloadTask>> g_rowTasks;   // 列表行 -> 任务
static std::vector<std::wstring> g_rowText;                     // 行状态文本缓存，避免重复刷新

// ---------------- 小工具 ----------------
static int g_dpi = 96;
static inline int S(int v) { return MulDiv(v, g_dpi, 96); }

static std::wstring ExeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t p = s.find_last_of(L"\\/");
    if (p != std::wstring::npos) s.resize(p + 1);
    return s;
}

static std::wstring IniPath() { return ExeDir() + L"config.ini"; }

static std::wstring GetText(HWND h)
{
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return std::wstring();
    std::wstring s((size_t)n + 1, L'\0');
    int got = GetWindowTextW(h, &s[0], n + 1);
    if (got <= 0) return std::wstring();
    s.resize((size_t)got);
    return s;
}

static void SetText(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }

static std::wstring DefaultDownloadDir()
{
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) {
        dir = p;
        CoTaskMemFree(p);
    }
    if (dir.empty()) dir = ExeDir() + L"Downloads";
    dir += L"\\BiliVideo";
    return dir;
}

static void AppendLog(const std::wstring& text)
{
    if (!g_log) return;
    int len = GetWindowTextLengthW(g_log);
    if (len > 200000) {  // 防止无限增长
        SetWindowTextW(g_log, L"");
        len = 0;
    }
    SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring line = text + L"\r\n";
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

// 供后台线程调用（跨线程安全）
static void PostLog(HWND hwnd, const std::wstring& text)
{
    auto* m = new LogMsg{text};
    if (!PostMessageW(hwnd, WM_LOG_MSG, 0, (LPARAM)m)) delete m;
}

static void SetStatus(const std::wstring& s) { SetText(g_status, s); }

static void SetRowText(int row, int sub, const std::wstring& text)
{
    if (row < 0) return;
    if (row >= (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0)) return;
    ListView_SetItemText(g_list, row, sub, (LPWSTR)text.c_str());
}

static const wchar_t* StateName(int st)
{
    switch (st) {
    case TS_QUEUED: return L"排队中";
    case TS_RUNNING: return L"下载中";
    case TS_DONE: return L"完成";
    case TS_FAILED: return L"失败";
    case TS_CANCELED: return L"已取消";
    default: return L"待下载";
    }
}

// ---------------- 列表操作 ----------------
static void FillList(const MediaInfo& media)
{
    SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
    g_rowTasks.assign(media.eps.size(), nullptr);
    g_rowText.assign(media.eps.size(), std::wstring());
    for (size_t i = 0; i < media.eps.size(); ++i) {
        wchar_t idx[16];
        swprintf(idx, 16, L"%d", (int)i + 1);

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = idx;
        SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&item);
        ListView_SetItemText(g_list, (int)i, 1, (LPWSTR)media.eps[i].title.c_str());

        std::wstring dur = FormatDuration(media.eps[i].durationSec);
        if (media.eps[i].locked) dur += L" 会员";
        ListView_SetItemText(g_list, (int)i, 2, (LPWSTR)dur.c_str());
        ListView_SetItemText(g_list, (int)i, 3, (LPWSTR)L"-");
        ListView_SetItemText(g_list, (int)i, 4, (LPWSTR)L"待下载");
    }
}

static void FillQualities(const std::vector<Quality>& qs, int current)
{
    SendMessageW(g_qn, CB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (size_t i = 0; i < qs.size(); ++i) {
        std::wstring text = qs[i].desc;
        if (text.empty()) text = L"qn " + std::to_wstring(qs[i].qn);
        int idx = (int)SendMessageW(g_qn, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        SendMessageW(g_qn, CB_SETITEMDATA, idx, (LPARAM)qs[i].qn);
        if (qs[i].qn == current) sel = idx;
    }
    if (qs.empty()) {
        SendMessageW(g_qn, CB_ADDSTRING, 0, (LPARAM)L"默认");
        SendMessageW(g_qn, CB_SETITEMDATA, 0, 127);
    }
    SendMessageW(g_qn, CB_SETCURSEL, sel, 0);
}

static int SelectedQn()
{
    int idx = (int)SendMessageW(g_qn, CB_GETCURSEL, 0, 0);
    if (idx < 0) return 127;
    int qn = (int)SendMessageW(g_qn, CB_GETITEMDATA, idx, 0);
    return qn <= 0 ? 127 : qn;
}

// ---------------- 后台线程 ----------------
static void ParseThread(HWND hwnd, std::wstring input, std::string cookie)
{
    auto* msg = new ParseMsg();

    BiliApi api;
    api.http.cookie = cookie;

    if (!api.Parse(input, msg->media, msg->error)) {
        msg->ok = false;
        if (!PostMessageW(hwnd, WM_PARSE_DONE, 0, (LPARAM)msg)) delete msg;
        return;
    }

    // 再取一次播放信息，拿到可用画质列表
    int64_t probeId = msg->media.eps.empty() ? 0 : msg->media.eps[0].id;
    int fi = msg->media.focusIndex;
    if (fi > 0 && fi <= (int)msg->media.eps.size()) probeId = msg->media.eps[fi - 1].id;

    PlayInfo pi;
    if (api.GetPlayInfo(msg->media, probeId, 127, pi)) {
        msg->qualities = pi.qualities;
    } else if (!pi.error.empty()) {
        PostLog(hwnd, L"提示: " + pi.error);
    }

    msg->ok = true;
    if (!PostMessageW(hwnd, WM_PARSE_DONE, 0, (LPARAM)msg)) delete msg;
}


// ---------------- 队列驱动 ----------------
// 刷新进度条/状态栏/每一行的进度与状态
static void RefreshQueueUi()
{
    QueueStats s = g_queue.Stats();

    // 进度条：拿到总大小就按字节算，否则按完成个数算
    int permille = 0;
    if (s.totalBytes > 0 && s.doneBytes > 0) {
        permille = (int)(s.doneBytes * 1000 / s.totalBytes);
    } else if (s.all > 0) {
        permille = (s.ok + s.failed + s.canceled) * 1000 / s.all;
    }
    if (permille > 1000) permille = 1000;
    SendMessageW(g_progress, PBM_SETPOS, (WPARAM)permille, 0);

    // 状态栏
    std::wstring st = L"下载中 " + std::to_wstring(s.running) + L" 并发 / 共 " + std::to_wstring(s.all) +
                      L" 项｜完成 " + std::to_wstring(s.ok);
    if (s.failed) st += L"｜失败 " + std::to_wstring(s.failed);
    if (s.doneBytes > 0) st += L"｜" + FormatSize(s.doneBytes);
    if (s.totalBytes > 0) st += L" / " + FormatSize(s.totalBytes);
    if (s.queued > 0) st += L"｜排队 " + std::to_wstring(s.queued);
    SetStatus(st);

    // 每行
    for (size_t i = 0; i < g_rowTasks.size(); ++i) {
        auto& t = g_rowTasks[i];
        if (!t) continue;
        int st2 = t->state.load();
        std::wstring prog;
        if (st2 == TS_RUNNING) {
            long long done = t->done.load(), total = t->total.load();
            if (total > 0) {
                prog = std::to_wstring((int)(done * 100 / total)) + L"%";
                prog += L"  " + FormatSize(done);
            } else {
                prog = FormatSize(done);
            }
        } else if (st2 == TS_DONE) {
            prog = L"100%";
        } else if (st2 == TS_FAILED || st2 == TS_CANCELED) {
            prog = L"-";
        } else {
            prog = L"-";
        }

        std::wstring stateText = StateName(st2);
        std::wstring key = prog + L"|" + stateText;
        if (key == g_rowText[i]) continue;      // 没变化就不刷，避免闪烁
        g_rowText[i] = key;
        SetRowText((int)i, 3, prog);
        SetRowText((int)i, 4, stateText);
    }

    if (!g_queue.Busy()) {
        KillTimer(g_hwnd, TIMER_QUEUE);
        g_downloading = false;
        EnableWindow(g_parse, TRUE);
        EnableWindow(g_download, TRUE);
        EnableWindow(g_stop, FALSE);

        SendMessageW(g_progress, PBM_SETPOS, (WPARAM)(s.failed || s.canceled ? 0 : 1000), 0);
        std::wstring fin;
        if (s.canceled > 0) {
            fin = L"已停止。成功 " + std::to_wstring(s.ok) + L" 项，失败 " + std::to_wstring(s.failed) +
                  L" 项，取消 " + std::to_wstring(s.canceled) + L" 项";
        } else {
            fin = L"完成。成功 " + std::to_wstring(s.ok) + L" 项，失败 " + std::to_wstring(s.failed) + L" 项";
            if (s.totalBytes > 0) fin += L"，共 " + FormatSize(s.totalBytes);
        }
        SetStatus(fin);
        AppendLog(L"==== " + fin + L" ====");

        if (s.failed > 0) {
            MessageBoxW(g_hwnd, (fin + L"\n详情见下方日志。").c_str(), L"下载结束", MB_ICONWARNING);
        }
    }
}


// ---------------- 命令处理 ----------------
static std::string CurrentCookie()
{
    std::string cookie = WideToUtf8(Trim(GetText(g_cookie)));
    if (!cookie.empty() && cookie.find('=') == std::string::npos) cookie = "SESSDATA=" + cookie;
    return cookie;
}

static void StartParse()
{
    std::wstring input = GetText(g_url);
    if (Trim(input).empty()) {
        SetStatus(L"请先粘贴链接或输入 BV 号");
        AppendLog(L"请先粘贴视频链接或输入 BV 号。");
        SetFocus(g_url);
        return;
    }
    if (g_parsing.load()) return;

    g_parsing = true;
    EnableWindow(g_parse, FALSE);
    EnableWindow(g_download, FALSE);
    SetStatus(L"正在解析...");
    AppendLog(L"解析: " + Trim(input));

    std::thread(ParseThread, g_hwnd, Trim(input), CurrentCookie()).detach();
}

static void StartDownload()
{
    if (g_parsing.load()) return;
    if (g_downloading) return;
    if (g_media.eps.empty()) {
        MessageBoxW(g_hwnd, L"请先解析视频。", L"提示", MB_ICONINFORMATION);
        return;
    }

    std::vector<int> indices;
    int count = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < count; ++i) {
        if (ListView_GetCheckState(g_list, i)) indices.push_back(i);
    }
    if (indices.empty()) {
        MessageBoxW(g_hwnd, L"请至少勾选一个分P/剧集。", L"提示", MB_ICONINFORMATION);
        return;
    }

    std::wstring dir = Trim(GetText(g_dir));
    if (dir.empty()) {
        dir = DefaultDownloadDir();
        SetText(g_dir, dir);
    }
    if (!EnsureDirectory(dir)) {
        MessageBoxW(g_hwnd, L"保存目录无法创建，请检查路径。", L"错误", MB_ICONERROR);
        return;
    }

    const std::string cookie = CurrentCookie();
    const int qn = SelectedQn();

    // 空闲时重置上一轮的队列
    if (!g_queue.Busy()) {
        g_queue.Reset();
        g_rowTasks.assign(g_media.eps.size(), nullptr);
        g_rowText.assign(g_media.eps.size(), std::wstring());
        for (int i = 0; i < count; ++i) {
            SetRowText(i, 3, L"-");
            SetRowText(i, 4, L"待下载");
        }
    }

    g_downloading = true;
    EnableWindow(g_download, FALSE);
    EnableWindow(g_parse, FALSE);
    EnableWindow(g_stop, TRUE);
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);

    int conc = g_queue.Concurrency();
    AppendLog(L"==== 开始下载，共 " + std::to_wstring(indices.size()) + L" 项，并发 " +
              std::to_wstring(conc) + L" ====");

    for (int idx : indices) {
        const Episode& ep = g_media.eps[idx];

        auto t = std::make_shared<DownloadTask>();
        t->media = g_media;
        t->ep = ep;
        t->qn = qn;
        t->dir = dir;
        t->cookie = cookie;
        t->display = ep.title.empty() ? g_media.title : ep.title;

        // 文件名：多集时带上集名，避免同名覆盖
        t->fileNameBase = g_media.title;
        if (g_media.eps.size() > 1) t->fileNameBase += L" - " + ep.title;

        g_queue.Add(t);
        if (idx < (int)g_rowTasks.size()) g_rowTasks[idx] = t;
    }

    SetTimer(g_hwnd, TIMER_QUEUE, 120, nullptr);
    RefreshQueueUi();
}

static void StartLogin()
{
    if (g_downloading) {
        MessageBoxW(g_hwnd, L"下载进行中，请先停止再登录。", L"提示", MB_ICONINFORMATION);
        return;
    }

    std::string ck;
    std::wstring err;
    if (QrLogin(g_hwnd, ck, err)) {
        SetText(g_cookie, Utf8ToWide(ck));
        WritePrivateProfileStringW(L"settings", L"cookie", GetText(g_cookie).c_str(), IniPath().c_str());
        AppendLog(L"登录成功，Cookie 已保存到 config.ini。");
        SetStatus(L"登录成功，重新「解析」即可看到会员画质");
    } else {
        AppendLog(L"登录未完成：" + err);
        SetStatus(L"登录未完成：" + err);
    }
}

// ---------------- 布局 ----------------
static void DoLayout(HWND hwnd)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    const int pad = S(14);
    const int rowH = S(26);
    const int btnW = S(88);
    const int labelW = S(64);
    const int gap = S(8);

    int y = pad;

    // 第 1 行：视频地址
    MoveWindow(g_lblUrl, pad, y + S(5), labelW, S(20), TRUE);
    MoveWindow(g_url, pad + labelW, y, W - pad * 2 - labelW - btnW - gap, rowH, TRUE);
    MoveWindow(g_parse, W - pad - btnW, y, btnW, rowH, TRUE);

    // 第 2 行：Cookie + 扫码登录
    y += rowH + S(6);
    MoveWindow(g_lblCookie, pad, y + S(5), labelW, S(20), TRUE);
    MoveWindow(g_cookie, pad + labelW, y, W - pad * 2 - labelW - btnW - gap, rowH, TRUE);
    MoveWindow(g_login, W - pad - btnW, y, btnW, rowH, TRUE);

    // 第 3 行：保存目录
    y += rowH + S(6);
    MoveWindow(g_lblDir, pad, y + S(5), labelW, S(20), TRUE);
    MoveWindow(g_dir, pad + labelW, y, W - pad * 2 - labelW - btnW - gap, rowH, TRUE);
    MoveWindow(g_browse, W - pad - btnW, y, btnW, rowH, TRUE);

    // 列表
    y += rowH + S(8);
    int listTop = y;
    int reserved = rowH + S(10) + S(18) + S(6) + S(20) + S(6) + S(90) + pad;
    int listH = H - listTop - reserved;
    if (listH < S(100)) listH = S(100);
    MoveWindow(g_list, pad, listTop, W - pad * 2, listH, TRUE);
    int listBottom = listTop + listH;

    // 工具行：全选 |（右起）并发、画质、开始、停止
    int ty = listBottom + S(10);
    MoveWindow(g_selAll, pad, ty + S(2), S(96), S(22), TRUE);
    MoveWindow(g_stop, W - pad - btnW, ty, btnW, rowH, TRUE);
    MoveWindow(g_download, W - pad - btnW * 2 - gap, ty, btnW, rowH, TRUE);

    int rx = W - pad - btnW * 2 - gap - S(12);
    rx -= S(132); MoveWindow(g_qn, rx, ty, S(132), S(200), TRUE);
    rx -= S(38);  MoveWindow(g_lblQn, rx, ty + S(5), S(36), S(20), TRUE);
    rx -= S(12);
    rx -= S(58);  MoveWindow(g_conc, rx, ty, S(58), S(200), TRUE);
    rx -= S(38);  MoveWindow(g_lblConc, rx, ty + S(5), S(36), S(20), TRUE);

    // 进度条
    int py = ty + rowH + S(10);
    MoveWindow(g_progress, pad, py, W - pad * 2, S(18), TRUE);

    // 状态
    int sy = py + S(18) + S(6);
    MoveWindow(g_status, pad, sy, W - pad * 2, S(20), TRUE);

    // 日志
    int ly = sy + S(20) + S(6);
    int lh = H - pad - ly;
    if (lh < S(40)) lh = S(40);
    MoveWindow(g_log, pad, ly, W - pad * 2, lh, TRUE);

    // 标题列自适应（序号 44 + 时长 110 + 进度 100 + 状态 90）
    if (g_list) {
        int titleW = (W - pad * 2) - S(44) - S(110) - S(100) - S(90) - S(8);
        if (titleW < S(180)) titleW = S(180);
        ListView_SetColumnWidth(g_list, 1, titleW);
    }
}

// ---------------- 控件创建 ----------------
static HWND MakeLabel(HWND parent, const wchar_t* text)
{
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           0, 0, 0, 0, parent, nullptr, g_hInst, nullptr);
}

static void SetAllFont(HWND parent)
{
    EnumChildWindows(
        parent,
        [](HWND child, LPARAM lp) -> BOOL {
            SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE);
            return TRUE;
        },
        (LPARAM)g_font);
}

static void InitControls(HWND hwnd)
{
    g_lblUrl = MakeLabel(hwnd, L"视频地址");
    g_lblCookie = MakeLabel(hwnd, L"Cookie");
    g_lblDir = MakeLabel(hwnd, L"保存到");
    g_lblQn = MakeLabel(hwnd, L"画质");
    g_lblConc = MakeLabel(hwnd, L"并发");

    g_url = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                            0, 0, 0, 0, hwnd, (HMENU)IDC_URL, g_hInst, nullptr);
    SendMessageW(g_url, EM_SETCUEBANNER, TRUE, (LPARAM)L"粘贴 B 站链接 / BV 号 / av 号 / ss·ep 号，回车开始解析");

    g_parse = CreateWindowExW(0, L"BUTTON", L"解析", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              0, 0, 0, 0, hwnd, (HMENU)IDC_PARSE, g_hInst, nullptr);

    g_cookie = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               0, 0, 0, 0, hwnd, (HMENU)IDC_COOKIE, g_hInst, nullptr);
    SendMessageW(g_cookie, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"可选：登录后的 SESSDATA（1080P/大会员画质需要）");

    g_login = CreateWindowExW(0, L"BUTTON", L"扫码登录", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              0, 0, 0, 0, hwnd, (HMENU)IDC_LOGIN, g_hInst, nullptr);

    g_dir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                            0, 0, 0, 0, hwnd, (HMENU)IDC_DIR, g_hInst, nullptr);
    g_browse = CreateWindowExW(0, L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               0, 0, 0, 0, hwnd, (HMENU)IDC_BROWSE, g_hInst, nullptr);

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                             0, 0, 0, 0, hwnd, (HMENU)IDC_LIST, g_hInst, nullptr);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = (LPWSTR)L"#";
    col.cx = S(44);
    SendMessageW(g_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = (LPWSTR)L"标题";
    col.cx = S(520);
    col.iSubItem = 1;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.pszText = (LPWSTR)L"时长";
    col.cx = S(110);
    col.iSubItem = 2;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.pszText = (LPWSTR)L"进度";
    col.cx = S(100);
    col.iSubItem = 3;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
    col.pszText = (LPWSTR)L"状态";
    col.cx = S(90);
    col.iSubItem = 4;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);

    g_selAll = CreateWindowExW(0, L"BUTTON", L"全选/取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               0, 0, 0, 0, hwnd, (HMENU)IDC_SELALL, g_hInst, nullptr);
    SendMessageW(g_selAll, BM_SETCHECK, BST_CHECKED, 0);

    g_qn = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                           0, 0, 0, 0, hwnd, (HMENU)IDC_QN, g_hInst, nullptr);

    g_conc = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                             0, 0, 0, 0, hwnd, (HMENU)IDC_CONC, g_hInst, nullptr);

    g_download = CreateWindowExW(0, L"BUTTON", L"开始下载", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 0, 0, 0, 0, hwnd, (HMENU)IDC_DOWNLOAD, g_hInst, nullptr);
    g_stop = CreateWindowExW(0, L"BUTTON", L"停止", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             0, 0, 0, 0, hwnd, (HMENU)IDC_STOP, g_hInst, nullptr);
    EnableWindow(g_stop, FALSE);

    g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                                 0, 0, 0, 0, hwnd, (HMENU)IDC_PROGRESS, g_hInst, nullptr);
    SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));

    g_status = CreateWindowExW(0, L"STATIC", L"就绪。粘贴链接后点「解析」。", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, g_hInst, nullptr);

    g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                            0, 0, 0, 0, hwnd, (HMENU)IDC_LOG, g_hInst, nullptr);

    SetAllFont(hwnd);
    DoLayout(hwnd);
}

// ---------------- 窗口过程 ----------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_bgBrush = CreateSolidBrush(RGB(250, 250, 252));

        g_dpi = 96;
        {
            HMODULE u32 = GetModuleHandleW(L"user32.dll");
            typedef UINT(WINAPI * GetDpiForWindowProc)(HWND);
            auto getDpi = (GetDpiForWindowProc)GetProcAddress(u32, "GetDpiForWindow");
            if (getDpi) {
                UINT d = getDpi(hwnd);
                if (d >= 96) g_dpi = (int)d;
            }
        }
        g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        InitControls(hwnd);

        // 队列日志回调（工作线程调用，必须走消息回传 UI 线程）
        g_queue.SetLog([](const std::wstring& s) { PostLog(g_hwnd, s); });

        // 并发数下拉
        {
            const int opts[] = {1, 2, 3, 4, 6, 8};
            for (int v : opts) {
                std::wstring t = std::to_wstring(v);
                int idx = (int)SendMessageW(g_conc, CB_ADDSTRING, 0, (LPARAM)t.c_str());
                SendMessageW(g_conc, CB_SETITEMDATA, idx, (LPARAM)v);
            }
        }

        // 读取配置
        wchar_t buf[1024]{};
        GetPrivateProfileStringW(L"settings", L"cookie", L"", buf, 1024, IniPath().c_str());
        if (buf[0]) SetText(g_cookie, buf);
        GetPrivateProfileStringW(L"settings", L"dir", L"", buf, 1024, IniPath().c_str());
        SetText(g_dir, buf[0] ? buf : DefaultDownloadDir());

        int conc = (int)GetPrivateProfileIntW(L"settings", L"concurrency", 3, IniPath().c_str());
        if (conc < 1) conc = 1;
        if (conc > 8) conc = 8;
        g_queue.SetConcurrency(conc);
        {
            int n = (int)SendMessageW(g_conc, CB_GETCOUNT, 0, 0);
            int sel = 2;
            for (int i = 0; i < n; ++i) {
                if ((int)SendMessageW(g_conc, CB_GETITEMDATA, i, 0) == conc) {
                    sel = i;
                    break;
                }
            }
            SendMessageW(g_conc, CB_SETCURSEL, sel, 0);
            g_queue.SetConcurrency((int)SendMessageW(g_conc, CB_GETITEMDATA, sel, 0));
        }

        AppendLog(L"准备就绪。支持：BV/av 号、bilibili.com 视频链接、番剧 ss/ep 号、b23.tv 短链。");
        AppendLog(L"画质不够高就点右上「扫码登录」，无需手动填 Cookie。");
        return 0;
    }

    case WM_SIZE:
        DoLayout(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = S(760);
        mmi->ptMinTrackSize.y = S(560);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wParam;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)g_bgBrush;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_PARSE) {
            StartParse();
        } else if (id == IDC_BROWSE) {
            BROWSEINFOW bi{};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"选择保存目录";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH]{};
                if (SHGetPathFromIDListW(pidl, path)) SetText(g_dir, path);
                CoTaskMemFree(pidl);
            }
        } else if (id == IDC_DOWNLOAD) {
            StartDownload();
        } else if (id == IDC_STOP) {
            g_queue.CancelAll();
            SetStatus(L"正在停止...");
            AppendLog(L"已请求停止，等待在途任务退出。");
        } else if (id == IDC_LOGIN) {
            StartLogin();
        } else if (id == IDC_CONC && HIWORD(wParam) == CBN_SELCHANGE) {
            int idx = (int)SendMessageW(g_conc, CB_GETCURSEL, 0, 0);
            if (idx >= 0) {
                int v = (int)SendMessageW(g_conc, CB_GETITEMDATA, idx, 0);
                g_queue.SetConcurrency(v);
                AppendLog(L"并发数已设为 " + std::to_wstring(v));
            }
        } else if (id == IDC_SELALL) {
            bool check = IsDlgButtonChecked(hwnd, IDC_SELALL) == BST_CHECKED;
            int count = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
            for (int i = 0; i < count; ++i) ListView_SetCheckState(g_list, i, check);
        }
        return 0;
    }

    case WM_PARSE_DONE: {
        std::unique_ptr<ParseMsg> m((ParseMsg*)lParam);
        g_parsing = false;
        EnableWindow(g_parse, TRUE);
        if (!g_downloading) EnableWindow(g_download, TRUE);

        if (!m->ok) {
            SetStatus(L"解析失败：" + m->error);
            AppendLog(L"解析失败: " + m->error);
            return 0;
        }

        g_media = m->media;
        FillList(g_media);

        // 默认勾选：番剧全选，投稿视频只勾当前分P
        bool isPgc = (g_media.kind == MediaKind::Pgc);
        int fi = g_media.focusIndex > 0 ? g_media.focusIndex - 1 : 0;
        for (size_t i = 0; i < g_media.eps.size(); ++i) {
            bool check = isPgc || (int)i == fi;
            ListView_SetCheckState(g_list, (int)i, check);
        }
        if (fi >= 0 && fi < (int)g_media.eps.size()) {
            ListView_SetItemState(g_list, fi, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_list, fi, FALSE);
        }

        FillQualities(m->qualities, m->qualities.empty() ? 0 : m->qualities[0].qn);

        AppendLog(L"标题: " + g_media.title);
        AppendLog(L"共 " + std::to_wstring(g_media.eps.size()) + L" 个分P/剧集，可用画质 " +
                  std::to_wstring(m->qualities.size()) + L" 种");
        SetStatus(L"解析完成：" + g_media.title);
        return 0;
    }

    case WM_LOG_MSG: {
        std::unique_ptr<LogMsg> m((LogMsg*)lParam);
        AppendLog(m->text);
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_QUEUE) {
            RefreshQueueUi();
            return 0;
        }
        break;

    case WM_CLOSE: {
        if (g_parsing.load() || g_downloading) {
            if (MessageBoxW(hwnd, L"仍在解析/下载中，确定退出吗？", L"确认", MB_ICONQUESTION | MB_YESNO) != IDYES)
                return 0;
            g_queue.CancelAll();
        }
        WritePrivateProfileStringW(L"settings", L"cookie", GetText(g_cookie).c_str(), IniPath().c_str());
        WritePrivateProfileStringW(L"settings", L"dir", GetText(g_dir).c_str(), IniPath().c_str());
        wchar_t num[16];
        swprintf(num, 16, L"%d", g_queue.Concurrency());
        WritePrivateProfileStringW(L"settings", L"concurrency", num, IniPath().c_str());
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
        if (g_font) DeleteObject(g_font);
        if (g_bgBrush) DeleteObject(g_bgBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- 入口 ----------------
static void EnableDpiAwareness()
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI * SetCtxProc)(void*);
    auto setCtx = (SetCtxProc)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (setCtx) {
        if (setCtx((void*)-4)) return;  // PER_MONITOR_AWARE_V2
    }
    typedef BOOL(WINAPI * SetAwareProc)();
    auto setAware = (SetAwareProc)GetProcAddress(u32, "SetProcessDPIAware");
    if (setAware) setAware();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    g_hInst = hInst;
    EnableDpiAwareness();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"BiliDownloaderWnd";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    // 客户区目标尺寸 + 边框，换算成整窗尺寸
    RECT rc{ 0, 0, S(940), S(700) };
    DWORD style = (DWORD)WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_CONTROLPARENT);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    // 按主显示器工作区居中（自动避开任务栏）
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (work.right <= work.left || work.bottom <= work.top) {
        work.left = 0;
        work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    // 小屏时收缩，避免窗口超出可用区域
    int areaW = work.right - work.left;
    int areaH = work.bottom - work.top;
    if (w > areaW) w = areaW;
    if (h > areaH) h = areaH;

    int x = work.left + (areaW - w) / 2;
    int y = work.top + (areaH - h) / 2;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"BiliDownloaderWnd", L"B 站视频下载器（Win32 原生版）",
                                style, x, y, w, h,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return (int)msg.wParam;
}

