#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
AIMP NCM Python 侧载服务（零依赖，仅标准库 + requests）
提供与 Node 镜像相同的极简 API，供插件或独立使用。
也可直接用于生成 m3u8 播放列表，无需 AIMP 插件。

启动: python app.py  (默认 http://localhost:3000)
生成播放列表: python app.py --sync --cookie "MUSIC_U=xxx; __csrf=xxx" --quality exhigh --output playlist.m3u8
"""
import http.server, urllib.parse, json, os, sys, argparse, time, random
try:
    import requests
except ImportError:
    print("请先 pip install requests")
    sys.exit(1)

API_DOMAIN = "https://music.163.com"
EAPI_KEY = "e82ckenh8dichen8"
# 简化的 weapi/eapi 仅用于演示直连；完整实现建议用 Node 服务
# 此处 Python 版直接使用已封装的 pyncm 或手动 weapi（可选）
# 为简洁，Python 服务默认走 官方 weapi 直连（需安装 pycryptodome）

try:
    from Crypto.Cipher import AES
    import base64, hashlib, binascii
    HAS_CRYPTO=True
except ImportError:
    HAS_CRYPTO=False

PRESET_KEY="0CoJUm6Qyw8W8jud"
IV="0102030405060708"
BASE62="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
# weapi RSA 公钥 (e=0x010001)，服务端用它解出二次 AES 随机密钥；
# 若返回随机占位值，服务端无法解密 params，请求会被判定异常 -> 风控
PUBKEY_MOD="e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ecbda92557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d813cfe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7"
PUBKEY_EXP=0x010001

def aes_cbc_encrypt(text, key, iv):
    from Crypto.Cipher import AES
    import base64
    pad = 16 - len(text) % 16
    text = text + chr(pad)*pad
    cipher = AES.new(key.encode(), AES.MODE_CBC, iv.encode())
    return base64.b64encode(cipher.encrypt(text.encode())).decode()

def rsa_encrypt(text):
    # 明文为反转后的随机密钥，左补零对齐到128字节后 raw RSA（无填充）
    plain = text.encode()
    if len(plain) > 128:
        raise ValueError("rsa plaintext too long")
    m = int.from_bytes(b"\x00"*(128-len(plain)) + plain, "big")
    c = pow(m, PUBKEY_EXP, int(PUBKEY_MOD, 16))
    return format(c, "x").zfill(256)

def weapi(data:dict):
    import json as js
    text = js.dumps(data, ensure_ascii=False)
    sec = ''.join(random.choice(BASE62) for _ in range(16))
    enc1 = aes_cbc_encrypt(text, PRESET_KEY, IV)
    enc2 = aes_cbc_encrypt(enc1, sec, IV)
    encSec = rsa_encrypt(sec[::-1])
    return {"params": enc2, "encSecKey": encSec}

# ---------- 设备指纹 Cookie ----------
# 网易云会校验设备指纹；扫码确认时的 chainId 也依赖其中的 sDeviceId。
# 无任何指纹 Cookie 直接请求 unikey 接口容易命中风控。
def gen_device_cookie():
    hexu = "0123456789ABCDEF"
    dev = ''.join(random.choice(hexu) for _ in range(32))
    sdev = ''.join(random.choice(hexu) for _ in range(52))
    return (f"os=pc; appver=2.7.1.198277; osver=10; __remember_me=true; "
            f"deviceId={dev}; sDeviceId={sdev}; WEVNSM=1.0.0; channel=netease")

_DEFAULT_COOKIE = None
def default_cookie():
    global _DEFAULT_COOKIE
    if _DEFAULT_COOKIE is None:
        _DEFAULT_COOKIE = gen_device_cookie()
    return _DEFAULT_COOKIE

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

def ncm_request(uri, data, cookie=""):
    # 优先尝试 Node 风格的直连 weapi，若失败回退提示
    if not HAS_CRYPTO:
        raise RuntimeError("未安装 pycryptodome，请 pip install pycryptodome 或使用 Node 服务")
    enc = weapi(data)
    headers={
        "Referer": "https://music.163.com",
        "Origin": "https://music.163.com",
        # 完整浏览器 UA（截断的 UA 是风控特征之一）
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
        # 无调用方 cookie 时补默认设备指纹，避免裸请求被风控
        "Cookie": merge_cookie(default_cookie(), cookie),
    }
    url = f"{API_DOMAIN}/weapi{uri[4:]}"
    body = urllib.parse.urlencode(enc)
    headers["Content-Type"]="application/x-www-form-urlencoded"
    r = requests.post(url, data=body, headers=headers, timeout=15)
    return r.json()

def api_login_qr_key(cookie=""):
    return ncm_request("/api/login/qrcode/unikey", {"type":3}, cookie)

def api_login_qr_check(key, cookie=""):
    return ncm_request("/api/login/qrcode/client/login", {"key":key,"type":3}, cookie)

def api_user_playlist(uid, cookie="", limit=100):
    return ncm_request("/api/user/playlist", {"uid":uid,"limit":limit,"offset":0}, cookie)

def api_song_url(ids, level="exhigh", cookie=""):
    # 使用 eapi 更稳定，此处简化用 weapi 的 enhance/player/url/v1
    return ncm_request("/api/song/enhance/player/url/v1", {"ids":str(ids),"level":level,"encodeType":"flac"}, cookie)

def api_playlist_track_all(pid, limit=1000, offset=0, cookie=""):
    """与 Node 镜像 /playlist/track/all 对齐: 按 offset/limit 分页返回歌曲"""
    detail = ncm_request("/api/v6/playlist/detail", {"id": pid, "n": 100000}, cookie)
    playlist = detail.get("playlist") or detail.get("body",{}).get("playlist") or {}
    trackIds = playlist.get("trackIds") or []
    ids = [t["id"] for t in trackIds[offset:offset+limit]]
    songs = []
    if ids:
        c = [{"id": i} for i in ids]
        song_data = ncm_request("/api/v3/song/detail", {"c": json.dumps(c)}, cookie)
        songs = song_data.get("songs") or song_data.get("body",{}).get("songs") or []
    return {"songs": songs, "playlist": playlist}

# ---------- HTTP 服务 ----------
class Handler(http.server.BaseHTTPRequestHandler):
    def _dispatch(self, q):
        self.send_response(200)
        self.send_header("Content-Type","application/json; charset=utf-8")
        # M5: 不发送通配 CORS 头 —— 本服务只面向本机插件(WinHTTP 客户端, 不受 CORS 约束),
        #     开放跨域会让任意网页(含 DNS rebinding)把本机当作访问网易云的匿名中转
        self.end_headers()
        try:
            path = q.pop("_path","")
            if path=="login/qr/key":
                r=api_login_qr_key(q.get("cookie",""))
            elif path=="login/qr/check":
                r=api_login_qr_check(q.get("key",""), q.get("cookie",""))
            elif path=="user/playlist":
                r=api_user_playlist(int(q.get("uid",0)), q.get("cookie",""), int(q.get("limit",30)))
            elif path=="song/url/v1":
                r=api_song_url(q.get("id",0), q.get("level","exhigh"), q.get("cookie",""))
            elif path=="playlist/track/all":
                r=api_playlist_track_all(int(q.get("id",0)), int(q.get("limit",1000)),
                                         int(q.get("offset",0)), q.get("cookie",""))
            elif path=="song/detail":
                r=ncm_request("/api/v3/song/detail", {"c": q.get("c", "[]")}, q.get("cookie",""))
            elif path=="lyric":
                r=ncm_request("/api/song/lyric", {"id": int(q.get("id",0)), "lv":1, "tv":1}, q.get("cookie",""))
            elif path=="user/account":
                r=ncm_request("/api/nuser/account/get", {}, q.get("cookie",""))
            else:
                r={"code":404,"msg":"not found: "+path}
            self.wfile.write(json.dumps(r, ensure_ascii=False).encode())
        except Exception as e:
            self.wfile.write(json.dumps({"code":500,"msg":str(e)}).encode())

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        qs = urllib.parse.parse_qs(parsed.query)
        q = {k:v[0] for k,v in qs.items()}
        q["_path"] = parsed.path.lstrip("/")
        self._dispatch(q)

    def do_POST(self):
        # 兼容 POST 调用（扫码/GUI 已移除，保留以兼容旧客户端）
        length = int(self.headers.get("Content-Length","0") or 0)
        raw = self.rfile.read(length).decode("utf-8","ignore") if length else ""
        parsed = urllib.parse.urlparse(self.path)
        q = {k:v[0] for k,v in urllib.parse.parse_qs(raw).items()}
        for k,v in urllib.parse.parse_qs(parsed.query).items():
            q.setdefault(k, v[0])
        q["_path"] = parsed.path.lstrip("/")
        self._dispatch(q)

    def log_message(self, format, *args):
        sys.stdout.write("%s - - [%s] %s\n" % (self.client_address[0], self.log_date_time_string(), format%args))

# ---------- 独立同步到 m3u8 ----------
def sync_to_m3u8(cookie, quality, output):
    # 1. 获取账号信息 -> uid
    r = ncm_request("/api/nuser/account/get", {}, cookie)
    uid = r.get("account",{}).get("id") or r.get("profile",{}).get("userId")
    if not uid:
        print("无法获取 uid，请检查 cookie 是否包含 MUSIC_U")
        print(r)
        return
    print(f"uid={uid}")
    pls = ncm_request("/api/user/playlist", {"uid":uid,"limit":100}, cookie)
    playlists = pls.get("playlist",[])
    print(f"找到 {len(playlists)} 个歌单")
    for pl in playlists:
        print(f"  {pl['id']} {pl['name']} ({pl['trackCount']}首)")
    # 选择前 2 个歌单演示，或让用户输入
    target = playlists[:2] if len(playlists)>=2 else playlists
    all_songs=[]
    for pl in target:
        pid=pl["id"]
        # 获取歌单所有歌曲
        detail = ncm_request("/api/v6/playlist/detail", {"id":pid,"n":100000}, cookie)
        tids = detail.get("playlist",{}).get("trackIds",[])
        ids = [str(t["id"]) for t in tids[:200]]  # 限制 200 首演示
        if not ids: continue
        # 批量获取歌曲详情
        songs = ncm_request("/api/v3/song/detail", {"c": json.dumps([{"id":int(i)} for i in ids])}, cookie)
        for s in songs.get("songs",[]):
            sid=s["id"]
            url_r = api_song_url(sid, quality, cookie)
            url = ""
            if url_r.get("data"):
                url = url_r["data"][0].get("url","")
            if url:
                line = f"#EXTINF:{s.get('dt',0)//1000},{s['ar'][0]['name']} - {s['name']}\n{url}\n"
                all_songs.append(line)
    with open(output,"w",encoding="utf-8") as f:
        f.write("#EXTM3U\n")
        f.writelines(all_songs)
    print(f"已写入 {len(all_songs)} 首到 {output}，可用 AIMP 直接打开")

if __name__=="__main__":
    ap=argparse.ArgumentParser()
    ap.add_argument("--sync", action="store_true", help="同步歌单到 m3u8")
    ap.add_argument("--cookie", default="", help="MUSIC_U cookie")
    ap.add_argument("--quality", default="exhigh", help="音质 level")
    ap.add_argument("--output", default="ncm.m3u8")
    ap.add_argument("--port", type=int, default=3000)
    args=ap.parse_args()
    if args.sync:
        sync_to_m3u8(args.cookie, args.quality, args.output)
    else:
        print(f"Python NCM service http://localhost:{args.port}")
        print("  GET /login/qr/key")
        print("  GET /login/qr/check?key=xxx")
        print("  GET /user/playlist?uid=xxx")
        print("  GET /playlist/track/all?id=xxx&limit=1000&offset=0")
        print("  GET /lyric?id=xxx")
        print("或 python app.py --sync --cookie 'MUSIC_U=xxx' 自动生成 m3u8")
        # 仅监听回环地址: 镜像服务按设计接受 cookie 参数, 不应暴露到局域网/公网
        http.server.HTTPServer(("127.0.0.1",args.port), Handler).serve_forever()
