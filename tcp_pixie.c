#include <linux/module.h>
#include <net/tcp.h>
#include <linux/inet.h>
#include <linux/slab.h> // 必须包含，用于 kmalloc

static int rate = 900000000;
module_param(rate, int, 0644);
static int feedback = 2;
module_param(feedback, int, 0644);

struct sample {
    u32 _acked;
    u32 _losses;
    u32 _tstamp_us;
};

struct pixie {
    u64 rate;
    u16 start;
    u16 end;
    u32 curr_acked;
    u32 curr_losses;
    struct sample *samples;
};

static void pixie_main(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct pixie *pixie = inet_csk_ca(sk);
    u32 now = tp->tcp_mstamp;
    u32 cwnd_val; // 改名避免与变量冲突
    u16 start, end;
    u64 prate;

    // 基础安全检查
    if (!pixie || !pixie->samples || rs->delivered < 0 || rs->interval_us <= 0)
        return;

    cwnd_val = (u32)pixie->rate; 
    
    // 丢包统计与滑动窗口更新
    pixie->curr_acked += rs->acked_sacked;
    pixie->curr_losses += (u32)rs->losses;
    
    end = pixie->end;
    pixie->samples[end]._acked = (u32)rs->acked_sacked;
    pixie->samples[end]._losses = (u32)rs->losses;
    pixie->samples[end]._tstamp_us = now;
    pixie->end++; // 后加，防止索引越界

    start = pixie->start;
    // 修正：使用循环处理过期样本，避免除零
    while (start != pixie->end) {
        if (2 * (now - pixie->samples[start]._tstamp_us) > (u32)feedback * tp->srtt_us) {
            pixie->curr_acked -= pixie->samples[start]._acked;
            pixie->curr_losses -= pixie->samples[start]._losses;
            pixie->start++;
            start = pixie->start;
        } else {
            break; 
        }
    }

    // 防止除零保护
    u32 safe_acked = (pixie->curr_acked > 0) ? pixie->curr_acked : 1;

    // 计算 CWND
    u64 temp_cwnd = (u64)cwnd_val;
    temp_cwnd *= (pixie->curr_acked + pixie->curr_losses);
    do_div(temp_cwnd, tp->mss_cache > 0 ? tp->mss_cache : 1460);
    do_div(temp_cwnd, safe_acked);
    temp_cwnd *= (tp->srtt_us >> 3);
    do_div(temp_cwnd, USEC_PER_SEC);
    cwnd_val = (u32)temp_cwnd;

    // 计算 Pacing Rate
    prate = ((u64)(pixie->curr_acked + pixie->curr_losses)) << 10;
    do_div(prate, safe_acked);
    prate *= pixie->rate;
    prate >>= 10;

    // 修复 printk 格式错误：强制转换为 (unsigned long long)
    printk(KERN_INFO "##### curr_ack:%llu curr_loss:%llu rsloss:%llu start:%llu end:%llu cwnd:%llu rate:%llu prate:%llu\n",
            (unsigned long long)pixie->curr_acked,
            (unsigned long long)pixie->curr_losses,
            (unsigned long long)rs->losses,
            (unsigned long long)pixie->start,
            (unsigned long long)pixie->end,
            (unsigned long long)cwnd_val,
            (unsigned long long)rate,
            (unsigned long long)prate);

    tp->snd_cwnd = min(cwnd_val, tp->snd_cwnd_clamp);
    sk->sk_pacing_rate = min_t(u64, prate, READ_ONCE(sk->sk_max_pacing_rate));
}

static void pixie_init(struct sock *sk)
{
    struct pixie *pixie = inet_csk_ca(sk);
    pixie->rate = (u64)rate;
    pixie->start = 0;
    pixie->end = 0;
    pixie->curr_acked = 0;
    pixie->curr_losses = 0;
    
    // 注意：U16_MAX * sizeof(struct sample) 约为 780KB，GFP_ATOMIC 可能分配失败
    pixie->samples = kmalloc(sizeof(struct sample) * 65536, GFP_ATOMIC);
    if (!pixie->samples) {
        printk(KERN_ERR "pixie: 内存分配失败！\n");
        return;
    }
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static void pixie_release(struct sock *sk)
{
    struct pixie *pixie = inet_csk_ca(sk);
    if (pixie->samples) {
        kfree(pixie->samples);
        pixie->samples = NULL;
    }
}

static u32 pixie_ssthresh(struct sock *sk)
{
    return TCP_INFINITE_SSTHRESH;
}

static u32 pixie_undo_cwnd(struct sock *sk)
{
    return tcp_sk(sk)->snd_cwnd;
}

static struct tcp_congestion_ops tcp_pixie_cong_ops __read_mostly = {
    .flags      = TCP_CONG_NON_RESTRICTED,
    .name       = "pixie",
    .owner      = THIS_MODULE,
    .init       = pixie_init,
    .release    = pixie_release,
    .cong_control = pixie_main,
    .ssthresh   = pixie_ssthresh,
    .undo_cwnd  = pixie_undo_cwnd,
};

static int __init pixie_register(void)
{
    // 确保私有数据空间足够，否则会覆盖内核其他内存
    if (sizeof(struct pixie) > ICSK_CA_PRIV_SIZE) {
        printk(KERN_ERR "pixie: struct too large\n");
        return -EINVAL;
    }
    return tcp_register_congestion_control(&tcp_pixie_cong_ops);
}

static void __exit pixie_unregister(void)
{
    tcp_unregister_congestion_control(&tcp_pixie_cong_ops);
}

module_init(pixie_register);
module_exit(pixie_unregister);
MODULE_LICENSE("GPL");
