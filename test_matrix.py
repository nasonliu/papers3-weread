#!/usr/bin/env python3
"""PaperS3 微信读书阅读器 · 全流程矩阵测试
覆盖：详情页 → 目录屏(翻页/选章) → 拉正文(计时) → 翻页(计时) → 插图 → 下载整本
用法：python3 test_matrix.py [串口，默认 /dev/cu.usbmodem2101]
"""
import serial, time, sys, re

PORT = sys.argv[1] if len(sys.argv) > 1 else '/dev/cu.usbmodem2101'
BOOKS = [  # (书名关键词, 选章, 类型标签)
    ("霍比特人", 4, "epub/中文/插图"),
    ("小妇人", 8, "epub/英文"),
    ("怪兽迷宫", 3, "epub/中文/图多"),
    ("时间之箭", 5, "epub/中文"),
    ("大唐狄公案", 6, "长目录100+"),
]
DL_BOOKS = ["霍比特人"]  # 下载整本测试（21 章，时长可控）

s = serial.Serial(PORT, 115200, timeout=1)

def drain(dur, keep=None):
    """收集 dur 秒内的串口行，返回全部行；keep 过滤"""
    out, t0 = [], time.time()
    while time.time() - t0 < dur:
        line = s.readline()
        if not line:
            continue
        txt = line.decode('utf-8', 'replace').rstrip()
        if txt and (keep is None or keep(txt)):
            out.append(txt)
    return out

def cmd(c, wait=0.3):
    s.write((c + '\n').encode()); s.flush()
    time.sleep(wait)

def wait_boot():
    """等设备就绪：发 's' 直到看到书架刷新或书架列表（设备可能在下载插图，耐心等）"""
    lines = []
    for attempt in range(15):  # 20s x 15 = 最多 5 分钟
        s.write(b's\n'); s.flush()
        t1 = time.time()
        while time.time() - t1 < 20:
            line = s.readline()
            if not line:
                continue
            txt = line.decode('utf-8', 'replace').rstrip()
            lines.append(txt)
            if '[书架]' in txt or '[刷新]' in txt:
                return lines
        print(f'  就绪等待中（第 {attempt+1} 轮，设备可能在下载插图）...')
    raise RuntimeError('等待书架超时')

def find_book(lines, kw):
    for t in lines:
        m = re.match(r'\s*(\d+)\. .*' + kw, t)
        if m:
            return int(m.group(1))
    return None

def check_health(tag):
    """最近日志里不应出现崩溃/重启"""
    bad = [l for l in drain(0.5) if ('Guru' in l or 'Backtrace' in l or 'rst:' in l.lower())]
    if bad:
        print(f'  ✗ [{tag}] 检测到崩溃/重启: {bad[0][:80]}')
        return False
    return True

print('=== 启动，等设备就绪 ===')
# 反复发 's' 激活，再发 'books' 拿书单；书单为空说明还在启动中，继续等
boot = []
t0 = time.time()
while time.time() - t0 < 300:
    s.write(b's\n'); s.flush()
    drain(3)
    s.write(b'books\n'); s.flush()
    boot = drain(6, lambda t: t.strip().startswith(tuple(f'{i}.' for i in range(1, 200))) and '[' in t)
    if len(boot) >= 5:
        break
    print(f'  等书架加载中（当前 {len(boot)} 本）...')
    time.sleep(3)
if len(boot) < 5:
    raise RuntimeError('书架加载超时')
print(f'书架就绪（{len(boot)} 本）\n')

results = []
for kw, ch, label in BOOKS:
    print(f'--- 《{kw}》 ({label}) ---')
    n = find_book(boot, kw)
    if not n:
        print(f'  跳过：书架里没找到《{kw}》\n'); continue
    r = {'book': kw, 'label': label}

    # 1. 详情页
    t0 = time.time()
    cmd(str(n))
    det = drain(25, lambda t: '[详情]' in t or '错误' in t or '[目录]' in t)
    r['detail_s'] = round(time.time() - t0, 1)
    ok = any('[详情] 《' in l for l in det)
    r['detail'] = 'OK' if ok else 'FAIL:' + (det[-1][:40] if det else '无响应')
    print(f'  详情页: {r["detail"]} ({r["detail_s"]}s)')

    # 2. 目录屏 + 翻页
    cmd('t'); time.sleep(2.5)
    toc1 = drain(3, lambda t: '[刷新]' in t)
    cmd('tp'); time.sleep(2)
    toc2 = drain(2.5, lambda t: '[刷新]' in t)
    cmd('tm'); time.sleep(1.5)
    r['toc'] = 'OK' if toc1 and toc2 else 'FAIL'
    print(f'  目录屏/翻页: {r["toc"]}')

    # 3. 目录选章（等待拉正文，计时）
    t0 = time.time()
    cmd(f'ts {ch}')
    got = {'page': None, 'err': None}
    t1 = time.time()
    while time.time() - t1 < 60:
        line = s.readline()
        if not line:
            continue
        txt = line.decode('utf-8', 'replace').rstrip()
        if '[分页]' in txt:
            got['page'] = txt; break
        if '获取失败' in txt or '[错误]' in txt:
            got['err'] = txt; break
    r['open_s'] = round(time.time() - t0, 1)
    if got['page']:
        r['open'] = f"OK {r['open_s']}s {got['page'][:26]}"
    else:
        r['open'] = 'FAIL ' + str(got['err'])[:50]
    print(f'  选章打开: {r["open"]}')

    # 4. 连翻 5 页（逐页计时）
    times, imgs = [], 0
    for i in range(5):
        t0 = time.time()
        cmd('n')
        turn = drain(8, lambda t: '[翻页计时]' in t or '[翻页]' in t or '[img]' in t)
        el = time.time() - t0
        times.append(round(el, 1))
        imgs += sum(1 for l in turn if '[img] 已缓存' in l)
    r['turns'] = times
    r['imgs_dl'] = imgs
    r['turn_ok'] = 'OK' if all(t < 3.0 for t in times) else 'SLOW'
    print(f'  翻页 5 次: {times} {r["turn_ok"]}（新下载图 {imgs} 张）')

    # 5. 回书架
    cmd('s'); time.sleep(2)
    r['health'] = check_health('整体')
    results.append(r)
    print()

# 6. 下载整本
for kw in DL_BOOKS:
    print(f'--- 下载整本《{kw}》 ---')
    n = find_book(boot, kw)
    if not n:
        continue
    cmd(str(n))
    drain(25, lambda t: '[详情]' in t)
    t0 = time.time()
    cmd('dl')
    done, last = None, time.time()
    while time.time() - last < 240:
        line = s.readline()
        if not line:
            continue
        txt = line.decode('utf-8', 'replace').rstrip()
        if '下载完成' in txt:
            done = txt; break
        if '下载已取消' in txt or '失败' in txt:
            done = '异常: ' + txt; break
    print(f'  结果: {done or "超时"} ({round(time.time()-t0,1)}s)')
    cmd('s'); time.sleep(2)

print('\n=== 汇总 ===')
for r in results:
    print(f"《{r['book']}》 详情{r['detail_s']}s {r['detail']} | 目录 {r['toc']} | 打开 {r['open']} | 翻页 {r['turn_ok']} {r['turns']} | 健康 {'✓' if r['health'] else '✗'}")
s.close()
