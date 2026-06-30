#pragma once

// crd-hesap-quadrature v13-h — QNG: the NON-ADAPTIVE automatic integrator (QUADPACK dqng). A nested Patterson
// sequence 10→21→43→87 points where each level REUSES the previous level's function evaluations (via savfun),
// stopping at the first level that meets the tolerance. For SMOOTH integrands this beats the adaptive QAG/QAGS (no
// subdivision, no work-stack, no extrapolation bookkeeping — just successively higher-order rules on the whole
// interval). Allocation-free (savfun is a 21-element stack array). Faithful port; the error estimate's x^1.5 is the
// x·√x form (one hardware sqrt, not the heavy double-double pow).

#include <crd/hesap/quadrature/integrate.hpp>

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

#include <limits>

namespace crd::hesap::quadrature
{

// Non-adaptive integral of f over [a,b]. At most 87 f-evaluations; returns Ok if a level met max(epsabs, epsrel·|I|),
// else MaxSubdivisions (the 87-point estimate is the best available). f: callable T→T.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qng(F&& f, T a, T b, T epsabs, T epsrel)
{
    const T epmach = std::numeric_limits<T>::epsilon();
    const T uflow  = std::numeric_limits<T>::min();

    static constexpr T x1[5]   = {static_cast<T>(0.973906528517171720077964012084452),
                                  static_cast<T>(0.865063366688984510732096688423493),
                                  static_cast<T>(0.679409568299024406234327365114874),
                                  static_cast<T>(0.433395394129247190799265943165784),
                                  static_cast<T>(0.148874338981631210884826001129720)};
    static constexpr T w10[5]  = {static_cast<T>(0.066671344308688137593568809893332),
                                  static_cast<T>(0.149451349150580593145776339657697),
                                  static_cast<T>(0.219086362515982043995534934228163),
                                  static_cast<T>(0.269266719309996355091226921569469),
                                  static_cast<T>(0.295524224714752870173892994651338)};
    static constexpr T x2[5]   = {static_cast<T>(0.995657163025808080735527280689003),
                                  static_cast<T>(0.930157491355708226001207180059508),
                                  static_cast<T>(0.780817726586416897063717578345042),
                                  static_cast<T>(0.562757134668604683339000099272694),
                                  static_cast<T>(0.294392862701460198131126603103866)};
    static constexpr T w21a[5] = {static_cast<T>(0.032558162307964727478818972459390),
                                  static_cast<T>(0.075039674810919952767043140916190),
                                  static_cast<T>(0.109387158802297641899210590325805),
                                  static_cast<T>(0.134709217311473325928054001771707),
                                  static_cast<T>(0.147739104901338491374841515972068)};
    static constexpr T w21b[6] = {static_cast<T>(0.011694638867371874278064396062192),
                                  static_cast<T>(0.054755896574351996031381300244580),
                                  static_cast<T>(0.093125454583697605535065465083366),
                                  static_cast<T>(0.123491976262065851077958109831074),
                                  static_cast<T>(0.142775938577060080797094273138717),
                                  static_cast<T>(0.149445554002916905664936468389821)};
    static constexpr T x3[11]  = {static_cast<T>(0.999333360901932081394099323919911),
                                  static_cast<T>(0.987433402908088869795961478381209),
                                  static_cast<T>(0.954807934814266299257919200290473),
                                  static_cast<T>(0.900148695748328293625099494069092),
                                  static_cast<T>(0.825198314983114150847066732588520),
                                  static_cast<T>(0.732148388989304982612354848755461),
                                  static_cast<T>(0.622847970537725238641159120344323),
                                  static_cast<T>(0.499479574071056499952214885499755),
                                  static_cast<T>(0.364901661346580768043989548502644),
                                  static_cast<T>(0.222254919776601296498260928066212),
                                  static_cast<T>(0.074650617461383322043914435796506)};
    static constexpr T w43a[10] = {static_cast<T>(0.016296734289666564924281974617663),
                                   static_cast<T>(0.037522876120869501461613795898115),
                                   static_cast<T>(0.054694902058255442147212685465005),
                                   static_cast<T>(0.067355414609478086075553166302174),
                                   static_cast<T>(0.073870199632393953432140695251367),
                                   static_cast<T>(0.005768556059769796184184327908655),
                                   static_cast<T>(0.027371890593248842081276069289151),
                                   static_cast<T>(0.046560826910428830743339154433824),
                                   static_cast<T>(0.061744995201442564496240336030883),
                                   static_cast<T>(0.071387267268693397768559114425516)};
    static constexpr T w43b[12] = {static_cast<T>(0.001844477640212414100389106552965),
                                   static_cast<T>(0.010798689585891651740465406741293),
                                   static_cast<T>(0.021895363867795428102523123075149),
                                   static_cast<T>(0.032597463975345689443882222526137),
                                   static_cast<T>(0.042163137935191811847627924327955),
                                   static_cast<T>(0.050741939600184577780189020092084),
                                   static_cast<T>(0.058379395542619248375475369330206),
                                   static_cast<T>(0.064746404951445885544689259517511),
                                   static_cast<T>(0.069566197912356484528633315038405),
                                   static_cast<T>(0.072824441471833208150939535192842),
                                   static_cast<T>(0.074507751014175118273571813842889),
                                   static_cast<T>(0.074722147517403005594425168280423)};
    static constexpr T x4[22]   = {static_cast<T>(0.999902977262729234490529830591582),
                                   static_cast<T>(0.997989895986678745427496322365960),
                                   static_cast<T>(0.992175497860687222808523352251425),
                                   static_cast<T>(0.981358163572712773571916941623894),
                                   static_cast<T>(0.965057623858384619128284110607926),
                                   static_cast<T>(0.943167613133670596816416634507426),
                                   static_cast<T>(0.915806414685507209591826430720050),
                                   static_cast<T>(0.883221657771316501372117548744163),
                                   static_cast<T>(0.845710748462415666605902011504855),
                                   static_cast<T>(0.803557658035230982788739474980964),
                                   static_cast<T>(0.757005730685495558328942793432020),
                                   static_cast<T>(0.706273209787321819824094274740840),
                                   static_cast<T>(0.651589466501177922534422205016736),
                                   static_cast<T>(0.593223374057961088875273770349144),
                                   static_cast<T>(0.531493605970831932285268948562671),
                                   static_cast<T>(0.466763623042022844871966781659270),
                                   static_cast<T>(0.399424847859218804732101665817923),
                                   static_cast<T>(0.329874877106188288265053371824597),
                                   static_cast<T>(0.258503559202161551802280975429025),
                                   static_cast<T>(0.185695396568346652015917141167606),
                                   static_cast<T>(0.111842213179907468172398359241362),
                                   static_cast<T>(0.037352123394619870814998165437704)};
    static constexpr T w87a[21] = {static_cast<T>(0.008148377384149172900002878448190),
                                   static_cast<T>(0.018761438201562822243935059003794),
                                   static_cast<T>(0.027347451050052286161582829741283),
                                   static_cast<T>(0.033677707311637930046581056957588),
                                   static_cast<T>(0.036935099820427907614589586742499),
                                   static_cast<T>(0.002884872430211530501334156248695),
                                   static_cast<T>(0.013685946022712701888950035273128),
                                   static_cast<T>(0.023280413502888311123409291030404),
                                   static_cast<T>(0.030872497611713358675466394126442),
                                   static_cast<T>(0.035693633639418770719351355457044),
                                   static_cast<T>(0.000915283345202241360843392549948),
                                   static_cast<T>(0.005399280219300471367738743391053),
                                   static_cast<T>(0.010947679601118931134327826856808),
                                   static_cast<T>(0.016298731696787335262665703223280),
                                   static_cast<T>(0.021081568889203835112433060188190),
                                   static_cast<T>(0.025370969769253827243467999831710),
                                   static_cast<T>(0.029189697756475752501446154084920),
                                   static_cast<T>(0.032373202467202789685788194889595),
                                   static_cast<T>(0.034783098950365142750781997949596),
                                   static_cast<T>(0.036412220731351787562801163687577),
                                   static_cast<T>(0.037253875503047708539592001191226)};
    static constexpr T w87b[23] = {static_cast<T>(0.000274145563762072350016527092881),
                                   static_cast<T>(0.001807124155057942948341311753254),
                                   static_cast<T>(0.004096869282759164864458070683480),
                                   static_cast<T>(0.006758290051847378699816577897424),
                                   static_cast<T>(0.009549957672201646536053581325377),
                                   static_cast<T>(0.012329447652244853694626639963780),
                                   static_cast<T>(0.015010447346388952376697286041943),
                                   static_cast<T>(0.017548967986243191099665352925900),
                                   static_cast<T>(0.019938037786440888202278192730714),
                                   static_cast<T>(0.022194935961012286796332102959499),
                                   static_cast<T>(0.024339147126000805470360647041454),
                                   static_cast<T>(0.026374505414839207241503786552615),
                                   static_cast<T>(0.028286910788771200659968002987960),
                                   static_cast<T>(0.030052581128092695322521110347341),
                                   static_cast<T>(0.031646751371439929404586051078883),
                                   static_cast<T>(0.033050413419978503290785944862689),
                                   static_cast<T>(0.034255099704226061787082821046821),
                                   static_cast<T>(0.035262412660156681033782717998428),
                                   static_cast<T>(0.036076989622888701185500318003895),
                                   static_cast<T>(0.036698604498456094498018047441094),
                                   static_cast<T>(0.037120549269832576114119958413599),
                                   static_cast<T>(0.037334228751935040321235449094698),
                                   static_cast<T>(0.037361073762679023410321241766599)};

    if ((epsabs <= T{0}) && (epsrel < detail::qmax<T>(static_cast<T>(50) * epmach, static_cast<T>(0.5e-28))))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }

    const T hlgth  = static_cast<T>(0.5) * (b - a);
    const T dhlgth = crd::math::fabs(hlgth);
    const T centr  = static_cast<T>(0.5) * (b + a);
    const T fcentr = f(centr);
    T       fv1[5], fv2[5], fv3[5], fv4[5], savfun[21];
    T       result = T{0};
    T       abserr = T{0};
    T       resabs = T{0};
    T       resasc = T{0};
    crd::u32 neval = 21;
    int      ier   = 1;

    // refine a raw error |Δresult| with QUADPACK's resasc scaling (x^1.5 = x·√x) + the roundoff floor.
    auto refine_err = [&](T raw) -> T
    {
        T ae = raw;
        if (resasc != T{0} && ae != T{0})
        {
            const T rat = static_cast<T>(200) * ae / resasc;
            ae          = resasc * detail::qmin<T>(T{1}, rat * crd::math::sqrt(rat));
        }
        if (resabs > uflow / (static_cast<T>(50) * epmach))
        {
            ae = detail::qmax<T>(epmach * static_cast<T>(50) * resabs, ae);
        }
        return ae;
    };

    // --- Level 1: the 10- and 21-point formulae (15 evals incl. the centre) ---
    T   res10 = T{0};
    T   res21 = w21b[5] * fcentr;
    resabs    = w21b[5] * crd::math::fabs(fcentr);
    for (int k = 0; k < 5; ++k)
    {
        const T absc = hlgth * x1[k];
        const T f1   = f(centr + absc);
        const T f2   = f(centr - absc);
        const T fval = f1 + f2;
        res10 += w10[k] * fval;
        res21 += w21a[k] * fval;
        resabs += w21a[k] * (crd::math::fabs(f1) + crd::math::fabs(f2));
        savfun[k] = fval;
        fv1[k]    = f1;
        fv2[k]    = f2;
    }
    int ipx = 4;
    for (int k = 0; k < 5; ++k)
    {
        ++ipx;
        const T absc = hlgth * x2[k];
        const T f1   = f(centr + absc);
        const T f2   = f(centr - absc);
        const T fval = f1 + f2;
        res21 += w21b[k] * fval;
        resabs += w21b[k] * (crd::math::fabs(f1) + crd::math::fabs(f2));
        savfun[ipx] = fval;
        fv3[k]      = f1;
        fv4[k]      = f2;
    }
    result        = res21 * hlgth;
    resabs        = resabs * dhlgth;
    const T reskh = static_cast<T>(0.5) * res21;
    resasc        = w21b[5] * crd::math::fabs(fcentr - reskh);
    for (int k = 0; k < 5; ++k)
    {
        resasc += w21a[k] * (crd::math::fabs(fv1[k] - reskh) + crd::math::fabs(fv2[k] - reskh))
                  + w21b[k] * (crd::math::fabs(fv3[k] - reskh) + crd::math::fabs(fv4[k] - reskh));
    }
    resasc = resasc * dhlgth;
    abserr = refine_err(crd::math::fabs((res21 - res10) * hlgth));
    if (abserr <= detail::qmax<T>(epsabs, epsrel * crd::math::fabs(result)))
    {
        ier = 0;
    }
    else
    {
        // --- Level 2: the 43-point formula (reuses savfun[0..9], 11 new evals) ---
        neval     = 43;
        T res43   = w43b[11] * fcentr;
        for (int k = 0; k < 10; ++k)
        {
            res43 += savfun[k] * w43a[k];
        }
        for (int k = 0; k < 11; ++k)
        {
            ++ipx;
            const T absc = hlgth * x3[k];
            const T f1   = f(centr + absc);
            const T f2   = f(centr - absc);
            const T fval = f1 + f2;
            res43 += fval * w43b[k];
            savfun[ipx] = fval;
        }
        result = res43 * hlgth;
        abserr = refine_err(crd::math::fabs((res43 - res21) * hlgth));
        if (abserr <= detail::qmax<T>(epsabs, epsrel * crd::math::fabs(result)))
        {
            ier = 0;
        }
        else
        {
            // --- Level 3: the 87-point formula (reuses savfun[0..20], 22 new evals) ---
            neval   = 87;
            T res87 = w87b[22] * fcentr;
            for (int k = 0; k < 21; ++k)
            {
                res87 += savfun[k] * w87a[k];
            }
            for (int k = 0; k < 22; ++k)
            {
                const T absc = hlgth * x4[k];
                const T f1   = f(centr + absc);
                const T f2   = f(centr - absc);
                res87 += w87b[k] * (f1 + f2);
            }
            result = res87 * hlgth;
            abserr = refine_err(crd::math::fabs((res87 - res43) * hlgth));
            if (abserr <= detail::qmax<T>(epsabs, epsrel * crd::math::fabs(result)))
            {
                ier = 0;
            }
        }
    }

    QuadResult<T> r;
    r.value          = result;
    r.error_estimate = abserr;
    r.eval_count     = neval;
    r.tolerance_met  = (ier == 0);
    r.status         = (ier == 0) ? QuadStatus::Ok : QuadStatus::MaxSubdivisions;
    return r;
}

} // namespace crd::hesap::quadrature
