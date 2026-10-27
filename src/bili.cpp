#include "bili.h"
#include "json.h"
#include <algorithm>

using mini::Json;

static bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix)
{
    size_t n = wcslen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (towlower(s[i]) != towlower(prefix[i])) return false;
    }
    return true;
}

static std::wstring ToLower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static bool AllDigits(const std::wstring& s)
{
    if (s.empty()) return false;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

// 从 path 中提取 BV 号（保留原始大小写）
static std::wstring ExtractBvId(const std::wstring& path)
{
    std::wstring low = ToLower(path);
    size_t pos = low.find(L"bv");
    while (pos != std::wstring::npos) {
        size_t end = pos;
        while (end < path.size() && (iswalnum(path[end]))) ++end;
        std::wstring cand = path.substr(pos, end - pos);
        if (cand.size() >= 10) return cand;
        pos = low.find(L"bv", pos + 2);
    }
    return std::wstring();
}

// 提取形如 "av12345" 的 aid
static int64_t ExtractAvId(const std::wstring& path)
{
    std::wstring low = ToLower(path);
    size_t pos = low.find(L"av");
    while (pos != std::wstring::npos) {
        size_t i = pos + 2;
        std::wstring digits;
        while (i < path.size() && path[i] >= L'0' && path[i] <= L'9') digits += path[i++];
        if (!digits.empty()) return _wtoi64(digits.c_str());
        pos = low.find(L"av", pos + 2);
    }
    return 0;
}

// 在 path 中找 ss/ep/md 后面的数字
static int64_t ExtractIdAfter(const std::wstring& path, const wchar_t* tag)
{
    std::wstring low = ToLower(path);
    std::wstring t = tag;
    size_t pos = low.find(t);
    if (pos == std::wstring::npos) return 0;
    size_t i = pos + t.size();
    std::wstring digits;
    while (i < path.size() && path[i] >= L'0' && path[i] <= L'9') digits += path[i++];
    if (digits.empty()) return 0;
    return _wtoi64(digits.c_str());
}

// 取 query 中的整数参数
static int64_t QueryInt(const std::wstring& query, const wchar_t* key)
{
    std::wstring k = std::wstring(key) + L"=";
    size_t pos = query.find(k);
    if (pos == std::wstring::npos) return 0;
    size_t i = pos + k.size();
    std::wstring digits;
    while (i < query.size() && query[i] >= L'0' && query[i] <= L'9') digits += query[i++];
    return digits.empty() ? 0 : _wtoi64(digits.c_str());
}

static std::wstring ErrFromJson(const Json& root, const std::wstring& fallback)
{
    std::wstring msg;
    if (root.has("message")) msg = Utf8ToWide(root["message"].asString());
    else if (root.has("msg")) msg = Utf8ToWide(root["msg"].asString());
    if (msg.empty()) msg = fallback;
    return msg + L" (code=" + std::to_wstring(root["code"].asInt()) + L")";
}

bool BiliApi::Parse(const std::wstring& input, MediaInfo& out, std::wstring& err)
{
    std::wstring s = Trim(input);
    if (s.empty()) {
        err = L"请输入视频链接、BV 号或 av 号";
        return false;
    }

    // 纯编号形式
    if (AllDigits(s) && s.size() > 3) {
        // 裸数字当成 av 号
        return Recurse(L"https://www.bilibili.com/video/av" + s, out, err, 0);
    }
    if (StartsWithNoCase(s, L"BV") && s.size() >= 10) {
        return Recurse(L"https://www.bilibili.com/video/" + s, out, err, 0);
    }
    if (StartsWithNoCase(s, L"av") && AllDigits(s.substr(2))) {
        return Recurse(L"https://www.bilibili.com/video/" + s, out, err, 0);
    }
    if ((StartsWithNoCase(s, L"ss") || StartsWithNoCase(s, L"ep")) && AllDigits(s.substr(2))) {
        return Recurse(L"https://www.bilibili.com/bangumi/play/" + s, out, err, 0);
    }
    if (StartsWithNoCase(s, L"md") && AllDigits(s.substr(2))) {
        return Recurse(L"https://www.bilibili.com/bangumi/media/" + s, out, err, 0);
    }

    std::wstring url = s;
    if (!StartsWithNoCase(url, L"http")) url = L"https://" + url;

    return Recurse(url, out, err, 0);
}

bool BiliApi::Recurse(const std::wstring& urlIn, MediaInfo& out, std::wstring& err, int depth)
{
    if (depth > 3) {
        err = L"跳转层数过多";
        return false;
    }

    // 拆 host / path / query
    size_t schemePos = urlIn.find(L"://");
    size_t hostStart = (schemePos == std::wstring::npos) ? 0 : schemePos + 3;
    size_t pathStart = urlIn.find(L'/', hostStart);
    std::wstring host = (pathStart == std::wstring::npos)
                            ? urlIn.substr(hostStart)
                            : urlIn.substr(hostStart, pathStart - hostStart);
    size_t portPos = host.find(L':');
    if (portPos != std::wstring::npos) host = host.substr(0, portPos);
    host = ToLower(host);

    std::wstring rest = (pathStart == std::wstring::npos) ? std::wstring() : urlIn.substr(pathStart);
    size_t qPos = rest.find(L'?');
    std::wstring path = (qPos == std::wstring::npos) ? rest : rest.substr(0, qPos);
    std::wstring query = (qPos == std::wstring::npos) ? std::wstring() : rest.substr(qPos + 1);
    std::wstring pathLow = ToLower(path);

    // 短链跳转
    if (host.find(L"b23.tv") != std::wstring::npos || host.find(L"b22.top") != std::wstring::npos) {
        HttpResponse r = http.Get(urlIn, true);
        if (!r.error.empty()) {
            err = r.error;
            return false;
        }
        if (r.finalUrl.empty() || r.finalUrl == urlIn) {
            err = L"短链解析失败";
            return false;
        }
        return Recurse(r.finalUrl, out, err, depth + 1);
    }

    // 番剧 / 影视
    if (pathLow.find(L"/bangumi/play/") == 0) {
        int64_t ssid = ExtractIdAfter(pathLow, L"ss");
        int64_t epid = ExtractIdAfter(pathLow, L"ep");
        if (ssid > 0 || epid > 0) {
            std::wstring api = L"https://api.bilibili.com/pgc/view/web/season?";
            api += (ssid > 0) ? (L"season_id=" + std::to_wstring(ssid))
                              : (L"ep_id=" + std::to_wstring(epid));
            HttpResponse r = http.Get(api);
            if (!r.error.empty()) {
                err = r.error;
                return false;
            }
            Json root;
            if (!Json::Parse(r.body, root)) {
                err = L"接口返回内容解析失败";
                return false;
            }
            if (root["code"].asInt() != 0) {
                err = ErrFromJson(root, L"获取番剧信息失败");
                return false;
            }
            const Json& res = root["result"];
            out = MediaInfo();
            out.kind = MediaKind::Pgc;
            out.ssid = res["season_id"].asInt64();
            out.title = Utf8ToWide(res["title"].asString());
            if (out.title.empty()) out.title = Utf8ToWide(res["season_title"].asString());
            int total = res["total"].asInt();
            int width = (int)std::to_wstring(total > 0 ? total : 1).size();

            auto addEps = [&](const Json& arr) {
                for (size_t i = 0; i < arr.size(); ++i) {
                    const Json& e = arr.at(i);
                    Episode ep;
                    ep.id = e["id"].asInt64();
                    if (ep.id == 0) continue;
                    std::wstring t = Utf8ToWide(e["title"].asString());
                    std::wstring lt = Utf8ToWide(e["long_title"].asString());
                    // 纯数字标题补零：2 -> 第02话
                    bool numeric = !t.empty() && AllDigits(t);
                    if (numeric && (int)t.size() < width) {
                        t = std::wstring(width - t.size(), L'0') + t;
                        std::wstring suffix = L"话";
                        int type = res["type"].asInt();
                        if (type != 1 && type != 4) suffix = L"集";
                        t = L"第" + t + suffix;
                    }
                    if (!lt.empty()) t += L" " + lt;
                    ep.title = t;
                    ep.durationSec = (int)(e["duration"].asDouble() / 1000.0);
                    int st = e["status"].asInt();
                    // 2: 免费  13: 会员  6,7,8,9,12: 付费
                    ep.locked = (st == 13 || st == 6 || st == 7 || st == 8 || st == 9 || st == 12);
                    out.eps.push_back(ep);
                    if (epid > 0 && ep.id == epid) out.focusIndex = (int)out.eps.size();
                }
            };

            addEps(res["episodes"]);
            const Json& sections = res["section"];
            for (size_t i = 0; i < sections.size(); ++i) {
                addEps(sections.at(i)["episodes"]);
            }
            if (out.eps.empty()) {
                err = L"该番剧没有可下载的剧集";
                return false;
            }
            return true;
        }
        err = L"无法识别该番剧链接";
        return false;
    }

    // 剧集详情页 md 号
    if (pathLow.find(L"/bangumi/media/") == 0) {
        int64_t md = ExtractIdAfter(pathLow, L"md");
        if (md > 0) {
            HttpResponse r = http.Get(L"https://api.bilibili.com/pgc/review/user?media_id=" + std::to_wstring(md));
            Json root;
            if (r.error.empty() && Json::Parse(r.body, root) && root["code"].asInt() == 0) {
                int64_t ssid = root["result"]["media"]["season_id"].asInt64();
                if (ssid > 0) {
                    return Recurse(L"https://www.bilibili.com/bangumi/play/ss" + std::to_wstring(ssid), out, err,
                                   depth + 1);
                }
            }
        }
        err = L"无法识别该番剧页面";
        return false;
    }

    // 普通投稿视频
    if (host.find(L"bilibili.com") != std::wstring::npos) {
        std::wstring bvid = ExtractBvId(path);
        int64_t aid = bvid.empty() ? ExtractAvId(path) : 0;
        if (!bvid.empty() || aid > 0) {
            std::wstring api = L"https://api.bilibili.com/x/web-interface/view?";
            api += bvid.empty() ? (L"aid=" + std::to_wstring(aid)) : (L"bvid=" + bvid);
            HttpResponse r = http.Get(api);
            if (!r.error.empty()) {
                err = r.error;
                return false;
            }
            Json root;
            if (!Json::Parse(r.body, root)) {
                err = L"接口返回内容解析失败";
                return false;
            }
            if (root["code"].asInt() != 0) {
                err = ErrFromJson(root, L"获取视频信息失败");
                return false;
            }
            const Json& data = root["data"];
            std::wstring redirect = Utf8ToWide(data["redirect_url"].asString());
            if (!redirect.empty()) {
                return Recurse(redirect, out, err, depth + 1);
            }
            const Json& pages = data["pages"];
            if (pages.size() == 0) {
                err = L"没有找到视频分P信息";
                return false;
            }
            out = MediaInfo();
            out.kind = MediaKind::Ugc;
            out.aid = data["aid"].asInt64();
            out.title = Utf8ToWide(data["title"].asString());
            int p = (int)QueryInt(query, L"p");
            for (size_t i = 0; i < pages.size(); ++i) {
                const Json& pg = pages.at(i);
                Episode ep;
                ep.id = pg["cid"].asInt64();
                ep.title = Utf8ToWide(pg["part"].asString());
                ep.durationSec = pg["duration"].asInt();
                if (pages.size() == 1 && (ep.title.empty() || ep.title == out.title)) {
                    ep.title = out.title;
                }
                out.eps.push_back(ep);
            }
            if (p > 0 && p <= (int)out.eps.size()) out.focusIndex = p;
            else if (out.eps.size() == 1) out.focusIndex = 1;
            return true;
        }
        err = L"链接里没有找到 BV / av 号";
        return false;
    }

    err = L"暂不支持该类型的链接";
    return false;
}

bool BiliApi::GetPlayInfo(const MediaInfo& media, int64_t epId, int qn, PlayInfo& out)
{
    if (qn <= 0) qn = 127;

    struct Candidate {
        std::wstring url;
        const char* dataKey;
        const wchar_t* tag;
    };
    std::vector<Candidate> cands;

    const std::string qnStr = std::to_string(qn);

    if (media.kind == MediaKind::Ugc) {
        // 优先：wbi 签名的新接口（官方逐步收紧老接口，先走后者的替代品）
        if (useWbi) {
            std::string q;
            std::wstring signErr;
            std::vector<std::pair<std::string, std::string>> params = {
                {"avid", std::to_string(media.aid)},
                {"cid", std::to_string(epId)},
                {"qn", qnStr},
                {"fnval", "1"},
                {"fourk", "1"},
            };
            if (wbi.Sign(http, params, q, signErr)) {
                cands.push_back({L"https://api.bilibili.com/x/player/wbi/playurl?" + Utf8ToWide(q), "data",
                                 L"wbi"});
            }
        }
        // 兜底：老接口
        cands.push_back({L"https://api.bilibili.com/x/player/playurl?avid=" + std::to_wstring(media.aid) +
                             L"&cid=" + std::to_wstring(epId) + L"&qn=" + std::to_wstring(qn) +
                             L"&fnval=1&fourk=1",
                         "data", L"legacy"});
    } else {
        cands.push_back({L"https://api.bilibili.com/pgc/player/web/playurl?ep_id=" + std::to_wstring(epId) +
                             L"&qn=" + std::to_wstring(qn) + L"&fnval=1&fourk=1",
                         "result", L"pgc"});
    }

    std::wstring lastErr;
    for (const auto& c : cands) {
        HttpResponse r = http.Get(c.url);
        if (!r.error.empty()) {
            lastErr = r.error;
            continue;
        }
        PlayInfo got;
        if (ParsePlayBody(r.body, c.dataKey, qn, got)) {
            out = got;
            return true;
        }
        lastErr = got.error;
    }

    out.error = lastErr.empty() ? L"获取播放地址失败" : lastErr;
    return false;
}bool BiliApi::ParsePlayBody(const std::string& body, const char* dataKey, int reqQn, PlayInfo& out)
{
    Json root;
    if (!Json::Parse(body, root)) {
        out.error = L"播放地址接口返回解析失败";
        return false;
    }
    if (root["code"].asInt() != 0) {
        out.error = ErrFromJson(root, L"获取播放地址失败");
        return false;
    }

    const Json& data = root[dataKey];
    if (data.isNull()) {
        out.error = L"该视频没有返回播放地址（可能地区限制或需要登录）";
        return false;
    }

    out.qn = data["quality"].asInt();
    if (out.qn <= 0) out.qn = reqQn;
    out.preview = data["is_preview"].asInt() == 1;

    // 可用画质
    const Json& formats = data["support_formats"];
    if (formats.size() > 0) {
        for (size_t i = 0; i < formats.size(); ++i) {
            Quality q;
            q.qn = formats.at(i)["quality"].asInt();
            q.desc = Utf8ToWide(formats.at(i)["new_description"].asString());
            if (q.desc.empty()) q.desc = Utf8ToWide(formats.at(i)["display_desc"].asString());
            if (q.qn > 0) out.qualities.push_back(q);
        }
    } else {
        const Json& qs = data["accept_quality"];
        const Json& ds = data["accept_description"];
        for (size_t i = 0; i < qs.size(); ++i) {
            Quality q;
            q.qn = qs.at(i).asInt();
            q.desc = (i < ds.size()) ? Utf8ToWide(ds.at(i).asString()) : std::to_wstring(q.qn);
            if (q.qn > 0) out.qualities.push_back(q);
        }
    }
    if (out.qualities.empty()) {
        Quality q;
        q.qn = out.qn;
        q.desc = L"默认画质";
        out.qualities.push_back(q);
    }

    // 直链：优先 durl（完整 mp4），否则 dash 分离流
    const Json& durl = data["durl"];
    if (durl.size() > 0) {
        for (size_t i = 0; i < durl.size(); ++i) {
            std::wstring u = Utf8ToWide(durl.at(i)["url"].asString());
            if (!u.empty()) out.urls.push_back(u);
        }
    }

    if (out.urls.empty()) {
        const Json& dash = data["dash"];
        if (!dash.isNull()) {
            const Json& videos = dash["video"];
            const Json& audios = dash["audio"];
            if (videos.size() > 0) {
                std::wstring v = Utf8ToWide(videos.at(0)["baseUrl"].asString());
                if (!v.empty()) out.urls.push_back(v);
            }
            if (audios.size() > 0) {
                std::wstring a = Utf8ToWide(audios.at(0)["baseUrl"].asString());
                if (!a.empty()) out.urls.push_back(a);
            }
            out.separated = out.urls.size() == 2;
        }
    }

    if (out.urls.empty()) {
        out.error = L"没有取到可下载的地址（可能需要大会员或该视频受限）";
        return false;
    }
    return true;
}
