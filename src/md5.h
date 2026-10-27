#pragma once
#include <string>
#include <cstdint>

// 计算 MD5，返回 32 位小写十六进制字符串
std::string Md5Hex(const std::string& data);
