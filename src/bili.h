#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "http.h"
#include "wbi.h"

enum class MediaKind { Ugc, Pgc };

struct Episode {
    int64_t id = 0;          // UGC: cid, PGC: ep_id
    std::wstring title;
    int durationSec = 0;
    bool locked = false;     // 付费/会员限定
};

struct MediaInfo {
    MediaKind kind = MediaKind::Ugc;
    int64_t aid = 0;
    int64_t ssid = 0;
    std::wstring title;
    std::vector<Episode> eps;
    int focusIndex = 0;      // 链接里 ?p=N 指向的分P（0 表示未指定）
};

struct Quality {
    int qn = 0;
    std::wstring desc;
};

struct PlayInfo {
    std::vector<std::wstring> urls;   // 多段则按 order 顺序
    std::vector<Quality> qualities;
    int qn = 0;
    bool preview = false;             // 预告片/试看
    bool separated = false;           // true 表示音视频分离（DASH），urls = [video, audio]
    std::wstring title;
    std::wstring error;
};

class BiliApi {
public:
    Http http;
    WbiSigner wbi;
    bool useWbi = true;      // 关闭后只走老接口（留作兼容开关）

    // 解析用户输入的链接/编号
    bool Parse(const std::wstring& input, MediaInfo& out, std::wstring& err);

    // 取播放地址（qn=0 表示用接口默认/最高可用）
    bool GetPlayInfo(const MediaInfo& media, int64_t epId, int qn, PlayInfo& out);

private:
    bool Recurse(const std::wstring& url, MediaInfo& out, std::wstring& err, int depth);
    // 解析 playurl 返回体（UGC 用 data，PGC 用 result）
    static bool ParsePlayBody(const std::string& body, const char* dataKey, int reqQn, PlayInfo& out);
};
