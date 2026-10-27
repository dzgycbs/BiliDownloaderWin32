#include "qr.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace qr {
namespace {

// ---------------- GF(256) 与 Reed-Solomon ----------------
struct GF256 {
    unsigned char exp[512];
    unsigned char logv[256];
    GF256()
    {
        memset(logv, 0, sizeof(logv));
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = (unsigned char)x;
            logv[x] = (unsigned char)i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    }
};

const GF256& G()
{
    static GF256 g;
    return g;
}

unsigned char gmul(unsigned char a, unsigned char b)
{
    if (!a || !b) return 0;
    const GF256& g = G();
    return g.exp[(int)g.logv[a] + (int)g.logv[b]];
}

// 生成多项式，返回长度 degree+1，最高次项系数恒为 1
std::vector<unsigned char> GeneratorPoly(int degree)
{
    std::vector<unsigned char> poly(1, 1);
    unsigned char root = 1;
    for (int i = 0; i < degree; ++i) {
        std::vector<unsigned char> next(poly.size() + 1, 0);
        for (size_t j = 0; j < poly.size(); ++j) {
            next[j] ^= poly[j];
            next[j + 1] ^= gmul(poly[j], root);
        }
        poly.swap(next);
        root = gmul(root, 2);
    }
    return poly;
}

std::vector<unsigned char> RsEcc(const std::vector<unsigned char>& data, int ecLen)
{
    std::vector<unsigned char> gen = GeneratorPoly(ecLen);
    std::vector<unsigned char> rem((size_t)ecLen, 0);
    for (unsigned char b : data) {
        unsigned char factor = (unsigned char)(b ^ rem[0]);
        rem.erase(rem.begin());
        rem.push_back(0);
        for (int i = 0; i < ecLen; ++i) rem[i] ^= gmul(gen[i + 1], factor);
    }
    return rem;
}

// ---------------- 版本参数表（纠错等级 M） ----------------
const int ECC_PER_BLOCK[11] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
const int NUM_BLOCKS[11] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
const int ALIGN_POS[11][3] = {{0, 0, 0},  {0, 0, 0},  {6, 18, 0}, {6, 22, 0}, {6, 26, 0}, {6, 30, 0},
                              {6, 34, 0}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50}};
const int ALIGN_CNT[11] = {0, 0, 2, 2, 2, 2, 2, 3, 3, 3, 3};
const unsigned int FORMAT_M[8] = {0x5412, 0x5125, 0x5E7C, 0x5B4B, 0x45F9, 0x40CE, 0x4F97, 0x4AA0};
const unsigned int VER_INFO[11] = {0, 0, 0, 0, 0, 0, 0, 0x07C94, 0x085BC, 0x09A99, 0x0A4D3};

int RawDataModules(int ver)
{
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int numAlign = ver / 7 + 2;
        result -= (25 * numAlign - 10) * numAlign - 55;
        if (ver >= 7) result -= 36;
    }
    return result;
}

bool MaskBit(int mask, int x, int y)
{
    switch (mask) {
    case 0: return ((x + y) % 2) == 0;
    case 1: return (y % 2) == 0;
    case 2: return (x % 3) == 0;
    case 3: return ((x + y) % 3) == 0;
    case 4: return ((y / 2 + x / 3) % 2) == 0;
    case 5: return ((x * y) % 2 + (x * y) % 3) == 0;
    case 6: return (((x * y) % 2 + (x * y) % 3) % 2) == 0;
    default: return (((x + y) % 2 + (x * y) % 3) % 2) == 0;
    }
}

// ---------------- 掩码罚分（ISO/IEC 18004 四条规则） ----------------
long Penalty(const std::vector<unsigned char>& m, int size)
{
    long p = 0;

    // 规则 1：行/列连续同色 >= 5
    for (int y = 0; y < size; ++y) {
        int run = 1;
        for (int x = 1; x < size; ++x) {
            if (m[(size_t)y * size + x] == m[(size_t)y * size + x - 1]) {
                ++run;
                if (run == 5) p += 3;
                else if (run > 5) p += 1;
            } else {
                run = 1;
            }
        }
    }
    for (int x = 0; x < size; ++x) {
        int run = 1;
        for (int y = 1; y < size; ++y) {
            if (m[(size_t)y * size + x] == m[(size_t)(y - 1) * size + x]) {
                ++run;
                if (run == 5) p += 3;
                else if (run > 5) p += 1;
            } else {
                run = 1;
            }
        }
    }

    // 规则 2：2x2 同色块
    for (int y = 0; y + 1 < size; ++y) {
        for (int x = 0; x + 1 < size; ++x) {
            unsigned char c = m[(size_t)y * size + x];
            if (m[(size_t)(y + 1) * size + x] == c && m[(size_t)y * size + x + 1] == c &&
                m[(size_t)(y + 1) * size + x + 1] == c) {
                p += 3;
            }
        }
    }

    // 规则 3：形似定位图案的 1:1:3:1:1 序列
    static const unsigned char PAT1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
    static const unsigned char PAT2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x + 11 <= size; ++x) {
            bool m1 = true, m2 = true;
            for (int k = 0; k < 11; ++k) {
                unsigned char v = m[(size_t)y * size + x + k];
                if (v != PAT1[k]) m1 = false;
                if (v != PAT2[k]) m2 = false;
            }
            if (m1) p += 40;
            if (m2) p += 40;
        }
    }
    for (int x = 0; x < size; ++x) {
        for (int y = 0; y + 11 <= size; ++y) {
            bool m1 = true, m2 = true;
            for (int k = 0; k < 11; ++k) {
                unsigned char v = m[(size_t)(y + k) * size + x];
                if (v != PAT1[k]) m1 = false;
                if (v != PAT2[k]) m2 = false;
            }
            if (m1) p += 40;
            if (m2) p += 40;
        }
    }

    // 规则 4：黑白比例偏离 50%
    int dark = 0;
    for (auto c : m) dark += c ? 1 : 0;
    int total = size * size;
    int pct10 = total > 0 ? dark * 1000 / total : 500;   // 千分比
    int dev = pct10 - 500;
    if (dev < 0) dev = -dev;
    p += (dev / 50) * 10;
    return p;
}

// ---------------- 主流程 ----------------
bool BuildBits(const std::string& text, int ver, std::vector<unsigned char>& bits)
{
    int totalCodewords = RawDataModules(ver) / 8;
    int dataCodewords = totalCodewords - ECC_PER_BLOCK[ver] * NUM_BLOCKS[ver];
    int cap = dataCodewords * 8;

    if (text.size() > 0xFFFF) return false;

    auto append = [&bits](unsigned int val, int len) {
        for (int i = len - 1; i >= 0; --i) bits.push_back((unsigned char)((val >> i) & 1));
    };

    append(0x4, 4);                                    // 字节模式
    append((unsigned int)text.size(), ver <= 9 ? 8 : 16);
    for (unsigned char c : text) append(c, 8);
    if ((int)bits.size() > cap) return false;

    int term = std::min(4, cap - (int)bits.size());
    append(0, term);
    while (bits.size() % 8) bits.push_back(0);

    for (int pad = 0xEC; (int)bits.size() < cap; pad ^= (0xEC ^ 0x11)) append((unsigned int)pad, 8);
    return true;
}

} // namespace

bool Encode(const std::string& text, Matrix& out)
{
    if (text.empty()) return false;

    // 选版本
    int ver = 0;
    std::vector<unsigned char> bits;
    for (int v = 1; v <= 10; ++v) {
        bits.clear();
        if (BuildBits(text, v, bits)) {
            ver = v;
            break;
        }
    }
    if (!ver) return false;

    const int size = ver * 4 + 17;
    const int ecLen = ECC_PER_BLOCK[ver];
    const int numBlocks = NUM_BLOCKS[ver];
    const int totalCodewords = RawDataModules(ver) / 8;

    // 位流 -> 码字
    std::vector<unsigned char> data((size_t)totalCodewords - ecLen * numBlocks, 0);
    for (size_t i = 0; i < data.size(); ++i) {
        unsigned char v = 0;
        for (int k = 0; k < 8; ++k) v = (unsigned char)((v << 1) | (bits[i * 8 + k] ? 1 : 0));
        data[i] = v;
    }

    // 分块 + 纠错
    int numShort = numBlocks - totalCodewords % numBlocks;
    int shortLen = totalCodewords / numBlocks;
    std::vector<std::vector<unsigned char>> blocks, eccs;
    size_t off = 0;
    for (int i = 0; i < numBlocks; ++i) {
        int dl = shortLen - ecLen + (i < numShort ? 0 : 1);
        std::vector<unsigned char> blk(data.begin() + off, data.begin() + off + dl);
        off += dl;
        eccs.push_back(RsEcc(blk, ecLen));
        blocks.push_back(blk);
    }

    std::vector<unsigned char> cw;
    cw.reserve(totalCodewords);
    size_t maxData = 0;
    for (auto& b : blocks) maxData = std::max(maxData, b.size());
    for (size_t j = 0; j < maxData; ++j) {
        for (auto& b : blocks)
            if (j < b.size()) cw.push_back(b[j]);
    }
    for (int j = 0; j < ecLen; ++j) {
        for (auto& e : eccs) cw.push_back(e[j]);
    }

    // 画功能图案
    std::vector<unsigned char> mod((size_t)size * size, 0);
    std::vector<unsigned char> fn((size_t)size * size, 0);

    auto setFn = [&](int x, int y, int v) {
        if (x < 0 || y < 0 || x >= size || y >= size) return;
        mod[(size_t)y * size + x] = (unsigned char)(v ? 1 : 0);
        fn[(size_t)y * size + x] = 1;
    };

    // 顺序很关键：先定时图案，再定位图案，最后对齐图案（后者覆盖前者重叠部分）
    for (int i = 0; i < size; ++i) {
        setFn(6, i, (i % 2) == 0);
        setFn(i, 6, (i % 2) == 0);
    }

    auto drawFinder = [&](int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                int dist = std::max(std::abs(dx), std::abs(dy));
                setFn(cx + dx, cy + dy, (dist != 2 && dist != 4) ? 1 : 0);
            }
        }
    };
    drawFinder(3, 3);
    drawFinder(size - 4, 3);
    drawFinder(3, size - 4);

    const int* ap = ALIGN_POS[ver];
    for (int i = 0; i < ALIGN_CNT[ver]; ++i) {
        for (int j = 0; j < ALIGN_CNT[ver]; ++j) {
            int ax = ap[i], ay = ap[j];
            bool skip = (ax == 6 && ay == 6) || (ax == 6 && ay == size - 7) || (ax == size - 7 && ay == 6);
            if (skip) continue;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    int dist = std::max(std::abs(dx), std::abs(dy));
                    setFn(ax + dx, ay + dy, (dist != 1) ? 1 : 0);
                }
            }
        }
    }

    // 预留格式信息区（先用掩码 0 占位，后面按最终掩码重画）
    auto writeFormat = [&](std::vector<unsigned char>& m, int mask) {
        unsigned int b = FORMAT_M[mask];
        auto bit = [b](int i) { return (unsigned char)((b >> i) & 1); };
        for (int i = 0; i <= 5; ++i) m[(size_t)i * size + 8] = bit(i);
        m[(size_t)7 * size + 8] = bit(6);
        m[(size_t)8 * size + 8] = bit(7);
        m[(size_t)8 * size + 7] = bit(8);
        for (int i = 9; i < 15; ++i) m[(size_t)8 * size + (14 - i)] = bit(i);
        for (int i = 0; i < 8; ++i) m[(size_t)8 * size + (size - 1 - i)] = bit(i);
        for (int i = 8; i < 15; ++i) m[(size_t)(size - 15 + i) * size + 8] = bit(i);
    };
    writeFormat(mod, 0);
    // 标记格式区为功能模块
    for (int i = 0; i <= 5; ++i) fn[(size_t)i * size + 8] = 1;
    fn[(size_t)7 * size + 8] = 1;
    fn[(size_t)8 * size + 8] = 1;
    fn[(size_t)8 * size + 7] = 1;
    for (int i = 9; i < 15; ++i) fn[(size_t)8 * size + (14 - i)] = 1;
    for (int i = 0; i < 8; ++i) fn[(size_t)8 * size + (size - 1 - i)] = 1;
    for (int i = 8; i < 15; ++i) fn[(size_t)(size - 15 + i) * size + 8] = 1;

    if (ver >= 7) {
        unsigned int vb = VER_INFO[ver];
        for (int i = 0; i < 18; ++i) {
            unsigned char v = (unsigned char)((vb >> i) & 1);
            int a = size - 11 + i % 3, b = i / 3;
            setFn(a, b, v);
            setFn(b, a, v);
        }
    }

    // 固定的暗模块
    setFn(8, size - 8, 1);

    // 数据填充（右下角起的蛇形）
    size_t bi = 0;
    const size_t totalBits = cw.size() * 8;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; ++vert) {
            for (int j = 0; j < 2; ++j) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? size - 1 - vert : vert;
                if (fn[(size_t)y * size + x] || bi >= totalBits) continue;
                size_t idx = bi++;
                unsigned char v = (unsigned char)((cw[idx >> 3] >> (7 - (idx & 7))) & 1);
                mod[(size_t)y * size + x] = v;
            }
        }
    }

    // 选罚分最小的掩码
    long best = -1;
    int bestMask = 0;
    std::vector<unsigned char> bestMod;
    for (int mask = 0; mask < 8; ++mask) {
        std::vector<unsigned char> m = mod;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (!fn[(size_t)y * size + x] && MaskBit(mask, x, y)) m[(size_t)y * size + x] ^= 1;
            }
        }
        writeFormat(m, mask);
        long p = Penalty(m, size);
        if (best < 0 || p < best) {
            best = p;
            bestMask = mask;
            bestMod.swap(m);
        }
    }
    (void)bestMask;

    out.size = size;
    out.cells.swap(bestMod);
    return true;
}

} // namespace qr
