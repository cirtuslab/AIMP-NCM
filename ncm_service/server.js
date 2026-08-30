// NCM API 镜像服务 - 基于 nooblong/NeteaseCloudMusicApiBackup
// 用法: npm install && node server.js
// 默认端口 3000，插件 apiUrl 填 http://localhost:3000
//
// 安全说明(与插件配套):
//  - 仅监听回环地址; 部署到公网请自行反代并加 TLS
//  - 不发送 CORS 头, 浏览器页面无法跨域借用本服务
//  - 可选共享 Token: 设置环境变量 NCM_MIRROR_TOKEN 后, 所有请求必须带
//    X-NCM-Token 头(插件「镜像 Token」设置项), 否则 403
const express = require('express')
const cookieParser = require('cookie-parser')

let api
try { api = require('NeteaseCloudMusicApi') } catch(e){ console.error('请先 npm install NeteaseCloudMusicApi'); process.exit(1) }

const TOKEN = process.env.NCM_MIRROR_TOKEN || ''

const app = express()
app.use(express.json())
app.use(express.urlencoded({extended:true}))
app.use(cookieParser())

// M5: 不提供通配 CORS —— 本镜像按设计只服务本机插件(WinHTTP 客户端, 不受 CORS 约束),
//     开放跨域会让任意网页(含 DNS rebinding)把本机当作访问网易云的匿名中转

// 路径白名单: 只暴露插件用到的接口
const ALLOWED = new Set([
  'login/qr/key',
  'login/qr/check',
  'user/playlist',
  'playlist/track/all',
  'song/url/v1',
  'song/detail',
  'lyric',
  'user/account',
])

// 鉴权中间件: Token 校验(可选) + 路径白名单
app.all('*', (req, res, next)=>{
  if(TOKEN){
    const got = req.get('X-NCM-Token') || ''
    if(got !== TOKEN) return res.status(403).json({code:403, msg:'forbidden: bad or missing X-NCM-Token'})
  }
  if(!ALLOWED.has(req.path.replace(/^\//,''))){
    return res.status(404).json({code:404, msg:'module not found'})
  }
  next()
})

// 通用转发： /song/url/v1 (POST: id/level/cookie 走 body)
app.all('*', async (req,res)=>{
  const url = req.path.replace(/^\//,'') // e.g. song/url/v1
  // 将查询参数合并
  const query = {...req.query, ...req.body}
  // cookie 特殊处理
  // 注意: express 的 query 解析已完成一次 URL 解码, 这里不能再 decodeURIComponent,
  // 否则 cookie 值中的 %XX 序列会被二次解码破坏(新版 MUSIC_U 较长, 风险更高)
  if(query.cookie) { query.cookie = String(query.cookie) }
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
  const modName = map[url] || url.replace(/\//g,'_')
  const fn = api[modName]
  if(!fn) return res.status(404).json({code:404, msg:'module not found: '+modName})
  try{
    const result = await fn(query)
    // result.cookie 可能为数组
    if(result.cookie) res.set('Set-Cookie', result.cookie)
    res.json(result.body || result)
  }catch(e){
    console.error(e)
    res.status(500).json({code:500, msg: e.message})
  }
})

const PORT = process.env.PORT || 3000
// 仅监听回环地址: 镜像服务按设计接受 cookie 参数, 不应暴露到局域网/公网
app.listen(PORT, '127.0.0.1', ()=> console.log(`NCM API server running http://127.0.0.1:${PORT}${TOKEN ? ' (token auth enabled)' : ' (no token auth)'}`))
