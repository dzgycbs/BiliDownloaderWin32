// 自测：md5 / wbi / 队列
#include "../src/bili.h"
#include "../src/md5.h"
#include "../src/queue.h"
#include "../src/wbi.h"
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>

static void PrintUtf8(const std::string& s) { fputs(s.c_str(), stdout); }

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("usage: selftest md5 <text> | wbi <bvid> | queue <bvid> <n>\n");
        return 2;
    }
    std::string mode = argv[1];

    if (mode == "md5") {
        printf("%s\n", Md5Hex(argv[2]).c_str());
        return 0;
    }

    if (mode == "wbi") {
        BiliApi api;
        MediaInfo mi;
        std::wstring err;
        if (!api.Parse(Utf8ToWide(argv[2]), mi, err)) {
            printf("parse failed\n");
            return 1;
        }
        int64_t cid = mi.eps.empty() ? 0 : mi.eps[0].id;
        std::string q;
        std::wstring serr;
        std::vector<std::pair<std::string, std::string>> params = {
            {"avid", std::to_string(mi.aid)}, {"cid", std::to_string(cid)},
            {"qn", "64"},                     {"fnval", "1"},
            {"fourk", "1"},
        };
        if (!api.wbi.Sign(api.http, params, q, serr)) {
            printf("sign failed\n");
            return 1;
        }
        printf("QUERY %s\n", q.c_str());

        PlayInfo pi;
        bool ok = api.GetPlayInfo(mi, cid, 64, pi);
        printf("PLAYURL ok=%d err=%s qn=%d urls=%d qualities=%d\n", ok ? 1 : 0,
               WideToUtf8(pi.error).c_str(), pi.qn, (int)pi.urls.size(), (int)pi.qualities.size());
        return ok ? 0 : 1;
    }

    if (mode == "queue") {
        BiliApi api;
        MediaInfo mi;
        std::wstring err;
        if (!api.Parse(Utf8ToWide(argv[2]), mi, err)) {
            printf("parse failed\n");
            return 1;
        }
        int n = argc > 3 ? atoi(argv[3]) : 3;
        if (n < 1) n = 1;
        if (mi.eps.empty()) {
            printf("no episodes\n");
            return 1;
        }

        DownloadQueue q;
        q.SetConcurrency(3);
        q.SetLog([](const std::wstring& s) { printf("%s\n", WideToUtf8(s).c_str()); });

        std::wstring dir = L"build/qtest";
        EnsureDirectory(dir);

        for (int i = 0; i < n; ++i) {
            auto t = std::make_shared<DownloadTask>();
            t->media = mi;
            t->ep = mi.eps[i % mi.eps.size()];
            t->qn = 16;   // 360P，文件小
            t->dir = dir;
            t->display = mi.eps[i % mi.eps.size()].title + L" #" + std::to_wstring(i + 1);
            t->fileNameBase = L"q" + std::to_wstring(i + 1) + L"_" + mi.title;
            q.Add(t);
        }

        auto t0 = std::chrono::steady_clock::now();
        while (q.Busy()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            QueueStats s = q.Stats();
            printf("  [poll] running=%d queued=%d ok=%d failed=%d bytes=%lld\n", s.running, s.queued, s.ok,
                   s.failed, s.doneBytes);
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        QueueStats s = q.Stats();
        printf("DONE all=%d ok=%d failed=%d canceled=%d elapsed=%lldms\n", s.all, s.ok, s.failed, s.canceled,
               (long long)ms);
        return s.ok == s.all ? 0 : 1;
    }

    if (mode == "cancel") {
        fprintf(stderr, "[t] start\n"); fflush(stderr);
        BiliApi api;
        MediaInfo mi;
        std::wstring err;
        if (!api.Parse(Utf8ToWide(argv[2]), mi, err)) {
            printf("parse failed\n");
            return 1;
        }
        fprintf(stderr, "[t] parsed eps=%d\n", (int)mi.eps.size()); fflush(stderr);
        DownloadQueue q;
        q.SetConcurrency(2);
        std::wstring dir = L"build/qtest";
        EnsureDirectory(dir);
        for (int i = 0; i < 3; ++i) {
            auto t = std::make_shared<DownloadTask>();
            t->media = mi;
            t->ep = mi.eps[0];
            t->qn = 16;
            t->dir = dir;
            t->fileNameBase = L"c" + std::to_wstring(i + 1);
            q.Add(t);
        }
        fprintf(stderr, "[t] added\n"); fflush(stderr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        fprintf(stderr, "[t] canceling\n"); fflush(stderr);
        q.CancelAll();
        while (q.Busy()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fprintf(stderr, "[t] not busy\n"); fflush(stderr);
        q.WaitIdle();
        fprintf(stderr, "[t] idle\n"); fflush(stderr);
        QueueStats s = q.Stats();
        fprintf(stderr, "CANCELED all=%d ok=%d failed=%d canceled=%d\n", s.all, s.ok, s.failed, s.canceled);
        fflush(stderr);
        return s.canceled == 3 ? 0 : 1;
    }

    printf("unknown mode\n");
    return 2;
}
