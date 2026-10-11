/*
 * fzs-atio —— 蜂助手面板专用 AT 会话助手（内置二进制；静态链接、零依赖）
 *
 * 目标平台：MIBOX-668M2 / OpenWrt 22.03.7 / ramips-mt76x8 (mipsel) + EC200T / EC200N
 *
 * ============================================================================
 * 为什么需要它（改这个文件之前，先把下面三条读完）
 * ============================================================================
 * 1) picocom 打开串口时会操作 DTR / RTS 两根 modem 控制线，可能把模组"抖"到
 *    重新枚举；而本设备上网走模组的 RNDIS 网口，模组一重枚举，网络就抖。
 *    → 本程序只 open + tcsetattr，并显式清掉 HUPCL，使 close 时不拉低 DTR，
 *      从根上避免"开一次端口抖一次"。
 *
 * 2) picocom 做不了"发一条、等到 OK 再发下一条"，只能靠固定 sleep 盲等：
 *    一次 6 条查询要 7.5 秒（其中 4 秒纯属白等），丢命令还只能整轮重发。
 *    → 本程序逐条握手（写一条 → 读到终结符再写下一条），毫秒级超时，
 *      天然完整，不需要任何固定等待、也不需要整轮重发。
 *
 * 3) 设备上既没有 stty / timeout / socat，shell 的 `read -t` 与 `sleep`
 *    又只认整数秒（实测 read -t 0.2 报 invalid timeout、sleep 0.25 只睡 0.04s），
 *    纯 shell 根本做不出亚秒级同步。
 *    → 本程序把"设串口 + 逐条握手 + 超时"全部包圆。
 *
 * ============================================================================
 * 用法
 * ============================================================================
 *   fzs-atio [-t 超时ms] [-d 排空ms] <设备>
 *       从 stdin 逐行读命令（空行忽略；以 '#' 开头的行为注释）
 *       例：printf 'AT\nATE0\nAT+CGSN\n' | fzs-atio /dev/ttyUSB5
 *
 *   fzs-atio [-t 超时ms] [-d 排空ms] -m sms <设备> <TPDU字节数> <PDU的hex>
 *       短信发送：AT → ATE0 → AT+CMGF=0 → AT+CMGS=<len>（等 '>'）→ PDU + Ctrl-Z
 *
 * 输出：模块的原始响应（已去掉 '\r'），与原先 picocom + `tr -d '\r'` 逐字一致，
 *       因此上层 fzs-at 的解析代码不需要任何改动。
 *
 * 退出码：0 = 每条命令都拿到了应答（OK 或 ERROR 都算拿到）
 *         1 = 有命令超时 / 短信未获确认
 *         2 = 参数错误
 *         3 = 打不开串口
 *         4 = 串口读错误
 */

/*
 * 用 _GNU_SOURCE：CRTSCTS 是 BSD 扩展，若只声明 _POSIX_C_SOURCE 会进入严格模式，
 * musl 下就看不到它了。_GNU_SOURCE 在 musl / glibc 上都有效。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RBUF_SZ            16384   /* 单条命令的响应缓冲 */
#define DEF_TIMEOUT_MS     2000    /* 普通命令等待终结符的超时 */
#define DEF_DRAIN_MS       200     /* 会话开始前只读排空残留的时长 */
#define SMS_TIMEOUT_FACTOR 4       /* 短信发送/入库比普通命令慢，超时放大 */
#define POLL_SLICE_MS      30      /* 单次 poll 最大等待，便于及时收工 */

/* 回复状态 */
#define R_TERM_OK     1
#define R_TERM_ERR    2
#define R_TERM_PROMPT 3
#define R_TIMEOUT     (-1)
#define R_IOERR       (-2)

static unsigned char rbuf[RBUF_SZ];
static size_t rlen = 0;

/* 批量模式（-b）的"整轮捕获"缓冲：一次会话的全部响应攒在这里，
 * 供 count_answers() 数应答行、并原样写进该模组的输出文件。 */
static char cap_buf[256 * 1024];
static size_t cap_len = 0;
static int cap_on = 0;

/* ---------------------------------------------------------------- 小工具 */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* 把当前缓冲输出到 stdout（去掉 '\r'，与 tr -d '\r' 行为一致），然后清空。
 * 批量模式（-b）下改为攒进内存：既要数"应答行数"判完整性，又要写进各自的输出文件。 */
static void out_flush(void)
{
    size_t i;

    if (cap_on) {
        for (i = 0; i < rlen && cap_len + 1 < sizeof(cap_buf); i++) {
            if (rbuf[i] != '\r') cap_buf[cap_len++] = rbuf[i];
        }
        cap_buf[cap_len] = '\0';
        rlen = 0;
        return;
    }

    for (i = 0; i < rlen; i++) {
        if (rbuf[i] != '\r') putchar(rbuf[i]);
    }
    fflush(stdout);
    rlen = 0;
}

/* ---------------------------------------------------------- 响应终结判定 */

/*
 * 扫描当前缓冲，判断是否已经收到"终结符"。
 * 返回 0 表示还没到终结，见宏定义。
 *
 * 注意：AT+CMGS=<len> 的 '>' 提示符 **不带换行**，所以不能只按行判断，
 *       必须额外检查"最后一个非空白字符是不是 '>'"。
 */
static int scan_term(void)
{
    size_t i = 0;

    if (rlen == 0) return 0;

    /* 1) 提示符：尾部最后一个非空白字符是 '>' */
    {
        size_t k = rlen;
        while (k > 0 && isspace(rbuf[k - 1])) k--;
        if (k > 0 && rbuf[k - 1] == '>') return R_TERM_PROMPT;
    }

    /* 2) 逐行找 OK / ERROR / +CME ERROR / +CMS ERROR */
    while (i < rlen) {
        size_t j = i, ls, le, ll;
        while (j < rlen && rbuf[j] != '\n') j++;
        if (j >= rlen) break;            /* 这一行还没收完整，等下次 */
        ls = i;
        le = j;                          /* 行内容为 [ls, le)，不含 '\n' */
        while (ls < le && isspace(rbuf[ls])) ls++;
        while (le > ls && isspace(rbuf[le - 1])) le--;
        ll = le - ls;
        if (ll == 2 && memcmp(rbuf + ls, "OK", 2) == 0) return R_TERM_OK;
        if (ll == 5 && memcmp(rbuf + ls, "ERROR", 5) == 0) return R_TERM_ERR;
        if (ll >= 10 && memcmp(rbuf + ls, "+CME ERROR", 10) == 0) return R_TERM_ERR;
        if (ll >= 10 && memcmp(rbuf + ls, "+CMS ERROR", 10) == 0) return R_TERM_ERR;
        i = j + 1;
    }
    return 0;
}

/* 收数据直到出现终结符或超时 */
static int recv_until(int fd, int timeout_ms)
{
    struct pollfd p;
    long long t0;

    p.fd = fd;
    p.events = POLLIN;
    t0 = now_ms();

    for (;;) {
        int st = scan_term();
        if (st != 0) return st;

        {
            long long el = now_ms() - t0;
            int slice;
            if (el >= timeout_ms) return R_TIMEOUT;
            slice = (int)(timeout_ms - el);
            if (slice > POLL_SLICE_MS) slice = POLL_SLICE_MS;

            p.revents = 0;
            {
                int r = poll(&p, 1, slice);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    return R_IOERR;
                }
                if (r == 0) continue;                 /* 本轮没数据，继续等 */
                /* ★★ 2026-10-07 晚 代码审查修复：设备消失/线路错误（POLLERR /
                 *   POLLHUP / POLLNVAL）时，poll 会**立即返回**但不带 POLLIN。
                 *   旧代码只判 POLLIN，于是回到循环顶部立刻再 poll → **无 sleep
                 *   的紧密空转**，在单核 580MHz 上会把 CPU 打满直到超时。
                 *   现在：遇到这类事件直接判为 I/O 错误返回。 */
                if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return R_IOERR;
                if (p.revents & POLLIN) {
                    ssize_t n;
                    if (rlen >= RBUF_SZ - 1) return R_IOERR;   /* 缓冲满，异常 */
                    n = read(fd, rbuf + rlen, RBUF_SZ - 1 - rlen);
                    if (n > 0) {
                        rlen += (size_t)n;
                        rbuf[rlen] = '\0';
                    } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                        return R_IOERR;
                    }
                }
            }
        }
    }
}

/* 只读丢弃残留数据（替代原先固定 1 秒的排空）
 *
 * ★★ 2026-10-07 晚 重写（修「长短信收信延迟 100 秒」的真因）：
 *   旧实现是「固定读 ms 毫秒就返回」。问题在于 `AT+CMGL=4` 在模组忙时
 *   （正在收长短信）响应会晚几百毫秒甚至更久，旧实现在 drain 期满后立刻
 *   返回、马上下一条命令 —— **迟到的 PDU/OK 会留在串口缓冲里，被下一条
 *   命令（或整会话重试）读到**，造成「两轮响应叠加」：
 *   实测症状 = 本应 7 条 `+CMGL`/5 个 `OK`，却读到 14 条 / 10 个。
 *   叠加后的分片 ref/seq 错乱 → 长短信永远凑不齐 → 收信延迟上百秒。
 *
 *   新语义 = 「**排到真正静默为止**」：只要还在收到数据就继续等；
 *   连续 `QUIET_MS` 毫秒收不到任何字节才算排空完成。
 *   `ms` 变为**总时长上限**（防止模组持续吐数据时卡住）。
 */
#define DRAIN_QUIET_MS   80    /* 连续静默多久算"排空完成"
                                * ★ 2026-10-07 晚 从 250 降到 80：
                                *   250ms 在"模组持续有零星数据/噪声"时永远不满足，
                                *   导致 drain 每次跑满时长上限 → 单轮拖到十几秒
                                *   （实测收信延迟 20 秒的真因之一）。
                                *   80ms 足以确认"真的没数据了"，且不会拖慢。 */

static int drain(int fd, int ms)
{
    struct pollfd p;
    long long t0 = now_ms();
    long long last_rx = t0;        /* 最后一次收到数据的时刻 */
    char tmp[512];

    p.fd = fd;
    p.events = POLLIN;

    for (;;) {
        long long el = now_ms() - t0;
        long long quiet = now_ms() - last_rx;
        int slice;

        /* 结束条件①：总时长到上限（防止无限等待） */
        if (el >= ms) break;
        /* 结束条件②：已经读到过数据，且连续静默超过阈值 → 认为排空完成 */
        if (quiet >= DRAIN_QUIET_MS) break;

        slice = (int)(ms - el);
        if (slice > POLL_SLICE_MS) slice = POLL_SLICE_MS;
        /* 每次最多等到「距静默阈值还差多久」或本次 slice */
        if (quiet < DRAIN_QUIET_MS) {
            long long need = DRAIN_QUIET_MS - quiet;
            if (need < slice) slice = (int)need;
        }
        if (slice < 1) slice = 1;

        p.revents = 0;
        if (poll(&p, 1, slice) < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        /* 设备消失/线路错误 → 直接结束，避免紧密空转（见 recv_until 同处注释） */
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (p.revents & POLLIN) {
            if (read(fd, tmp, sizeof(tmp)) > 0) {
                last_rx = now_ms();      /* 又收到数据 → 静默计时重置 */
                continue;
            }
            break;                       /* read<=0：异常，退出 */
        }
    }
    rlen = 0;
    return 0;
}

/* -------------------------------------------------------------- 串口打开 */

/*
 * ★★ 铁律：绝不碰 DTR/RTS。
 *   - 用 O_NOCTTY | O_NONBLOCK 打开，不等载波、不抢控制终端；
 *   - 清掉 HUPCL —— 这是关键：HUPCL 置位时 close() 会拉低 DTR，
 *     下次 open 再拉高，就是一次"抖动"；清掉它，端口保持 DTR 高电平，不再抖；
 *   - 不设 CRTSCTS，避免程序去动 RTS；
 *   - 自己实现 cfmakeraw 等价设置（不依赖 BSD 扩展），关掉 ECHO/ICANON/ISIG
 *     以及所有输入输出的字符转换 —— 回显污染就是从这里根治的。
 */
static int serial_open(const char *dev)
{
    int fd;
    struct termios tio;

    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    if (tcgetattr(fd, &tio) != 0) {
        close(fd);
        return -1;
    }

    /* cfmakeraw 等价（不依赖 cfmakeraw 这个 BSD 扩展） */
    tio.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                               INLCR | IGNCR | ICRNL | IXON);
    tio.c_oflag &= (tcflag_t)~OPOST;
    tio.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    /* 8N1 + 局部模式（忽略 modem 控制线状态） */
    tio.c_cflag &= (tcflag_t)~(CSIZE | PARENB | CSTOPB);
    tio.c_cflag |= CS8;
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= (tcflag_t)~CRTSCTS;   /* 不动 RTS */
    tio.c_cflag &= (tcflag_t)~HUPCL;     /* ★ close 时不拉低 DTR —— 防模组被抖到重枚举 */

    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);   /* 丢掉内核缓冲里的陈货 */
    return fd;
}

/* ---------------------------------------------------------- 会话（普通） */

static int do_cmd(int fd, const char *cmd, int timeout_ms)
{
    rlen = 0;
    if (write_all(fd, cmd, strlen(cmd)) < 0) return R_IOERR;
    if (write_all(fd, "\r", 1) < 0) return R_IOERR;
    return recv_until(fd, timeout_ms);
}

static int run_session(int fd, int timeout_ms)
{
    char line[1024];
    int bad = 0, ncmd = 0;

    while (fgets(line, sizeof(line), stdin)) {
        size_t l, s = 0;
        int st;

        l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        while (line[s] == ' ' || line[s] == '\t') s++;
        if (line[s] == '\0' || line[s] == '#') continue;   /* 空行 / 注释 */

        ncmd++;
        st = do_cmd(fd, line + s, timeout_ms);
        out_flush();

        if (st == R_TIMEOUT) {
            fprintf(stderr, "fzs-atio: [%s] 超时（%dms 内无终结符）\n", line + s, timeout_ms);
            bad = 1;
            /* ★★★ 2026-10-07 晚 22:3x 关键优化（实测证据驱动）：
             *   给守护的日志显示：模组**接收长短信重组期间，AT 口整段无响应** ——
             *   连最简单的 `AT` 都超时 2000ms，5 条命令全部超时 = 10 秒，
             *   再加排空与重试 → 单轮 17.68 秒（用户感知"收信延迟 20 秒"）。
             *   既然第一条就超时说明"模组正忙"，**继续把余下命令逐条超时是纯浪费**
             *   （它们必然也超时）。故：**首条命令超时即整体放弃本轮**，
             *   立刻 return，把时间让给下一次重试（模组忙完就能立刻读走）。
             *   收益：忙时单轮 10s → 2s（只剩第一条的超时）。 */
            if (ncmd == 1) {
                fprintf(stderr, "fzs-atio: 热身命令无响应，模组可能正忙 —— 放弃本轮（避免逐条空等）\n");
                drain(fd, 200);
                return 1;
            }
            drain(fd, 200);
        } else if (st == R_IOERR) {
            fprintf(stderr, "fzs-atio: 串口读错误：%s\n", strerror(errno));
            return 4;
        }
        /* R_TERM_ERR（模块回 ERROR / +CME / +CMS）不算我们的失败：
           ERROR 本身也是有效应答，交给上层按内容解析。 */
    }

    if (ncmd == 0) {
        fprintf(stderr, "fzs-atio: stdin 里没有任何命令\n");
        return 2;
    }
    return bad ? 1 : 0;
}

/* ---------------------------------------------------------- 短信发送 */

static int run_sms(int fd, const char *len, const char *pdu, int timeout_ms)
{
    int st;

    if (len == NULL || pdu == NULL) {
        fprintf(stderr, "fzs-atio: -m sms 需要 <TPDU字节数> <PDU的hex>\n");
        return 2;
    }

    /* 1) 热身 + 关回显 */
    st = do_cmd(fd, "AT", timeout_ms);
    out_flush();
    if (st == R_TIMEOUT) {
        fprintf(stderr, "fzs-atio: AT 无响应，模组可能不在线\n");
        return 1;
    }
    do_cmd(fd, "ATE0", timeout_ms);
    out_flush();

    /* 2) 进入 PDU 模式 */
    st = do_cmd(fd, "AT+CMGF=0", timeout_ms);
    out_flush();
    if (st == R_TIMEOUT || st == R_TERM_ERR) {
        fprintf(stderr, "fzs-atio: 切换到 PDU 模式失败\n");
        return 1;
    }

    /* 3) CMGS：发长度，等 '>' 提示符 */
    rlen = 0;
    if (write_all(fd, "AT+CMGS=", 9) < 0) return 4;
    if (write_all(fd, len, strlen(len)) < 0) return 4;
    if (write_all(fd, "\r", 1) < 0) return 4;
    st = recv_until(fd, timeout_ms);
    out_flush();
    if (st == R_TERM_ERR) {
        fprintf(stderr, "fzs-atio: CMGS 被模块拒绝\n");
        return 1;
    }
    /* 个别固件不回显 '>'：超时也照发，最终以 +CMGS: / ERROR 为准 */

    /* 4) 发 PDU 正文 + Ctrl-Z */
    rlen = 0;
    if (write_all(fd, pdu, strlen(pdu)) < 0) return 4;
    {
        unsigned char ctrlz = 0x1A;
        if (write_all(fd, &ctrlz, 1) < 0) return 4;
    }
    st = recv_until(fd, timeout_ms * SMS_TIMEOUT_FACTOR);
    out_flush();

    if (st == R_TIMEOUT) {
        fprintf(stderr, "fzs-atio: 发送未获模块确认（超时）\n");
        return 1;
    }
    if (st == R_TERM_ERR) {
        fprintf(stderr, "fzs-atio: 模块返回错误\n");
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- 入口 */

/* ====================================================== 批量轮询（-b） ====
 *
 * 为什么要有它：短信守护每 2 秒要读一次两个模组。原先的做法是"每模组起一层
 * shell + 若干小命令（readlink/cat/grep/flock…）"，实测**一轮 40 个进程**、
 * fork 18 次/秒 —— 在单核 580MHz 上是实打实的负担（实测整机 CPU 14~16%）。
 * 而真正必要的只有一件事：开串口、发命令、收响应。
 *   ⇒ 把这件事收进**一个进程**：一次调用读完全部模组。
 *
 * 与 shell 版 fzs-at 的判据**逐条对齐**（否则会出现"读到一半当成功"的脏数据）：
 *   · 完整性：**应答行数恰好 = 2 + 命令数**（2 = 会话开头的 AT + ATE0）；
 *     少于期望（丢应答，模组忙）要重试；**多于期望**（实测模组忙时整份响应重复，
 *     会导致同一条短信重复入库）也必须重试；
 *   · 最多 3 次尝试，每次之间等 1 秒；
 *   · 首条（AT 热身）超时 = 模组正忙 → 放弃本次尝试（逐条空等没有意义）；
 *   · 3 次都不合格 → **写空文件**（宁可当"本轮没读到"，绝不交出可能翻倍的脏数据）。
 * 另外：串口锁与 shell 版共用同一个文件（/tmp/fzs-at-<端口>.lock），
 *       但用的是 flock(2) 系统调用 —— 内核持锁，**不额外起进程**。
 */
#define BATCH_MAX_ARGS 10
#define BATCH_MAX_CMDS 32
#define BATCH_TRIES    3

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* 数"应答行"：行首为 OK / ERROR / +CME ERROR / +CMS ERROR（与 shell 的 grep -cE 一致） */
static int count_answers(const char *s)
{
    int n = 0;
    const char *p = s;

    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);

        if ((l >= 2 && memcmp(p, "OK", 2) == 0) ||
            (l >= 5 && memcmp(p, "ERROR", 5) == 0) ||
            (l >= 10 && memcmp(p, "+CME ERROR", 10) == 0) ||
            (l >= 10 && memcmp(p, "+CMS ERROR", 10) == 0))
            n++;
        if (!e) break;
        p = e + 1;
    }
    return n;
}

/* 读命令文件（空行与 # 开头的行忽略） */
static int read_cmds(const char *path, char cmds[][128], int maxn)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int n = 0;

    if (!f) return -1;
    while (fgets(line, sizeof(line), f) && n < maxn) {
        size_t l = strlen(line), s = 0;

        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        while (line[s] == ' ' || line[s] == '\t') s++;
        if (line[s] == '\0' || line[s] == '#') continue;
        snprintf(cmds[n], 128, "%s", line + s);
        n++;
    }
    fclose(f);
    return n;
}

/* 目标 → 设备节点。规则与 shell 版 fzs-at 完全一致：
 *   /dev/ttyUSBx 或 ttyUSBx 直接使用；modem1/ec200t/1/t → idProduct 6026（EC200T）；
 *   modem2/ec200n/2/n → 6002（EC200N）；在 /sys/class/tty 里找"接口路径含 :1.4"的那个口。
 *   ★ 全程用 libc（realpath/open+read），不起任何子进程 —— 这正是替代 readlink+cat 的关键。 */
static int resolve_port(const char *spec, char *out, size_t outsz)
{
    const char *want = NULL;
    DIR *d;
    struct dirent *de;

    if (strncmp(spec, "/dev/", 5) == 0 || strncmp(spec, "ttyUSB", 6) == 0) {
        snprintf(out, outsz, spec[0] == '/' ? "%s" : "/dev/%s", spec);
        return 0;
    }
    if (!strcmp(spec, "modem1") || !strcmp(spec, "ec200t") || !strcmp(spec, "EC200T") ||
        !strcmp(spec, "1") || !strcmp(spec, "t") || !strcmp(spec, "T"))
        want = "6026";
    else if (!strcmp(spec, "modem2") || !strcmp(spec, "ec200n") || !strcmp(spec, "EC200N") ||
             !strcmp(spec, "2") || !strcmp(spec, "n") || !strcmp(spec, "N"))
        want = "6002";
    else
        return -1;

    d = opendir("/sys/class/tty");
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char path[PATH_MAX], real[PATH_MAX], up[PATH_MAX];
        int i;

        if (strncmp(de->d_name, "ttyUSB", 6) != 0) continue;
        snprintf(path, sizeof(path), "/sys/class/tty/%s/device", de->d_name);
        if (realpath(path, real) == NULL) continue;
        if (strstr(real, ":1.4") == NULL) continue;      /* 只认 AT 口那个接口 */

        snprintf(up, sizeof(up), "%s", real);
        for (i = 0; i < 6; i++) {                        /* 向上找 idProduct（同 shell 的 find_up） */
            char f[PATH_MAX], buf[16];
            FILE *fp;
            char *slash;

            snprintf(f, sizeof(f), "%s/idProduct", up);
            fp = fopen(f, "r");
            if (fp) {
                if (fgets(buf, sizeof(buf), fp)) {
                    size_t l = strlen(buf);
                    while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = '\0';
                    if (strcmp(buf, want) == 0) {
                        fclose(fp);
                        closedir(d);
                        snprintf(out, outsz, "/dev/%s", de->d_name);
                        return 0;
                    }
                }
                fclose(fp);
            }
            slash = strrchr(up, '/');
            if (slash == NULL || slash == up) break;
            *slash = '\0';
        }
    }
    closedir(d);
    return -1;
}

static void write_file(const char *path, const char *data, size_t n)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    if (n) fwrite(data, 1, n, f);
    fclose(f);
}

static int run_batch(const char *cmdfile, const char *const *pairs, int npairs,
                     int timeout_ms, int drain_ms)
{
    char cmds[BATCH_MAX_CMDS][128];
    int ncmd = read_cmds(cmdfile, cmds, BATCH_MAX_CMDS);
    int bad = 0, i;

    if (ncmd <= 0) {
        fprintf(stderr, "fzs-atio: 命令文件读不到或没有有效命令：%s\n", cmdfile);
        return 2;
    }

    for (i = 0; i < npairs; i++) {
        const char *spec = pairs[i * 2], *outf = pairs[i * 2 + 1];
        char dev[PATH_MAX], lockpath[PATH_MAX];
        int fd, lf, attempt, ok = 0;

        if (resolve_port(spec, dev, sizeof(dev)) != 0) {
            fprintf(stderr, "fzs-atio: [%s] 找不到 AT 口\n", spec);
            write_file(outf, "", 0);
            bad = 1;
            continue;
        }

        snprintf(lockpath, sizeof(lockpath), "/tmp/fzs-at-%s.lock", strrchr(dev, '/') + 1);
        lf = open(lockpath, O_CREAT | O_RDWR, 0644);
        if (lf >= 0) flock(lf, LOCK_EX);                 /* 与 shell 版共用同一把锁 */

        fd = serial_open(dev);
        if (fd < 0) {
            fprintf(stderr, "fzs-atio: [%s] 打不开 %s: %s\n", spec, dev, strerror(errno));
            write_file(outf, "", 0);
            if (lf >= 0) close(lf);
            bad = 1;
            continue;
        }

        for (attempt = 1; attempt <= BATCH_TRIES && !ok; attempt++) {
            int k, st, nans;

            cap_len = 0;
            cap_buf[0] = '\0';
            cap_on = 1;
            rlen = 0;
            if (drain_ms > 0) drain(fd, drain_ms);

            st = do_cmd(fd, "AT", timeout_ms);
            out_flush();
            if (st == R_TIMEOUT) {                       /* 热身就超时 = 模组忙，别逐条空等 */
                cap_on = 0;
                if (attempt < BATCH_TRIES) sleep_ms(1000);
                continue;
            }
            if (st == R_IOERR) break;

            st = do_cmd(fd, "ATE0", timeout_ms);
            out_flush();
            if (st == R_IOERR) break;

            for (k = 0; k < ncmd; k++) {
                st = do_cmd(fd, cmds[k], timeout_ms);
                out_flush();
                if (st == R_IOERR) break;
                if (st == R_TIMEOUT) { drain(fd, 200); break; }
            }

            nans = count_answers(cap_buf);
            if (nans == 2 + ncmd) {
                ok = 1;
            } else {
                fprintf(stderr, "fzs-atio: [%s] 第 %d 次应答数 %d ≠ 期望 %d，重试\n",
                        spec, attempt, nans, 2 + ncmd);
                if (attempt < BATCH_TRIES) sleep_ms(1000);
            }
            cap_on = 0;
        }

        if (ok) write_file(outf, cap_buf, cap_len);
        else    write_file(outf, "", 0);                 /* 不合格 → 空（下轮再读，不丢短信） */
        if (!ok) bad = 1;
        close(fd);
        if (lf >= 0) close(lf);
    }
    return bad ? 1 : 0;
}

static void usage(void)
{
    fprintf(stderr,
        "fzs-atio —— 蜂助手面板 AT 会话助手\n"
        "\n"
        "用法:\n"
        "  fzs-atio [-t 超时ms] [-d 排空ms] <设备>\n"
        "      从 stdin 逐行读 AT 命令（空行/以 # 开头的行忽略）\n"
        "  fzs-atio [-t 超时ms] [-d 排空ms] -m sms <设备> <TPDU字节数> <PDU的hex>\n"
        "      发送一条短信（纯 PDU）\n"
        "  fzs-atio [-t 超时ms] [-d 排空ms] -b <命令文件> <目标> <输出文件> [<目标> <输出文件> ...]\n"
        "      批量轮询：一个进程读完全部模组（命令从文件读；含完整性判据与 3 次重试；\n"
        "      不合格则写空文件）。目标可写 modem1/modem2/ec200t/ec200n 或 /dev/ttyUSBx\n"
        "\n"
        "默认: -t %d  -d %d\n"
        "退出码: 0 全部有应答 / 1 超时 / 2 参数错 / 3 打不开串口 / 4 读错误\n",
        DEF_TIMEOUT_MS, DEF_DRAIN_MS);
}

int main(int argc, char **argv)
{
    const char *dev = NULL;
    const char *cmdfile = NULL;
    const char *pos[BATCH_MAX_ARGS];
    int npos = 0;
    int timeout_ms = DEF_TIMEOUT_MS;
    int drain_ms = DEF_DRAIN_MS;
    int sms_mode = 0;
    int fd, rc, i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] != '\0') {
            if (strcmp(a, "-t") == 0 && i + 1 < argc) {
                timeout_ms = atoi(argv[++i]);
                if (timeout_ms <= 0) { fprintf(stderr, "fzs-atio: 超时必须为正\n"); return 2; }
            } else if (strcmp(a, "-d") == 0 && i + 1 < argc) {
                drain_ms = atoi(argv[++i]);
                if (drain_ms < 0) { fprintf(stderr, "fzs-atio: 排空时长不能为负\n"); return 2; }
            } else if (strcmp(a, "-b") == 0 && i + 1 < argc) {
                cmdfile = argv[++i];
            } else if (strcmp(a, "-m") == 0 && i + 1 < argc) {
                const char *m = argv[++i];
                if (strcmp(m, "sms") == 0) sms_mode = 1;
                else { fprintf(stderr, "fzs-atio: 未知模式 '%s'\n", m); return 2; }
            } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
                usage();
                return 0;
            } else {
                fprintf(stderr, "fzs-atio: 未知选项 '%s'\n", a);
                usage();
                return 2;
            }
        } else if (npos < BATCH_MAX_ARGS) {
            pos[npos++] = a;
        } else {
            fprintf(stderr, "fzs-atio: 参数过多\n");
            return 2;
        }
    }

    /* 批量轮询：走"一次读完全部模组"的路径，不碰单设备逻辑 */
    if (cmdfile != NULL) {
        if (npos < 2 || npos % 2 != 0) {
            fprintf(stderr, "fzs-atio: -b 需要 <目标> <输出文件> 成对出现\n");
            return 2;
        }
        return run_batch(cmdfile, pos, npos / 2, timeout_ms, drain_ms);
    }

    if (npos < 1) { usage(); return 2; }
    dev = pos[0];

    if (sms_mode && npos < 3) {
        fprintf(stderr, "fzs-atio: -m sms 需要 <设备> <TPDU字节数> <PDU的hex>\n");
        return 2;
    }

    fd = serial_open(dev);
    if (fd < 0) {
        fprintf(stderr, "fzs-atio: 打不开 %s: %s\n", dev, strerror(errno));
        return 3;
    }

    if (drain_ms > 0) drain(fd, drain_ms);

    if (sms_mode) rc = run_sms(fd, pos[1], pos[2], timeout_ms);
    else          rc = run_session(fd, timeout_ms);

    close(fd);
    return rc;
}
