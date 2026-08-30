#pragma once
#include <string>
#include <vector>
#include "ncm_client.h"

// 歌曲元数据与封面公共服务 (插件 FileInfoProvider 与本地代理共用):
//  - 元数据缓存: %APPDATA%\AIMP\NcmPlugin\song_meta.json (与 config.json 同目录;
//    同步时写入, 按文件修改时间自动重载)
//  - 封面缓存:   %TEMP%\aimp_ncm\artwork\{tid}.img
//  - 流标签:     生成 ID3v2.3 前缀标签(标题/歌手/专辑/时长/封面), 由本地代理注入到串流响应,
//                使 AIMP 从音频流中解析并显示歌曲信息(网络流不经 FileInfoProvider)
namespace NcmMeta {

// 同步后写入歌单歌曲元数据缓存
void WritePlaylist(long long pid, const std::vector<NcmSong>& songs);
// 按 pid+tid 或仅 tid 查询歌曲信息, 未命中返回 false
bool Lookup(long long pid, long long tid, NcmSong& out);
bool LookupByTid(long long tid, NcmSong& out);
// 写入单曲信息并回写缓存文件
void Upsert(long long pid, const NcmSong& song);
// 取封面字节(磁盘缓存优先, 未命中下载并落盘); 返回是否成功
bool GetCoverBytes(long long tid, const std::wstring& coverUrl, std::string& out);
// 取歌词文本(磁盘缓存优先, 未命中经 NcmClient 拉取并落盘); 返回是否成功
bool GetLyricText(long long tid, std::string& out);
// 构建 ID3v2.3 标签(含 APIC 封面)作为串流前缀; 无可用信息返回空串(不注入)
std::string BuildStreamTag(long long pid, long long tid);

} // namespace NcmMeta
