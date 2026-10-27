#pragma once
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include "bili.h"

struct DownloadCallbacks {
    std::function<void(const std::wstring&)> log;
    std::function<bool(int64_t done, int64_t total)> progress;  // 返回 false 取消
};

std::wstring SanitizeFileName(const std::wstring& name);
std::wstring FormatSize(int64_t bytes);
std::wstring FormatDuration(int sec);
bool EnsureDirectory(const std::wstring& dir);
std::wstring UniquePath(const std::wstring& path);

// 下载一集，成功返回 true 并写入 outPath
bool DownloadEpisode(BiliApi& api, const MediaInfo& media, const Episode& ep, int qn,
                     const std::wstring& dir, const std::wstring& fileNameBase,
                     const DownloadCallbacks& cb, std::wstring& outPath, std::wstring& err);
