# 第三方组件与依赖声明

本文件列出 AIMP NCM（`cirtuslab/AIMP-NCM`）收录或使用的第三方组件及其许可条款。

本项目**自身代码**以 MIT 许可发布，见 [LICENSE](LICENSE)。本仓库不包含 AIMP 本体，
AIMP 为 Artem Izmaylov（<https://www.aimp.ru>）的专有软件，使用本插件需自行安装 AIMP。

---

## 1. nlohmann/json 3.12.0

| 项 | 内容 |
|---|---|
| 位置 | `plugin/third_party/nlohmann/json.hpp`（单头文件，vendored） |
| 分发形式 | 编译进 `aimp_ncm.dll`，随 `aimp_ncm.aimppack` 与 Release 压缩包分发 |
| 许可 | MIT |
| 上游 | <https://github.com/nlohmann/json> |

```
MIT License

Copyright (c) 2013-2026 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### 1.1 内嵌的 Grisu2 浮点打印实现

`json.hpp` 中内嵌了 Florian Loitsch 的 Grisu2 二进制浮点转十进制参考实现（经修改），
同样以 MIT 许可分发（该组件在 vendored 文件中保留了原始声明）：

```
MIT License

Copyright (c) 2009 Florian Loitsch

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 2. AIMP SDK（apiCore.h 等 22 个头文件）

| 项 | 内容 |
|---|---|
| 位置 | `plugin/third_party/aimp_sdk/`（vendored 头文件，目标版本 v5.40 build 2650） |
| 版权 | © 2006-2025 Artem Izmaylov，<https://www.aimp.ru> |
| 许可 | 随 SDK 分发，头文件本身未附带许可条款文本 |

这些头文件仅声明 AIMP 的插件编程接口，用于编译本插件；插件在运行时通过 AIMP 宿主
加载并调用该接口。**所有权利归原作者所有**，此处收录仅用于构建本插件；
再分发或用于其他用途请以 AIMP SDK 的官方条款为准。

---

## 3. 可选镜像服务的运行时依赖（不随本仓库/发行包分发）

`ncm_service/` 下的 Node/Python 镜像服务在运行时依赖下列 npm 包，均由使用者自行
`npm install` 安装，**本仓库不收录其源码，发行包中也不含 `node_modules`**：

| 包 | 许可 | 用途 |
|---|---|---|
| `NeteaseCloudMusicApi`（Binaryify 等） | MIT（npm 包元数据声明） | 网易云接口实现，被 `server.js` 以 `require` 方式调用 |
| `express` | MIT | HTTP 服务框架 |
| `cookie-parser` | MIT | Cookie 解析 |

Python 版 `app.py` 为纯标准库 + `requests`（Apache-2.0）实现，同样不 vendor 依赖源码。

---

## 4. 参考实现（未收录代码，仅作协议/集成参考）

以下项目为协议行为或 AIMP 集成方式的参考来源，本仓库未复制其代码，特此致谢：

- [Binaryify/NeteaseCloudMusicApi](https://github.com/Binaryify/NeteaseCloudMusicApi) — weapi/eapi 接口行为基准
- [NeteaseCloudMusicApiEnhanced/api-enhanced](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced) — 设备指纹 / Cookie 处理（MIT）
- [go-musicfox/go-musicfox](https://github.com/go-musicfox/go-musicfox) — 设备指纹 Cookie 与 eapi 链接参数构造（MIT）
- [chaunsin/netease-cloud-music](https://github.com/chaunsin/netease-cloud-music) — eapi 加密细节
- [AdrianEddy/AIMPYouTube](https://github.com/AdrianEddy/AIMPYouTube) — AIMP 文件系统扩展注册与虚拟流播放
- [cirtuslab/aimp_desktop_lyrics](https://github.com/cirtuslab/aimp_desktop_lyrics)、[cirtuslab/AIMPLyricsSaver](https://github.com/cirtuslab/AIMPLyricsSaver)、[cirtuslab/SongMetaFixer](https://github.com/cirtuslab/SongMetaFixer)
- [imsyy/SPlayer](https://github.com/imsyy/SPlayer) — 网页登录读取 Cookie 流程
- [martin211/aimp_dotnet](https://github.com/martin211/aimp_dotnet) — AIMP 扩展类别 IID 映射佐证
