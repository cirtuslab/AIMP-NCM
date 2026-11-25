// SPDX-License-Identifier: MIT
// NCM API 镜像服务 - 基于 nooblong/NeteaseCloudMusicApiBackup
// 用法: npm install && node server.js
// 默认端口 3000，插件 apiUrl 填 http://localhost:3000
//
// 安全说明(与插件配套):
//  - 仅监听回环地址; 部署到公网请自行反代并加 TLS
//  - 不发送 CORS 头, 浏览器页面无法跨域借用本服务
//  - 可选共享 Token: 设置环境变量 NCM_MIRROR_TOKEN 后, 所有请求必须带
//    X-NCM-Token 头(插件「镜像 Token」设置项), 否则 403
const crypto = require('crypto')
const express = require('express')
const cookieParser = require('cookie-parser')

let api
try { api = require('NeteaseCloudMusicApi') } catch(e){ console.error('请先 npm install NeteaseCloudMusicApi'); process.exit(1) }

const TOKEN = process.env.NCM_MIRROR_TOKEN || ''

// 恒定时间比较, 降低时序侧信道
function tokenEquals(a, b){
  if(typeof a !== 'string' || typeof b !== 'string') return false
  const ba = Buffer.from(a, 'utf8')
  const bb = Buffer.from(b, 'utf8')
  if(ba.length !== bb.length) return false
  return crypto.timingSafeEqual(ba, bb)
}

const app = express()
app.use(express.json({ limit: '128kb' }))
app.use(express.urlencoded({ extended: false, limit: '128kb' }))
app.use(cookieParser())

// M5: 不提供通配 CORS —— 本镜像按设计只服务本机插件(WinHTTP 客户端, 不受 CORS 约束),
//     开放跨域会让任意网页(含 DNS rebinding)把本机当作访问网易云的匿名中转

// 路径与参数白名单: 只暴露插件用到的接口及明确允许的业务参数,
// 禁止透传 proxy / realIP / crypto / ua 等 NeteaseCloudMusicApi 内部控制字段(防 SSRF / 选项注入)
const ALLOWED_PARAMS = Object.assign(Object.create(null), {
  'login/qr/key': ['cookie'],
  'login/qr/check': ['key', 'cookie'],
  'user/playlist': ['uid', 'limit', 'offset', 'includeVideo', 'cookie'],
  'playlist/track/all': ['id', 'limit', 'offset', 'cookie'],
  'song/url/v1': ['id', 'level', 'encodeType', 'cookie'],
  'song/detail': ['ids', 'c', 'cookie'],
  'lyric': ['id', 'lv', 'tv', 'kv', 'yv', 'cookie'],
  'user/account': ['cookie'],
})

function pickAllowedParams(route, rawQuery, rawBody){
  const keys = ALLOWED_PARAMS[route]
  if(!keys) return null
  const merged = Object.assign(Object.create(null), rawQuery || {}, rawBody || {})
  const safe = {}
  for(const k of keys){
    if(!Object.prototype.hasOwnProperty.call(merged, k)) continue
    const v = merged[k]
    if(v === undefined || v === null) continue
    if(typeof v === 'string' || typeof v === 'number' || typeof v === 'boolean'){
      safe[k] = v
    }
  }
  // cookie 特殊处理:
  // 注意: express 的 query/body 解析已完成一次 URL 解码, 这里不能再 decodeURIComponent,
  // 否则 cookie 值中的 %XX 序列会被二次解码破坏(新版 MUSIC_U 较长, 风险更高)
  if(safe.cookie !== undefined) safe.cookie = String(safe.cookie)
  return safe
}

// 鉴权中间件: Token 校验(可选) + 路径白名单
app.all('*', (req, res, next)=>{
  if(TOKEN){
    const got = req.get('X-NCM-Token') || ''
    if(!tokenEquals(got, TOKEN)) return res.status(403).json({code:403, msg:'forbidden: bad or missing X-NCM-Token'})
  }
  const route = req.path.replace(/^\//,'')
  if(!Object.prototype.hasOwnProperty.call(ALLOWED_PARAMS, route)){
    return res.status(404).json({code:404, msg:'module not found'})
  }
  next()
})

// 通用转发： /song/url/v1 (POST: id/level/cookie 走 body)
app.all('*', async (req,res)=>{
  const url = req.path.replace(/^\//,'') // e.g. song/url/v1
  const query = pickAllowedParams(url, req.query, req.body)
  if(!query) return res.status(404).json({code:404, msg:'module not found'})
  // 映射表
  const map = {
    'login/qr/key': 'login_qr_key',
    'login/qr/check': 'login_qr_check',
    'user/playlist': 'user_playlist',
    'playlist/track/all': 'playlist_track_all',
    'song/url/v1': 'song_url_v1',
    'song/detail': 'song_detail',
    'lyric': 'lyric',
    'user/account': 'user_account',
  }
  const modName = map[url]
  const fn = modName && api[modName]
  if(!fn) return res.status(404).json({code:404, msg:'module not found'})
  try{
    const result = await fn(query)
    // result.cookie 可能为数组
    if(result.cookie) res.set('Set-Cookie', result.cookie)
    res.json(result.body || result)
  }catch(e){
    console.error(e)
    res.status(500).json({code:500, msg: 'internal server error'})
  }
})

const PORT = process.env.PORT || 3000
// 仅监听回环地址: 镜像服务按设计接受 cookie 参数, 不应暴露到局域网/公网
app.listen(PORT, '127.0.0.1', ()=> console.log(`NCM API server running http://127.0.0.1:${PORT}${TOKEN ? ' (token auth enabled)' : ' (no token auth)'}`))
