#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 一键同步歌单到 AIMP (无需插件对话框)
# 用法: python sync_to_aimp.py --quality lossless
# 会读取 %APPDATA%/AIMP/NcmPlugin/config.json 的 cookie/apiUrl/selectedPlaylists
import os, json, requests, urllib.parse, sys, concurrent.futures

def load_cfg():
    appdata = os.environ.get("APPDATA", os.path.expanduser("~"))
    p = os.path.join(appdata, "AIMP", "NcmPlugin", "config.json")
    if not os.path.exists(p):
        print(f"未找到配置 {p}, 请先运行 qr_login.py 登录")
        sys.exit(1)
    with open(p,"r",encoding="utf-8") as f:
        return json.load(f), p

def api_get(url, cookie=""):
    headers={"Cookie": cookie} if cookie else {}
    r = requests.get(url, headers=headers, timeout=15)
    return r.json()

def fetch_urls_batch(api, cookie, ids, level, chunk=100, workers=4):
    """批量并发取链，返回 id->url"""
    if not ids:
        return {}
    chunks = [ids[i:i+chunk] for i in range(0, len(ids), chunk)]
    id2url = {}
    def one(chunk_ids):
        try:
            # 批量 id 逗号拼接，兼容 /song/url 和 /song/url/v1
            ids_str = ",".join(map(str, chunk_ids))
            # 优先 v1
            url = f"{api}/song/url/v1?id={ids_str}&level={level}"
            if cookie:
                url += f"&cookie={urllib.parse.quote(cookie)}"
            j = api_get(url, cookie)
            data = j.get("data") or j.get("body",{}).get("data") or []
            for d in data:
                if d.get("url"):
                    id2url[d["id"]] = d["url"]
            # 补空
            for cid in chunk_ids:
                if cid not in id2url:
                    id2url[cid] = ""
            return len([d for d in data if d.get("url")])
        except Exception as e:
            print(f"  批量失败 {chunk_ids[:3]}: {e}")
            # 回退逐条
            for cid in chunk_ids:
                try:
                    uj = api_get(f"{api}/song/url/v1?id={cid}&level={level}&cookie={urllib.parse.quote(cookie)}" if cookie else f"{api}/song/url/v1?id={cid}&level={level}", cookie)
                    d = (uj.get("data") or uj.get("body",{}).get("data") or [{}])[0]
                    id2url[cid] = d.get("url","")
                except:
                    id2url[cid] = ""
            return 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
        list(ex.map(one, chunks))
    return id2url

def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--quality", default=None, help="exhigh/lossless/hires 等，默认读配置")
    ap.add_argument("--api", default=None)
    ap.add_argument("--uid", type=int, default=None)
    ap.add_argument("--all", action="store_true", help="同步所有歌单，否则只同步配置中 selectedPlaylists")
    args = ap.parse_args()

    cfg, cfg_path = load_cfg()
    api = args.api or cfg.get("apiUrl","http://localhost:3000")
    cookie = cfg.get("cookie","")
    quality = args.quality or cfg.get("quality","exhigh")
    print(f"API: {api} quality={quality}")

    # 取 uid
    uid = args.uid
    if not uid:
        # 尝试从配置读，否则请求 /user/account
        if "uid" in cfg and cfg["uid"]:
            uid = int(str(cfg["uid"]))
        else:
            j = api_get(f"{api}/user/account?cookie={urllib.parse.quote(cookie)}" if cookie else f"{api}/user/account", cookie)
            # 兼容
            uid = (j.get("account",{}).get("id") or j.get("profile",{}).get("userId") or
                   j.get("body",{}).get("account",{}).get("id") or j.get("body",{}).get("profile",{}).get("userId"))
            if not uid:
                print(f"无法获取 uid: {j}")
                print("请检查 cookie 是否有效，或手动在 config.json 填 uid")
                return
            print(f"uid={uid}")
            # 回写 uid
            cfg["uid"] = str(uid)
            with open(cfg_path,"w",encoding="utf-8") as f: json.dump(cfg,f,ensure_ascii=False,indent=2)

    # 取歌单列表
    j = api_get(f"{api}/user/playlist?uid={uid}&limit=100&cookie={urllib.parse.quote(cookie)}" if cookie else f"{api}/user/playlist?uid={uid}", cookie)
    playlists = j.get("playlist") or j.get("body",{}).get("playlist") or []
    if not playlists:
        print(f"未获取到歌单: {j}")
        return
    print(f"找到 {len(playlists)} 个歌单:")
    for pl in playlists:
        print(f"  {pl['id']} | {pl['name']} ({pl['trackCount']}首) {'[已选]' if pl['id'] in cfg.get('selectedPlaylists',[]) else ''}")

    # 确定要同步的
    if args.all or not cfg.get("selectedPlaylists"):
        targets = playlists
        print("同步全部歌单 (或配置为空)")
    else:
        sel = set(cfg["selectedPlaylists"])
        targets = [p for p in playlists if p["id"] in sel]
        print(f"同步已选 {len(targets)} 个")

    # 为每个歌单生成 m3u8 并调用 AIMP 播放列表
    # 由于插件目前通过 m3u8 导入，我们生成临时 m3u8 再调用 AIMP 的命令行或直接写入播放列表目录
    tmpdir = os.path.join(os.environ.get("TEMP", "."), "aimp_ncm")
    os.makedirs(tmpdir, exist_ok=True)
    import subprocess

    for pl in targets:
        pid = pl["id"]
        print(f"\n处理歌单 {pid} {pl['name']} ...")
        # 取所有歌曲
        j = api_get(f"{api}/playlist/track/all?id={pid}&limit=1000&cookie={urllib.parse.quote(cookie)}" if cookie else f"{api}/playlist/track/all?id={pid}", cookie)
        songs = j.get("songs") or j.get("body",{}).get("songs") or []
        # 兼容 playlist.tracks
        if not songs and "playlist" in j:
            songs = j["playlist"].get("tracks",[])
        if not songs:
            print(f"  无歌曲: {j.get('code')}")
            continue
        print(f"  {len(songs)} 首，批量并发获取链接...")
        # 是否懒加载：若配置 lazyM3U=true 则写 ncm:// 秒级
        use_lazy = cfg.get("lazyM3U", False)
        m3u = os.path.join(tmpdir, f"ncm_{pid}.m3u8")
        with open(m3u,"w",encoding="utf-8") as f:
            f.write("#EXTM3U\n")
            if use_lazy:
                for s in songs[:1000]:
                    sid = s["id"]
                    artist = "/".join([a["name"] for a in s.get("ar",[])]) or "Unknown"
                    title = s.get("name","")
                    dur = s.get("dt",0)//1000
                    f.write(f"#EXTINF:{dur},{artist} - {title}\n")
                    f.write(f"ncm://{pid}/{sid}.mp3\n")
            else:
                ids = [s["id"] for s in songs[:1000]]
                id2url = fetch_urls_batch(api, cookie, ids, quality, chunk=100, workers=4)
                for s in songs[:1000]:
                    sid = s["id"]
                    url = id2url.get(sid,"")
                    if not url:
                        continue
                    artist = "/".join([a["name"] for a in s.get("ar",[])]) or "Unknown"
                    title = s.get("name","")
                    dur = s.get("dt",0)//1000
                    f.write(f"#EXTINF:{dur},{artist} - {title}\n{url}\n")
        print(f"  已生成 {m3u} -> 尝试用 AIMP 打开")
        # 尝试用 AIMP 打开 m3u8 (创建新播放列表)
        try:
            # 查找 AIMP.exe
            aimp = None
            for cand in [r"C:\Program Files\AIMP\AIMP.exe", r"C:\Program Files (x86)\AIMP\AIMP.exe", os.path.join(os.environ.get("LOCALAPPDATA",""), "AIMP","AIMP.exe")]:
                if os.path.exists(cand):
                    aimp = cand
                    break
            if aimp:
                subprocess.Popen([aimp, m3u], shell=False)
                print(f"  已调用 AIMP 打开")
            else:
                print(f"  请手动用 AIMP 打开: {m3u}")
        except Exception as e:
            print(f"  自动打开失败: {e}, 请手动打开 {m3u}")

    print("\n完成。提示: 链接 10分钟过期，过期后重跑此脚本刷新。")

if __name__=="__main__":
    main()
