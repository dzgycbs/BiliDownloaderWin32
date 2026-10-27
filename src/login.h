#pragma once
#include <string>
#include <windows.h>

// 弹窗扫码登录：成功返回 true，cookieOut 形如 "SESSDATA=xxx; bili_jct=yyy; DedeUserID=zzz"
// 取消或失败返回 false，err 写原因
bool QrLogin(HWND parent, std::string& cookieOut, std::wstring& err);
