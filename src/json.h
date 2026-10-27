// 迷你 JSON 解析器（只读，够用就行）
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace mini {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() : type_(Type::Null), bool_(false), num_(0) {}

    // 解析失败返回 false
    static bool Parse(const std::string& text, Json& out);

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }
    bool isString() const { return type_ == Type::String; }

    size_t size() const;
    const Json& at(size_t i) const;                 // 数组下标，越界返回空对象
    const Json& operator[](const char* key) const;  // 对象取值，缺失返回空对象
    bool has(const char* key) const;

    std::string asString() const;   // 非字符串返回空
    double asDouble() const;
    int64_t asInt64() const;
    int asInt(int def = 0) const;
    bool asBool(bool def = false) const;

    // 便捷：对象 -> 子对象/数组
    const Json& obj(const char* key) const { return (*this)[key]; }

private:
    friend class Parser;
    Type type_;
    bool bool_;
    double num_;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

} // namespace mini
