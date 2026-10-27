import ctypes, ctypes.wintypes as wt, sys
from ctypes import windll

u32 = windll.user32
gdi32 = windll.gdi32

def find(cls, title=None):
    if title:
        return u32.FindWindowW(cls, title)
    return u32.FindWindowW(cls, None)

def shot(hwnd, path):
    rect = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    hdc = u32.GetWindowDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    u32.RedrawWindow(hwnd, None, None, 0x0001 | 0x0100 | 0x0080)
    windll.user32.UpdateWindow(hwnd)
    # PrintWindow(PW_RENDERFULLCONTENT) 避免截到上层遮挡的其它窗口
    ok = u32.PrintWindow(hwnd, mdc, 2)
    if not ok:
        gdi32.BitBlt(mdc, 0, 0, w, h, hdc, 0, 0, 0x00CC0020)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                    ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                    ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -h
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)

    from PIL import Image
    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB")
    img.save(path)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    u32.ReleaseDC(hwnd, hdc)
    print(f"saved {path} {w}x{h}")

if __name__ == "__main__":
    cls = sys.argv[1] if len(sys.argv) > 1 else "BiliDownloaderWnd"
    out = sys.argv[2] if len(sys.argv) > 2 else "build/ui.png"
    h = find(cls)
    if not h:
        print("window not found")
        sys.exit(1)
    shot(h, out)
