// NCM API 镜像服务 - 基于 nooblong/NeteaseCloudMusicApiBackup
// 用法: npm install && node server.js
// 默认端口 3000，插件 apiUrl 填 http://localhost:3000
const express = require('express')
const cookieParser = require('cookie-parser')

let api
try { api = require('NeteaseCloudMusicApi') } catch(e){ console.error('请先 npm install NeteaseCloudMusicApi'); process.exit(1) }

const app = express()
app.use(express.json())
app.use(express.urlencoded({extended:true}))
app.use(cookieParser())

// 允许跨域
app.use((req,res,next)=>{ res.header('Access-Control-Allow-Origin','*'); res.header('Access-Control-Allow-Headers','*'); next(); })

// 通用转发： /song/url/v1?level=exhigh&id=123&cookie=xxx
app.all('*', async (req,res)=>{
  const url = req.path.replace(/^\//,'') // e.g. song/url/v1
  // 将查询参数合并
  const query = {...req.query, ...req.body}
  // cookie 特殊处理
  // 注意: express 的 query 解析已完成一次 URL 解码, 这里不能再 decodeURIComponent,
  // 否则 cookie 值中的 %XX 序列会被二次解码破坏(新版 MUSIC_U 较长, 风险更高)
  if(query.cookie) { query.cookie = String(query.cookie) }
  // 若无指定模块，尝试直接调用
  const mod = url.replace(/\//g,'_').replace(/-/g,'_')
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
app.listen(PORT, '127.0.0.1', ()=> console.log(`NCM API server running http://127.0.0.1:${PORT}`))
