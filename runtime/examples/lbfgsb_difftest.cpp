// lbfgsb_difftest.cpp — Phase 3.1.6 v7-d-3 DIFFERENTIAL-TEST HARNESS. The faithful L-BFGS-B port is a confirmed
// bug farm (the 1-based↔f2c-0-based-BLAS pointer convention produced 4 off-by-ones in the EASY routines; manual
// audit is NOT verification). This harness is the ONLY honest oracle: for each Cerid routine it feeds the reference
// C (stephenbeckr/L-BFGS-B-C, built by scripts/setup-lbfgsb-ref.sh into liblbfgsb_ref.a) and the Cerid port
// IDENTICAL inputs and asserts the outputs match — REALS AND INTEGER ARRAYS — in bug-HIDING regimes (dtrsl all 4
// jobs, matupd iupdat>m ring-wrap, hpsolb ties), NOT random-small. Dev-only, WSL, never CI/shipped.
//
// LAYOUTS: the reference does f2c parameter adjustment (--x / a-=1+lda) ⇒ caller passes 0-based arrays: 1-D x[i-1],
// 2-D column-major a[(i-1)+(j-1)*lda]. Cerid uses 1-based over-allocated buffers: x[i], a[i+j*lda]. Each test
// generates one logical input and marshals it into BOTH layouts so the numbers are bit-identical going in.

#include <crd/hesap/opt/lbfgsb.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstdio>
#include <random>

namespace lb = crd::hesap::opt::detail::lbfgsb;

// ---- reference routines (C linkage; integer == long int). ----
extern "C"
{
    int dpofa(double* a, long* lda, long* n, long* info);
    int dtrsl(double* t, long* ldt, long* n, double* b, long* job, long* info);
    int active(long* n, double* l, double* u, long* nbd, double* x, long* iwhere, long* iprint, long* prjctd,
               long* cnstnd, long* boxed);
    int bmv(long* m, double* sy, double* wt, long* col, double* v, double* p, long* info);
    int formt(long* m, double* wt, double* sy, double* ss, long* col, double* theta, long* info);
    int cmprlb(long* n, long* m, double* x, double* g, double* ws, double* wy, double* sy, double* wt, double* z,
               double* r, double* wa, long* index, double* theta, long* col, long* head, long* nfree, long* cnstnd,
               long* info);
    int freev(long* n, long* nfree, long* index, long* nenter, long* ileave, long* indx2, long* iwhere, long* wrk,
              long* updatd, long* cnstnd, long* iprint, long* iter);
    int hpsolb(long* n, double* t, long* iorder, long* iheap);
    int matupd(long* n, long* m, double* ws, double* wy, double* sy, double* ss, double* d, double* r, long* itail,
               long* iupdat, long* col, long* head, double* theta, double* rr, double* dr, double* stp, double* dtd);
    int projgr(long* n, double* l, double* u, long* nbd, double* x, double* g, double* sbgnrm);
    int cauchy(long* n, double* x, double* l, double* u, long* nbd, double* g, long* iorder, long* iwhere, double* t,
               double* d, double* xcp, long* m, double* wy, double* ws, double* sy, double* wt, double* theta,
               long* col, long* head, double* p, double* c, double* wbp, double* v, long* nseg, long* iprint,
               double* sbgnrm, long* info, double* epsmch);
    int formk(long* n, long* nsub, long* ind, long* nenter, long* ileave, long* indx2, long* iupdat, long* updatd,
              double* wn, double* wn1, long* m, double* ws, double* wy, double* sy, double* theta, long* col,
              long* head, long* info);
    int subsm(long* n, long* m, long* nsub, long* ind, double* l, double* u, long* nbd, double* x, double* d,
              double* xp, double* ws, double* wy, double* theta, double* xx, double* gg, long* col, long* head,
              long* iword, double* wv, double* wn, long* iprint, long* info);
    int setulb(long* n, long* m, double* x, double* l, double* u, long* nbd, double* f, double* g, double* factr,
               double* pgtol, double* wa, long* iwa, long* task, long* iprint, long* csave, long* lsave,
               long* isave, double* dsave);
}

namespace
{
std::mt19937_64 g_rng(123456789ULL);
int             g_fail = 0;
int             g_pass = 0;

double rr() { return std::uniform_real_distribution<double>(-1.0, 1.0)(g_rng); }

bool closed(double a, double b) { return std::fabs(a - b) <= 1e-9 * (1.0 + std::fabs(a) + std::fabs(b)); }

void check(bool ok, const char* what)
{
    if (ok)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
        std::printf("  FAIL: %s\n", what);
    }
}

// Compare a 1-based Cerid 1-D array (cer[1..n]) vs a 0-based reference (ref[0..n-1]).
bool cmp1d(const double* ref, const double* cer, int n)
{
    for (int i = 1; i <= n; ++i)
    {
        if (!closed(ref[i - 1], cer[i]))
        {
            return false;
        }
    }
    return true;
}
bool cmp1d_int(const long* ref, const int* cer, int n)
{
    for (int i = 1; i <= n; ++i)
    {
        if (ref[i - 1] != cer[i])
        {
            return false;
        }
    }
    return true;
}
// Compare a (rows×cols) matrix: Cerid cer[i+j*lda] vs reference ref[(i-1)+(j-1)*lda].
bool cmp2d(const double* ref, const double* cer, int rows, int cols, int lda)
{
    for (int j = 1; j <= cols; ++j)
    {
        for (int i = 1; i <= rows; ++i)
        {
            if (!closed(ref[(i - 1) + (j - 1) * lda], cer[i + j * lda]))
            {
                return false;
            }
        }
    }
    return true;
}

constexpr int kBuf = 1024;

// ============================ dpofa ============================
void test_dpofa()
{
    for (int n : {1, 2, 5, 8})
    {
        double aref[kBuf] = {};
        double acer[kBuf] = {};
        // SPD M = BᵀB + n·I (symmetric); fill both layouts identically.
        double B[64] = {}; // n×n, B[i-1 + (j-1)*n]
        for (int j = 1; j <= n; ++j)
        {
            for (int i = 1; i <= n; ++i)
            {
                B[(i - 1) + (j - 1) * n] = rr();
            }
        }
        for (int j = 1; j <= n; ++j)
        {
            for (int i = 1; i <= n; ++i)
            {
                double s = (i == j) ? static_cast<double>(n) : 0.0;
                for (int k = 1; k <= n; ++k)
                {
                    s += B[(k - 1) + (i - 1) * n] * B[(k - 1) + (j - 1) * n];
                }
                aref[(i - 1) + (j - 1) * n] = s;
                acer[i + j * n] = s;
            }
        }
        long lda = n, nn = n, inforef = 0;
        dpofa(aref, &lda, &nn, &inforef);
        int infocer = 0;
        lb::dpofa<double>(acer, n, n, infocer);
        bool ok = (inforef == static_cast<long>(infocer));
        for (int j = 1; j <= n && ok; ++j)
        {
            for (int i = 1; i <= j; ++i) // upper triangle = R
            {
                if (!closed(aref[(i - 1) + (j - 1) * n], acer[i + j * n]))
                {
                    ok = false;
                }
            }
        }
        char w[64];
        std::snprintf(w, sizeof(w), "dpofa n=%d", n);
        check(ok, w);
    }
}

// ============================ dtrsl ============================
void test_dtrsl()
{
    for (int n : {1, 2, 5, 8})
    {
        for (int job : {0, 1, 10, 11})
        {
            double tref[kBuf] = {};
            double tcer[kBuf] = {};
            double bref[kBuf] = {};
            double bcer[kBuf] = {};
            for (int j = 1; j <= n; ++j)
            {
                for (int i = 1; i <= n; ++i)
                {
                    double v = (i == j) ? (1.5 + std::fabs(rr())) : rr(); // nonzero diagonal
                    tref[(i - 1) + (j - 1) * n] = v;
                    tcer[i + j * n] = v;
                }
            }
            for (int i = 1; i <= n; ++i)
            {
                double v = rr();
                bref[i - 1] = v;
                bcer[i] = v;
            }
            long ldt = n, nn = n, jb = job, inforef = 0;
            dtrsl(tref, &ldt, &nn, bref, &jb, &inforef);
            int infocer = 0;
            lb::dtrsl<double>(tcer, n, n, bcer, job, infocer);
            bool ok = (inforef == static_cast<long>(infocer)) && cmp1d(bref, bcer, n);
            char w[64];
            std::snprintf(w, sizeof(w), "dtrsl n=%d job=%d", n, job);
            check(ok, w);
        }
    }
}

// ============================ projgr ============================
void test_projgr()
{
    for (int n : {1, 5, 12})
    {
        double l[kBuf], u[kBuf], x[kBuf], g[kBuf];
        long   nbdref[kBuf];
        int    nbdcer[kBuf];
        for (int i = 1; i <= n; ++i)
        {
            l[i] = -1.0 + rr();
            u[i] = 1.0 + std::fabs(rr());
            x[i] = rr();
            g[i] = rr();
            long nb = (i % 4);
            nbdref[i - 1] = nb;
            nbdcer[i] = static_cast<int>(nb);
        }
        // reference uses 0-based l/u/x/g; build 0-based copies.
        double l0[kBuf], u0[kBuf], x0[kBuf], g0[kBuf];
        for (int i = 1; i <= n; ++i)
        {
            l0[i - 1] = l[i];
            u0[i - 1] = u[i];
            x0[i - 1] = x[i];
            g0[i - 1] = g[i];
        }
        long   nn = n;
        double sbref = 0.0;
        projgr(&nn, l0, u0, nbdref, x0, g0, &sbref);
        double sbcer = 0.0;
        lb::projgr<double>(n, l, u, nbdcer, x, g, sbcer);
        char w[64];
        std::snprintf(w, sizeof(w), "projgr n=%d", n);
        check(closed(sbref, sbcer), w);
    }
}

// ============================ active ============================
void test_active()
{
    for (int n : {1, 6, 12})
    {
        double lref[kBuf], uref[kBuf], xref[kBuf];
        double lcer[kBuf], ucer[kBuf], xcer[kBuf];
        long   nbdref[kBuf];
        int    nbdcer[kBuf];
        for (int i = 1; i <= n; ++i)
        {
            double lo = -1.0, hi = 1.0, xx = 2.0 * rr(); // x sometimes outside [lo,hi] ⇒ projection fires
            long   nb = (i % 4);
            lref[i - 1] = lo;
            uref[i - 1] = hi;
            xref[i - 1] = xx;
            lcer[i] = lo;
            ucer[i] = hi;
            xcer[i] = xx;
            nbdref[i - 1] = nb;
            nbdcer[i] = static_cast<int>(nb);
        }
        long iwref[kBuf];
        int  iwcer[kBuf];
        long nn = n, iprint = -1, pr = 0, cn = 0, bx = 0;
        active(&nn, lref, uref, nbdref, xref, iwref, &iprint, &pr, &cn, &bx);
        bool prc = false, cnc = false, bxc = false;
        lb::active<double>(n, lcer, ucer, nbdcer, xcer, iwcer, prc, cnc, bxc);
        bool ok = cmp1d(xref, xcer, n) && cmp1d_int(iwref, iwcer, n) && (pr != 0) == prc && (cn != 0) == cnc &&
                  (bx != 0) == bxc;
        char w[64];
        std::snprintf(w, sizeof(w), "active n=%d", n);
        check(ok, w);
    }
}

// ============================ hpsolb (with ties — the moat landmine) ============================
void test_hpsolb()
{
    for (int n : {1, 4, 9})
    {
        for (int iheap : {0, 1, 2})
        {
            double tref[kBuf], tcer[kBuf];
            long   orref[kBuf];
            int    orcer[kBuf];
            for (int i = 1; i <= n; ++i)
            {
                double v = static_cast<double>((i * 7) % 5); // deliberate TIES (values 0..4 repeat)
                tref[i - 1] = v;
                tcer[i] = v;
                orref[i - 1] = i;
                orcer[i] = i;
            }
            long nn = n, ih = iheap;
            hpsolb(&nn, tref, orref, &ih);
            lb::hpsolb<double>(n, tcer, orcer, iheap);
            bool ok = cmp1d(tref, tcer, n) && cmp1d_int(orref, orcer, n);
            char w[64];
            std::snprintf(w, sizeof(w), "hpsolb n=%d iheap=%d (ties)", n, iheap);
            check(ok, w);
        }
    }
}

// ============================ bmv ============================
void test_bmv()
{
    for (int m : {1, 3, 6})
    {
        const int col = m;
        double    syref[kBuf] = {}, sycer[kBuf] = {};
        double    wtref[kBuf] = {}, wtcer[kBuf] = {};
        for (int j = 1; j <= m; ++j)
        {
            for (int i = 1; i <= m; ++i)
            {
                double sv = (i == j) ? (1.5 + std::fabs(rr())) : rr();   // positive diagonal (D)
                double wv = (i <= j) ? ((i == j) ? (1.5 + std::fabs(rr())) : rr()) : 0.0; // upper J
                syref[(i - 1) + (j - 1) * m] = sv;
                sycer[i + j * m] = sv;
                wtref[(i - 1) + (j - 1) * m] = wv;
                wtcer[i + j * m] = wv;
            }
        }
        double vref[kBuf] = {}, vcer[kBuf] = {}, pref[kBuf] = {}, pcer[kBuf] = {};
        for (int i = 1; i <= 2 * col; ++i)
        {
            double v = rr();
            vref[i - 1] = v;
            vcer[i] = v;
        }
        long mm = m, cc = col, inforef = 0;
        bmv(&mm, syref, wtref, &cc, vref, pref, &inforef);
        int infocer = 0;
        lb::bmv<double>(m, sycer, wtcer, col, vcer, pcer, infocer);
        bool ok = (inforef == static_cast<long>(infocer)) && cmp1d(pref, pcer, 2 * col);
        char w[64];
        std::snprintf(w, sizeof(w), "bmv m=%d", m);
        check(ok, w);
    }
}

// ============================ formt ============================
void test_formt()
{
    for (int m : {1, 3, 6})
    {
        const int col = m;
        double    syref[kBuf] = {}, sycer[kBuf] = {}, ssref[kBuf] = {}, sscer[kBuf] = {};
        double    wtref[kBuf] = {}, wtcer[kBuf] = {};
        for (int j = 1; j <= m; ++j)
        {
            for (int i = 1; i <= m; ++i)
            {
                double sv = (i == j) ? (1.5 + std::fabs(rr())) : rr();
                double ssv = (i == j) ? (2.0 + std::fabs(rr())) : 0.3 * rr();
                syref[(i - 1) + (j - 1) * m] = sv;
                sycer[i + j * m] = sv;
                ssref[(i - 1) + (j - 1) * m] = ssv;
                sscer[i + j * m] = ssv;
            }
        }
        double theta = 1.2;
        long   mm = m, cc = col, inforef = 0;
        formt(&mm, wtref, syref, ssref, &cc, &theta, &inforef);
        int infocer = 0;
        lb::formt<double>(m, wtcer, sycer, sscer, col, theta, infocer);
        bool ok = (inforef == static_cast<long>(infocer)) && cmp2d(wtref, wtcer, m, m, m);
        char w[64];
        std::snprintf(w, sizeof(w), "formt m=%d", m);
        check(ok, w);
    }
}

// ============================ matupd (iupdat>m ring-wrap — the bug regime) ============================
void test_matupd()
{
    const int n = 6, m = 4;
    for (int iupdat : {1, 2, 4, 5, 7}) // 5,7 exercise iupdat>m (the sy/ss shift bug)
    {
        double wsref[kBuf] = {}, wscer[kBuf] = {}, wyref[kBuf] = {}, wycer[kBuf] = {};
        double syref[kBuf] = {}, sycer[kBuf] = {}, ssref[kBuf] = {}, sscer[kBuf] = {};
        double dref[kBuf] = {}, dcer[kBuf] = {}, rref[kBuf] = {}, rcer[kBuf] = {};
        for (int j = 1; j <= m; ++j) // ws,wy are n×m
        {
            for (int i = 1; i <= n; ++i)
            {
                double a = rr(), b = rr();
                wsref[(i - 1) + (j - 1) * n] = a;
                wscer[i + j * n] = a;
                wyref[(i - 1) + (j - 1) * n] = b;
                wycer[i + j * n] = b;
            }
        }
        for (int j = 1; j <= m; ++j) // sy,ss are m×m
        {
            for (int i = 1; i <= m; ++i)
            {
                double a = rr(), b = rr();
                syref[(i - 1) + (j - 1) * m] = a;
                sycer[i + j * m] = a;
                ssref[(i - 1) + (j - 1) * m] = b;
                sscer[i + j * m] = b;
            }
        }
        for (int i = 1; i <= n; ++i)
        {
            double a = rr(), b = rr();
            dref[i - 1] = a;
            dcer[i] = a;
            rref[i - 1] = b;
            rcer[i] = b;
        }
        const int col0 = (iupdat <= m) ? iupdat : m;
        long      mm = n, mmm = m, itailref = m, iupd = iupdat, colref = col0, headref = 1;
        double    thetaref = 0.0, rrv = 1.3, drv = 0.7, stp = 0.9, dtd = 1.1;
        matupd(&mm, &mmm, wsref, wyref, syref, ssref, dref, rref, &itailref, &iupd, &colref, &headref, &thetaref,
               &rrv, &drv, &stp, &dtd);
        int    itailcer = m, colcer = col0, headcer = 1;
        double thetacer = 0.0;
        lb::matupd<double>(n, m, wscer, wycer, sycer, sscer, dcer, rcer, itailcer, iupdat, colcer, headcer,
                           thetacer, 1.3, 0.7, 0.9, 1.1);
        bool ok = cmp2d(wsref, wscer, n, m, n) && cmp2d(wyref, wycer, n, m, n) &&
                  cmp2d(syref, sycer, m, m, m) && cmp2d(ssref, sscer, m, m, m) &&
                  (itailref == itailcer) && (colref == colcer) && (headref == headcer) &&
                  closed(thetaref, thetacer);
        char w[64];
        std::snprintf(w, sizeof(w), "matupd iupdat=%d (col=%d)", iupdat, col0);
        check(ok, w);
    }
}

// ============================ cmprlb ============================
void test_cmprlb()
{
    const int n = 8, m = 4;
    const int col = m, head = 1, nfree = 5;
    double    x[kBuf], g[kBuf], z[kBuf];
    double    x0[kBuf], g0[kBuf], z0[kBuf];
    for (int i = 1; i <= n; ++i)
    {
        x[i] = rr();
        g[i] = rr();
        z[i] = rr();
        x0[i - 1] = x[i];
        g0[i - 1] = g[i];
        z0[i - 1] = z[i];
    }
    double wsref[kBuf] = {}, wscer[kBuf] = {}, wyref[kBuf] = {}, wycer[kBuf] = {};
    double syref[kBuf] = {}, sycer[kBuf] = {}, wtref[kBuf] = {}, wtcer[kBuf] = {};
    for (int j = 1; j <= m; ++j)
    {
        for (int i = 1; i <= n; ++i)
        {
            double a = rr(), b = rr();
            wsref[(i - 1) + (j - 1) * n] = a;
            wscer[i + j * n] = a;
            wyref[(i - 1) + (j - 1) * n] = b;
            wycer[i + j * n] = b;
        }
    }
    for (int j = 1; j <= m; ++j)
    {
        for (int i = 1; i <= m; ++i)
        {
            double sv = (i == j) ? (1.5 + std::fabs(rr())) : rr();
            double wv = (i <= j) ? ((i == j) ? (1.5 + std::fabs(rr())) : rr()) : 0.0;
            syref[(i - 1) + (j - 1) * m] = sv;
            sycer[i + j * m] = sv;
            wtref[(i - 1) + (j - 1) * m] = wv;
            wtcer[i + j * m] = wv;
        }
    }
    long indexref[kBuf];
    int  indexcer[kBuf];
    for (int i = 1; i <= n; ++i) // free vars = identity subset 1..nfree
    {
        indexref[i - 1] = i;
        indexcer[i] = i;
    }
    double rref[kBuf] = {}, rcer[kBuf] = {}, waref[kBuf] = {}, wacer[kBuf] = {};
    double theta = 1.1;
    long   nn = n, mm = m, cc = col, hh = head, nf = nfree, cnstnd = 1, inforef = 0;
    cmprlb(&nn, &mm, x0, g0, wsref, wyref, syref, wtref, z0, rref, waref, indexref, &theta, &cc, &hh, &nf, &cnstnd,
           &inforef);
    int infocer = 0;
    lb::cmprlb<double>(n, m, x, g, wscer, wycer, sycer, wtcer, z, rcer, wacer, indexcer, theta, col, head, nfree,
                       true, infocer);
    bool ok = (inforef == static_cast<long>(infocer)) && cmp1d(rref, rcer, nfree);
    check(ok, "cmprlb n=8 m=4 cnstnd");
}

// ============================ freev ============================
void test_freev()
{
    for (int n : {4, 10})
    {
        long iwref[kBuf];
        int  iwcer[kBuf];
        long idxref[kBuf], i2ref[kBuf];
        int  idxcer[kBuf], i2cer[kBuf];
        for (int i = 1; i <= n; ++i)
        {
            long iw = (i % 3) - 1; // -1,0,1 mix (free vs active)
            iwref[i - 1] = iw;
            iwcer[i] = static_cast<int>(iw);
            long id = ((i * 5) % n) + 1;
            idxref[i - 1] = id;
            idxcer[i] = static_cast<int>(id);
            i2ref[i - 1] = 0;
            i2cer[i] = 0;
        }
        long nfref = (n / 2), nentref = 0, ileaveref = 0;
        long nn = n, updatd = 1, cnstnd = 1, iprint = -1, iter = 3;
        long wrkref = 0;
        freev(&nn, &nfref, idxref, &nentref, &ileaveref, i2ref, iwref, &wrkref, &updatd, &cnstnd, &iprint, &iter);
        int  nfcer = (n / 2), nentcer = 0, ileavecer = 0;
        bool wrkcer = false;
        lb::freev<double>(n, nfcer, idxcer, nentcer, ileavecer, i2cer, iwcer, wrkcer, true, true, 3);
        bool ok = (nfref == nfcer) && (nentref == nentcer) && (ileaveref == ileavecer) &&
                  ((wrkref != 0) == wrkcer) && cmp1d_int(idxref, idxcer, n);
        char w[64];
        std::snprintf(w, sizeof(w), "freev n=%d", n);
        check(ok, w);
    }
}
// ============================ cauchy (the GCP — the critical routine) ============================
void test_cauchy()
{
    struct Dim
    {
        int n, m;
    };
    for (Dim dim : {Dim{2, 1}, Dim{10, 4}, Dim{16, 6}})
    {
        const int n = dim.n;
        const int m = dim.m;
        const int col = m;
        const int head = 1;
        const double theta = 1.1;
        // x strictly inside [l,u]; bounded vars (nbd mix) ⇒ breakpoints exist ⇒ the GCP loop runs.
        double xref[kBuf], lref[kBuf], uref[kBuf], gref[kBuf];
        double xcer[kBuf], lcer[kBuf], ucer[kBuf], gcer[kBuf];
        long   nbdref[kBuf];
        int    nbdcer[kBuf];
        for (int i = 1; i <= n; ++i)
        {
            double lo = -2.0, hi = 2.0, xx = rr(), gg = rr();
            long   nb = 2; // mostly bounded both sides ⇒ rich breakpoint structure
            if (i % 4 == 0)
            {
                nb = 0; // a few free
            }
            xref[i - 1] = xx;
            lref[i - 1] = lo;
            uref[i - 1] = hi;
            gref[i - 1] = gg;
            nbdref[i - 1] = nb;
            xcer[i] = xx;
            lcer[i] = lo;
            ucer[i] = hi;
            gcer[i] = gg;
            nbdcer[i] = static_cast<int>(nb);
        }
        long iwref[kBuf] = {};
        int  iwcer[kBuf] = {};
        long iordref[kBuf] = {};
        int  iordcer[kBuf] = {};
        double tref[kBuf] = {}, tcer[kBuf] = {}, dref[kBuf] = {}, dcer[kBuf] = {}, xcpref[kBuf] = {},
               xcpcer[kBuf] = {};
        double wyref[kBuf] = {}, wycer[kBuf] = {}, wsref[kBuf] = {}, wscer[kBuf] = {}, syref[kBuf] = {},
               sycer[kBuf] = {}, wtref[kBuf] = {}, wtcer[kBuf] = {};
        for (int j = 1; j <= m; ++j)
        {
            for (int i = 1; i <= n; ++i)
            {
                double a = rr(), b = rr();
                wyref[(i - 1) + (j - 1) * n] = a;
                wycer[i + j * n] = a;
                wsref[(i - 1) + (j - 1) * n] = b;
                wscer[i + j * n] = b;
            }
        }
        for (int j = 1; j <= m; ++j)
        {
            for (int i = 1; i <= m; ++i)
            {
                double sv = (i == j) ? (1.5 + std::fabs(rr())) : rr();
                double wv = (i <= j) ? ((i == j) ? (1.5 + std::fabs(rr())) : rr()) : 0.0;
                syref[(i - 1) + (j - 1) * m] = sv;
                sycer[i + j * m] = sv;
                wtref[(i - 1) + (j - 1) * m] = wv;
                wtcer[i + j * m] = wv;
            }
        }
        double pref[kBuf] = {}, pcer[kBuf] = {}, cref[kBuf] = {}, ccer[kBuf] = {}, wbpref[kBuf] = {},
               wbpcer[kBuf] = {}, vref[kBuf] = {}, vcer[kBuf] = {};
        long   nn = n, mm = m, cc = col, hh = head, nsegref = 0, iprint = -1, inforef = 0;
        double th = theta, sbg = 1.0, eps = 2.220446049250313e-16;
        cauchy(&nn, xref, lref, uref, nbdref, gref, iordref, iwref, tref, dref, xcpref, &mm, wyref, wsref, syref,
               wtref, &th, &cc, &hh, pref, cref, wbpref, vref, &nsegref, &iprint, &sbg, &inforef, &eps);
        int nsegcer = 0, infocer = 0;
        lb::cauchy<double>(n, xcer, lcer, ucer, nbdcer, gcer, iordcer, iwcer, tcer, dcer, xcpcer, m, wycer, wscer,
                           sycer, wtcer, theta, col, head, pcer, ccer, wbpcer, vcer, nsegcer, 1.0, infocer,
                           2.220446049250313e-16);
        bool ok = (inforef == static_cast<long>(infocer)) && (nsegref == static_cast<long>(nsegcer)) &&
                  cmp1d(xcpref, xcpcer, n) && cmp1d(dref, dcer, n) && cmp1d_int(iwref, iwcer, n) &&
                  cmp1d_int(iordref, iordcer, n) && cmp1d(cref, ccer, 2 * col) && cmp1d(pref, pcer, 2 * col);
        char w[64];
        std::snprintf(w, sizeof(w), "cauchy n=%d m=%d", n, m);
        check(ok, w);
    }
}
// ============================ formk (compact-rep K matrix + inner Cholesky) ============================
void test_formk()
{
    const int n = 10, m = 4, col = m;
    const int m2 = 2 * m;
    const int nsub = 5, head = 1, iupdat = m + 2; // iupdat>m ⇒ the wn1 shift
    const int nenter = 2, ileave = n - 1;
    long      indref[kBuf], i2ref[kBuf];
    int       indcer[kBuf], i2cer[kBuf];
    for (int i = 1; i <= n; ++i)
    {
        indref[i - 1] = i;
        indcer[i] = i;
        i2ref[i - 1] = i;
        i2cer[i] = i;
    }
    double wsref[kBuf] = {}, wscer[kBuf] = {}, wyref[kBuf] = {}, wycer[kBuf] = {};
    double syref[kBuf] = {}, sycer[kBuf] = {};
    for (int j = 1; j <= m; ++j)
    {
        for (int i = 1; i <= n; ++i)
        {
            double a = 0.1 * rr(), b = 0.1 * rr();
            wsref[(i - 1) + (j - 1) * n] = a;
            wscer[i + j * n] = a;
            wyref[(i - 1) + (j - 1) * n] = b;
            wycer[i + j * n] = b;
        }
    }
    for (int j = 1; j <= m; ++j)
    {
        for (int i = 1; i <= m; ++i)
        {
            double sv = (i == j) ? (10.0 + std::fabs(rr())) : 0.1 * rr(); // diag-dominant ⇒ inner dpofa succeeds
            syref[(i - 1) + (j - 1) * m] = sv;
            sycer[i + j * m] = sv;
        }
    }
    double wnref[kBuf] = {}, wncer[kBuf] = {}, wn1ref[kBuf] = {}, wn1cer[kBuf] = {};
    for (int j = 1; j <= m2; ++j)
    {
        for (int i = 1; i <= m2; ++i)
        {
            double v = 0.1 * rr();
            wn1ref[(i - 1) + (j - 1) * m2] = v;
            wn1cer[i + j * m2] = v;
        }
    }
    double theta = 1.2;
    long   nn = n, nsubl = nsub, nent = nenter, ilv = ileave, iupd = iupdat, updatd = 1, mm = m, cc = col, hh = head,
         inforef = 0;
    formk(&nn, &nsubl, indref, &nent, &ilv, i2ref, &iupd, &updatd, wnref, wn1ref, &mm, wsref, wyref, syref, &theta,
          &cc, &hh, &inforef);
    int infocer = 0;
    lb::formk<double>(n, nsub, indcer, nenter, ileave, i2cer, iupdat, true, wncer, wn1cer, m, wscer, wycer, sycer,
                      theta, col, head, infocer);
    bool ok = (inforef == static_cast<long>(infocer)) && cmp2d(wnref, wncer, m2, m2, m2) &&
              cmp2d(wn1ref, wn1cer, m2, m2, m2);
    char w[64];
    std::snprintf(w, sizeof(w), "formk n=%d m=%d info=%d", n, m, infocer);
    check(ok, w);
}

// ============================ subsm (subspace minimization; wn from formk) ============================
void test_subsm()
{
    const int n = 10, m = 4, col = m, m2 = 2 * m, head = 1;
    const int nsub = 5;
    const double theta = 1.2;
    // Build ws/wy/sy (strongly diag-dominant sy, tiny off) + run formk to get a reliably-factorable wn.
    double wsref[kBuf] = {}, wscer[kBuf] = {}, wyref[kBuf] = {}, wycer[kBuf] = {};
    double syref[kBuf] = {}, sycer[kBuf] = {};
    for (int j = 1; j <= m; ++j)
    {
        for (int i = 1; i <= n; ++i)
        {
            double a = 0.01 * rr(), b = 0.01 * rr();
            wsref[(i - 1) + (j - 1) * n] = a;
            wscer[i + j * n] = a;
            wyref[(i - 1) + (j - 1) * n] = b;
            wycer[i + j * n] = b;
        }
    }
    for (int j = 1; j <= m; ++j)
    {
        for (int i = 1; i <= m; ++i)
        {
            double sv = (i == j) ? (50.0 + std::fabs(rr())) : 0.01 * rr();
            syref[(i - 1) + (j - 1) * m] = sv;
            sycer[i + j * m] = sv;
        }
    }
    long indref[kBuf], i2ref[kBuf];
    int  indcer[kBuf], i2cer[kBuf];
    for (int i = 1; i <= n; ++i)
    {
        indref[i - 1] = i;
        indcer[i] = i;
        i2ref[i - 1] = i;
        i2cer[i] = i;
    }
    double wnref[kBuf] = {}, wncer[kBuf] = {}, wn1ref[kBuf] = {}, wn1cer[kBuf] = {};
    // Drive formk with updatd=false + a directly-controlled diagonally-dominant wn1 ⇒ K is reliably SPD (no
    // dependence on ws/wy history). Both (1,1) and (2,2) blocks get a large diagonal.
    for (int i = 1; i <= m2; ++i)
    {
        wn1ref[(i - 1) + (i - 1) * m2] = 10.0;
        wn1cer[i + i * m2] = 10.0;
    }
    {
        long nn = n, nsubl = nsub, nent = 0, ilv = n + 1, iupd = 1, updatd = 0, mm = m, cc = col, hh = head,
             inf = 0;
        double th = theta;
        formk(&nn, &nsubl, indref, &nent, &ilv, i2ref, &iupd, &updatd, wnref, wn1ref, &mm, wsref, wyref, syref, &th,
              &cc, &hh, &inf);
        int infc = 0;
        lb::formk<double>(n, nsub, indcer, 0, n + 1, i2cer, 1, false, wncer, wn1cer, m, wscer, wycer, sycer,
                          theta, col, head, infc);
        if (inf != 0 || infc != 0)
        {
            check(false, "subsm setup: formk did not factor");
            return;
        }
    }
    // subsm inputs: bounded vars [-1,1], x inside, d (Newton dir) random + large enough to hit bounds.
    double lref[kBuf], uref[kBuf], xref[kBuf], dref[kBuf], xpref[kBuf], xxref[kBuf], ggref[kBuf], wvref[kBuf] = {};
    double lcer[kBuf], ucer[kBuf], xcer[kBuf], dcer[kBuf], xpcer[kBuf], xxcer[kBuf], ggcer[kBuf], wvcer[kBuf] = {};
    long   nbdref[kBuf];
    int    nbdcer[kBuf];
    for (int i = 1; i <= n; ++i)
    {
        double lo = -1.0, hi = 1.0, xx = 0.5 * rr(), gg = rr();
        lref[i - 1] = lo;
        uref[i - 1] = hi;
        xref[i - 1] = xx;
        xxref[i - 1] = xx;
        ggref[i - 1] = gg;
        nbdref[i - 1] = 2;
        lcer[i] = lo;
        ucer[i] = hi;
        xcer[i] = xx;
        xxcer[i] = xx;
        ggcer[i] = gg;
        nbdcer[i] = 2;
    }
    for (int i = 1; i <= nsub; ++i)
    {
        double dd = 2.0 * rr(); // large ⇒ projection onto the box fires
        dref[i - 1] = dd;
        dcer[i] = dd;
    }
    long   nn = n, mm = m, nsubl = nsub, cc = col, hh = head, iwordref = 0, iprint = -1, inforef = 0;
    double th = theta;
    subsm(&nn, &mm, &nsubl, indref, lref, uref, nbdref, xref, dref, xpref, wsref, wyref, &th, xxref, ggref, &cc,
          &hh, &iwordref, wvref, wnref, &iprint, &inforef);
    int iwordcer = 0, infocer = 0;
    lb::subsm<double>(n, m, nsub, indcer, lcer, ucer, nbdcer, xcer, dcer, xpcer, wscer, wycer, theta, xxcer, ggcer,
                      col, head, iwordcer, wvcer, wncer, infocer);
    bool ok = (inforef == static_cast<long>(infocer)) && (iwordref == static_cast<long>(iwordcer)) &&
              cmp1d(xref, xcer, n) && cmp1d(dref, dcer, nsub) && cmp1d(wvref, wvcer, 2 * col);
    char w[80];
    std::snprintf(w, sizeof(w), "subsm n=%d m=%d nsub=%d iword=%d", n, m, nsub, iwordcer);
    check(ok, w);
}
// ============================ END-TO-END: minimize_lbfgsb vs reference setulb (bounded Rosenbrock) ============
double rosen_fg(const double* x, double* g, int n)
{
    for (int i = 0; i < n; ++i)
    {
        g[i] = 0.0;
    }
    double f = 0.0;
    for (int i = 0; i + 1 < n; ++i)
    {
        const double a = 1.0 - x[i];
        const double b = x[i + 1] - x[i] * x[i];
        f += a * a + 100.0 * b * b;
        g[i] += -2.0 * a - 400.0 * x[i] * b;
        g[i + 1] += 200.0 * b;
    }
    return f;
}

class RosenObj final : public crd::hesap::opt::Objective<double>
{
public:
    explicit RosenObj(int n) : crd::hesap::opt::Objective<double>(true, false), m_n(n) {}
    void set_scr(double* s) { m_scr = s; }
    [[nodiscard]] double value(crd::containers::ConstSpan<double> x) const override
    {
        return rosen_fg(x.data(), m_scr, m_n);
    }
    [[nodiscard]] crd::usize n() const noexcept override { return static_cast<crd::usize>(m_n); }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<double> x, crd::containers::Span<double> g) const override
    {
        (void)rosen_fg(x.data(), g.data(), m_n);
        return true;
    }

private:
    int     m_n;
    double* m_scr = nullptr;
};

void test_minimize_e2e()
{
    const int    n = 4;
    const double lo = -2.0, hi = 0.5; // unconstrained min (all ones) is INFEASIBLE ⇒ bounds bind at the solution
    // --- reference setulb (reverse-communication loop) ---
    long   nn = n, mm = 5;
    double xref[64], lref[64], uref[64], gref[64];
    long   nbdref[64];
    for (int i = 0; i < n; ++i)
    {
        xref[i] = (i % 2 == 0) ? -1.2 : 1.0;
        lref[i] = lo;
        uref[i] = hi;
        nbdref[i] = 2;
    }
    double fref = 0.0, factr = 1e7, pgtol = 1e-8;
    double wa[4096];
    long   iwa[256];
    long   task = 1 /*START*/, csave = 0, lsave[8] = {0}, isave[64] = {0}, iprint = -1;
    double dsave[64] = {0};
    int    guard = 0;
    while (guard++ < 200000)
    {
        setulb(&nn, &mm, xref, lref, uref, nbdref, &fref, gref, &factr, &pgtol, wa, iwa, &task, &iprint, &csave,
               lsave, isave, dsave);
        if (task >= 10 && task <= 15) // IS_FG
        {
            fref = rosen_fg(xref, gref, n);
        }
        else if (task == 2) // NEW_X
        {
            continue;
        }
        else
        {
            break; // CONV / WARN / ERROR
        }
    }
    // --- Cerid minimize_lbfgsb on the same problem ---
    crd::memory::TlsfAllocator alloc(1u << 22);
    double                     scr[64];
    RosenObj                   obj(n);
    obj.set_scr(scr);
    crd::containers::Array<double> x0(&alloc), low(&alloc), up(&alloc);
    x0.resize(static_cast<crd::usize>(n));
    low.resize(static_cast<crd::usize>(n));
    up.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        x0[static_cast<crd::usize>(i)] = (i % 2 == 0) ? -1.2 : 1.0;
        low[static_cast<crd::usize>(i)] = lo;
        up[static_cast<crd::usize>(i)] = hi;
    }
    crd::hesap::opt::OptOptions<double> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 1000;
    const auto un = static_cast<crd::usize>(n);
    auto       res = crd::hesap::opt::minimize_lbfgsb<double>(obj, {x0.data(), un}, {low.data(), un}, {up.data(), un},
                                                             opts, &alloc, 5, 1e7);
    bool ok = closed(fref, res.fx);
    for (int i = 0; i < n; ++i)
    {
        if (!closed(xref[i], res.x[static_cast<crd::usize>(i)]))
        {
            ok = false;
        }
    }
    bool pinned = false; // non-vacuous: the constrained minimizer pins ≥1 variable to a bound
    for (int i = 0; i < n; ++i)
    {
        const double xi = res.x[static_cast<crd::usize>(i)];
        if (std::fabs(xi - hi) < 1e-6 || std::fabs(xi - lo) < 1e-6)
        {
            pinned = true;
        }
    }
    char w[112];
    std::snprintf(w, sizeof(w), "minimize_lbfgsb vs setulb (bounded Rosenbrock): f_cerid=%.8e f_ref=%.8e pinned=%d",
                  res.fx, fref, pinned ? 1 : 0);
    check(ok && pinned, w);
}
} // namespace

int main()
{
    std::printf("# L-BFGS-B differential test — Cerid port vs reference C "
                "(foundation + cauchy/formk/subsm)\n");
    test_dpofa();
    test_dtrsl();
    test_projgr();
    test_active();
    test_hpsolb();
    test_bmv();
    test_formt();
    test_matupd();
    test_cmprlb();
    test_freev();
    test_cauchy();
    test_formk();
    test_subsm();
    test_minimize_e2e();
    std::printf("# RESULT: %d passed, %d FAILED\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
