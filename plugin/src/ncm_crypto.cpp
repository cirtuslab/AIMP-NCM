#include "ncm_crypto.h"
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <random>
#include <algorithm>
#include <cctype>

static const char* PRESET_KEY = "0CoJUm6Qyw8W8jud";
static const char* IV = "0102030405060708";
static const char* EAPI_KEY = "e82ckenh8dichen8";
static const char* BASE62 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

std::string NcmCrypto::RandomString(int len){
    std::string s; s.reserve(len);
    std::random_device rd; std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0,61);
    for(int i=0;i<len;i++) s+=BASE62[dis(gen)];
    return s;
}
std::string NcmCrypto::Base64Encode(const std::string& s){
    DWORD outLen=0;
    CryptBinaryToStringA((BYTE*)s.data(), (DWORD)s.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &outLen);
    std::string out(outLen,0);
    CryptBinaryToStringA((BYTE*)s.data(), (DWORD)s.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &outLen);
    if(!out.empty() && out.back()=='\0') out.pop_back();
    return out;
}
std::string NcmCrypto::BytesToHex(const std::string& s, bool upper){
    static const char* hexUpper="0123456789ABCDEF";
    static const char* hexLower="0123456789abcdef";
    const char* tbl=upper?hexUpper:hexLower;
    std::string o; o.reserve(s.size()*2);
    for(unsigned char c: s){ o+=tbl[c>>4]; o+=tbl[c&0xF]; }
    return o;
}
std::string NcmCrypto::HexToBytes(const std::string& hex){
    std::string out; out.reserve(hex.size()/2);
    for(size_t i=0;i+1<hex.size();i+=2){ unsigned int b; sscanf_s(hex.c_str()+i, "%02x", &b); out.push_back((char)b); }
    return out;
}
static std::string AesEncryptBCrypt(const std::string& plain, const std::string& key, const std::string& iv, bool useCbc, bool hexOut){
    BCRYPT_ALG_HANDLE hAlg=nullptr; BCRYPT_KEY_HANDLE hKey=nullptr;
    std::string out;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if(!BCRYPT_SUCCESS(st)) return out;
    DWORD objLen=0, res=0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &res, 0);
    std::string keyObj(objLen,0);
    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)(useCbc? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB), (ULONG) (useCbc? sizeof(BCRYPT_CHAIN_MODE_CBC): sizeof(BCRYPT_CHAIN_MODE_ECB)), 0);
    st = BCryptGenerateSymmetricKey(hAlg, &hKey, (PUCHAR)keyObj.data(), objLen, (PUCHAR)key.data(), (ULONG)key.size(), 0);
    if(!BCRYPT_SUCCESS(st)){
        if(hKey) BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg,0);
        return out;
    }
    // 注意: 不要手动 PKCS7 padding —— BCRYPT_BLOCK_PADDING 会自动补。
    // 手动 padding + BCRYPT_BLOCK_PADDING 会导致双重 padding，且输出缓冲区
    // 大小不足导致 BCryptEncrypt 失败（返回空串），weapi 加密全挂。
    std::string cipher(plain.size()+16, 0);
    ULONG outLen=0;
    PUCHAR ivBuf = useCbc ? (PUCHAR)iv.data() : nullptr;
    st = BCryptEncrypt(hKey, (PUCHAR)plain.data(), (ULONG)plain.size(), nullptr, ivBuf, (ULONG)iv.size(), (PUCHAR)cipher.data(), (ULONG)cipher.size(), &outLen, BCRYPT_BLOCK_PADDING);
    if(BCRYPT_SUCCESS(st)){
        cipher.resize(outLen);
        if(hexOut) out = NcmCrypto::BytesToHex(cipher, true);
        else {
            DWORD b64Len=0;
            CryptBinaryToStringA((BYTE*)cipher.data(), (DWORD)cipher.size(), CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF, nullptr, &b64Len);
            out.resize(b64Len);
            CryptBinaryToStringA((BYTE*)cipher.data(), (DWORD)cipher.size(), CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF, out.data(), &b64Len);
            if(!out.empty() && out.back()=='\0') out.pop_back();
        }
    }
    if(hKey) BCryptDestroyKey(hKey);
    if(hAlg) BCryptCloseAlgorithmProvider(hAlg,0);
    return out;
}
static std::string AesDecryptBCryptHex(const std::string& hexCipher, const std::string& key){
    std::string cipher = NcmCrypto::HexToBytes(hexCipher);
    BCRYPT_ALG_HANDLE hAlg=nullptr; BCRYPT_KEY_HANDLE hKey=nullptr;
    std::string out;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if(!BCRYPT_SUCCESS(st)) return out;
    DWORD objLen=0,res=0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &res, 0);
    std::string keyObj(objLen,0);
    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB),0);
    st = BCryptGenerateSymmetricKey(hAlg, &hKey, (PUCHAR)keyObj.data(), objLen, (PUCHAR)key.data(), (ULONG)key.size(), 0);
    if(!BCRYPT_SUCCESS(st)){
        if(hKey) BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg,0);
        return out;
    }
    // BCRYPT_BLOCK_PADDING 解密时自动去除 PKCS7 padding，不要手动再去一次
    std::string plain(cipher.size(), 0);
    ULONG outLen=0;
    st = BCryptDecrypt(hKey, (PUCHAR)cipher.data(), (ULONG)cipher.size(), nullptr, nullptr, 0, (PUCHAR)plain.data(), (ULONG)plain.size(), &outLen, BCRYPT_BLOCK_PADDING);
    if(BCRYPT_SUCCESS(st)){
        plain.resize(outLen);
        out=plain;
    }
    if(hKey) BCryptDestroyKey(hKey);
    if(hAlg) BCryptCloseAlgorithmProvider(hAlg,0);
    return out;
}
std::string NcmCrypto::AesEncryptCbc(const std::string& text, const std::string& key, const std::string& iv){ return AesEncryptBCrypt(text,key,iv,true,false); }
std::string NcmCrypto::AesEncryptEcbHex(const std::string& text, const std::string& key){ return AesEncryptBCrypt(text,key,"",false,true); }
std::string NcmCrypto::AesDecryptEcbHex(const std::string& hexCipher, const std::string& key){ return AesDecryptBCryptHex(hexCipher,key); }
std::string NcmCrypto::RsaEncrypt(const std::string& text){
    // Netease RSA 1024bit, e=0x010001, n as below (from PEM)
    static const char* MOD_HEX = "e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ecbda92557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d813cfe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7";
    static const char* EXP_HEX = "010001";
    // Convert hex to bytes
    auto hexToBytes = [](const char* hex)->std::string{
        std::string out;
        size_t len = strlen(hex);
        out.reserve(len/2);
        for(size_t i=0;i+1<len;i+=2){
            unsigned int b=0;
            sscanf_s(hex+i, "%02x", &b);
            out.push_back((char)b);
        }
        return out;
    };
    std::string mod = hexToBytes(MOD_HEX);
    std::string exp = hexToBytes(EXP_HEX);
    // Prepare plaintext: 128 bytes, 16-byte reversed key at end
    std::string plain(128, 0);
    if(text.size()>128) return "";
    size_t off = 128 - text.size();
    for(size_t i=0;i<text.size();++i) plain[off+i] = text[i];

    // Import RSA public key via BCrypt
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::string outHex;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RSA_ALGORITHM, nullptr, 0);
    if(!BCRYPT_SUCCESS(st)) goto fallback;
    {
        // Build BCRYPT_RSAKEY_BLOB
        DWORD blobSize = sizeof(BCRYPT_RSAKEY_BLOB) + (DWORD)exp.size() + (DWORD)mod.size();
        std::string blob(blobSize, 0);
        BCRYPT_RSAKEY_BLOB* hdr = (BCRYPT_RSAKEY_BLOB*)blob.data();
        hdr->Magic = 0x31415352; // BCRYPT_RSAPUBLIC_MAGIC
        hdr->BitLength = 1024;
        hdr->cbPublicExp = (ULONG)exp.size();
        hdr->cbModulus = (ULONG)mod.size();
        hdr->cbPrime1 = 0;
        hdr->cbPrime2 = 0;
        memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB), exp.data(), exp.size());
        memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB) + exp.size(), mod.data(), mod.size());
        st = BCryptImportKeyPair(hAlg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &hKey, (PUCHAR)blob.data(), blobSize, 0);
        if(!BCRYPT_SUCCESS(st)) { BCryptCloseAlgorithmProvider(hAlg,0); goto fallback; }
        // Encrypt with no padding
        DWORD outLen = 0;
        st = BCryptEncrypt(hKey, (PUCHAR)plain.data(), (ULONG)plain.size(), nullptr, nullptr, 0, nullptr, 0, &outLen, BCRYPT_PAD_NONE);
        if(!BCRYPT_SUCCESS(st) || outLen!=128){ BCryptDestroyKey(hKey); BCryptCloseAlgorithmProvider(hAlg,0); goto fallback; }
        std::string cipher(outLen, 0);
        st = BCryptEncrypt(hKey, (PUCHAR)plain.data(), (ULONG)plain.size(), nullptr, nullptr, 0, (PUCHAR)cipher.data(), outLen, &outLen, BCRYPT_PAD_NONE);
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg,0);
        if(!BCRYPT_SUCCESS(st)) goto fallback;
        // to hex lowercase
        outHex.reserve(256);
        const char* hexDigits="0123456789abcdef";
        for(unsigned char c: cipher){
            outHex.push_back(hexDigits[c>>4]);
            outHex.push_back(hexDigits[c&0xF]);
        }
        return outHex;
    }
fallback:
    // Fallback random (only for proxy mode, direct will fail but not crash)
    {
        std::string rnd; rnd.reserve(256);
        std::random_device rd; std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0,15);
        const char* hex="0123456789abcdef";
        for(int i=0;i<256;i++) rnd+=hex[dis(gen)];
        return rnd;
    }
}
WeapiResult NcmCrypto::Weapi(const std::string& jsonText){
    std::string sec = RandomString(16);
    std::string enc1 = AesEncryptCbc(jsonText, PRESET_KEY, IV);
    std::string enc2 = AesEncryptCbc(enc1, sec, IV);
    std::string rev = sec; std::reverse(rev.begin(), rev.end());
    std::string encSec = RsaEncrypt(rev);
    return {enc2, encSec};
}
EapiResult NcmCrypto::Eapi(const std::string& url, const std::string& jsonText){
    std::string msg = "nobody" + url + "use" + jsonText + "md5forencrypt";
    BCRYPT_ALG_HANDLE hAlg=nullptr; BCRYPT_HASH_HANDLE hHash=nullptr;
    std::string digestHex;
    if(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_MD5_ALGORITHM, nullptr, 0)==0){
        DWORD hashLen=0, res=0;
        BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &res,0);
        std::string hashObj; DWORD objLen=0; BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &res,0);
        hashObj.resize(objLen);
        BCryptCreateHash(hAlg, &hHash, (PUCHAR)hashObj.data(), objLen, nullptr,0,0);
        BCryptHashData(hHash, (PUCHAR)msg.data(), (ULONG)msg.size(),0);
        std::string hash(hashLen,0);
        BCryptFinishHash(hHash, (PUCHAR)hash.data(), hashLen,0);
        digestHex = BytesToHex(hash, false);
        BCryptDestroyHash(hHash); BCryptCloseAlgorithmProvider(hAlg,0);
    } else {
        digestHex = "00000000000000000000000000000000";
    }
    std::string data = url + "-36cd479b6b5-" + jsonText + "-36cd479b6b5-" + digestHex;
    std::string enc = AesEncryptEcbHex(data, EAPI_KEY);
    return {enc};
}
std::string NcmCrypto::EapiDecrypt(const std::string& hexCipher){
    // 服务器可能直接返回明文 JSON（不走 eapi 加密响应），此时 hex 解码会失败。
    // 检测: 若是合法 hex（偶数长度且全是 hex 字符）才解密，否则视为明文原样返回。
    if(hexCipher.size() < 32 || hexCipher.size() % 2 != 0) return hexCipher;
    for(char c : hexCipher){
        if(!isxdigit((unsigned char)c)) return hexCipher;
    }
    std::string dec = AesDecryptEcbHex(hexCipher, EAPI_KEY);
    if(dec.empty()) return hexCipher;
    return dec;
}
