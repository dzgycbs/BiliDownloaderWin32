import subprocess, sys
import qrcode
from qrcode.constants import ERROR_CORRECT_M
from qrcode.util import QRData, MODE_8BIT_BYTE

EXE = r"D:/github/B23Downloader-main/BiliDownloaderWin32/build/qr_test.exe"

def mine(text):
    out = subprocess.run([EXE, text], capture_output=True, text=True)
    if out.returncode != 0:
        return None, out.stdout
    lines = out.stdout.strip().splitlines()
    size = int(lines[0].split("=")[1])
    grid = lines[1:]
    assert len(grid) == size, (len(grid), size)
    return [list(map(int, row)) for row in grid], None

def ref(text, mask):
    q = qrcode.QRCode(error_correction=ERROR_CORRECT_M, border=0, mask_pattern=mask)
    # 强制字节模式，否则参考实现会为纯大写/数字内容选字母数字模式
    q.add_data(QRData(text.encode("ascii"), mode=MODE_8BIT_BYTE))
    q.make(fit=True)
    return q.get_matrix()

def check(text):
    m, err = mine(text)
    if m is None:
        print(f"[FAIL] 编码失败 len={len(text)} {err}")
        return False
    size = len(m)
    for mask in range(8):
        r = ref(text, mask)
        if len(r) != size:
            continue
        if r == m:
            print(f"[OK]   len={len(text):3d} version={(size-17)//4:2d} size={size} mask={mask}")
            return True
    # 输出参考矩阵尺寸以便诊断
    rs = {len(ref(text, k)) for k in range(8)}
    print(f"[MISMATCH] len={len(text)} mine_size={size} ref_sizes={sorted(rs)}")
    for mask in range(8):
        r = ref(text, mask)
        if len(r) != size:
            continue
        diff = sum(1 for y in range(size) for x in range(size) if r[y][x] != m[y][x])
        print(f"   mask={mask} diff_modules={diff}/{size*size}")
    return False

cases = [
    "https://account.bilibili.com/h5/account-h5/auth/scan-web?navhide=1&callback=close&qrcode_key=030ddfb0b2652b43398b8b7dfebd0001&from=",
    "hello",
    "B23",
    "x" * 100,
    "x" * 120,
    "x" * 131,
    "x" * 150,
    "x" * 180,
    "x" * 200,
    "x" * 213,
    "https://www.bilibili.com/h5/login?qrcode_key=0123456789abcdef0123456789abcdef&navhide=1&callback=close",
]
ok = all(check(c) for c in cases)
print("ALL OK" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
