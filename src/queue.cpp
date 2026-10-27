#include "queue.h"
#include <algorithm>
#include <chrono>
#include <thread>

DownloadQueue::~DownloadQueue()
{
    stop_ = true;
    WaitIdle(3000);
}

void DownloadQueue::WaitIdle(int timeoutMs)
{
    int waited = 0;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(spawnMtx_);
            if (active_ == 0) return;
        }
        if (waited >= timeoutMs) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        waited += 20;
    }
}

void DownloadQueue::SetConcurrency(int n)
{
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    {
        std::lock_guard<std::mutex> lk(spawnMtx_);
        concurrency_ = n;
    }
    if (Busy()) SpawnMore();
}

void DownloadQueue::Reset()
{
    if (Busy()) return;   // 有任务在跑就别清，避免悬空引用
    std::lock_guard<std::mutex> lk(m_);
    tasks_.clear();
    cursor_ = 0;
    stop_ = false;
}

int DownloadQueue::Add(std::shared_ptr<DownloadTask> task)
{
    if (!task) return 0;
    int seq = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        task->seq = (int)tasks_.size() + 1;
        seq = task->seq;
        tasks_.push_back(task);
    }
    SpawnMore();
    return seq;
}

void DownloadQueue::SpawnMore()
{
    std::lock_guard<std::mutex> lk(spawnMtx_);
    for (;;) {
        size_t pending = 0;
        {
            std::lock_guard<std::mutex> lk2(m_);
            if (stop_) return;
            if (cursor_ < tasks_.size()) pending = tasks_.size() - cursor_;
        }
        if (pending == 0) return;
        if (active_ >= concurrency_) return;

        ++active_;
        std::thread(&DownloadQueue::Worker, this).detach();
    }
}

void DownloadQueue::Worker()
{
    for (;;) {
        std::shared_ptr<DownloadTask> t;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!stop_ && cursor_ < tasks_.size()) t = tasks_[cursor_++];
        }
        if (t) {
            RunOne(t);
            continue;
        }

        // 没抢到活：确认确实没活了再退出（避免与 Add 竞态丢任务）
        {
            std::lock_guard<std::mutex> lkS(spawnMtx_);
            std::lock_guard<std::mutex> lk(m_);
            if (!stop_ && cursor_ >= tasks_.size()) {
                --active_;
                return;
            }
        }
        // 有新任务冒出来，继续循环重新抢
    }
}

void DownloadQueue::RunOne(const std::shared_ptr<DownloadTask>& t)
{
    if (stop_) {
        t->state = TS_CANCELED;
        t->error = L"已取消";
        return;
    }
    t->state = TS_RUNNING;

    BiliApi api;
    api.http.cookie = t->cookie;

    if (log_) log_(L"[" + std::to_wstring(t->seq) + L"] 开始: " + t->display);

    DownloadCallbacks cb;
    cb.log = [this](const std::wstring& s) {
        if (log_) log_(L"        " + s);
    };
    cb.progress = [this, t](int64_t done, int64_t total) {
        t->done = (long long)done;
        if (total > 0) t->total = (long long)total;
        return !stop_.load();
    };

    std::wstring outPath, err;
    bool ok = DownloadEpisode(api, t->media, t->ep, t->qn, t->dir, t->fileNameBase, cb, outPath, err);

    if (ok) {
        t->state = TS_DONE;
        t->outPath = outPath;
        if (t->total.load() <= 0) t->total = t->done.load();
        if (log_) log_(L"[" + std::to_wstring(t->seq) + L"] 完成: " + t->display);
    } else if (stop_.load()) {
        t->state = TS_CANCELED;
        t->error = L"已取消";
        if (log_) log_(L"[" + std::to_wstring(t->seq) + L"] 已取消: " + t->display);
    } else {
        t->state = TS_FAILED;
        t->error = err;
        if (log_) log_(L"[" + std::to_wstring(t->seq) + L"] 失败: " + t->display + L" —— " + err);
    }
}

void DownloadQueue::CancelAll()
{
    stop_ = true;
    // 还没被领取的任务直接置为已取消，否则它们会永远停在「排队中」，
    // 导致 Busy() 一直为真、界面无法收尾
    std::lock_guard<std::mutex> lk(m_);
    for (size_t i = cursor_; i < tasks_.size(); ++i) {
        auto& t = tasks_[i];
        if (t->state.load() == TS_QUEUED) {
            t->state = TS_CANCELED;
            t->error = L"已取消";
        }
    }
}

bool DownloadQueue::Busy() const
{
    QueueStats s = Stats();
    return s.queued > 0 || s.running > 0;
}

std::vector<std::shared_ptr<DownloadTask>> DownloadQueue::Tasks() const
{
    std::lock_guard<std::mutex> lk(m_);
    return tasks_;
}

QueueStats DownloadQueue::Stats() const
{
    QueueStats s;
    std::lock_guard<std::mutex> lk(m_);
    s.all = (int)tasks_.size();
    for (const auto& t : tasks_) {
        switch (t->state.load()) {
        case TS_QUEUED: ++s.queued; break;
        case TS_RUNNING: ++s.running; break;
        case TS_DONE: ++s.ok; break;
        case TS_FAILED: ++s.failed; break;
        case TS_CANCELED: ++s.canceled; break;
        default: break;
        }
        s.doneBytes += t->done.load();
        long long tot = t->total.load();
        if (tot > 0) s.totalBytes += tot;
        else ++s.unknownTotal;
    }
    return s;
}
