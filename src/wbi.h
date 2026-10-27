#pragma once
#include <string>
#include <vector>
#include <utility>
#include "http.h"

// B 站 wbi 签名（web 端接口的防爬参数，2023 年起 playurl 等接口逐步要求）
// 算法：从 nav 接口取 img_key/sub_key，按固定混淆表拼出 mixin_key，
//       再把参数按键名排序、URL 编码后拼接，md5(query + mixin_key) 即 w_rid。
class WbiSigner {
public:
    // 对 params 做签名，输出完整 query（含 w_rid 与 wts）。失败返回 false 并写 err
    bool Sign(Http& http, std::vector<std::pair<std::string, std::string>> params, std::string& query,
              std::wstring& err);

    bool Ready() const { return !mixin_.empty(); }
    void Invalidate() { fetched_ = 0; }

private:
    bool EnsureKeys(Http& http, std::wstring& err);

    std::string mixin_;
    unsigned long long fetched_ = 0;
};
