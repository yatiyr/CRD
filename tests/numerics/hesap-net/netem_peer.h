/* Linux's netem as a peer of hesap-net's link models (HESAP-NET): the loss and delay functions of the pinned
 * net/sched/sch_netem.c, cut verbatim at configure time and compiled on netem_peer.c's few stand-ins. Every word they
 * draw comes from one array, read in a cycle, so the two sides can be held to the same words and the same count. */

#ifndef CRD_NETEM_PEER_H
#define CRD_NETEM_PEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    void crd_netem_set_words(const uint32_t* words, size_t count);
    size_t crd_netem_words_used(void);

    /* netem_change()'s three loss configurations, in iproute2's terms. */
    void crd_netem_loss_random(uint32_t chance, uint32_t correlation);
    void crd_netem_loss_gilbert_elliott(uint32_t p, uint32_t r, uint32_t h, uint32_t k1);
    void crd_netem_loss_four_state(uint32_t p13, uint32_t p31, uint32_t p32, uint32_t p23, uint32_t p14);

    /* loss_event(), and the same over a run of packets inside this unit, for the board. */
    bool crd_netem_loss_event(void);
    uint32_t crd_netem_loss_run(uint32_t packets);

    /* init_crandom() and tabledist(). A null table is netem's uniform law. */
    void crd_netem_delay_correlation(uint32_t correlation);
    void crd_netem_delay_table(const int16_t* table, uint32_t size);
    int64_t crd_netem_delay(int64_t mean, int32_t deviation);
    int64_t crd_netem_delay_run(int64_t mean, int32_t deviation, uint32_t packets);

    /* The slot lines of get_slot_config() and netem_dequeue(), on the pinned get_slot_next(). */
    void crd_netem_slot_init(int64_t min_delay, int64_t max_delay, int32_t max_packets, int32_t max_bytes,
                             int64_t dist_delay, int32_t dist_jitter, uint64_t now);
    bool crd_netem_slot_admit(uint64_t now, uint64_t time_to_send, uint32_t bytes);
    uint64_t crd_netem_slot_next(void);

#ifdef __cplusplus
}
#endif

#endif
