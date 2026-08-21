#pragma once
#include <string>
#include <windows.h>
struct WeapiResult { std::string params; std::string encSecKey; };
struct EapiResult { std::string params; };
class NcmCrypto {
public:
    static WeapiResult Weapi(const std::string& jsonText);
    static EapiResult Eapi(const std::string& url, const std::string& jsonText);
    static std::string EapiDecrypt(const std::string& hexCipher);
    static std::string AesEncryptCbc(const std::string& text, const std::string& key, const std::string& iv);
    static std::string AesEncryptEcbHex(const std::string& text, const std::string& key);
    static std::string AesDecryptEcbHex(const std::string& hexCipher, const std::string& key);
    static std::string RsaEncrypt(const std::string& text);
    static std::string RandomString(int len);
    static std::string Base64Encode(const std::string& s);
    static std::string BytesToHex(const std::string& s, bool upper=true);
    static std::string HexToBytes(const std::string& hex);
};
