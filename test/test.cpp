// 控制台自测程序：验证解析与 playurl 接口
#include <windows.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include "bili.h"
#include "download.h"

static void DumpMedia(const MediaInfo& m)
{
    wprintf(L"  title = %s\n", m.title.c_str());
    wprintf(L"  kind  = %s  aid=%lld ssid=%lld eps=%d focus=%d\n",
            m.kind == MediaKind::Ugc ? L"UGC" : L"PGC", (long long)m.aid, (long long)m.ssid,
            (int)m.eps.size(), m.focusIndex);
    int n = m.eps.size() > 5 ? 5 : (int)m.eps.size();
    for (int i = 0; i < n; ++i) {
        wprintf(L"    [%d] id=%lld dur=%ds locked=%d  %s\n", i + 1, (long long)m.eps[i].id,
                m.eps[i].durationSec, (int)m.eps[i].locked, m.eps[i].title.c_str());
    }
}

static void TestParse(const wchar_t* input)
{
    wprintf(L"\n=== 解析: %s ===\n", input);
    BiliApi api;
    MediaInfo m;
    std::wstring err;
    if (!api.Parse(input, m, err)) {
        wprintf(L"  [FAIL] %s\n", err.c_str());
        return;
    }
    DumpMedia(m);

    PlayInfo pi;
    if (!api.GetPlayInfo(m, m.eps[0].id, 127, pi)) {
        wprintf(L"  [FAIL] playurl: %s\n", pi.error.c_str());
        return;
    }
    wprintf(L"  playurl ok: qn=%d preview=%d separated=%d segs=%d\n", pi.qn, (int)pi.preview,
            (int)pi.separated, (int)pi.urls.size());
    for (auto& q : pi.qualities) wprintf(L"    qn=%d  %s\n", q.qn, q.desc.c_str());
    for (auto& u : pi.urls) wprintf(L"    url(%.40s...) len=%d\n", u.c_str(), (int)u.size());
}

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U16TEXT);

    // 下载模式: test.exe -d <输入> <目录> <qn>
    if (argc >= 4 && wcscmp(argv[1], L"-d") == 0) {
        BiliApi api;
        MediaInfo m;
        std::wstring err;
        if (!api.Parse(argv[2], m, err)) {
            wprintf(L"[FAIL] %s\n", err.c_str());
            return 1;
        }
        int qn = argc >= 5 ? _wtoi(argv[4]) : 16;
        DownloadCallbacks cb;
        cb.log = [](const std::wstring& s) { wprintf(L"  %s\n", s.c_str()); };
        int64_t last = 0;
        cb.progress = [&last](int64_t done, int64_t total) {
            int64_t now = GetTickCount64();
            if (now - last > 500) {
                last = now;
                wprintf(L"    %lld / %lld\n", (long long)done, (long long)total);
            }
            return true;
        };
        std::wstring outPath;
        bool ok = DownloadEpisode(api, m, m.eps[0], qn, argv[3], m.title, cb, outPath, err);
        wprintf(L"[%s] %s\n", ok ? L"OK" : L"FAIL", ok ? outPath.c_str() : err.c_str());
        return ok ? 0 : 1;
    }

    if (argc > 1) {
        for (int i = 1; i < argc; ++i) TestParse(argv[i]);
        return 0;
    }

    TestParse(L"BV1GJ411x7h7");
    TestParse(L"https://www.bilibili.com/video/BV1GJ411x7h7?p=1");
    TestParse(L"av80433022");
    TestParse(L"ep281280");
    TestParse(L"ss28341");
    wprintf(L"\n测试结束\n");
    return 0;
}
