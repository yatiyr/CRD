/* HESAP-NET's ntpd peer (CRD_BUILD_CLOCK_VS_NTPD): ntpd 4.2.8p18's own clock_filter(), cut verbatim from the pinned
 * ntpd/ntp_proto.c at configure time, on the few fields of struct peer it touches and with ntp.h's constants. Never
 * linked or shipped. */

#ifndef CRD_TESTS_NTPD_PEER_H
#define CRD_TESTS_NTPD_PEER_H

#define CRD_NTPD_STAGES 8

#ifdef __cplusplus
extern "C"
{
#endif

    struct crd_ntpd_peer
    {
        unsigned char filter_nextpt;
        double filter_offset[CRD_NTPD_STAGES];
        double filter_delay[CRD_NTPD_STAGES];
        double filter_disp[CRD_NTPD_STAGES];
        unsigned long filter_epoch[CRD_NTPD_STAGES];
        unsigned char filter_order[CRD_NTPD_STAGES];
        unsigned long update;
        unsigned long epoch;
        double disp;
        double jitter;
        double offset;
        double delay;
        unsigned char hpoll;
        unsigned char burst;
        int srcadr;
    };

    enum crd_ntpd_outcome
    {
        CRD_NTPD_NO_SAMPLE,
        CRD_NTPD_SPIKE,
        CRD_NTPD_STALE,
        CRD_NTPD_FRESH
    };

    /* peer_clear()'s effect on the fields above, at this time and with this sys_precision (log 2). */
    void crd_ntpd_clear(struct crd_ntpd_peer* peer, unsigned long now, signed char sys_precision);

    /* clock_filter(), with the globals it reads. */
    enum crd_ntpd_outcome crd_ntpd_filter(struct crd_ntpd_peer* peer, unsigned long now, double offset, double delay,
                                          double dispersion, double clock_phi, double max_distance,
                                          unsigned char allan_intercept, signed char sys_precision);

#ifdef __cplusplus
}
#endif

#endif
