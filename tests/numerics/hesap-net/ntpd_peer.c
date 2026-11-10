/* See ntpd_peer.h. The included file is the text of ntpd/ntp_proto.c from clock_filter's comment to clock_select's,
 * unchanged; everything above it stands in for the headers and globals that text names.
 *
 * The constants and macros under "include/ntp.h" are copied from ntpd 4.2.8p18's include/ntp.h, and the included text
 * is ntpd's own: Copyright (c) University of Delaware 1992-2015 and Copyright (c) Network Time Foundation 2011-2024,
 * under the NTP licence and BSD-2-Clause; notice in THIRD-PARTY-NOTICES.md. */

#include "ntpd_peer.h"

#include <math.h>
#include <string.h>

#define peer crd_ntpd_peer
typedef unsigned char u_char;
typedef unsigned long u_long;

/* include/ntp.h */
#define MAXDISPERSE 16.
#define NTP_SHIFT 8
#define NTP_FWEIGHT .5
#define CLOCK_SGATE 3.
#define SQUARE(x) ((x) * (x))
#define SQRT(x) (sqrt(x))
#define DIFF(x, y) (SQUARE((x) - (y)))
#define LOGTOD(a) ldexp(1., (int)(a))
#define ULOGTOD(a) ldexp(1., (int)(a))
#define max(a, b) (((a) > (b)) ? (a) : (b))

/* The state clock_filter reads. */
static u_long current_time;
static double clock_phi;
static double sys_maxdist;
static u_char allan_xpt;
static signed char sys_precision;
static int freq_cnt; /* 0: the clock has stabilised, the state this filter is compared in. */
static int sys_leap;
#define LEAP_NOTINSYNC 3
#define PEVNT_POPCORN 0

/* What it reports through. */
static enum crd_ntpd_outcome outcome;
#define clock_select() (outcome = (outcome == CRD_NTPD_FRESH) ? CRD_NTPD_FRESH : CRD_NTPD_NO_SAMPLE)
#define ctlpeerstatus(p) 0
#define record_peer_stats(address, status, offset_value, delay_value, disp_value, jitter_value)                        \
    (outcome = CRD_NTPD_FRESH)
#define mprintf_event(event, source, ...) (outcome = CRD_NTPD_SPIKE)
#define DPRINTF(level, arguments) ((void)0)

#include "ntpd_clock_filter_slice.inc"

#undef peer

void crd_ntpd_clear(struct crd_ntpd_peer* state, unsigned long now, signed char precision)
{
    memset(state, 0, sizeof(*state));
    state->disp = MAXDISPERSE;
    state->jitter = LOGTOD(precision);
    state->update = now;
    for (int stage = 0; stage < NTP_SHIFT; ++stage)
    {
        state->filter_order[stage] = (u_char)stage;
        state->filter_disp[stage] = MAXDISPERSE;
    }
}

enum crd_ntpd_outcome crd_ntpd_filter(struct crd_ntpd_peer* state, unsigned long now, double offset, double delay,
                                      double dispersion, double phi, double max_distance, unsigned char allan_intercept,
                                      signed char precision)
{
    current_time = now;
    clock_phi = phi;
    sys_maxdist = max_distance;
    allan_xpt = allan_intercept;
    sys_precision = precision;
    freq_cnt = 0;
    sys_leap = 0;

    /* The stale return is the one that reports through nothing. */
    outcome = CRD_NTPD_STALE;
    clock_filter(state, offset, delay, dispersion);
    return outcome;
}
