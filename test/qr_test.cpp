// 二维码编码器对照测试：把矩阵打印成 0/1 文本，交给 Python 参考实现比对
#include "../src/qr.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: qr_test <text>\n");
        return 2;
    }
    qr::Matrix m;
    if (!qr::Encode(argv[1], m)) {
        printf("FAIL\n");
        return 1;
    }
    printf("size=%d\n", m.size);
    for (int y = 0; y < m.size; ++y) {
        for (int x = 0; x < m.size; ++x) putchar(m.at(x, y) ? '1' : '0');
        putchar('\n');
    }
    return 0;
}
