#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "bili.h"
#include "download.h"

// 任务状态，与界面「状态」列一一对应
enum TaskState {
    TS_QUEUED = 0,
    TS_RUNNING = 1,
    TS_DONE = 2,
    TS_FAILED = 3,
    TS_CANCELED = 4,
};

struct DownloadTask {
    int seq = 0;                 // 批次内序号（1 起）
    std::wstring display;        // 列表显示名
    std::wstring fileNameBase;   // 落盘文件名（不含扩展名）
    MediaInfo media;             // 自带一份媒体信息，互不干扰
    Episode ep;
    int qn = 127;
    std::wstring dir;
    std::string cookie;

    std::atomic<int> state{TS_QUEUED};
    std::atomic<long long> done{0};
    std::atomic<long long> total{-1};
    std::wstring error;          // 终态后由 UI 线程读取
    std::wstring outPath;
};

struct QueueStats {
    int all = 0;
    int queued = 0;
    int running = 0;
    int ok = 0;
    int failed = 0;
    int canceled = 0;
    long long doneBytes = 0;
    long long totalBytes = 0;
    int unknownTotal = 0;        // 尚未拿到总大小的任务数
};

// 多任务并发队列：固定并发度的线程池 + 可随时追加 + 可整体取消
class DownloadQueue {
public:
    DownloadQueue() = default;
    ~DownloadQueue();
    DownloadQueue(const DownloadQueue&) = delete;
    DownloadQueue& operator=(const DownloadQueue&) = delete;

    using LogFn = std::function<void(const std::wstring&)>;

    void SetLog(LogFn fn) { log_ = std::move(fn); }
    void SetConcurrency(int n);
    int Concurrency() const { return concurrency_; }

    // 清空并重置为可运行状态（仅在空闲时调用）
    void Reset();

    // 追加任务，返回分配到的序号
    int Add(std::shared_ptr<DownloadTask> task);

    void CancelAll();
    // 等待所有工作线程真正退出（析构/退出前的安全屏障）
    void WaitIdle(int timeoutMs = 5000);
    bool Busy() const;                       // 还有排队或运行中的
    std::vector<std::shared_ptr<DownloadTask>> Tasks() const;
    QueueStats Stats() const;

private:
    void SpawnMore();
    void Worker();
    void RunOne(const std::shared_ptr<DownloadTask>& t);

    mutable std::mutex m_;        // 保护 tasks_ / cursor_
    std::mutex spawnMtx_;         // 保护 active_ / 线程派生
    std::vector<std::shared_ptr<DownloadTask>> tasks_;
    size_t cursor_ = 0;
    int active_ = 0;
    std::atomic<bool> stop_{false};
    int concurrency_ = 3;
    LogFn log_;
};
