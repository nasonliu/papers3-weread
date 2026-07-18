// img_render.h — 图片渲染模块：HTTPS 下载 / SD 缓存 / JPEG·PNG 解码 / 灰度缩放绘制
// 仅支持 JPEG（基线）与 PNG（非隔行）；magic bytes 判定格式，不信任 URL 后缀
#pragma once
#include <Arduino.h>
#include <M5GFX.h>
namespace img_render {
// setup 调一次（建缓存目录）
void begin();
// 会话闲置关闭（主循环周期调用）：释放 keep-alive TLS 占用的 DRAM
void housekeeping();
// 按 URL 取图：先查 SD 缓存，未命中 HTTPS 下载（magic bytes 判 jpeg/png 定扩展名），返回缓存文件路径；失败返回空串
String fetch(const String& url);
// 只查缓存不下载：命中返回路径，未命中/会话负缓存返回空串（分页估算用，避免触发下载）
String cached(const String& url);
// 下载任意内容到指定 SD 路径（tar 包等非图片资源用；不带 magic 校验，带 cookie 重试，上限 4MB）
// 已存在非空文件则直接返回路径；失败返回空串
String fetch_raw(const String& url, const String& out_path);
// 读图片宽高（只解析文件头，不全解码）；失败返回 false
bool size(const String& path, int& w, int& h);
// 把图片等比缩放画到 canvas 的 (x, y, max_w, max_h) 区域内（居中、保宽高比、转灰度 color565(v,v,v)），返回实际绘制高度，失败 0
int draw(M5Canvas* cv, const String& path, int x, int y, int max_w, int max_h);
}
