// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#pragma once
#include <array>
#include <cstddef>
#include <functional>
#include <mspice/sim/dual.hpp>
#include <mspice/sim/linear_solver.hpp>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mspice::sim {
/** @brief A voltage source that can either be DC or a periodic ideal pulse. */
/** @brief Timestamped trace vertex; duplicate timestamps encode instantaneous jumps. */
struct WavePoint {
    double time{}, volts{};
};
/** @brief Source waveform (voltage in volts, or current in amperes for current sources):
 *  DC, pulse (optionally with a pulse count), sine or piecewise-linear trace. */
struct Waveform {
    double low{}, high{}, delay{}, rise{}, fall{}, on{}, period{};
    bool pulse{false};
    std::vector<WavePoint> trace{};
    /** PULSE pulse count NP (ngspice): 0 repeats forever (default); n > 0 stops after n periods
     *  and the source stays at low afterwards. Only used when pulse is set. */
    int cycles{0};
    /** SIN source (ngspice SIN(vo va freq td theta phase)): offset = low, amplitude = high,
     *  delay = delay. Value vo for t < td, else vo + va*sin(2 pi (freq (t-td) + phase/360))
     *  * exp(-(t-td) damping). Mutually exclusive with pulse and trace. */
    bool sine{false};
    double freq{}, damping{}, phase{}; ///< Hz, 1/s, degrees.
    /** @brief Evaluate the waveform at physical time in seconds. */
    double at(double t) const;
    /** @brief Find a voltage transition after t for breakpoint placement. */
    double next_event(double t) const;
};
/** @brief Numerical integration scheme. */
enum class Method { BackwardEuler, Trapezoid, BDF2 };
/** @brief Local-truncation-error estimate of the adaptive step control. */
enum class LteMethod {
    StepDoubling, ///< One full and two half steps per step (three solves), historical default.
    Predictor     ///< Quadratic predictor vs. trapezoid corrector (one solve per step).
};
/** @brief Transient solver settings. */
struct Settings {
    double stop{0.01}, max_step{1e-5}, abs_tol{1e-8}, rel_tol{1e-4};
    bool adaptive{false};
    bool reuse_matrix{false}; // Fast mode: cache LU for exactly identical MNA matrices
    /** Simplified Newton: later iterations of a step reuse the LU factors of an earlier
     *  iteration and solve for the correction, x = guess + LU_old^-1 (rhs - A guess). The
     *  current Jacobian is factored again when the update does not at least halve per
     *  iteration or after four reuses; a reused iteration that does not halve is discarded,
     *  and one is accepted as converged only below a tenth of the Newton tolerance.
     *  Off by default: every iteration factors its own Jacobian (results bit for bit). */
    bool reuse_jacobian{false};
    bool prefer_sparse{true}; // Optional SuiteSparse-KLU when compiled and n >= 8
    /** Linear-solver backend. Auto keeps the historical choice bit for bit: KLU when compiled,
     *  n >= 8 and prefer_sparse, else dense LU. Plan uses the fixed-structure SparsePlan
     *  (see sparse_plan.hpp); Dense and Klu force that backend (Klu falls back to dense without KLU). */
    LinearBackend linear_backend{LinearBackend::Auto};
    Method method{Method::Trapezoid};
    /** Solve a DC operating point at t = 0 before the transient (capacitors open, inductors
     *  short, sources at their t = 0 value), like ngspice without "uic". Off by default: the
     *  transient then starts from 0 V as before. */
    bool operating_point{false};
    /** Truncation-error tolerance factor of the adaptive step control, as ngspice TRTOL: the
     *  estimated local error may be trtol times (abs_tol + rel_tol * |x|). 1 keeps the strict
     *  historical behaviour; ngspice uses 7. */
    double trtol{1};
    /** Error estimate of the adaptive step control. Predictor solves each step once and
     *  compares the trapezoid result with a quadratic extrapolation of the last three points
     *  (as ngspice); it falls back to step doubling without three smooth trapezoid points
     *  (start, after breakpoints, events and BE restarts) and for methods other than
     *  Trapezoid. StepDoubling keeps the historical results bit for bit. */
    LteMethod lte_method{LteMethod::StepDoubling};
    /** Newton iteration limit per transient step attempt (SPICE ITL4 = 10). A failed attempt
     *  shrinks the step by newton_cut and tries again; the DC operating point always allows
     *  200 iterations. 200 keeps the historical behaviour (in profiling, failed attempts at
     *  200 used half of all iterations). */
    int newton_max{200};
    /** Step reduction factor after a failed Newton attempt (historical 2, ngspice 8). */
    double newton_cut{2};
    /** Start Newton from a linear extrapolation of the last two time points instead of the
     *  previous point, as ngspice does with device voltages (MODEINITPRED). Off keeps the
     *  historical results bit for bit. */
    bool predict_guess{false};
    /** SPICE CHGTOL in coulomb: the local-error norm of a capacitor node is at least chgtol
     *  divided by the total capacitance at that node, so tiny internal capacitors (1 pF
     *  behavioural states) do not force picosecond steps. LTspice and ngspice use 1e-14 and
     *  react to it the same way. 0 keeps the voltage-only norm. */
    double lte_chgtol{0};
    /** lte_chgtol only applies to nodes whose total capacitance is at most this value (farad),
     *  e.g. 2e-12 for the internal 1 pF states of behavioural logic models; infinite applies it
     *  to every capacitor node as SPICE does. */
    double lte_chgtol_cmax{std::numeric_limits<double>::infinity()};
    /** Diagnostics: when not empty, one CSV line per Newton attempt (t, h, trapezoid flag,
     *  iterations, converged) is written to this file, and the unknowns that most often
     *  limited the predictor error estimate are printed to stderr at the end of the run. */
    std::string steplog{};
};
/** @brief Diode model parameters of Circuit::diode(a, b, DiodeParams). */
struct DiodeParams {
    double is{1e-12};   ///< Saturation current IS in amperes.
    double vt{0.02585}; ///< Emission-scaled thermal voltage N*Vt in volts.
    double bv{0};       ///< Reverse breakdown voltage BV in volts (positive); 0 disables it.
    double ibv{1e-3};   ///< Current at breakdown IBV in amperes (ngspice default 1 mA).
    double nbv{1};      ///< Breakdown emission coefficient NBV.
    /** Zero-bias junction capacitance CJO in farads; 0 (default) stores no depletion charge.
     *  The depletion charge follows SPICE: vj*cjo*(1-(1-v/vj)^(1-m))/(1-m) below fc*vj, the
     *  linearised capacitance cjo/(1-fc)^(1+m)*(1-fc*(1+m)+m*v/vj) above. */
    double cjo{0};
    double m{0.5};  ///< Grading coefficient M (0 <= m < 1).
    double vj{1};   ///< Junction potential VJ in volts.
    double fc{0.5}; ///< Forward-bias depletion coefficient FC (0 <= fc < 1).
    /** Transit time TT in seconds; 0 (default) stores no diffusion charge, else the junction
     *  stores tt * Id (Id = diode current including breakdown, without GMIN). */
    double tt{0};
};
/** @brief Gummel-Poon subset of Circuit::npn/pnp(c, b, e, BjtParams): Ebers-Moll transport
 *  currents plus junction (CJE/CJC) and diffusion (TF/TR) charges. Capacitance and transit
 *  time defaults 0 give exactly the results of the scalar npn()/pnp() overloads. */
struct BjtParams {
    double is{1e-15};   ///< Transport saturation current IS in amperes.
    double bf{100};     ///< Forward current gain BF.
    double br{1};       ///< Reverse current gain BR.
    double vt{0.02585}; ///< Emission-scaled thermal voltage NF*Vt (also used for NR) in volts.
    double cje{0};      ///< Base-emitter zero-bias depletion capacitance CJE in farads.
    double vje{0.75};   ///< Base-emitter built-in potential VJE in volts.
    double mje{0.33};   ///< Base-emitter grading coefficient MJE.
    double cjc{0};      ///< Base-collector zero-bias depletion capacitance CJC in farads (XCJC=1).
    double vjc{0.75};   ///< Base-collector built-in potential VJC in volts.
    double mjc{0.33};   ///< Base-collector grading coefficient MJC.
    double fc{0.5};     ///< Forward-bias depletion coefficient FC of both junctions.
    double tf{0};       ///< Forward transit time TF in seconds (charge tf * Icc, XTF = 0).
    double tr{0};       ///< Reverse transit time TR in seconds (charge tr * Iec).
};
/** @brief Time, named node potentials and probed device currents returned by the engine. */
struct Sample {
    double time{};
    std::vector<double> volts;
    std::vector<double> amps{}; ///< One entry per Result::currents name, in amperes.
};
/** @brief Fitted LED-strand curve I(U,T) = k*(s*ln(1+exp((U-U0+tc*(T-20))/s)))^p.
 *
 *  U in volts, I in milliamperes (as in a fit to measured data); the circuit converts to
 *  amperes. Parameters come from a fit of the user's own measurements.
 */
struct LedCurve {
    double k{}, u0{}, s{}, p{1};
    double temp_c{20};        ///< Strand temperature in degrees Celsius.
    double tc_v_per_k{0.012}; ///< Left shift of the curve per kelvin above 20 degC.
    /** @brief Strand current in amperes at strand voltage u (anode minus cathode). */
    double amps(double u) const;
    /** @brief Analytic derivative dI/dU in siemens. */
    double siemens(double u) const;
};
/** @brief Behavioural low-side gate-driver channel (non-inverting). Defaults are typical
 *  values of a small CMOS low-side driver. */
struct GateDriver {
    double r_high{7};        ///< Output resistance to VDD while high, ohms.
    double r_low{7};         ///< Output resistance to GND while low, ohms.
    double threshold{1.6};   ///< Input switching threshold relative to the driver GND pin, volts.
    double hysteresis{0};    ///< Half width of the input hysteresis band, volts.
    double delay_on{20e-9};  ///< Input rising crossing to output switching high, seconds.
    double delay_off{40e-9}; ///< Input falling crossing to output switching low, seconds.
};
/** @brief Smooth behavioural expression of control-net potentials (volts) for B sources. */
using Behaviour = std::function<double(const std::vector<double> &)>;
/** @brief Behavioural expression with exact gradient: returns f(v) for the n control
 *  values v[0..n-1] and writes df/dv_k into grad[0..n-1]; n is fixed at registration. */
using BehaviourGrad = std::function<double(const double *v, double *grad)>;
/** @brief Largest control count that gets an exact dual-number gradient; sources with more
 *  controls keep the numeric finite differences. */
inline constexpr int max_dual_controls = 8;
/** @brief Extended-precision evaluation f(v[0..n-1]) of a generic expression; only used as
 *  reference for gradient checks (behavioural_sources()), never by the solver. */
using BehaviourLong = std::function<long double(const long double *v)>;

/** @brief Gradient function of a generic expression for exactly K controls.
 *
 *  Seeds K dual variables on the stack (no heap traffic per call), evaluates f once and
 *  copies the K partial derivatives into grad. f receives a pointer to the K variables, so
 *  it indexes v[0..K-1] exactly as the std::vector<double> it gets for plain values.
 *  @tparam K number of controls (1..max_dual_controls)
 *  @param f generic callable `f(const auto &v)`
 *  @return gradient function for BehaviourGrad
 */
template <int K, class F> BehaviourGrad dual_gradient(F f) {
    return [f](const double *x, double *grad) -> double {
        const std::array<Dual<K>, K> vars = make_variables<K>(x);
        const Dual<K> r = f(vars.data());
        for (int i = 0; i < K; ++i)
            grad[i] = r.d[i];
        return r.v;
    };
}

/** @brief Exact gradient of a generic expression with n controls via Dual<n>.
 *
 *  The runtime count n selects a fixed-size Dual<1..8> instantiation, so every call works on
 *  stack arrays only. Returns an empty function when n is outside 1..max_dual_controls; the
 *  stamping then falls back to finite differences.
 *  @param f generic callable `f(const auto &v)` written with unqualified math functions
 *  @param n number of controls the expression reads
 *  @return gradient function, empty when n is not supported
 */
template <class F> BehaviourGrad differentiate(F f, std::size_t n) {
    switch (n) {
    case 1:
        return dual_gradient<1>(std::move(f));
    case 2:
        return dual_gradient<2>(std::move(f));
    case 3:
        return dual_gradient<3>(std::move(f));
    case 4:
        return dual_gradient<4>(std::move(f));
    case 5:
        return dual_gradient<5>(std::move(f));
    case 6:
        return dual_gradient<6>(std::move(f));
    case 7:
        return dual_gradient<7>(std::move(f));
    case 8:
        return dual_gradient<8>(std::move(f));
    default:
        return {};
    }
}

/** @brief True for generic expressions that can be evaluated both on dual numbers and on
 *  `std::vector<double>` (`f(const auto &v)`); plain vector lambdas and Behaviour are not. */
template <class F>
inline constexpr bool is_dual_behaviour_v =
    std::is_invocable_v<const F &, const Dual<1> *> &&
    std::is_invocable_v<const F &, const long double *> &&
    std::is_invocable_r_v<double, const F &, const std::vector<double> &>;
/** @brief Run information and numerical diagnostics. */
struct Result {
    std::vector<std::string> nodes;
    std::vector<std::string> currents{}; ///< Names of probed device currents (I(<name>)).
    std::vector<Sample> samples;
    unsigned switch_events{}; ///< Accepted state changes of switches and gate drivers.
    unsigned accepted{}, rejected{}, newton_iterations{}, matrix_reuses{};
    /** Post-switch right-hand-limit steps skipped because Newton failed at the tiny step. */
    unsigned post_switch_skipped{};
    unsigned sparse_symbolic_analyses{}, sparse_fallbacks{};
    /** SparsePlan builds (incl. pivot rebuilds) and matrices handed to dense LU instead. */
    unsigned plan_builds{}, plan_fallbacks{};
    std::string backend{"dense-LU"};
    std::string warning;
    bool operating_point{}; ///< True when Settings::operating_point found a DC solution.
};
/** @brief Independently testable MNA solver (optional KLU or dense LU). */
class Circuit {
  public:
    /** @brief Return an existing or newly registered node, with 0/GND mapped to ground. */
    int node(const std::string &name);
    /** @brief Stamp a resistor with Ohms. */
    void resistor(const std::string &a, const std::string &b, double ohms);
    /** @brief Stamp a capacitor with Farads. */
    void capacitor(const std::string &a, const std::string &b, double farads);
    /** @brief Stamp an inductor with Henries; a non-empty probe reports its current as
     *  I(<probe>) in every Sample (positive from a through the inductor to b, as ngspice). */
    void inductor(const std::string &a, const std::string &b, double henries,
                  const std::string &probe = "");
    /** @brief Stamp a voltage source (DC, pulse with optional count, sine or PWL); a
     *  non-empty probe reports its branch current as I(<probe>) in every Sample (positive from
     *  a through the source to b, as ngspice i(Vx)). */
    void voltage(const std::string &a, const std::string &b, Waveform wave,
                 const std::string &probe = "");
    /** @brief Report the branch current of an existing voltage source, behavioural voltage
     *  source or ammeter as I(<probe>) in every Sample.
     *  @param control "#I<n>" as returned by ammeter() or used as behavioural control
     *  @param probe   non-empty output name */
    void probe_current(const std::string &control, const std::string &probe);
    /** @brief Set the initial potential of a net for a transient start with uic (ngspice
     *  ".ic V(net)=v" plus ".tran ... uic"). Nets without a value start at 0 V; capacitors
     *  start charged to the difference of their nets. The transient then starts from this state
     *  (with Settings::operating_point it only seeds the operating-point search). Ground and
     *  non-finite values throw std::invalid_argument; a later call for the same net replaces
     *  the value. */
    void initial_voltage(const std::string &net, double volts);
    /** @brief Connect a finite-impedance event-trace driver to an analog net. */
    void trace_driver(const std::string &net, const std::string &driver_node,
                      const std::vector<WavePoint> &trace, double output_ohms);
    /** @brief Stamp an ideal independent current source, a to b. */
    void current(const std::string &a, const std::string &b, double amps);
    /** @brief Stamp an independent current source a to b with a time-dependent waveform
     *  (PWL, PULSE with optional count, SIN; the Waveform values are amperes). */
    void current(const std::string &a, const std::string &b, Waveform wave);
    /** @brief Stamp a simple exponential silicon diode model. */
    void diode(const std::string &a, const std::string &b, double is = 1e-12, double vt = 0.02585);
    /** @brief Stamp a diode with optional reverse breakdown (ngspice BV, IBV, NBV).
     *
     *  Without bv this is identical to the overload above. With bv > 0 the reverse current is
     *  -is*exp(-(v+bv_eff)/(nbv*Vt)) below -bv_eff, where bv_eff is chosen like ngspice dioTemp
     *  so that the current at -bv equals ibv (thermal Vt = 25.85 mV); an ibv below
     *  is*bv/Vt keeps bv_eff = bv. Throws std::invalid_argument for non-positive or non-finite
     *  parameters.
     *
     *  With cjo > 0 or tt > 0 the junction also stores the charge q(v) = depletion + tt*Id
     *  (SPICE dioload), integrated charge-conserving with the circuit's method (see
     *  junction charges in run()). Its nets then enter the local-error estimate like capacitor
     *  nets, with cjo as their capacitance for Settings::lte_chgtol.
     */
    void diode(const std::string &a, const std::string &b, const DiodeParams &params);
    /** @brief Stamp approximate Shichman-Hodges enhancement N-channel MOSFET. */
    void nmos(const std::string &drain, const std::string &gate, const std::string &source,
              double k, double threshold, double lambda = 0.0);
    /** @brief Stamp an NPN transistor (Ebers-Moll transport model, all four regions).
     *
     *  Icc = is*(exp(vbe/Vt)-1), Iec = is*(exp(vbc/Vt)-1), Ic = Icc - Iec*(1+1/beta_r),
     *  Ib = Icc/beta + Iec/beta_r, Vt = 25.85 mV. Equals ngspice Gummel-Poon with only IS, BF
     *  and BR set (no Early effect, no high injection, no capacitances). vt replaces Vt
     *  (emission-scaled thermal voltage, e.g. NF*Vt); the default keeps the results.
     */
    void npn(const std::string &collector, const std::string &base, const std::string &emitter,
             double is = 1e-15, double beta = 100.0, double beta_r = 1.0, double vt = 0.02585);
    /** @brief Stamp a PNP transistor, the voltage/current mirror of npn(). */
    void pnp(const std::string &collector, const std::string &base, const std::string &emitter,
             double is = 1e-15, double beta = 100.0, double beta_r = 1.0, double vt = 0.02585);
    /** @brief Stamp an NPN transistor with junction and diffusion charges (BjtParams).
     *
     *  Currents as the scalar overload. Base-emitter charge depletion(vbe; cje, vje, mje, fc)
     *  + tf*Icc, base-collector charge depletion(vbc; cjc, vjc, mjc, fc) + tr*Iec, both
     *  between the external base and emitter/collector (no RB, XCJC = 1, as ngspice with these
     *  parameters only). Throws std::invalid_argument for invalid parameters.
     */
    void npn(const std::string &collector, const std::string &base, const std::string &emitter,
             const BjtParams &params);
    /** @brief Stamp a PNP transistor with junction and diffusion charges, the mirror of the
     *  BjtParams npn() overload. */
    void pnp(const std::string &collector, const std::string &base, const std::string &emitter,
             const BjtParams &params);
    /** @brief Stamp approximate Shichman-Hodges enhancement P-channel MOSFET.
     *
     *  Mirror of nmos() with the same parameter names; threshold follows the SPICE VTO
     *  sign convention (negative for enhancement PMOS), body tied to source.
     */
    void pmos(const std::string &drain, const std::string &gate, const std::string &source,
              double k, double threshold, double lambda = 0.0);
    /** @brief Stamp a voltage-controlled switch between a and b.
     *
     *  Closed (ron) once v(ctrl_p)-v(ctrl_n) > threshold + hysteresis, open (roff) once it
     *  falls below threshold - hysteresis, otherwise the previous state is kept (ngspice SW
     *  semantics). The state is evaluated after every accepted step; initially open unless
     *  initially_on.
     */
    void voltage_switch(const std::string &a, const std::string &b, const std::string &ctrl_p,
                        const std::string &ctrl_n, double ron, double roff, double threshold,
                        double hysteresis = 0.0, bool initially_on = false);
    /** @brief Stamp one behavioural gate-driver channel (input, output, VDD, GND pins). */
    void gate_driver(const std::string &input, const std::string &output, const std::string &vdd,
                     const std::string &gnd, const GateDriver &model = GateDriver{});
    /** @brief Stamp a fitted LED-strand curve from anode to cathode; its current is probed
     *  as I(<probe>) in every Sample when probe is not empty. */
    void led_strand(const std::string &anode, const std::string &cathode, const LedCurve &curve,
                    const std::string &probe = "");
    /** @brief Control name of the simulation time for behavioural sources (ngspice "time"). */
    static constexpr const char *time_control = "#T";
    /** @brief Insert a 0 V ammeter from a to b (ngspice "V a b DC 0") and return the control
     *  name of its current (flowing a to b through the meter) for behavioural sources. */
    std::string ammeter(const std::string &a, const std::string &b);
    /** @brief Stamp a behavioural current source (ngspice "B a b I=f(...)").
     *
     *  The current f(v) flows from a through the source to b. v holds the control values in
     *  the given order: net potentials (ground = 0 V), currents of ammeter()/voltage-source
     *  controls ("#I<n>") or the time (time_control). f must be smooth; the Jacobian is taken
     *  numerically. When probe is not empty the current is reported as I(<probe>).
     */
    void behavioural_current(const std::string &a, const std::string &b,
                             const std::vector<std::string> &controls, Behaviour function,
                             const std::string &probe = "");
    /** @brief Behavioural current source with an exact gradient.
     *
     *  As behavioural_current() above; gradient(v, grad) must return f(v) and write the
     *  partial derivatives for all controls. The Jacobian is then taken from gradient instead
     *  of finite differences; function still serves the probe output. An empty gradient keeps
     *  the finite differences.
     */
    void behavioural_current(const std::string &a, const std::string &b,
                             const std::vector<std::string> &controls, Behaviour function,
                             BehaviourGrad gradient, const std::string &probe = "");
    /** @brief Behavioural current source from a generic expression `f(const auto &v)`.
     *
     *  The expression is used for values (on doubles) and for the exact Jacobian (on dual
     *  numbers, see differentiate()). Math functions inside must be called unqualified so
     *  the dual overloads are found.
     */
    template <class F, std::enable_if_t<is_dual_behaviour_v<F>, int> = 0>
    void behavioural_current(const std::string &a, const std::string &b,
                             const std::vector<std::string> &controls, F f,
                             const std::string &probe = "") {
        BehaviourGrad gradient = differentiate(f, controls.size());
        behavioural_current(a, b, controls,
                            Behaviour([f](const std::vector<double> &v) -> double { return f(v); }),
                            std::move(gradient), probe);
        parts_.back().fl = [f](const long double *v) -> long double { return f(v); };
    }
    /** @brief Stamp a behavioural voltage source (ngspice "B a b V=f(...)"): v(a)-v(b) = f(v).
     *
     *  v holds the potentials of the control nets in the given order. Adds one branch current
     *  unknown like an independent voltage source.
     */
    void behavioural_voltage(const std::string &a, const std::string &b,
                             const std::vector<std::string> &controls, Behaviour function);
    /** @brief Behavioural voltage source with an exact gradient; see the
     *  behavioural_current() overload with BehaviourGrad. An empty gradient keeps the finite
     *  differences. */
    void behavioural_voltage(const std::string &a, const std::string &b,
                             const std::vector<std::string> &controls, Behaviour function,
                             BehaviourGrad gradient);
    /** @brief Behavioural voltage source from a generic expression `f(const auto &v)` with
     *  exact Jacobian via dual numbers (see differentiate()). */
    template <class F, std::enable_if_t<is_dual_behaviour_v<F>, int> = 0>
    void behavioural_voltage(const std::string &a, const std::string &b,
                             const std::vector<std::string> &controls, F f) {
        BehaviourGrad gradient = differentiate(f, controls.size());
        behavioural_voltage(a, b, controls,
                            Behaviour([f](const std::vector<double> &v) -> double { return f(v); }),
                            std::move(gradient));
        parts_.back().fl = [f](const long double *v) -> long double { return f(v); };
    }
    /** @brief Expression of one behavioural source with its optional exact gradient. */
    struct BehaviouralSource {
        std::size_t controls{};    ///< Number of control values.
        Behaviour function{};      ///< Value f(v).
        BehaviourGrad gradient{};  ///< Exact gradient, empty for finite differences.
        BehaviourLong reference{}; ///< Long-double evaluation (generic expressions only).
    };
    /** @brief All behavioural sources in insertion order (for gradient checks and tools). */
    std::vector<BehaviouralSource> behavioural_sources() const;
    /** @brief Register a time breakpoint (ngspice breakpoint table).
     *
     *  Behavioural sources that jump in time (e.g. "u(time-4m)") are invisible to the step
     *  control: it only sees the smooth parts and interpolates across the jump. The transient
     *  lands exactly on every registered time and restarts with a short first-order step
     *  afterwards, as at source waveform vertices. Times <= 0 are ignored.
     */
    void breakpoint(double t);
    /** @brief Solve a transient; throws on singular matrices or non-convergence. */
    Result run(const Settings &settings) const;

    /** @brief Named port of a model instance: pin name and the net it connects to. */
    using Port = std::pair<std::string, std::string>;
    /** @brief RAII marker for the netlist export: while alive, every added device is
     *  tagged with the instance name and model of this scope.
     *
     *  Obtained from Circuit::instance(); not copyable or movable (C++17 guaranteed copy
     *  elision allows `Circuit::InstanceScope s = c.instance("U1", "regulator");`). Scopes may
     *  nest; the destructor restores the enclosing scope. Has no effect on the simulation.
     */
    class InstanceScope {
      public:
        /** @brief Activate instance index @p index of @p circuit. */
        InstanceScope(Circuit &circuit, int index);
        /** @brief Restore the instance that was active before this scope. */
        ~InstanceScope();
        InstanceScope(const InstanceScope &) = delete;
        InstanceScope &operator=(const InstanceScope &) = delete;
        InstanceScope(InstanceScope &&) = delete;
        InstanceScope &operator=(InstanceScope &&) = delete;

      private:
        Circuit &circuit_;
        int previous_; ///< Instance index active before this scope (-1: none).
    };
    /** @brief Open an instance scope for the netlist export (no effect on the simulation).
     *
     *  @param name  Instance name, e.g. "U1".
     *  @param model Model name, e.g. "regulator".
     *  @param ports Optional external pins (pin name, net name); an instance with ports is
     *               exported as one additional part of type "X" with these nodes.
     */
    InstanceScope instance(const std::string &name, const std::string &model,
                           std::vector<Port> ports = {});
    /** @brief Describe the netlist as JSON.
     *
     *  `{"nets":[names], "parts":[{"id","type","nodes":{pin:net},"value","params",
     *  "controls","instance","model"}]}`. Primitive ids are type plus device index (the index
     *  also used by "#I<n>" controls); each instance with ports adds a part of type "X" whose
     *  id is the instance name. Ground is reported as "0".
     */
    std::string describe_json() const;

  private:
    enum class Type { R, C, L, V, I, D, NMOS, NPN, PNP, PMOS, SW, DRV, LED, BI, BV };
    struct Part {
        Type type;
        int a{}, b{}, c{};
        double v{}, p{}, q{};
        Waveform wave{};
        int d{}, e{};              ///< Extra terminals (switch control-, driver GND).
        double r{}, s{}, t{}, u{}; ///< Extra parameters (switch/driver).
        bool initial{};            ///< Initial digital state for switches.
        LedCurve led{};
        std::string probe{};
        std::vector<int> terms{}; ///< Control nets of behavioural sources.
        Behaviour fn{};           ///< Behavioural expression f(v(terms)).
        BehaviourGrad fg{};       ///< Optional exact gradient of fn, else numeric.
        BehaviourLong fl{};       ///< Long-double evaluation for gradient checks.
        int instance{-1};         ///< Index into instances_ for the export (-1: top level).
        bool shaped{};            ///< Current source driven by wave instead of the constant v.
    };
    /** @brief Model instance recorded for the netlist export. */
    struct Instance {
        std::string name, model;
        std::vector<Port> ports;
    };
    /** @brief Nonlinear junction charge q(v) = depletion(v) + tt * I(v) between anode a and
     *  cathode b, with I(v) = is*(exp(v/vt)-1) and optional breakdown (knee, slope). */
    struct Junction {
        int a{}, b{};                         ///< Anode and cathode node.
        double cj0{}, vj{1}, m{0.5}, fc{0.5}; ///< Depletion charge (SPICE CJO, VJ, M, FC).
        double tt{};                          ///< Transit time of the diffusion charge.
        double is{}, vt{};                    ///< Junction current law of the diffusion charge.
        double knee{}, slope{};               ///< Breakdown (diode only; 0 = none).
        std::size_t part{};                   ///< Owning device in parts_ (for the export).
        const char *role{""};                 ///< Export prefix: "" (diode), "be", "bc".
    };
    std::vector<Junction> junctions_;
    /** @brief Validate and register the junction charges of a BJT added last to parts_. */
    void add_bjt_charges(const std::string &c, const std::string &b, const std::string &e,
                         const BjtParams &m, bool npn_type);
    std::vector<Instance> instances_;
    int current_instance_{-1}; ///< Instance tagged onto newly added devices (-1: none).
    std::vector<std::string> names_{"0"};
    std::map<std::string, int> mapping_{{"0", 0}, {"GND", 0}};
    std::vector<Part> parts_;
    std::vector<double> breakpoints_; ///< Sorted, unique registered breakpoint times.
    std::map<int, double> initial_;   ///< Initial node potentials (initial_voltage), by node index.
    /** @brief Validate and append a device. */
    void add(Part part);
    /** @brief Encode a behavioural control (net, "#I<n>" source current or time). */
    int control_term(const std::string &control);
};
} // namespace mspice::sim
