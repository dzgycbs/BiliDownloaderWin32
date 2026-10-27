#pragma once
#include <string>
#include <vector>

// 极简二维码编码器：字节模式、纠错等级 M、版本 1~10（最多 213 字节）
// 仅够本项目画登录二维码用，不追求覆盖全部 QR 特性。
namespace qr {

struct Matrix {
    int size = 0;
    std::vector<unsigned char> cells;   // size*size，1 = 黑块
    bool at(int x, int y) const { return cells[(size_t)y * size + x] != 0; }
};

bool Encode(const std::string& text, Matrix& out);

} // namespace qr
