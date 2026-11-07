/* See netem_peer.h. The included files are the text of net/sched/sch_netem.c from init_crandom's comment to
 * packet_time_ns and from get_slot_next's comment to netem_peek, unchanged; everything above them stands in for the
 * kernel headers and the fields of netem_sched_data those functions read. */

#include "netem_peer.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define NETEM_DIST_SCALE 8192
#define NETEM_DIST_MAX 16384

static const u32* g_words;
static size_t g_count;
static size_t g_used;

struct rnd_state
{
    int unused;
};

static inline u32 next_word(void)
{
    const u32 word = g_words[g_used % g_count];
    ++g_used;
    return word;
}

static inline u32 prandom_u32_state(struct rnd_state* state)
{
    (void)state;
    return next_word();
}

static inline u32 get_random_u32(void)
{
    return next_word();
}

struct crndstate
{
    u32 last;
    u32 rho;
};

struct prng
{
    struct rnd_state prng_state;
};

struct disttable
{
    u32 size;
    s16 table[NETEM_DIST_MAX];
};

enum
{
    CLG_RANDOM,
    CLG_4_STATES,
    CLG_GILB_ELL
};

enum
{
    TX_IN_GAP_PERIOD = 1,
    TX_IN_BURST_PERIOD,
    LOST_IN_BURST_PERIOD,
    LOST_IN_GAP_PERIOD
};

enum
{
    GOOD_STATE = 1,
    BAD_STATE
};

struct clgstate
{
    u8 state;
    u32 a1;
    u32 a2;
    u32 a3;
    u32 a4;
    u32 a5;
};

struct slotstate
{
    u64 slot_next;
    s32 packets_left;
    s32 bytes_left;
};

struct slot_config
{
    s64 min_delay;
    s64 max_delay;
    s32 max_packets;
    s32 max_bytes;
    s64 dist_delay;
    s64 dist_jitter;
};

struct netem_sched_data
{
    struct prng prng;
    u32 loss;
    struct crndstate loss_cor;
    u8 loss_model;
    struct clgstate clg;
    struct slot_config slot_config;
    struct slotstate slot;
    struct disttable* slot_dist;
};

#include "netem_models_slice.inc"
#include "netem_slot_slice.inc"

static struct netem_sched_data g_netem;
static struct crndstate g_delay_cor;
static struct disttable g_table;

void crd_netem_set_words(const uint32_t* words, size_t count)
{
    g_words = words;
    g_count = count;
    g_used = 0U;
}

size_t crd_netem_words_used(void)
{
    return g_used;
}

void crd_netem_loss_random(uint32_t chance, uint32_t correlation)
{
    g_netem.loss_model = CLG_RANDOM;
    g_netem.loss = chance;
    init_crandom(&g_netem.loss_cor, correlation);
}

void crd_netem_loss_gilbert_elliott(uint32_t p, uint32_t r, uint32_t h, uint32_t k1)
{
    g_netem.loss_model = CLG_GILB_ELL;
    g_netem.clg.state = GOOD_STATE;
    g_netem.clg.a1 = p;
    g_netem.clg.a2 = r;
    g_netem.clg.a3 = h;
    g_netem.clg.a4 = k1;
}

void crd_netem_loss_four_state(uint32_t p13, uint32_t p31, uint32_t p32, uint32_t p23, uint32_t p14)
{
    g_netem.loss_model = CLG_4_STATES;
    g_netem.clg.state = TX_IN_GAP_PERIOD;
    g_netem.clg.a1 = p13;
    g_netem.clg.a2 = p31;
    g_netem.clg.a3 = p32;
    g_netem.clg.a4 = p14;
    g_netem.clg.a5 = p23;
}

bool crd_netem_loss_event(void)
{
    return loss_event(&g_netem);
}

uint32_t crd_netem_loss_run(uint32_t packets)
{
    uint32_t lost = 0U;
    for (uint32_t i = 0U; i < packets; ++i)
    {
        lost += loss_event(&g_netem) ? 1U : 0U;
    }
    return lost;
}

void crd_netem_delay_correlation(uint32_t correlation)
{
    init_crandom(&g_delay_cor, correlation);
}

void crd_netem_delay_table(const int16_t* table, uint32_t size)
{
    g_table.size = 0U;
    if (table != NULL)
    {
        g_table.size = size;
        for (uint32_t i = 0U; i < size; ++i)
        {
            g_table.table[i] = table[i];
        }
    }
}

int64_t crd_netem_delay(int64_t mean, int32_t deviation)
{
    return tabledist(mean, deviation, &g_delay_cor, &g_netem.prng, g_table.size != 0U ? &g_table : NULL);
}

int64_t crd_netem_delay_run(int64_t mean, int32_t deviation, uint32_t packets)
{
    const struct disttable* table = g_table.size != 0U ? &g_table : NULL;
    int64_t sum = 0;
    for (uint32_t i = 0U; i < packets; ++i)
    {
        sum += tabledist(mean, deviation, &g_delay_cor, &g_netem.prng, table);
    }
    return sum;
}

void crd_netem_slot_init(int64_t min_delay, int64_t max_delay, int32_t max_packets, int32_t max_bytes,
                         int64_t dist_delay, int32_t dist_jitter, uint64_t now)
{
    /* get_slot_config(). */
    g_netem.slot_config.min_delay = min_delay;
    g_netem.slot_config.max_delay = max_delay;
    g_netem.slot_config.max_packets = max_packets == 0 ? INT32_MAX : max_packets;
    g_netem.slot_config.max_bytes = max_bytes == 0 ? INT32_MAX : max_bytes;
    g_netem.slot_config.dist_delay = dist_delay;
    g_netem.slot_config.dist_jitter = dist_jitter;
    g_netem.slot.packets_left = g_netem.slot_config.max_packets;
    g_netem.slot.bytes_left = g_netem.slot_config.max_bytes;
    g_netem.slot.slot_next = (min_delay | max_delay | dist_jitter) != 0 ? now : 0U;
    g_netem.slot_dist = g_table.size != 0U ? &g_table : NULL;
}

bool crd_netem_slot_admit(uint64_t now, uint64_t time_to_send, uint32_t bytes)
{
    /* netem_dequeue(). */
    if (g_netem.slot.slot_next && g_netem.slot.slot_next < time_to_send)
    {
        get_slot_next(&g_netem, now);
    }
    if (time_to_send <= now && g_netem.slot.slot_next <= now)
    {
        if (g_netem.slot.slot_next)
        {
            g_netem.slot.packets_left--;
            g_netem.slot.bytes_left -= (s32)bytes;
            if (g_netem.slot.packets_left <= 0 || g_netem.slot.bytes_left <= 0)
            {
                get_slot_next(&g_netem, now);
            }
        }
        return true;
    }
    return false;
}

uint64_t crd_netem_slot_next(void)
{
    return g_netem.slot.slot_next;
}
