// weread_api.h — 微信读书接口封装
#pragma once
#include <Arduino.h>
#include <vector>

struct BookEntry {
    String bookId;
    String title;
    String author;
    String cover;
    String format;      // "epub" 或 "txt"（可能为空）
    int progress = 0;   // 阅读进度百分比（来自 shelf/sync 的 bookProgress[]）
    String progressChapterUid;   // 进度所在章节 chapterUid（bookProgress[]，可能为空）
    uint32_t readUpdateTime = 0; // 最近阅读时间 unix 秒（bookProgress[].updateTime）
};

struct ChapterEntry {
    String chapterUid;
    String title;
    int chapterIdx = 0;
    int wordCount = 0;
    bool paid = false;
    String tar;         // 本章资源 tar 包 URL（chapterInfos 的 tar 字段，插图等资源；可能为空）
};

// 书籍详情（weread.qq.com/web/book/info 实测：i.weread/book/info 需签名返回 401）
// 字段来源均为 web/book/info 同名键；该接口无作者简介字段，authorIntro 恒为空（保留占位）
struct BookDetail {
    String title;
    String author;
    String intro;        // 简介
    String authorIntro;  // 作者简介（web/book/info 不返回，留空）
    String cover;
    String publisher;    // 出版社
};

// 热门评论（weread.qq.com/web/review/list?listType=3）
// web 接口不返回点赞数，likes 恒为 0
struct Review {
    String nick;     // 评论者昵称 = reviews[].review.author.name
    String content;  // 评论纯文本 = reviews[].review.content（截取前 ~200 字）
    int likes = 0;   // 点赞数（web 接口无此字段，恒 0）
};

// 章节内容块：图文分块阅读用
// is_image=false 时 text 为纯文本段；is_image=true 时 text 为图片地址：
//   - 网络图片：https 绝对 URL（分片章节插图多为 res.weread.qq.com CDN 图）
//   - EPUB 包内图片："epubimg:" 前缀 + 包内绝对路径（如 "epubimg:OEBPS/Images/x.jpg"），
//     仅当正文接口直接下发整本 EPUB(zip) 时出现；包内图片的提取渲染由上层另行处理
struct ContentBlock {
    bool is_image = false;
    String text;
    uint8_t style = 0;  // 0=正文，1..4 = <h1>..<h4> 标题（渲染用放大/仿粗体贴近原书样式）
};

namespace weread_api {

// 书架（gateway /shelf/sync，需 api_key；失败时回退到笔记书单 user/notebooks）
bool getBookshelf(std::vector<BookEntry>& out, String& err);

// 书籍信息（i.weread.qq.com/book/info，cookie）
bool getBookInfo(const String& bookId, BookEntry& out, String& err);

// 目录章节列表（weread.qq.com/web/book/chapterInfos，cookie + 自动格式探测）
bool getChapterInfos(const String& bookId, std::vector<ChapterEntry>& out, String& format_out, String& err);

// 从 reader HTML 中提取 psvts（正文请求必需）；char* 版直接在缓冲上扫描
String extractPsvts(const char* html, size_t len);
String extractPsvts(const String& readerHtml);

// 拉取一章纯文本正文（自动探测 epub/txt；txt 直出，epub 去标签）
// 返回 true 表示成功，text_out 为 UTF-8 纯文本
// 注意：text_out 是 Arduino String，正文超 ~64KB 的大章会触发 ESP32 String 损坏 bug
// ——大章请改用 getChapterBlocks（文本块按 ≤48KB 切分，不受影响）
bool getChapterText(const String& bookId, const ChapterEntry& ch, String& text_out, String& err);

// 上传阅读进度（POST weread.qq.com/web/book/read，web 签名 payload，实测 succ:1）
// chapterUid 进度所在章节；chapterIdx 章节序号；progressPercent 0-100
bool uploadProgress(const String& bookId, const String& chapterUid, int chapterIdx, int progressPercent, String& err);

// 章节拉取进度回调（main.cpp 注入显示进度）：stage 如 "reader"/"e_0"/"e_1"/"e_3"/"t_0"/"t_1"
extern void (*chapter_stage_cb)(const char* stage, int cur, int total);

// 会话闲置关闭（主循环周期调用）：释放 keep-alive TLS 占用的 DRAM，防后续连接内存不足
void housekeeping();

// 书籍详情（weread.qq.com/web/book/info，cookie）
bool getBookDetail(const String& bookId, BookDetail& out, String& err);

// 热门评论（weread.qq.com/web/review/list?listType=3，cookie）
// count 为拉取条数；content 截前 ~200 字
bool getHotReviews(const String& bookId, std::vector<Review>& out, int count, String& err);

// 拉取一章并按文档顺序切图文块。内容形态内部透明选择：
//   - TXT 书：整章纯文本切块
//   - 常规 EPUB 书：分片解码出单章 XHTML 后切块
//   - 少数 EPUB 书（正文接口直接下发整本 EPUB zip，如公版/原版书）：
//     本地解包 zip → container.xml → content.opf → spine 定位本章 XHTML 后切块，
//     包内相对路径图片产出 "epubimg:" 标记块（见 ContentBlock 注释）
// 下载解码全程 PSRAM（不受 String 64KB 限制）；单个文本块按 ≤48KB 切分
// （Arduino String 超 ~64KB 会损坏，48KB 留余量）。空文本块丢弃。
bool getChapterBlocks(const String& bookId, const ChapterEntry& ch, std::vector<ContentBlock>& out, String& err);

} // namespace weread_api
