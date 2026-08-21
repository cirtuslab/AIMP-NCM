#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 扫码登录并写入 AIMP 配置
import requests, time, json, os, sys, random, webbrowser, tempfile, subprocess, platform

try:
    import qrcode
    HAS_QR = True
except ImportError:
    HAS_QR = False

API = os.environ.get("AIMP_NCM_API", "http://localhost:3000")

# ---- 设备指纹 Cookie（防风控）----
# chainId 中的 sDeviceId 必须与请求携带的 cookie 一致，
# 否则手机扫码确认时会提示"设备环境异常"（风控）
_HEXL = "0123456789ABCDEF"
_SDEV = "".join(random.choice(_HEXL) for _ in range(52))
_DEV = "".join(random.choice(_HEXL) for _ in range(32))
DEVICE_COOKIE = (f"os=pc; appver=2.7.1.198277; osver=10; __remember_me=true; "
                 f"deviceId={_DEV}; sDeviceId={_SDEV}; WEVNSM=1.0.0; channel=netease")

# Session: 自动保持 key 接口下发的 Set-Cookie，轮询时回带（会话粘滞）
session = requests.Session()

def get_unikey():
    r = session.post(f"{API}/login/qr/key",
                     data={"type": 3, "cookie": DEVICE_COOKIE}, timeout=10)
    j = r.json()
    # 兼容多种返回
    if "data" in j and isinstance(j["data"], dict) and "unikey" in j["data"]:
        return j["data"]["unikey"]
    if "unikey" in j:
        return j["unikey"]
    if "data" in j and "data" in j["data"]:
        return j["data"]["data"]["unikey"]
    raise RuntimeError(f"get unikey failed: {j}")

def check(key):
    r = session.post(f"{API}/login/qr/check",
                     data={"key": key, "type": 3, "cookie": DEVICE_COOKIE}, timeout=10)
    j = r.json()
    code = j.get("code", 0)
    if "data" in j and isinstance(j["data"], dict) and "code" in j["data"]:
        code = j["data"]["code"]
    cookie = j.get("cookie","")
    if "data" in j and isinstance(j["data"], dict) and "cookie" in j["data"]:
        cookie = j["data"]["cookie"]
    return code, cookie, j

def save_config(cookie):
    # AIMP 配置路径
    appdata = os.environ.get("APPDATA", os.path.expanduser("~"))
    cfg_path = os.path.join(appdata, "AIMP", "NcmPlugin", "config.json")
    os.makedirs(os.path.dirname(cfg_path), exist_ok=True)
    cfg = {}
    if os.path.exists(cfg_path):
        try:
            with open(cfg_path, "r", encoding="utf-8") as f:
                cfg = json.load(f)
        except: cfg = {}
    cfg["cookie"] = cookie
    if "apiUrl" not in cfg:
        cfg["apiUrl"] = API
    if "quality" not in cfg:
        cfg["quality"] = "exhigh"
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    print(f"[OK] 已写入 {cfg_path}")
    print(f"cookie: {cookie[:60]}...")
    return cfg_path

def extract_challenge(j):
    """从响应中提取风控滑块验证跳转地址(若有)"""
    d = j.get("data") if isinstance(j.get("data"), dict) else {}
    for src in (j, d):
        for k in ("redirectUrl","redirect_url","verifyUrl","captchaUrl"):
            v = src.get(k)
            if isinstance(v, str) and v:
                return v
    return ""

def main():
    print(f"API: {API}")
    print("获取二维码...")
    key = get_unikey()
    # 新协议: 必须带 chainId(与本机 sDeviceId 一致)，否则扫码触发风控
    chain_id = f"v1_{_SDEV}_web_login_{int(time.time()*1000)}"
    url = f"https://music.163.com/login?codekey={key}&chainId={chain_id}"
    print(f"unikey: {key}")
    print(f"请用网易云音乐APP扫码: {url}")
    # 优先本地生成二维码图片（不把登录链接发给第三方在线服务）
    if HAS_QR:
        try:
            img = qrcode.make(url)
            img.show()
            print("已弹出二维码窗口，请扫码")
        except Exception as e:
            print(f"本地渲染失败({e})，请复制上方 url 自行生成二维码")
    else:
        qr_url = f"https://api.qrserver.com/v1/create-qr-code/?size=260x260&data={url}"
        print(f"未安装 qrcode 库(pip install qrcode pillow)，备用二维码图片: {qr_url}")
        try:
            if platform.system()=="Windows":
                os.startfile(qr_url)
        except: pass

    print("等待扫码 (60次轮询)...")
    challenge_opened = False
    for i in range(60):
        time.sleep(2)
        code, cookie, j = check(key)
        if code==803:
            print("扫码成功!")
            save_config(cookie)
            return
        elif code==800:
            print("二维码过期，请重新运行")
            return
        elif code==802:
            print(f"[{i}] 待确认...")
        elif code==801:
            print(f"[{i}] 等待扫码...")
        else:
            # 风控滑块 challenge: 打开验证页并继续轮询
            chal = extract_challenge(j)
            if chal and not challenge_opened:
                challenge_opened = True
                print(f"命中风控验证({code})，已在浏览器打开滑块验证页，完成后自动继续登录...")
                webbrowser.open(chal)
            print(f"code={code} {j}")

    print("超时，请重试")

if __name__=="__main__":
    main()
