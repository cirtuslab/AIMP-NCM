#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
AIMP NCM GUI - 可交付的图形化最终项目
功能: 扫码登录 / 音质选择 / 歌单同步到 AIMP
运行: python gui/app.py
打包: pip install pyinstaller && pyinstaller --onefile --windowed --icon=gui/icon.ico gui/app.py
"""
import os, sys, json, time, threading, subprocess, urllib.parse, tempfile, webbrowser, concurrent.futures
import random, hashlib, re
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from pathlib import Path

try:
    import requests
except ImportError:
    messagebox.showerror("缺少依赖", "请 pip install requests")
    sys.exit(1)

try:
    import qrcode
    from PIL import Image, ImageTk
    HAS_QR = True
except ImportError:
    HAS_QR = False

# 配置路径与 AIMP 插件共用
def get_config_path():
    appdata = os.environ.get("APPDATA", str(Path.home()))
    return os.path.join(appdata, "AIMP", "NcmPlugin", "config.json")

def normalize_api_url(u):
    # 兼容 "iwenwiki.com:3000" 这类无协议头写法，默认 http://
    u = (u or "").strip().rstrip("/")
    if u and not u.startswith(("http://", "https://")):
        u = "http://" + u
    return u

def normalize_cookie(c):
    # 去空白/引号；只粘贴了裸值(未带键名)时自动补 "MUSIC_U="
    # 新版 MUSIC_U 可达数百字符(如738字符)，属正常
    c = (c or "").strip().strip('"').strip("'")
    if c and "MUSIC_U=" not in c:
        c = "MUSIC_U=" + c
    return c

def load_config():
    p = get_config_path()
    cfg = {"apiUrl":"", "useProxy": False, "quality":"exhigh", "cookie":"", "uid":"", "selectedPlaylists":[]}
    if os.path.exists(p):
        try:
            with open(p, "r", encoding="utf-8") as f:
                j = json.load(f)
                # 兼容旧字段
                if "useDirectApi" in j and "useProxy" not in j:
                    j["useProxy"] = not j["useDirectApi"]
                if "apiUrl" in j and j["apiUrl"] and "useProxy" not in j:
                    j["useProxy"] = True
                cfg.update(j)
                # 归一化镜像地址（无协议头自动补 http://）
                cfg["apiUrl"] = normalize_api_url(cfg.get("apiUrl",""))
                # 默认直连时 apiUrl 可为空
                if not cfg.get("useProxy"):
                    cfg["apiUrl"] = cfg.get("apiUrl","")
        except: pass
    # 默认直连，代理可选
    if "useProxy" not in cfg:
        cfg["useProxy"] = bool(cfg.get("apiUrl"))
    return cfg

def save_config(cfg):
    p = get_config_path()
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    return p

QUALITY_MAP = [
    ("标准 128k", "standard"),
    ("较高 192k", "higher"),
    ("极高 320k", "exhigh"),
    ("无损 FLAC", "lossless"),
    ("Hi-Res", "hires"),
    ("高清臻音", "jymaster"),
    ("沉浸环绕", "jyeffect"),
    ("超清母带", "sky"),
]
QUALITY_DICT = {v:k for k,v in QUALITY_MAP}

# 直连 weapi (可选代理) - 仅当 useProxy=False 时使用
# 若未安装 pycryptodome，则直连会提示启用代理
try:
    from Crypto.Cipher import AES
    import base64, hashlib, random as rnd
    HAS_CRYPTO = True
except ImportError:
    HAS_CRYPTO = False

# ---- 设备指纹 Cookie（与插件 EnsureDeviceCookie / go-musicfox 同协议）----
# 网易云风控要点：
# 1) unikey/check 等请求需携带设备指纹 Cookie（sDeviceId/_ntes_nuid 等）
# 2) 扫码二维码内容必须带 chainId=v1_<sDeviceId>_web_login_<ms>，
#    且其中的 sDeviceId 要与请求携带的一致，否则手机端提示"设备环境异常"
_DEVICE_COOKIE = ""

def get_device_cookie():
    global _DEVICE_COOKIE
    if not _DEVICE_COOKIE:
        hexu = "0123456789ABCDEF"
        sdev = "".join(random.choice(hexu) for _ in range(52))
        dev = "".join(random.choice(hexu) for _ in range(32))
        nuid = hashlib.md5((str(time.time()) + sdev).encode()).hexdigest()
        _DEVICE_COOKIE = (
            f"os=pc; appver=2.7.1.198277; osver=10; __remember_me=true; "
            f"deviceId={dev}; sDeviceId={sdev}; _ntes_nuid={nuid}; "
            f"_ntes_nnid={nuid},{int(time.time()*1000)}; "
            f"NMTID={hashlib.md5((nuid + 'NMTID').encode()).hexdigest()}; "
            f"WEVNSM=1.0.0; ntes_kaola_ad=1; channel=netease"
        )
    return _DEVICE_COOKIE

def get_sdevice_id():
    m = re.search(r"sDeviceId=([^;]+)", get_device_cookie())
    return m.group(1) if m else ""

def merge_cookie(*parts):
    seen, out = set(), []
    for p in parts:
        if not p: continue
        for kv in str(p).split(";"):
            kv = kv.strip()
            if not kv or "=" not in kv: continue
            key = kv.split("=",1)[0].strip()
            if key in seen: continue
            seen.add(key); out.append(kv)
    return "; ".join(out)

def build_qr_url(key):
    # 新协议: 必须带 chainId，否则扫码确认触发风控("设备环境异常")
    chain_id = f"v1_{get_sdevice_id()}_web_login_{int(time.time()*1000)}"
    return f"https://music.163.com/login?codekey={key}&chainId={chain_id}"

def extract_challenge(j):
    """从响应中提取风控滑块验证跳转地址(若有)"""
    d = j.get("data") if isinstance(j.get("data"), dict) else {}
    for src in (j, d):
        for k in ("redirectUrl","redirect_url","verifyUrl","captchaUrl"):
            v = src.get(k)
            if isinstance(v, str) and v:
                return v
    return ""

# ---- 官方网页登录渠道: 从本机浏览器读取网易云 Cookie ----
# 流程(与 SPlayer/api-enhanced 官方推荐一致): 用户在官方页面正常登录(无风控)
# -> 程序从浏览器 Cookie 库读出 MUSIC_U 直接使用
import base64 as _b64, shutil as _shutil, sqlite3 as _sqlite3

def _dpapi_decrypt(data: bytes) -> bytes:
    import ctypes, ctypes.wintypes as wt
    class BLOB(ctypes.Structure):
        _fields_ = [("cbData", wt.DWORD),
                    ("pbData", ctypes.POINTER(ctypes.c_char))]
    buf = ctypes.create_string_buffer(data, len(data))
    din = BLOB(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
    dout = BLOB()
    if not ctypes.windll.crypt32.CryptUnprotectData(ctypes.byref(din), None, None, None, None, 0, ctypes.byref(dout)):
        raise OSError("CryptUnprotectData failed")
    out = ctypes.string_at(dout.pbData, dout.cbData)
    ctypes.windll.kernel32.LocalFree(dout.pbData)
    return out

def _chromium_decrypt_value(enc: bytes, aes_key: bytes) -> str:
    """解密 Chromium 系 encrypted_value; 返回明文, 失败返回空"""
    if not enc:
        return ""
    try:
        if enc[:3] in (b"v10", b"v11"):
            # AES-256-GCM: 3字节前缀 + 12字节nonce + 密文 + 16字节tag
            if not aes_key:
                return ""
            nonce, ct, tag = enc[3:15], enc[15:-16], enc[-16:]
            plain = AES.new(aes_key, AES.MODE_GCM, nonce=nonce).decrypt_and_verify(ct, tag)
            return plain.decode("utf-8", "ignore")
        if enc[:3] == b"v20":
            # Chrome 新版应用绑定加密(App-Bound), 无法离线解密 -> 走粘贴通道
            return ""
        return _dpapi_decrypt(enc).decode("utf-8", "ignore")
    except Exception:
        return ""

def _copy_sqlite(src: str) -> str:
    """复制库文件(-wal/-shm 一并)到临时目录, 规避浏览器运行时的文件锁"""
    tmp = os.path.join(tempfile.gettempdir(), "aimp_ncm_ck")
    os.makedirs(tmp, exist_ok=True)
    dst = os.path.join(tmp, os.path.basename(src))
    for suffix in ("", "-wal", "-shm"):
        s = src + suffix
        d = dst + suffix
        try:
            if os.path.exists(s): _shutil.copy2(s, d)
            elif os.path.exists(d): os.remove(d)
        except Exception: pass
    return dst

def _scan_chromium_browsers():
    """扫描 Edge/Chrome/Chromium 各 Profile 的 music.163.com Cookie, 返回 [(来源, {k:v})]"""
    local = os.environ.get("LOCALAPPDATA", "")
    candidates = [
        ("Edge",     os.path.join(local, r"Microsoft\Edge\User Data")),
        ("Chrome",   os.path.join(local, r"Google\Chrome\User Data")),
        ("Chromium", os.path.join(local, r"Chromium\User Data")),
    ]
    found = []
    for label, user_data in candidates:
        if not user_data or not os.path.isdir(user_data):
            continue
        # Local State -> DPAPI 保护的全局 AES key
        aes_key = b""
        ls = os.path.join(user_data, "Local State")
        if os.path.exists(ls):
            try:
                with open(ls, "r", encoding="utf-8") as f:
                    ek = _b64.b64decode(json.load(f)["os_crypt"]["encrypted_key"])
                if ek[:3] == b"DPA":
                    aes_key = _dpapi_decrypt(ek[3:])
            except Exception:
                pass
        profiles = [d for d in os.listdir(user_data)
                    if os.path.isdir(os.path.join(user_data, d)) and (d == "Default" or d.startswith("Profile "))]
        for prof in profiles:
            db = None
            for sub in (os.path.join(prof, "Network", "Cookies"), os.path.join(prof, "Cookies")):
                p = os.path.join(user_data, sub)
                if os.path.exists(p): db = p; break
            if not db: continue
            try:
                tmpdb = _copy_sqlite(db)
                con = _sqlite3.connect(tmpdb)
                rows = con.execute(
                    "SELECT name, value, encrypted_value FROM cookies WHERE host_key LIKE '%music.163.com%'"
                ).fetchall()
                con.close()
            except Exception:
                continue
            ck = {}
            for name, val, enc in rows:
                v = val or _chromium_decrypt_value(enc or b"", aes_key)
                if v: ck[name] = v
            if ck.get("MUSIC_U"):
                suffix = "" if prof == "Default" else f" ({prof})"
                found.append((label + suffix, ck))
    return found

def _scan_firefox():
    """扫描 Firefox profile 的 music.163.com Cookie(Firefox 默认不加密), 返回 [(来源, {k:v})]"""
    root = os.path.join(os.environ.get("APPDATA", ""), "Mozilla", "Firefox")
    profs_dir = os.path.join(root, "Profiles")
    found = []
    profs = []
    if os.path.isdir(profs_dir):
        profs = [os.path.join(profs_dir, d) for d in os.listdir(profs_dir)]
    for prof in profs:
        db = os.path.join(prof, "cookies.sqlite")
        if not os.path.exists(db): continue
        try:
            tmpdb = _copy_sqlite(db)
            con = _sqlite3.connect(tmpdb)
            rows = con.execute(
                "SELECT name, value FROM moz_cookies WHERE host LIKE '%music.163.com%'"
            ).fetchall()
            con.close()
        except Exception:
            continue
        ck = {n: v for n, v in rows if v}
        if ck.get("MUSIC_U"):
            label = "Firefox (" + os.path.basename(prof) + ")"
            found.append((label, ck))
    return found

def import_browser_cookie():
    """尝试所有本地浏览器, 返回 (来源说明, cookie字符串); 未找到时返回 (原因, "")"""
    results = []
    try: results += _scan_firefox()
    except Exception: pass
    try: results += _scan_chromium_browsers()
    except Exception: pass
    if not results:
        return "未在本机浏览器(Firefox/Edge/Chrome)中找到网易云登录信息", ""
    # 多个来源时取第一个成功的; 按优先级拼关键 Cookie
    priority = ["MUSIC_U", "__csrf", "MUSIC_A", "MUSIC_R", "NMTID", "WNMCID"]
    label, ck = results[0]
    parts = []
    for k in priority + [k for k in ck if k not in priority]:
        if k in ck and ck[k]:
            parts.append(f"{k}={ck[k]}")
    return label, "; ".join(parts)

PASTE_GUIDE = (
    "自动读取失败或未找到登录信息。\n\n"
    "请任选其一:\n"
    "1. 用 Edge/Firefox 登录 music.163.com 后重试本按钮 (Chrome 新版加密无法自动读取)\n"
    "2. 手动粘贴: 浏览器登录 music.163.com -> F12 打开开发者工具 -> "
    "应用/存储 -> Cookie -> 复制 MUSIC_U 整段(含键名，值可达数百字符属正常) -> 点「粘贴Cookie」保存\n"
    "   (只复制了值也没关系, 程序会自动补上 MUSIC_U= 键名)"
)

PRESET_KEY = "0CoJUm6Qyw8W8jud"
IV = "0102030405060708"
BASE62 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
PUBKEY_MOD = "e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ecbda92557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d813cfe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7"
PUBKEY_EXP = "010001"

def _aes_cbc_encrypt(text, key, iv):
    pad = 16 - len(text) % 16
    text = text + chr(pad)*pad
    cipher = AES.new(key.encode(), AES.MODE_CBC, iv.encode())
    return base64.b64encode(cipher.encrypt(text.encode())).decode()

def _weapi_encrypt(data: dict):
    import json as js
    text = js.dumps(data, ensure_ascii=False)
    sec = ''.join(rnd.choice(BASE62) for _ in range(16))
    enc1 = _aes_cbc_encrypt(text, PRESET_KEY, IV)
    enc2 = _aes_cbc_encrypt(enc1, sec, IV)
    # RSA: reversed sec -> hex -> pow
    rev = sec[::-1]
    # 需 pycryptodome 的 RSA 或手动 pow
    # 简化: 若无 Crypto.PublicKey 则回退为随机 (仅代理模式可用，直连会失败并提示)
    try:
        from Crypto.PublicKey import RSA
        from Crypto.Util.number import bytes_to_long, long_to_bytes
        # 构造 RSA key
        n = int(PUBKEY_MOD, 16)
        e = int(PUBKEY_EXP, 16)
        # plain = reversed sec 的 bytes
        plain = rev.encode()
        # pad to 128 bytes
        plain_padded = b'\x00'*(128-len(plain)) + plain
        m = bytes_to_long(plain_padded)
        c = pow(m, e, n)
        encSec = format(c, 'x').zfill(256)
        return {"params": enc2, "encSecKey": encSec}
    except Exception as ex:
        # 回退随机，仅代理可用
        import binascii, os
        encSec = binascii.hexlify(os.urandom(128)).decode()[:256]
        return {"params": enc2, "encSecKey": encSec}

def _eapi_encrypt(url, data):
    """eapi 加密, 返回 hex params"""
    import json as js, hashlib
    text = js.dumps(data, ensure_ascii=False) if isinstance(data, dict) else str(data)
    msg = f"nobody{url}use{text}md5forencrypt"
    digest = hashlib.md5(msg.encode()).hexdigest()
    data_str = f"{url}-36cd479b6b5-{text}-36cd479b6b5-{digest}"
    # AES ECB hex
    from Crypto.Cipher import AES
    import binascii
    key = b"e82ckenh8dichen8"
    # PKCS7
    pad = 16 - len(data_str) % 16
    data_str += chr(pad) * pad
    cipher = AES.new(key, AES.MODE_ECB)
    enc = cipher.encrypt(data_str.encode())
    return binascii.hexlify(enc).decode().upper()

def _direct_post(uri, data, cookie="", crypto="eapi"):
    """直连, 支持 weapi/eapi/api, 返回 json 并附带 Set-Cookie"""
    if crypto == "weapi":
        if not HAS_CRYPTO:
            raise RuntimeError("未安装 pycryptodome，直连不可用，请启用代理或 pip install pycryptodome")
        enc = _weapi_encrypt(data)
        headers = {
            "Referer": "https://music.163.com",
            "Origin": "https://music.163.com",
            # 完整浏览器 UA（截断 UA 是风控特征之一）
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
            # 携带设备指纹 Cookie，避免裸请求被风控
            "Cookie": merge_cookie(get_device_cookie(), cookie),
            "Content-Type": "application/x-www-form-urlencoded",
        }
        url = f"https://music.163.com/weapi{uri[4:]}"
        body = urllib.parse.urlencode(enc)
        r = requests.post(url, data=body, headers=headers, timeout=15)
        j = r.json()
    elif crypto == "eapi":
        if not HAS_CRYPTO:
            raise RuntimeError("未安装 pycryptodome，直连不可用，请启用代理或 pip install pycryptodome")
        # 构造 header (简化)
        import time, random
        csrf = ""
        # 从 cookie 解析 __csrf
        if "__csrf" in cookie:
            try:
                csrf = [p for p in cookie.split(";") if "__csrf" in p][0].split("=")[1]
            except: pass
        header = {
            "osver": "Microsoft-Windows-10-Professional-build-22631-64bit",
            "deviceId": "pyncm",
            "os": "pc",
            "appver": "3.0.18.203152",
            "versioncode": "140",
            "mobilename": "",
            "buildver": str(int(time.time()))[:10],
            "resolution": "1920x1080",
            "__csrf": csrf,
            "channel": "netease",
            "requestId": f"{int(time.time()*1000)}_{random.randint(0,9999):04d}",
        }
        # 解析 cookie 中的 MUSIC_U/A
        for part in cookie.split(";"):
            part=part.strip()
            if part.startswith("MUSIC_U="):
                header["MUSIC_U"] = part[len("MUSIC_U="):]
            if part.startswith("MUSIC_A="):
                header["MUSIC_A"] = part[len("MUSIC_A="):]
        # 若无 MUSIC_U，尝试 anonymous
        if "MUSIC_U" not in header:
            # 使用 NMTID 等
            pass
        # eapi data 需包含 header
        e_data = dict(data)
        e_data["header"] = header
        enc_params = _eapi_encrypt(uri, e_data)
        # headers Cookie 为 header 的 urlencode
        cookie_str = "; ".join([f"{urllib.parse.quote(k)}={urllib.parse.quote(str(v))}" for k,v in header.items()])
        headers = {
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.164 NeteaseMusicDesktop/3.0.18.203152",
            "Cookie": cookie_str,
            "Content-Type": "application/x-www-form-urlencoded",
            "Referer": "https://music.163.com",
        }
        url = f"https://interface.music.163.com/eapi{uri[4:]}"
        body = f"params={enc_params}"
        r = requests.post(url, data=body, headers=headers, timeout=15)
        # eapi 响应当 e_r=false 时为明文 json
        try:
            j = r.json()
        except:
            # 尝试解密 (若 e_r=true)
            try:
                from Crypto.Cipher import AES
                import binascii
                key = b"e82ckenh8dichen8"
                cipher = AES.new(key, AES.MODE_ECB)
                dec = cipher.decrypt(binascii.unhexlify(r.text.strip()))
                # 去除 PKCS7
                pad = dec[-1]
                dec = dec[:-pad]
                j = json.loads(dec.decode())
            except:
                j = {"code": 500, "msg": "eapi decrypt failed", "raw": r.text[:500]}
    else:  # api (no encrypt)
        url = f"https://interface.music.163.com{uri}"
        headers = {
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36",
            "Cookie": cookie,
        }
        r = requests.post(url, data=data, headers=headers, timeout=15)
        j = r.json()
    sc = r.headers.get("Set-Cookie", "")
    if sc:
        j["_qr_cookie"] = sc
        if "MUSIC_U" in sc and "cookie" not in j:
            j["cookie"] = "; ".join([p.split(";")[0].strip() for p in sc.split(",") if "=" in p])
            # 有些 Set-Cookie 用 ", " 分隔，简单处理
            if "MUSIC_U" not in j["cookie"]:
                # 尝试直接取 sc
                j["cookie"] = sc
    return j

def ncm_api(cfg, path, data=None, method="GET"):
    """统一入口: 代理可选，自动处理 QR 的 Set-Cookie"""
    use_proxy = cfg.get("useProxy", False)
    api_url = normalize_api_url(cfg.get("apiUrl",""))
    # 对于 QR 轮询，需要用 QR 首次返回的 cookie，而非用户 cookie
    # 调用方可在 cfg 中传入 "_qr_cookie" 覆盖
    # 前置设备指纹 Cookie（镜像模式随 cookie 参数透传给上游）
    cookie = merge_cookie(get_device_cookie(), cfg.get("_qr_cookie") or cfg.get("cookie",""))
    data = data or {}
    if use_proxy and api_url:
        # 走镜像
        base = api_url.rstrip("/")
        # 特殊：QR 相关需携带 QR cookie
        if method=="GET":
            qs = urllib.parse.urlencode({k: (v if isinstance(v,str) else json.dumps(v) if isinstance(v,(dict,list)) else str(v)) for k,v in data.items()})
            if cookie:
                qs += ("&" if qs else "") + "cookie=" + urllib.parse.quote(cookie)
            url = f"{base}{path}?{qs}" if qs else f"{base}{path}"
            try:
                r = requests.get(url, timeout=10)
                j = r.json()
                # 捕获 QR 的 Set-Cookie
                if path == "/login/qr/key" and r.headers.get("Set-Cookie"):
                    j["_qr_cookie"] = r.headers.get("Set-Cookie")
                if path == "/login/qr/check" and r.headers.get("Set-Cookie"):
                    # 登录成功时 Set-Cookie 含 MUSIC_U
                    sc = r.headers.get("Set-Cookie")
                    if "MUSIC_U" in sc:
                        j["cookie"] = sc
                        j["_qr_cookie"] = sc
                return j
            except:
                body = urllib.parse.urlencode(data)
                if cookie:
                    body += ("&" if body else "") + "cookie=" + urllib.parse.quote(cookie)
                r = requests.post(f"{base}{path}", data=body, timeout=10)
                j = r.json()
                if path == "/login/qr/key" and r.headers.get("Set-Cookie"):
                    j["_qr_cookie"] = r.headers.get("Set-Cookie")
                return j
        else:
            body = urllib.parse.urlencode(data)
            if cookie:
                body += ("&" if body else "") + "cookie=" + urllib.parse.quote(cookie)
            r = requests.post(f"{base}{path}", data=body, timeout=10)
            j = r.json()
            if path == "/login/qr/key" and r.headers.get("Set-Cookie"):
                j["_qr_cookie"] = r.headers.get("Set-Cookie")
            if path == "/login/qr/check" and r.headers.get("Set-Cookie"):
                sc = r.headers.get("Set-Cookie")
                if "MUSIC_U" in sc:
                    j["cookie"] = sc
            return j
    else:
        # 直连: 根据 endpoint 选择 weapi/eapi
        weapi_map = {
            "/login/qr/key": "/api/login/qrcode/unikey",
            "/login/qr/check": "/api/login/qrcode/client/login",
            "/user/playlist": "/api/user/playlist",
            "/playlist/track/all": "/api/v6/playlist/detail",
            "/song/url/v1": "/api/song/enhance/player/url/v1",
            "/user/account": "/api/nuser/account/get",
        }
        uri = weapi_map.get(path, path)
        # 决定 crypto: 登录/账号/歌单接口官方走 weapi；
        # 此前登录接口误走 eapi 且 deviceId 固定为 "pyncm"，属于被风控识别的指纹
        crypto = "eapi"
        if path in ("/user/playlist", "/login/qr/key", "/login/qr/check", "/user/account"):
            crypto = "weapi"
        # 特殊: song/url/v1 直连需转换 id->ids + encodeType
        if path == "/song/url/v1" and not (use_proxy and api_url):
            # 兼容批量 id 逗号拼接
            ids_val = data.get("id") or data.get("ids") or ""
            if isinstance(ids_val, (list, tuple)):
                ids_val = ",".join(map(str, ids_val))
            # 去除可能的 [] 包裹
            ids_val = str(ids_val).strip().strip("[]")
            level = data.get("level", "exhigh")
            e_data = {"ids": f"[{ids_val}]", "level": level, "encodeType": "flac"}
            if level == "sky":
                e_data["immerseType"] = "c51"
            return _direct_post(uri, e_data, cookie, crypto="eapi")
        if path == "/playlist/track/all" and not (use_proxy and api_url):
            pid = data.get("id")
            if not pid:
                return {"code": 400, "msg": "missing id"}
            detail = _direct_post("/api/v6/playlist/detail", {"id": pid, "n": 100000}, cookie, crypto="eapi")
            playlist = detail.get("playlist") or detail.get("body",{}).get("playlist") or {}
            trackIds = playlist.get("trackIds") or []
            if not trackIds:
                if "tracks" in playlist:
                    return {"songs": playlist["tracks"]}
                return detail
            ids = trackIds[:1000]
            c = [{"id": t["id"]} for t in ids]
            import json as js
            song_data = _direct_post("/api/v3/song/detail", {"c": js.dumps(c)}, cookie, crypto="eapi")
            songs = song_data.get("songs") or song_data.get("body",{}).get("songs") or []
            return {"songs": songs, "playlist": playlist}
        return _direct_post(uri, data, cookie, crypto=crypto)

def fetch_urls_batch(cfg, ids, level, max_workers=4, chunk=100):
    """批量取链，并发分片，返回 id->url 映射"""
    if not ids:
        return {}
    chunks = [ids[i:i+chunk] for i in range(0, len(ids), chunk)]
    id2url = {}
    def fetch_one(chunk_ids):
        try:
            # 批量 id 用逗号拼接
            j = ncm_api(cfg, "/song/url/v1", {"id": ",".join(map(str, chunk_ids)), "level": level}, method="GET")
            data = j.get("data") or j.get("body",{}).get("data") or []
            # data 可能乱序，按 id 映射
            for d in data:
                if d.get("url"):
                    id2url[d["id"]] = d["url"]
            # 对无 url 的也记录空
            for cid in chunk_ids:
                if cid not in id2url:
                    # 尝试在 data 中找对应 id 但 url 为空
                    for d in data:
                        if d.get("id")==cid and not d.get("url"):
                            id2url[cid] = ""
                            break
                    if cid not in id2url:
                        id2url[cid] = ""
            return len([d for d in data if d.get("url")])
        except Exception as ex:
            # 回退单条 (极少)
            for cid in chunk_ids:
                try:
                    j2 = ncm_api(cfg, "/song/url/v1", {"id": cid, "level": level}, method="GET")
                    d = (j2.get("data") or j2.get("body",{}).get("data") or [{}])[0]
                    id2url[cid] = d.get("url","")
                except:
                    id2url[cid] = ""
            return 0
    # 并发
    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as ex:
        futs = [ex.submit(fetch_one, c) for c in chunks]
        for f in concurrent.futures.as_completed(futs):
            try: f.result()
            except: pass
    return id2url

class NCMGui:
    def __init__(self, root):
        self.root = root
        self.root.title("AIMP NCM 网易云串流 - GUI 控制中心")
        self.root.geometry("820x700")
        self.root.minsize(820, 600)
        try:
            self.root.iconbitmap("gui/icon.ico")
        except: pass
        self.cfg = load_config()
        self.playlists = []  # list of dict
        self.qr_key = None
        self.qr_polling = False

        self._build_ui()
        self._load_cfg_to_ui()
        self._log(f"配置路径: {get_config_path()}")
        self._log("就绪。1.启动镜像 2.扫码登录 3.刷新歌单 4.同步到AIMP")

    def _build_ui(self):
        style = ttk.Style()
        try: style.theme_use("vista")
        except: pass

        # 顶部: API (代理可选)
        top = ttk.Frame(self.root, padding=10)
        top.pack(fill=tk.X)
        self.var_use_proxy = tk.BooleanVar(value=self.cfg.get("useProxy", False))
        self.chk_proxy = ttk.Checkbutton(top, text="启用代理(海外/被封时勾选)", variable=self.var_use_proxy, command=self.on_proxy_toggle)
        self.chk_proxy.pack(side=tk.LEFT)
        ttk.Label(top, text="镜像:").pack(side=tk.LEFT, padx=(10,0))
        self.var_api = tk.StringVar()
        self.entry_api = ttk.Entry(top, textvariable=self.var_api, width=28)
        self.entry_api.pack(side=tk.LEFT, padx=5)
        ttk.Button(top, text="测试", command=self.test_api).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="启动镜像", command=self.start_mirror).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="配置目录", command=self.open_cfg_dir).pack(side=tk.LEFT, padx=2)

        # 登录区
        login = ttk.LabelFrame(self.root, text="① 网易云登录", padding=10)
        login.pack(fill=tk.X, padx=10, pady=5)
        login.columnconfigure(1, weight=1)

        ttk.Button(login, text="获取二维码", command=self.qr_create).grid(row=0, column=0, sticky="w")
        self.lbl_qr_status = ttk.Label(login, text="未登录", foreground="gray")
        self.lbl_qr_status.grid(row=0, column=1, sticky="w", padx=10)
        ttk.Button(login, text="粘贴Cookie登录", command=self.cookie_login).grid(row=0, column=2, padx=5)
        ttk.Button(login, text="从浏览器导入", command=self.browser_login).grid(row=0, column=3, padx=5)
        ttk.Button(login, text="清除登录", command=self.clear_login).grid(row=0, column=4, padx=5)

        # 二维码图片
        self.qr_frame = ttk.Frame(login)
        self.qr_frame.grid(row=1, column=0, columnspan=5, pady=8)
        self.lbl_qr_img = ttk.Label(self.qr_frame, text="二维码将显示在此\n(需安装 qrcode/Pillow)", width=30, anchor="center", background="white", relief="solid")
        self.lbl_qr_img.pack(side=tk.LEFT, padx=10)
        # 右侧 cookie 显示
        right = ttk.Frame(self.qr_frame)
        right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=10)
        ttk.Label(right, text="当前 Cookie (MUSIC_U):").pack(anchor="w")
        self.txt_cookie = tk.Text(right, height=4, wrap="word")
        self.txt_cookie.pack(fill=tk.X, pady=2)
        self.txt_cookie.bind("<KeyRelease>", self.on_cookie_edit)
        ttk.Label(right, text="UID:").pack(anchor="w", pady=(5,0))
        self.var_uid = tk.StringVar()
        ttk.Entry(right, textvariable=self.var_uid, width=20, state="readonly").pack(anchor="w")

        # 音质区
        qf = ttk.LabelFrame(self.root, text="② 音质选择", padding=10)
        qf.pack(fill=tk.X, padx=10, pady=5)
        ttk.Label(qf, text="音质:").pack(side=tk.LEFT)
        self.cbo_quality = ttk.Combobox(qf, values=[k for k,_ in QUALITY_MAP], state="readonly", width=18)
        self.cbo_quality.pack(side=tk.LEFT, padx=5)
        self.cbo_quality.bind("<<ComboboxSelected>>", self.on_quality_change)
        ttk.Label(qf, text="  高品质需会员，否则自动降级", foreground="gray").pack(side=tk.LEFT, padx=10)
        ttk.Button(qf, text="保存配置", command=self.save_cfg).pack(side=tk.RIGHT)

        # 歌单区
        pf = ttk.LabelFrame(self.root, text="③ 歌单选择", padding=10)
        pf.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)
        btn_row = ttk.Frame(pf)
        btn_row.pack(fill=tk.X)
        ttk.Button(btn_row, text="刷新歌单", command=self.refresh_playlists).pack(side=tk.LEFT)
        ttk.Button(btn_row, text="全选", command=lambda: self.select_all(True)).pack(side=tk.LEFT, padx=5)
        ttk.Button(btn_row, text="全不选", command=lambda: self.select_all(False)).pack(side=tk.LEFT)
        self.lbl_pl_count = ttk.Label(btn_row, text="0 个歌单")
        self.lbl_pl_count.pack(side=tk.LEFT, padx=10)

        # 列表 - 用 Treeview 带复选框模拟
        self.tree = ttk.Treeview(pf, columns=("name","count","id"), show="headings", height=8)
        self.tree.heading("name", text="歌单名称")
        self.tree.heading("count", text="歌曲数")
        self.tree.heading("id", text="ID")
        self.tree.column("name", width=420)
        self.tree.column("count", width=80, anchor="center")
        self.tree.column("id", width=120, anchor="center")
        self.tree.pack(fill=tk.BOTH, expand=True, pady=5)
        # 使用 tag 来表示选中
        self.tree.tag_configure("selected", background="#E0F0FF")
        self.tree.bind("<Double-1>", self.toggle_selection)
        self.tree.bind("<space>", self.toggle_selection)
        # 右键菜单
        self._add_tree_menu()

        # 底部同步区
        sync = ttk.Frame(self.root, padding=10)
        sync.pack(fill=tk.X)
        self.var_lazy = tk.BooleanVar(value=self.cfg.get("lazyM3U", False))
        ttk.Checkbutton(sync, text="懒加载(秒级,ncm://)", variable=self.var_lazy, command=lambda: self.cfg.update({"lazyM3U": self.var_lazy.get()}) or self.save_cfg()).pack(side=tk.LEFT, padx=2)
        self.btn_sync = ttk.Button(sync, text="④ 同步选中歌单到 AIMP ▶", command=self.sync_to_aimp, style="Accent.TButton")
        self.btn_sync.pack(side=tk.LEFT, padx=5)
        ttk.Button(sync, text="生成 m3u8 到桌面", command=self.gen_m3u8).pack(side=tk.LEFT, padx=5)
        self.progress = ttk.Progressbar(sync, mode="indeterminate")
        self.progress.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=10)
        ttk.Button(sync, text="打开 AIMP", command=self.open_aimp).pack(side=tk.RIGHT)

        # 日志
        logf = ttk.LabelFrame(self.root, text="日志", padding=5)
        logf.pack(fill=tk.BOTH, padx=10, pady=(5,10))
        self.txt_log = tk.Text(logf, height=6, wrap="word", background="#1e1e1e", foreground="#d4d4d4", insertbackground="white")
        self.txt_log.pack(fill=tk.BOTH, expand=True)
        self.txt_log.configure(state="disabled")

    def _add_tree_menu(self):
        menu = tk.Menu(self.root, tearoff=0)
        menu.add_command(label="选中/取消", command=self.toggle_selection)
        def popup(e):
            try: menu.tk_popup(e.x_root, e.y_root)
            finally: menu.grab_release()
        self.tree.bind("<Button-3>", popup)

    def _log(self, msg):
        self.txt_log.configure(state="normal")
        self.txt_log.insert(tk.END, f"[{time.strftime('%H:%M:%S')}] {msg}\n")
        self.txt_log.see(tk.END)
        self.txt_log.configure(state="disabled")
        self.root.update_idletasks()

    def on_proxy_toggle(self):
        use = self.var_use_proxy.get()
        self.cfg["useProxy"] = use
        if use:
            self.entry_api.configure(state="normal")
            if not self.var_api.get().strip():
                self.var_api.set("http://localhost:3000")
        else:
            self.entry_api.configure(state="disabled")
        self.save_cfg()
        self._log(f"代理已{'启用' if use else '禁用'} (直连)" if not use else f"代理已启用: {self.var_api.get()}")

    def _load_cfg_to_ui(self):
        self.var_use_proxy.set(self.cfg.get("useProxy", False))
        self.var_api.set(self.cfg.get("apiUrl",""))
        try:
            self.entry_api.configure(state="normal" if self.var_use_proxy.get() else "disabled")
        except: pass
        try:
            self.var_lazy.set(bool(self.cfg.get("lazyM3U", False)))
        except: pass
        self.txt_cookie.delete("1.0", tk.END)
        self.txt_cookie.insert("1.0", self.cfg.get("cookie",""))
        self.var_uid.set(str(self.cfg.get("uid","")))
        q = self.cfg.get("quality","exhigh")
        qname = QUALITY_DICT.get(q, "极高 320k")
        self.cbo_quality.set(qname)

    def on_cookie_edit(self, e=None):
        self.cfg["cookie"] = normalize_cookie(self.txt_cookie.get("1.0", tk.END))

    def on_quality_change(self, e=None):
        name = self.cbo_quality.get()
        for k,v in QUALITY_MAP:
            if k==name:
                self.cfg["quality"] = v
                break
        self.save_cfg()

    def save_cfg(self):
        self.cfg["apiUrl"] = normalize_api_url(self.var_api.get())
        self.cfg["useProxy"] = bool(self.var_use_proxy.get())
        try:
            self.cfg["lazyM3U"] = bool(self.var_lazy.get())
        except:
            pass
        if self.cfg["useProxy"] and not self.cfg["apiUrl"]:
            self.cfg["apiUrl"] = "http://localhost:3000"
            self.var_api.set(self.cfg["apiUrl"])
        self.cfg["cookie"] = normalize_cookie(self.txt_cookie.get("1.0", tk.END))
        self.cfg["uid"] = self.var_uid.get().strip()
        # 保存已选
        sel = []
        for item in self.tree.get_children():
            tags = self.tree.item(item, "tags")
            if "selected" in tags:
                pid = self.tree.set(item, "id")
                try: sel.append(int(pid))
                except: pass
        self.cfg["selectedPlaylists"] = sel
        p = save_config(self.cfg)
        mode = f"代理 {self.cfg['apiUrl']}" if self.cfg["useProxy"] else "直连"
        self._log(f"已保存配置 {p} 模式={mode} 质量={self.cfg.get('quality')} 已选{len(sel)}个")

    def open_cfg_dir(self):
        p = get_config_path()
        d = os.path.dirname(p)
        os.makedirs(d, exist_ok=True)
        if sys.platform=="win32":
            os.startfile(d)
        else:
            webbrowser.open(d)
        self._log(f"已打开 {d}")

    def test_api(self):
        api = self.var_api.get().strip()
        threading.Thread(target=self._test_api_thread, args=(api,), daemon=True).start()

    def _test_api_thread(self, api):
        try:
            api = normalize_api_url(api)
            cfg = {"apiUrl": api, "useProxy": self.var_use_proxy.get(), "cookie": self.cfg.get("cookie","")}
            # 代理模式测试镜像，直连模式测试直连
            if cfg["useProxy"] and api:
                r = requests.get(f"{api}/login/qr/key", timeout=5)
                if r.status_code!=200:
                    r = requests.post(f"{api}/login/qr/key", data={"type":3}, timeout=5)
                self._log(f"API 测试 {api} -> {r.status_code} {r.text[:120]}")
                if r.status_code==200:
                    messagebox.showinfo("API 正常", f"{api} 连接成功")
                else:
                    messagebox.showwarning("API 异常", r.text[:300])
            else:
                # 直连测试
                if not HAS_CRYPTO:
                    raise RuntimeError("直连需 pycryptodome，请 pip install pycryptodome 或启用代理")
                j = ncm_api(cfg, "/login/qr/key", {"type":3}, method="POST")
                self._log(f"直连测试 -> {str(j)[:120]}")
                messagebox.showinfo("直连正常", f"直连 music.163.com 成功\n{j}")
        except Exception as e:
            self._log(f"API 连接失败: {e}")
            if self.var_use_proxy.get():
                messagebox.showerror("连接失败", f"{api}\n{e}\n请确认镜像已启动: cd ncm_service && npm start")
            else:
                messagebox.showerror("直连失败", f"{e}\n提示: 直连需 pycryptodome，且国内IP才可直连；海外请启用代理")

    def start_mirror(self):
        # 尝试启动本地 Node 镜像
        def run():
            try:
                # 优先 Node
                if os.path.exists("ncm_service/server.js"):
                    self._log("尝试启动 Node 镜像...")
                    subprocess.Popen(["node","ncm_service/server.js"], creationflags=subprocess.CREATE_NEW_CONSOLE if sys.platform=="win32" else 0)
                    time.sleep(2)
                    self._log("已尝试启动 Node，请等待 3s 后测试连接")
                # 回退 Python
                elif os.path.exists("ncm_service/app.py"):
                    self._log("尝试启动 Python 镜像...")
                    subprocess.Popen([sys.executable, "ncm_service/app.py"], creationflags=subprocess.CREATE_NEW_CONSOLE if sys.platform=="win32" else 0)
                else:
                    self._log("未找到 ncm_service，请手动启动")
                    messagebox.showinfo("启动镜像", "请在终端执行:\ncd ncm_service && npm install && npm start")
            except Exception as e:
                self._log(f"启动失败: {e}")
        threading.Thread(target=run, daemon=True).start()

    # 登录
    def qr_create(self):
        if self.qr_polling:
            messagebox.showinfo("提示","正在轮询，请稍候")
            return
        threading.Thread(target=self._qr_thread, daemon=True).start()

    def _qr_thread(self):
        # 使用统一 ncm_api，尊重代理开关，正确处理 QR 的 Set-Cookie 粘滞
        self.qr_polling = True
        try:
            self._log("获取二维码...")
            cfg0 = {"apiUrl": self.var_api.get().strip(), "useProxy": self.var_use_proxy.get(), "cookie": self.cfg.get("cookie","")}
            try:
                j = ncm_api(cfg0, "/login/qr/key", {"type":3}, method="POST")
            except Exception as ex:
                if not cfg0["useProxy"]:
                    self._log(f"直连失败: {ex}，尝试提示")
                    self.root.after(0, lambda: messagebox.showerror("直连失败", f"{ex}\n海外/被封请启用代理"))
                raise
            # 捕获 QR 会话的 cookie (Set-Cookie)，用于后续轮询
            qr_cookie = j.get("_qr_cookie") or j.get("cookie") or ""
            if not qr_cookie and "data" in j and isinstance(j["data"], dict):
                # 有些镜像把 cookie 放在 data.cookie
                qr_cookie = j["data"].get("cookie","")
            self._log(f"QR cookie: {str(qr_cookie)[:80]}...")
            key = None
            if "data" in j and isinstance(j["data"], dict) and "unikey" in j["data"]:
                key = j["data"]["unikey"]
            elif "unikey" in j:
                key = j["unikey"]
            elif "data" in j and "data" in j["data"]:
                key = j["data"]["data"]["unikey"]
            if not key and isinstance(j, dict) and "unikey" in str(j):
                pass
            if not key:
                self._log(f"获取二维码失败: {j}")
                self.root.after(0, lambda: messagebox.showerror("失败", str(j)))
                return
            self.qr_key = key
            # 新协议: 带 chainId(与请求 sDeviceId 一致)，否则手机扫码提示设备环境异常
            url = build_qr_url(key)
            self._log(f"unikey={key}")
            # 显示二维码
            if HAS_QR:
                qr = qrcode.make(url)
                # 缩放到 220
                qr = qr.resize((220,220), Image.NEAREST)
                # 在主线程更新 UI
                def show():
                    imgtk = ImageTk.PhotoImage(qr)
                    self.lbl_qr_img.configure(image=imgtk, text="")
                    self.lbl_qr_img.image = imgtk
                    self.lbl_qr_status.configure(text=f"请用网易云APP扫码\n{url}", foreground="blue")
                self.root.after(0, show)
                # 也打开浏览器二维码服务备用
                # webbrowser.open(f"https://api.qrserver.com/v1/create-qr-code/?size=220x220&data={urllib.parse.quote(url)}")
            else:
                self.root.after(0, lambda: self.lbl_qr_status.configure(text=url))
                webbrowser.open(url)

            # 轮询 (需携带首次的 QR cookie)
            check_cfg = {"apiUrl": self.var_api.get().strip(), "useProxy": self.var_use_proxy.get(), "cookie": self.cfg.get("cookie",""), "_qr_cookie": qr_cookie}
            # 若有 QR cookie，优先用它覆盖
            if qr_cookie:
                check_cfg["_qr_cookie"] = qr_cookie
                check_cfg["cookie"] = qr_cookie  # 同时填 cookie 字段以兼容镜像的 query 拼接
            self._challenge_opened = False
            max_polls = 60
            i = 0
            while i < max_polls and self.qr_polling:
                i += 1
                time.sleep(2)
                if not self.qr_polling:
                    break
                try:
                    j = ncm_api(check_cfg, "/login/qr/check", {"key":key,"type":3}, method="POST")
                    code = j.get("code",0)
                    if "data" in j and isinstance(j["data"], dict) and "code" in j["data"]:
                        code = j["data"]["code"]
                    self._log(f"轮询 {i}: code={code}")
                    if code==803:
                        # 登录成功，cookie 可能在 body.cookie 或 _qr_cookie/Set-Cookie
                        cookie = j.get("cookie","") or j.get("_qr_cookie","")
                        if "data" in j and isinstance(j["data"], dict) and "cookie" in j["data"]:
                            cookie = j["data"]["cookie"]
                        if not cookie:
                            # 尝试从所有字段找 MUSIC_U
                            import re
                            m = re.search(r"MUSIC_U=[^;]+", str(j))
                            if m:
                                cookie = m.group(0)
                        # 若仍无，尝试用 _qr_cookie 中的 MUSIC_U
                        if not cookie and qr_cookie and "MUSIC_U" in qr_cookie:
                            cookie = qr_cookie
                        self._log(f"登录成功 cookie: {str(cookie)[:100]}...")
                        self.root.after(0, lambda c=cookie: self._on_login_success(c))
                        return
                    elif code==800:
                        self.root.after(0, lambda: self.lbl_qr_status.configure(text="二维码过期，请重新获取", foreground="red"))
                        return
                    elif code==802:
                        self.root.after(0, lambda: self.lbl_qr_status.configure(text="待确认...", foreground="orange"))
                    elif code==801:
                        self.root.after(0, lambda: self.lbl_qr_status.configure(text="等待扫码...", foreground="gray"))
                    else:
                        # 风控滑块 challenge(8821/8822 等): 用默认浏览器弹出验证页，
                        # 不中止轮询——完成验证后同一 unikey 通常会直接放行
                        chal = extract_challenge(j)
                        if chal or code in (8821, 8822):
                            if chal and not self._challenge_opened:
                                self._challenge_opened = True
                                max_polls += 150  # 给用户留足验证时间 (约+5分钟)
                                self._log(f"命中风控验证({code})，打开滑块页面: {chal}")
                                webbrowser.open(chal)
                            msg = "已在浏览器打开滑块验证页\n完成后自动继续登录..." if self._challenge_opened else f"需要风控验证(code={code})\n请重新获取二维码"
                            self.root.after(0, lambda m=msg: self.lbl_qr_status.configure(text=m, foreground="orange"))
                except Exception as e:
                    self._log(f"轮询错误: {e}")
            self._log("轮询超时")
        finally:
            self.qr_polling = False

    def _on_login_success(self, cookie):
        if not cookie:
            # 尝试从响应头提取
            pass
        self.cfg["cookie"] = cookie
        self.txt_cookie.delete("1.0", tk.END)
        self.txt_cookie.insert("1.0", cookie)
        self.lbl_qr_status.configure(text="登录成功!", foreground="green")
        self._log(f"登录成功 cookie长度{len(cookie)}")
        # 自动获取 uid
        threading.Thread(target=self._fetch_uid, daemon=True).start()
        self.save_cfg()

    def _fetch_uid(self):
        # 使用统一 ncm_api，尊重代理开关
        try:
            j = ncm_api(self.cfg, "/user/account", {}, method="GET")
            uid = (j.get("account",{}).get("id") or j.get("profile",{}).get("userId") or
                   j.get("body",{}).get("account",{}).get("id") or j.get("body",{}).get("profile",{}).get("userId"))
            if uid:
                self.cfg["uid"] = str(uid)
                self.root.after(0, lambda: self.var_uid.set(str(uid)))
                self.root.after(0, lambda: self._log(f"获取 UID: {uid}"))
                self.save_cfg()
                # 自动刷新歌单
                self.root.after(100, self.refresh_playlists)
        except Exception as e:
            self._log(f"获取 UID 失败: {e}")

    def browser_login(self):
        # 官方渠道: 用户在浏览器正常登录 music.163.com 后, 自动读取其 Cookie
        self.lbl_qr_status.configure(text="正在读取浏览器登录态...", foreground="gray")
        self._log("正在扫描本机浏览器 (Firefox/Edge/Chrome/Chromium) 的网易云登录信息...")
        threading.Thread(target=self._browser_thread, daemon=True).start()

    def _browser_thread(self):
        try:
            label, cookie = import_browser_cookie()
        except Exception as e:
            label, cookie = f"读取失败: {e}", ""
        if not cookie:
            # 未找到: label 为原因说明, 弹出引导
            self.root.after(0, lambda r=label or "": messagebox.showwarning(
                "从浏览器导入", PASTE_GUIDE + (f"\n\n[{r}]" if r else "")))
            self.root.after(0, lambda: self.lbl_qr_status.configure(text="未找到登录信息", foreground="red"))
            return
        masked = cookie[:40] + "..." if len(cookie) > 40 else cookie
        self._log(f"已从 {label} 读取到登录态: {masked}")
        self.root.after(0, lambda s=label, c=cookie: self._on_browser_import(s, c))

    def _on_browser_import(self, source, cookie):
        self.cfg["cookie"] = cookie
        self.txt_cookie.delete("1.0", tk.END)
        self.txt_cookie.insert("1.0", cookie)
        self.lbl_qr_status.configure(text=f"已从 {source} 导入登录态!", foreground="green")
        self.save_cfg()
        threading.Thread(target=self._fetch_uid, daemon=True).start()

    def cookie_login(self):
        # 弹出输入框
        top = tk.Toplevel(self.root)
        top.title("粘贴 Cookie")
        top.geometry("500x200")
        ttk.Label(top, text="请粘贴从浏览器复制的 Cookie (需包含 MUSIC_U):").pack(pady=5)
        txt = tk.Text(top, height=6, wrap="word")
        txt.pack(fill=tk.BOTH, expand=True, padx=10)
        txt.insert("1.0", self.cfg.get("cookie",""))
        def ok():
            c = normalize_cookie(txt.get("1.0", tk.END))
            if not c:
                return
            if "MUSIC_U" not in c:
                if not messagebox.askyesno("确认","Cookie 中未发现 MUSIC_U，是否仍保存?"):
                    return
            self.cfg["cookie"] = c
            self.txt_cookie.delete("1.0", tk.END)
            self.txt_cookie.insert("1.0", c)
            self.save_cfg()
            top.destroy()
            self._log("已保存 Cookie")
            threading.Thread(target=self._fetch_uid, daemon=True).start()
        ttk.Button(top, text="保存", command=ok).pack(pady=5)

    def clear_login(self):
        if messagebox.askyesno("确认","清除登录信息?"):
            self.cfg["cookie"]=""
            self.cfg["uid"]=""
            self.txt_cookie.delete("1.0", tk.END)
            self.var_uid.set("")
            self.save_cfg()
            self.lbl_qr_status.configure(text="已清除", foreground="gray")
            self._log("已清除登录")

    # 歌单
    def refresh_playlists(self):
        threading.Thread(target=self._refresh_thread, daemon=True).start()

    def _refresh_thread(self):
        api = self.var_api.get().strip()
        cookie = self.cfg.get("cookie","")
        uid = self.cfg.get("uid","") or self.var_uid.get()
        # UID 为空但有 cookie 时, 先尝试通过 /user/account 自动补全, 不要直接要求扫码
        if (not uid) and cookie:
            self._log("UID 为空，正在通过 /user/account 自动获取...")
            try:
                j = ncm_api(self.cfg, "/user/account", {}, method="GET")
                acc = j.get("account") if isinstance(j.get("account"), dict) else {}
                prof = j.get("profile") if isinstance(j.get("profile"), dict) else {}
                body = j.get("body") if isinstance(j.get("body"), dict) else {}
                uid = (acc.get("id") or prof.get("userId")
                       or (body.get("account") or {}).get("id")
                       or (body.get("profile") or {}).get("userId") or "")
                if uid:
                    self.cfg["uid"] = str(uid)
                    self.root.after(0, lambda u=str(uid): self.var_uid.set(u))
                    self.save_cfg()
                    self._log(f"已自动获取 UID: {uid}")
            except Exception as e:
                self._log(f"自动获取 UID 失败: {e}")
        if not uid:
            msg = ("Cookie 为空，请先登录（扫码 / 从浏览器导入 / 粘贴Cookie）"
                   if not cookie else
                   "已填入 Cookie 但获取 UID 失败。\n请检查网络或镜像是否可用，确认粘贴的是完整有效的 MUSIC_U（值可达数百字符，只贴值也可以，程序会自动补键名），然后重试刷新。")
            self._log(msg.replace("\n", " "))
            self.root.after(0, lambda m=msg: messagebox.showwarning("无法获取歌单", m))
            return
        try:
            uid = int(str(uid).strip())
        except:
            self._log(f"UID 无效: {uid}")
            return
        self._log(f"拉取歌单 uid={uid} ...")
        self.root.after(0, lambda: self.lbl_pl_count.configure(text="加载中..."))
        try:
            j = ncm_api(self.cfg, "/user/playlist", {"uid":uid, "limit":100}, method="GET")
            pls = j.get("playlist") or j.get("body",{}).get("playlist") or []
            if not pls:
                self._log(f"未获取到歌单: {j}")
                # 若直连失败提示启用代理
                if not self.cfg.get("useProxy"):
                    self._log("提示: 直连可能被限制，尝试启用代理")
                self.root.after(0, lambda: messagebox.showwarning("失败", str(j)[:500]))
                return
            self.playlists = pls
            self.root.after(0, lambda: self._update_tree(pls))
            self._log(f"已获取 {len(pls)} 个歌单")
        except Exception as e:
            self._log(f"刷新失败: {e}")
            msg = str(e)
            if not self.cfg.get("useProxy") and "pycryptodome" in msg:
                msg += "\n请启用代理或 pip install pycryptodome"
            self.root.after(0, lambda m=msg: messagebox.showerror("失败", m))

    def _update_tree(self, pls):
        for i in self.tree.get_children():
            self.tree.delete(i)
        sel = set(self.cfg.get("selectedPlaylists",[]))
        for pl in pls:
            pid = pl["id"]
            name = pl["name"]
            cnt = pl.get("trackCount",0)
            tags = ("selected",) if pid in sel else ()
            item = self.tree.insert("", tk.END, values=(name, cnt, pid), tags=tags)
            if pid in sel:
                self.tree.selection_add(item)
        self.lbl_pl_count.configure(text=f"{len(pls)} 个歌单，已选 {len(sel)}")

    def toggle_selection(self, event=None):
        sel = self.tree.selection()
        if not sel:
            # 点击行切换
            item = self.tree.identify_row(event.y) if event else None
            if item:
                sel = (item,)
            else:
                return
        for item in sel:
            tags = self.tree.item(item, "tags")
            if "selected" in tags:
                self.tree.item(item, tags=())
                self.tree.selection_remove(item)
            else:
                self.tree.item(item, tags=("selected",))
                self.tree.selection_add(item)
        # 更新已选计数
        cnt = len([1 for i in self.tree.get_children() if "selected" in self.tree.item(i,"tags")])
        self.lbl_pl_count.configure(text=f"{len(self.playlists)} 个歌单，已选 {cnt}")

    def select_all(self, flag):
        for item in self.tree.get_children():
            if flag:
                self.tree.item(item, tags=("selected",))
                self.tree.selection_add(item)
            else:
                self.tree.item(item, tags=())
                self.tree.selection_remove(item)
        cnt = len(self.tree.get_children()) if flag else 0
        self.lbl_pl_count.configure(text=f"{len(self.playlists)} 个歌单，已选 {cnt}")

    # 同步
    def sync_to_aimp(self):
        # 保存
        self.save_cfg()
        sel = self.cfg.get("selectedPlaylists",[])
        if not sel:
            # 从 tree 获取
            sel = []
            for item in self.tree.get_children():
                if "selected" in self.tree.item(item, "tags"):
                    sel.append(int(self.tree.set(item, "id")))
            if not sel:
                messagebox.showwarning("未选择","请先勾选歌单")
                return
            self.cfg["selectedPlaylists"] = sel
            save_config(self.cfg)
        threading.Thread(target=self._sync_thread, args=(sel,), daemon=True).start()

    def _sync_thread(self, pids):
        api = self.var_api.get().strip()
        cookie = self.cfg.get("cookie","")
        quality = self.cfg.get("quality","exhigh")
        self.root.after(0, lambda: self.progress.start(10))
        self.root.after(0, lambda: self.btn_sync.configure(state="disabled"))
        self._log(f"开始同步 {len(pids)} 个歌单 质量={quality}")
        total = 0
        # 准备临时目录
        tmpdir = os.path.join(tempfile.gettempdir(), "aimp_ncm")
        os.makedirs(tmpdir, exist_ok=True)
        m3us = []
        try:
            # 合并所有选中歌单到单一 m3u，避免多次 Popen 触发 AIMP 加载器并发崩溃
            use_lazy = self.cfg.get("lazyM3U", False)
            merged_m3u = os.path.join(tmpdir, "ncm_playlist.m3u8")
            # 先收集所有歌曲
            all_songs = []  # list of (pid, song)
            for pid in pids:
                self._log(f"处理歌单 {pid} ...")
                try:
                    j = ncm_api(self.cfg, "/playlist/track/all", {"id": pid}, method="GET")
                    songs = j.get("songs") or j.get("body",{}).get("songs") or []
                    if not songs and "playlist" in j:
                        songs = j["playlist"].get("tracks",[])
                    if not songs:
                        self._log(f"  歌单 {pid} 无歌曲: {j.get('code')}")
                        if not self.cfg.get("useProxy"):
                            self._log("  提示: 直连可能被限制，请启用代理")
                        continue
                    for s in songs[:1000]:
                        all_songs.append((pid, s))
                    self._log(f"  歌单 {pid}: {len(songs)} 首已加入队列")
                except Exception as e:
                    self._log(f"  歌单 {pid} 失败: {e}")
                    continue

            if not all_songs:
                self._log("未获取到任何歌曲")
                return

            self._log(f"共 {len(all_songs)} 首，{'懒加载' if use_lazy else '批量获取链接'}...")
            with open(merged_m3u, "w", encoding="utf-8") as f:
                f.write("#EXTM3U\n")
                if use_lazy:
                    for pid, s in all_songs:
                        sid = s["id"]
                        title = s.get("name","")
                        artist = "/".join([a["name"] for a in s.get("ar",[])]) or "Unknown"
                        dur = s.get("dt",0)//1000
                        f.write(f"#EXTINF:{dur},{artist} - {title}\n")
                        f.write(f"ncm://{pid}/{sid}.mp3\n")
                        total += 1
                else:
                    ids = [s["id"] for _, s in all_songs]
                    id2url = fetch_urls_batch(self.cfg, ids, quality, max_workers=4, chunk=100)
                    for pid, s in all_songs:
                        sid = s["id"]
                        url2 = id2url.get(sid,"")
                        if not url2:
                            continue
                        title = s.get("name","")
                        artist = "/".join([a["name"] for a in s.get("ar",[])]) or "Unknown"
                        dur = s.get("dt",0)//1000
                        f.write(f"#EXTINF:{dur},{artist} - {title}\n{url2}\n")
                        total += 1
            m3us = [merged_m3u]
            mode = "懒加载 ncm://" if use_lazy else "批量预取"
            self._log(f"已生成 {merged_m3u} {total}首 [{mode}]")

            # 单次调用 AIMP，避免多重 Popen 并发加载器崩溃
            if m3us:
                aimp = None
                for cand in [r"C:\Program Files\AIMP\AIMP.exe", r"C:\Program Files (x86)\AIMP\AIMP.exe", os.path.join(os.environ.get("LOCALAPPDATA",""), "AIMP","AIMP.exe")]:
                    if os.path.exists(cand):
                        aimp = cand
                        break
                if aimp:
                    try:
                        # 仅调用一次，AIMP 会将 m3u 作为播放列表导入
                        subprocess.Popen([aimp, m3us[0]], shell=False)
                        self._log(f"已调用 AIMP 打开 {m3us[0]}，共 {total} 首")
                        self.root.after(0, lambda: messagebox.showinfo("完成", f"已同步 {len(pids)} 个歌单，共 {total} 首\n已自动用 AIMP 打开，请在播放列表查看\n[{mode}]"))
                    except Exception as e:
                        self._log(f"调用 AIMP 失败: {e}")
                        self.root.after(0, lambda: messagebox.showinfo("完成", f"已生成 {m3us[0]}\n请手动用 AIMP 打开"))
                else:
                    self._log("未找到 AIMP.exe，请手动打开 m3u8:")
                    self._log(f"  {m3us[0]}")
                    self.root.after(0, lambda: messagebox.showinfo("完成", f"已生成 {m3us[0]}\n请手动用 AIMP 打开"))
            else:
                self._log("未生成任何 m3u8")
        finally:
            self.root.after(0, lambda: self.progress.stop())
            self.root.after(0, lambda: self.btn_sync.configure(state="normal"))
            self._log("同步结束，链接10分钟过期，过期后重跑同步")

    def gen_m3u8(self):
        # 生成到桌面
        desktop = os.path.join(Path.home(), "Desktop")
        if not os.path.exists(desktop):
            desktop = os.getcwd()
        p = filedialog.asksaveasfilename(initialdir=desktop, defaultextension=".m3u8", filetypes=[("M3U8","*.m3u8")], initialfile="ncm_playlist.m3u8")
        if not p:
            return
        # 复用 sync 逻辑但只生成单个合并文件
        self.save_cfg()
        sel = self.cfg.get("selectedPlaylists",[])
        if not sel:
            messagebox.showwarning("未选择","请先勾选歌单")
            return
        threading.Thread(target=self._gen_m3u8_thread, args=(p, sel), daemon=True).start()

    def _gen_m3u8_thread(self, path, pids):
        quality = self.cfg.get("quality","exhigh")
        use_lazy = self.cfg.get("lazyM3U", False)
        self._log(f"生成合并 m3u8 -> {path} 模式={'代理' if self.cfg.get('useProxy') else '直连'} {'懒加载' if use_lazy else '批量'}")
        try:
            with open(path, "w", encoding="utf-8") as out:
                out.write("#EXTM3U\n")
                total=0
                for pid in pids:
                    j = ncm_api(self.cfg, "/playlist/track/all", {"id": pid}, method="GET")
                    songs = j.get("songs") or j.get("body",{}).get("songs") or []
                    if not songs and "playlist" in j:
                        songs = j["playlist"].get("tracks",[])
                    if use_lazy:
                        for s in songs[:1000]:
                            out.write(f"#EXTINF:{s.get('dt',0)//1000},{'/'.join([a['name'] for a in s.get('ar',[])])} - {s.get('name','')}\n")
                            out.write(f"ncm://{pid}/{s['id']}.mp3\n")
                            total+=1
                    else:
                        ids = [s["id"] for s in songs[:1000]]
                        id2url = fetch_urls_batch(self.cfg, ids, quality, max_workers=4, chunk=100)
                        for s in songs[:1000]:
                            url = id2url.get(s["id"],"")
                            if url:
                                out.write(f"#EXTINF:{s.get('dt',0)//1000},{'/'.join([a['name'] for a in s.get('ar',[])])} - {s.get('name','')}\n{url}\n")
                                total+=1
            self._log(f"已生成 {path} 共 {total} 首 {'懒加载' if use_lazy else '批量'}")
            self.root.after(0, lambda: messagebox.showinfo("完成", f"已生成 {path}\n共 {total} 首\n可用 AIMP 直接打开"))
        except Exception as e:
            self._log(f"生成失败: {e}")
            self.root.after(0, lambda: messagebox.showerror("失败", str(e)))

    def open_aimp(self):
        for cand in [r"C:\Program Files\AIMP\AIMP.exe", r"C:\Program Files (x86)\AIMP\AIMP.exe"]:
            if os.path.exists(cand):
                subprocess.Popen([cand], shell=False)
                return
        messagebox.showinfo("AIMP","未找到 AIMP.exe")

def main():
    root = tk.Tk()
    app = NCMGui(root)
    root.mainloop()

if __name__=="__main__":
    main()
