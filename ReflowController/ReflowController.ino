// ----------------------------------------------------------------------------
// Reflow Oven Controller
// (c) 2019      Patrick Knöbel
// (c) 2014 Karl Pitrich <karl@pitrich.com>
// (c) 2012-2013 Ed Simmons
// ----------------------------------------------------------------------------

// Firmware build identity.
//
// There is no other way to tell what is on the oven. The API exposed nothing
// version-shaped, and on 2026-09-04 that cost a diagnosis: the running profile
// could be dated from its step values, but the CODE could not be dated at all,
// because both that day's commits changed only control logic and left every
// JSON field identical.
//
// FW_VERSION is the deliberate part. __DATE__/__TIME__ are the honest part:
// they change on every rebuild whether or not the version was bumped, which is
// exactly the failure mode a hand-maintained number has. Compare the build
// stamp, not the version, when the question is "is what I just built actually
// on the oven".
//
// Semver, with the major deliberately expensive:
//
//   PATCH  a fix. Nothing a caller could not already do, nothing it did
//          before that stops working. Both fault-message corrections were
//          this.
//   MINOR  new capability, backward compatible. A new route, a new state, a
//          new field in a JSON response. Hold was this.
//   MAJOR  reserved, and it should stay rare enough to be an event. Breaking
//          an existing caller -- a route removed or its arguments changed, a
//          /status or /config field renamed or dropped -- or a change to the
//          safety model, such as giving faults a clear path or letting the
//          element run during a cooling step. Adding to a JSON response is
//          not breaking; changing what an existing key means is.
//
// A profile stored in NVS survives a flash whenever sizeof(Profile_t) is
// unchanged, so a release that alters the DEFAULT profile does not reach an
// oven that already has one saved. That is worth a line in the commit
// message; it is not by itself a major.
//
// Emitted by /config and printed at boot. Deliberately NOT in /status: that is
// polled once a second into a fixed 512-byte buffer that is already most of
// the way full, and a constant does not belong in a per-second poll.
#define FW_VERSION "2.8.0"
#define FW_BUILD   __DATE__ " " __TIME__

//Devdefins
//#define NOEDGEERRORREPORT 

//Pin Mapping
// One of the two original heater pins, so it is already routed for heater duty
// on the board. Non-strapping and output-capable.
//
// Note 25/26 -- suggested in the original rework notes -- are NOT available:
// they stay claimed by BUZZER and RGB_SDO. Still free if this ever has to
// move: 17 (the other original heater pin), 27, 22, 4, 33.
//
// 23 was on that list and has been removed: it is tied straight to 3V3 on
// this PCB, so driving it low would put the pad's low-side driver across the
// rail with no series resistance. That is worse than it first looks, because
// LOW is the *safe* heater state -- setup() and reportError() both write it --
// so the short would happen exactly when the firmware was trying to make the
// oven safe. The static_assert below stops that pin choice compiling.
//
// Two things this pin choice depends on, neither visible from the firmware:
//
// 1. 16 and 17 are the PSRAM interface on WROVER modules. This assumes a plain
//    WROOM with no PSRAM, which is also what platformio.ini assumes. If the
//    board turns out to carry PSRAM, this pin is not free -- move it to 27.
//
// 2. Nothing drives this pin between reset and the pinMode() in setup(): it is
//    a floating input for the whole of boot, and Serial.begin() and the ADC
//    setup happen first. The relay drive circuit therefore needs its own
//    pull-down to hold the heater off through that window. This is a hardware
//    requirement, not something the firmware can arrange for itself.
#define HEATER      16

// The heater drives a mains relay, so a wrong pin here is not the sort of
// mistake to leave discoverable by smoke. What these reject:
//
//   6-11   the internal SPI flash bus on a WROOM module; not usable at all
//   12     MTDI straps VDD_SDIO to 1.8 V when high at reset, and this
//          module's flash is a 3.3 V part -- it simply will not boot
//   23     tied to 3V3 on this PCB; see above
//   34-39  input-only, so pinMode(OUTPUT) silently does nothing and the
//          relay would never be driven at all
//
// Not rejected, but worth knowing before moving this pin: 1, 3, 5, 14 and 15
// emit pulses during reset, which is not something a relay gate should see.
// 0 and 2 boot fine but tying either defeats serial download mode, and OTA
// cannot recover a board that will not boot.
static_assert(!(HEATER >= 6 && HEATER <= 11),
              "HEATER: GPIO 6-11 are the internal SPI flash bus");
static_assert(HEATER != 12,
              "HEATER: GPIO 12 straps VDD_SDIO to 1.8V; the module will not boot");
static_assert(HEATER != 23,
              "HEATER: GPIO 23 is tied to 3V3 on this PCB; driving it low shorts the pad");
static_assert(!(HEATER >= 34 && HEATER <= 39),
              "HEATER: GPIO 34-39 are input-only and cannot drive the relay");

// AD595 analog output. Must be an ADC1 channel (32-39); ADC2 is unusable while
// WiFi is active. 34 is input-only, which suits a sensor input.
#define TEMP_ADC_GPIO   34
#define TEMP_ADC_CH     ADC1_CHANNEL_6

#define BUZZER      25
// LEDC channel 0, and the timer/speed-mode the Arduino wrapper maps it onto:
// group = chan/8, timer = (chan/2)%4. Kept as names because setup() has to
// hand the same mapping to the IDF driver directly -- see the channel init
// there for why. The resolution matches what ledcWriteTone() picks.
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_0
#define BUZZER_LEDC_TIMER   LEDC_TIMER_0
#define BUZZER_LEDC_MODE    LEDC_HIGH_SPEED_MODE
#define BUZZER_LEDC_BITS    10

#define RGB_CLK     21
#define RGB_SDO     26

//constance
// Time-proportional relay output. A mechanical relay cannot be phase-fired, so
// heater demand is expressed as on-time within a fixed window. The min on/off
// clamps suppress pulses too short to be worth a contact cycle.
//
// The controller quantises demand to POWER_LEVELS steps, so the shortest pulse
// it can ask for is one level: 4000/4 = 1000ms, comfortably clear of the 500ms
// minimum. Firing stays contiguous within the window -- the source project
// spread it (X-X- rather than XX--), but the oven's thermal mass integrates
// over minutes and cannot resolve the difference, while spreading would double
// the contact closures per run.
#define RELAY_WINDOW_MS   4000
#define RELAY_TICK_MS       50
#define RELAY_MIN_ON_MS    500
#define RELAY_MIN_OFF_MS   500
// relayDriver() runs from a timer and loop() is the only thing that ever
// lowers the demand, so a wedged loop() leaves the relay driving whatever it
// was last told -- indefinitely, on a mains heater, unattended. The control
// loop stamps a heartbeat every 100 ms; the relay refuses to close without a
// fresh one.
//
// Two thresholds, because a transient stall and a dead loop() want different
// answers. Holding off is free and self-healing, so it happens early. Latching
// costs a power cycle, so it waits for evidence that nothing is coming back.
// The oven barely moves in 5 s, and its own faults resume covering it the
// moment loop() does.
#define RELAY_HEARTBEAT_HOLD_MS   5000
#define RELAY_HEARTBEAT_FAULT_MS 30000
#define READ_TEMP_INTERVAL_MS 100 
#define READ_TEMP_AVERAGE_COUNT 10 

// How often to sample the ESP32's own die temperature, milliseconds.
//
// Slowly, and deliberately so. temperatureRead() is backed by the undocumented
// temprature_sens_read() -- the typo is the real symbol name -- which resolves
// out of librtc.a, the same blob that owns the RF calibration this sensor is
// shared with. That sharing is why the reading carries an offset that moves
// with WiFi state, and it is why there is nothing to gain from reading it
// often. The enclosure it is reporting on has a thermal time constant in
// minutes; 2 s is already far faster than the question needs.
#define MCU_TEMP_INTERVAL_MS 2000

// AD595 outputs 10 mV/degC, divided 1.5:1 in hardware before reaching the ADC
// so that the reflow range lands mid-scale where the ESP32 ADC is most linear
// (250degC -> 2.50V raw -> 1.67V at the pin) rather than against its ceiling.
// The ADC is specified linear only to 2450mV, which at this ratio is ~367degC:
// far more headroom than reflow needs.
//
// HARDWARE DEPENDENCY: this firmware assumes that divider is fitted. Without it
// every reading comes out 1.5x high, which trips the TEMP_PLAUSIBLE_MAX_C fault
// at 200degC real. That is the safe direction -- an over-reading sensor makes
// the controller back off, never overheat -- but it will fault out rather than
// run, so
// the symptom is a dead oven, not a damaged one.
#define AD595_MV_PER_C        10.0f

// Sensor calibration: true = c2*raw^2 + c1*raw + c0, raw being the
// uncorrected mv*ratio/10 reading.
//
// Measured 2026-09-04 against a K-type handheld, both probes in free air 5 mm
// apart, six points from 25 to 226 degC. Full data and method in
// logs/2026-09-04_probe_calibration.csv.
//
// It is a QUADRATIC because the response is not straight, and that only shows
// at the top. A line through the five points below 145 degC fitted them to
// +-0.03 degC and predicted the 226 degC point 3.6 degC wrong. Suspect the
// AD595: it linearises the K-type curve at a fixed 10 mV/degC which is exact
// only near its design point, while the meter uses a proper polynomial.
//
// These are properties of the SENSOR CHAIN -- thermocouple, AD595, divider,
// ADC -- and not of the oven. A different oven does not invalidate them; a
// different probe, amplifier or controller board does. They are settable at
// runtime for that reason.
//
// Identity (0, 1, 0) disables the correction and returns the raw reading.
#define TEMP_CAL_C2_DEFAULT    1.88645804e-4f
#define TEMP_CAL_C1_DEFAULT    0.926919005f
#define TEMP_CAL_C0_DEFAULT   -0.633558936f

// A calibration that reads LOW makes the oven run HOT, silently, which is the
// dangerous direction and the reason these are validated rather than trusted.
// Checked across the whole plausible span: strictly increasing, and never
// further than this from the raw reading.
//
// 30 degC is deliberately tight. The measured correction on this hardware
// peaks at 7.7 degC, so this still allows four times that for a different
// sensor chain -- but a thermometer needing more than 30 degC of correction is
// broken rather than uncalibrated. At 60 a flipped sign on c2 passed: it stays
// monotonic and peaks at 49 degC of deviation, while running the oven 21.7
// degC hot at reflow temperature. That is precisely the failure this is here
// to catch, so the window is sized to catch it.
#define TEMP_CAL_CHECK_MAX_C   350
#define TEMP_CAL_MAX_DELTA_C   30.0f
#define TEMP_DIVIDER_RATIO     1.5f
#define TEMP_OVERSAMPLE         64

// Spread the oversample burst evenly across exactly one mains cycle.
//
// A thermocouple on long leads inside an oven, next to the element's mains
// wiring, into a high-gain amplifier, is about the best hum antenna you could
// build. Taken back to back the 64 samples span about a millisecond -- a
// twentieth of a cycle -- so they all sit on the same part of the sine wave
// and average to that part of it rather than to zero. The burst then rides up
// and down at the beat between the sample rate and the mains, which lands
// straight on the rate term.
//
// Sampled uniformly over one whole period, the average cancels the mains
// fundamental *and* every harmonic of it, which is why this is worth 20 ms of
// each 100 ms interval and no extra parts.
//
// The input filter cannot do this job. Its corner is at 240 Hz, chosen to keep
// RF out of the ADC pin, and a single pole there attenuates 50 Hz by 2% -- for
// mains the sampling is the whole defence.
//
// The two do complement each other, though. Uniform sampling cancels every
// harmonic *except* multiples of the sample rate, which alias to DC: with 64
// samples across the period that is 3.2 kHz, and the filter is 22 dB down by
// there. Worth keeping in step if either number changes.
//
// Note also that the filter's 663 us time constant is longer than the 312 us
// sample spacing, so adjacent samples are correlated and the burst does not
// buy the full sqrt(64) on broadband noise. Mains rejection is unaffected --
// hum is deterministic, and uniform coverage of the period cancels it
// regardless of what the samples do in between.
//
// This oven runs on 50 Hz (Australia), so 20000. 60 Hz mains needs 16667 or
// rejection drops to 84%. 100000 would cover both -- 5 cycles at 50, 6 at 60
// -- but that is the entire read interval, leaving nothing for the web server.
#define TEMP_SAMPLE_SPAN_US  20000
// The MAX31855 reported open/short faults on its status bits. The AD595 has no
// such channel, but an open thermocouple drives its output to the rail, so an
// implausibly high reading stands in for the same fault.
//
// This threshold is coupled to TEMP_DIVIDER_RATIO and to the AD595 supply, so
// it cannot be chosen independently of them. On a single-supply 5V AD595 the
// output rails around 4V, which at 1.5:1 puts ~2.67V on the pin -- past the
// 2450mV linear limit, so it reads compressed and lands somewhere near 390degC
// rather than the 400 the arithmetic suggests. 300degC sits clear above any
// real reflow reading and clear below a railed one, so it survives that
// uncertainty. Confirm it by measuring the open-thermocouple output on the
// bench; if the divider ratio or the AD595 supply changes, revisit this and the
// peakTemp/soakTemp clamps in /profile/edit with it.
#define TEMP_PLAUSIBLE_MAX_C   300.0f
// IDF renamed the 11dB constant to DB_12 (same ~2.5x scaling, the old name was
// just a rounding) and deprecated the old spelling. These are enum values, not
// macros, so this cannot be probed with #ifdef -- it relies on the platform pin
// in platformio.ini.
#define TEMP_ADC_ATTEN ADC_ATTEN_DB_12

#define RGB_LED_BRITHNESS_1TO255  125 
#define IDLE_TEMP     50
#define MAX_PROFILES  30
#define PROFILE_NAME_LENGTH 11

// Rate-corridor control (see PORTING_ANALYSIS.md).
//
// A profile is a list of steps, each "reach targetTemp no sooner than this and
// no later than that". That pair defines a corridor rather than a single
// setpoint, and the controller nudges a discrete power level to keep both the
// temperature and its rate of change inside it. There are no gains to tune:
// the coarseness of a mechanical relay is the design premise rather than a
// problem to work around.
#define MAX_STEPS      8

// Discrete power levels, 0..POWER_LEVELS-1, evenly spaced to 100%.
//
// 9, was 5. At 5 every nudge was a 25% jump in duty, and the corridor law is a
// pure integrator -- no proportional term, no memory of which duty produced
// which rate -- so whenever the oven's capability did not match the rate the
// profile declared it could not sit still. The 2026-09-04 run showed the
// consequence in step 1: 100% down to 0 over 48 s and all the way back over
// 32 s, an 80 s limit cycle across the entire actuator range. The oven really
// does 2.1 degC/s at 50 degC and 0.88 at 200, against one declared bound of
// 0.72-0.94, so no fixed duty holds it -- but with 9 levels the hunt covers
// half the range it used to.
//
// 9 is the finest the relay allows: 12.5% of a 4 s window is a 500 ms pulse,
// exactly RELAY_MIN_ON_MS. The static_assert below is what holds that true.
#define POWER_LEVELS   9

// Every partial level has to be a pulse the relay will actually accept.
//
// The level -> powerHeater -> onTime chain divides twice, and at POWER_LEVELS 9
// the first level lands within 2 ms of the limit. Truncating rather than
// rounding at the first division is enough to break it: 255/8 = 31, which is
// 486 ms, and relayDriver() rounds anything under RELAY_MIN_ON_MS away to
// fully off -- so level 1 would have silently become another zero. Checked
// here rather than discovered on an oven.
#define PH_FOR_LEVEL(k)     (((uint32_t)(k) * 255u + (POWER_LEVELS - 1) / 2) \
                             / (POWER_LEVELS - 1))
#define ONTIME_FOR_LEVEL(k) ((PH_FOR_LEVEL(k) * RELAY_WINDOW_MS) / 255u)
static_assert(ONTIME_FOR_LEVEL(1) >= RELAY_MIN_ON_MS,
              "lowest partial power level rounds away to fully off");
static_assert(ONTIME_FOR_LEVEL(POWER_LEVELS - 2)
                <= RELAY_WINDOW_MS - RELAY_MIN_OFF_MS,
              "highest partial power level rounds up to fully on");

// How often the power level is re-evaluated, in whole relay windows.
//
// One window was too fast. The actuator can express a change that quickly, but
// the *oven* cannot show it: with a thermal lag around 20 s the loop was
// deciding four or five times before the result of the first decision reached
// the thermometer, so it chased its own dead time and never settled.
//
// Counted in windows rather than milliseconds, and clocked off relayDriver()'s
// own window counter, because the two used to run on independent phases --
// lastLevelUpdate_ms was reset at step entry while the relay window was not.
// A change landing mid-window is deferred to the next latch, so the effective
// control period jittered between one and two windows. Now every demand gets
// exactly CONTROL_WINDOWS whole windows, and the oven sees a stable duty long
// enough to respond to it.
//
// Why 2 and not more: the peak step of a lead-free profile is short. The
// default 220->240 degC step has a 21 s minimum, which is 3 decisions at 2
// windows but only 1 at 4 -- effectively open-loop through reflow, which is
// the last place to give up feedback. 2 windows is the slowest value that
// still closes the loop on the shortest step that matters.
#define CONTROL_WINDOWS      2
#define CONTROL_INTERVAL_MS  (CONTROL_WINDOWS * RELAY_WINDOW_MS)

// Outside the corridor by more than this, the level moves by two rather than
// one. Recovers from a badly-placed start without making normal tracking jumpy.
#define CORRIDOR_HARD_C     10.0f

// How many levels the hard arms move, when the oven is more than
// CORRIDOR_HARD_C from the wall it is chasing.
//
// Derived from POWER_LEVELS rather than written as 2, because the two have to
// move together. A literal 2 was half the range at POWER_LEVELS 5; left alone
// through the change to 9 it would have quietly halved the loop's recovery
// authority, which is the one thing finer resolution must not cost.
#define CORRIDOR_HARD_NUDGE ((POWER_LEVELS - 1) / 2)

// Thermal lag, seconds: how long the oven keeps rising after the element stops.
//
// The element is hotter than the air, so heat already in it lands in the oven
// after the relay opens. Driving until the thermometer reads the target spends
// all of that as overshoot -- at a ramp of 1 degC/s and this much lag, arriving
// under power costs roughly this many degrees past target.
//
// The controller instead projects where it will end up if it stops now:
//
//     projected = temperature + rate * thermalLagSec
//
// and stops driving once that reaches the target, letting stored heat carry
// the oven in rather than push it past.
//
// MEASURE THIS -- /measurelag does it for you, or derive it from a logged run
// (tools/reflowlog.py). It is a property of the oven, its element and its load,
// not of the profile, so it is stored alongside the profile rather than
// compiled in. This is only the default for a device that has never measured.
//
// Too small and the oven overshoots; too large and it backs off early and
// crawls the last few degrees. 20 s is a starting guess, not a measurement.
#define THERMAL_LAG_DEFAULT_S  20.0f
#define THERMAL_LAG_MIN_S       1.0f
#define THERMAL_LAG_MAX_S     120.0f

// Bound on the rate the projection is allowed to extrapolate, degC/s. No oven
// ramps faster than this, so anything beyond it is a measurement artefact, not
// a climb.
//
// This matters more than it looks. The projection now gates step advance, and
// it multiplies the rate by the lag -- so a rate glitch is amplified
// twentyfold into a temperature that sails past the target and advances the
// step. ADC noise correlated with WiFi transmit activity is a known risk on
// this hardware (see REWORK_NOTES.md 7.1), and at boot the ramp history is
// zero-filled, which reads as a large false climb for the first second.
// Bounding the rate costs nothing and turns both into a slow step rather than
// a profile that races to Complete.
// 2.0 degC/s, was 5.0. At 5.0 it clamped nothing this oven can do: the
// measured maximum is 0.88 degC/s heating at full power and 1.62 cooling with
// the door open, so the cap merely licensed a noise spike to project +25 degC
// at a 5 s lag and slam the approach clamp shut. 2.0 sits above every rate
// actually observed and well under the excursions, which makes it a safety
// clamp again rather than a control input. Lower would start clipping real
// cooling and bias the dwell arm.
#define PROJECTION_MAX_RATE_C_S  2.0f

// Half-width of the band a dwell step holds, in degC. Dwell steps have no
// corridor to sit inside -- start and target are the same -- so they regulate
// against the projected temperature instead.
#define DWELL_BAND_C         2.0f

// --- /measurelag: the oven measures its own thermal lag -----------------------
//
// Heat at a fixed level to MEASURE_TEMP, cut the power, and watch how far the
// temperature carries on rising:
//
//     lag = (peak - temp at cut) / rate at cut
//
// Drive level matters, because the coast comes out of the element and a hotter
// element coasts longer. This sits at the level the corridor typically uses on
// the approach to peak rather than at full power, so the number describes the
// case it will be used in.
#define MEASURE_LEVEL           3      // of POWER_LEVELS-1
#define MEASURE_TEMP_DEFAULT_C  200    // measure near peak: see REWORK_NOTES 7.3
#define MEASURE_TEMP_MIN_C      100
// Lower than the profile step clamp (280) on purpose: the measurement is the
// one cycle that *deliberately* overshoots its target, and the coast has to
// fit under TEMP_PLAUSIBLE_MAX_C or the run ends in a sensor fault instead of
// a number.
#define MEASURE_TEMP_MAX_C      250
// The coast has ended only once the climb has stopped and stayed stopped. A
// single sample dipping to zero is ADC noise -- a known risk on this hardware
// -- and taking it for the peak ends the measurement early and reports a lag
// that is too short, which is the dangerous direction.
#define MEASURE_PEAK_SETTLE_MS  3000
// Give up if the oven never stops rising, or never gets going.
#define MEASURE_COAST_TIMEOUT_S 180
#define MEASURE_HEAT_TIMEOUT_S  900
// The climb has to be real for the division to mean anything.
#define MEASURE_MIN_RATE_C_S    0.05f

// Abort if the oven runs this far above the corridor WHILE STILL CLIMBING: a
// welded relay contact or a shorted drive transistor looks like this.
//
// "And nothing else does" was wrong, and a completed run proved it. The gap
// alone is not evidence of anything on a COOLING step, because that corridor
// is a timer -- the note at defaultSteps already says a cooling step's
// declared duration says nothing about where the temperature is. The last
// step drives the corridor from 150 to 30 degC in 20-30 s; the oven coasts
// down far slower than that, so the gap opens past 100 degC on a healthy oven
// every time. A board-mounted probe widens it further, since a board sheds
// heat far more slowly than the air probe this was sized against.
//
// So the run ended in "Temperature is Way to HOT!!!!!" with the relay open,
// the element cold and the oven falling at 1.8 degC/s -- a hardware
// accusation built from evidence for a schedule miss. That is the same
// mistake as the "Oven not heating" misdiagnosis, which was fixed by making
// the test ask the question it was actually claiming to answer.
//
// Requiring a climb does that here, and it holds in every state rather than
// only on heating steps: a welded contact delivers heat nobody asked for, so
// it climbs. A coast never does.
#define CORRIDOR_ABORT_C   100.0f

// One positive sample does not make a runaway.
//
// The test now reads a rate, so it inherits the rule the rest of this file
// follows: never decide on an instantaneous one. controlRamp is the 4 s
// difference (sigma 0.30) rather than the 1 s aktSystemTemperatureRamp
// (sigma 0.48), and the condition then has to hold continuously. During a
// 1.8 degC/s coast a single positive reading is 6 sigma; sustained for three
// seconds at 10 Hz it is not noise. Same settling idea as
// COOLDOWN_BEEP_SETTLE_MS and MEASURE_PEAK_SETTLE_MS.
#define CORRIDOR_ABORT_SETTLE_MS 3000

// Hold a fixed temperature, for comparing the oven's probe against a
// reference meter at several levels.
//
// This is a MEASUREMENT mode, not a reflow mode. What a cross-calibration
// needs is for the oven to sit still while two instruments are read; it does
// NOT need the reading to equal the setpoint, because any steady-state offset
// is common to both instruments and cancels when they are read together. That
// is why this reuses the corridor law rather than introducing a PID: the
// error a PID exists to remove is the one term that does not matter here, and
// gains would be a new thing to tune in a controller whose whole point is
// that there is nothing to tune.
//
// What actually bounds how still the oven sits is POWER_LEVELS: 12.5% of a
// 4 s window is the finest demand the relay can express, and no control law
// beats its own actuator resolution. Expect a slow cycle about the setpoint
// and read both instruments together rather than trying to remove it.
#define HOLD_TEMP_MIN_C     40
#define HOLD_TEMP_MAX_C    250

// The element runs unattended here, with no profile to end the cycle. A hold
// that is walked away from has to stop on its own.
#define HOLD_TIMEOUT_S    3600

// Runaway guard for Hold. The corridor abort cannot serve: it lives in the
// Running block, and its corridor is a moving band. Here the setpoint is a
// constant, so a plain absolute ceiling is both correct and unambiguous --
// none of the cooling-timer reasoning behind CORRIDOR_ABORT_C applies.
#define HOLD_ABORT_C      40.0f

// Proportional band for the hold regulator, and the reason it exists.
//
// The obvious thing here is the dwell regulator -- nudge the level up while
// the projection sits below target, down while it sits above -- and it does
// not survive the move. A dwell starts AT temperature, so its level never has
// to travel far. A hold is entered from wherever the oven is, and a pure
// integrator then winds all the way to full power and stays there until the
// projection nearly arrives. The oven reaches its setpoint still at 100% and
// unwinds one level per control interval while continuing to absorb heat.
// Measured on this oven before the P term was added, not predicted.
//
// That breaks the projection's own premise. projectedTemp means "where the
// oven ends up if the relay opens NOW", and the controller was not opening it
// now -- it took 16-64 s to step down. Measured: a 200 degC hold entered at
// 1.18 degC/s sat at full power until 176 and peaked at 209.4.
//
// A proportional term fixes it at the source, because demand becomes a
// function of the error rather than an accumulation: at the moment the
// projection says "arriving", power is already near zero, which is what the
// projection assumed all along. Full demand at HOLD_PBAND_C below target,
// falling linearly to nothing at the target.
#define HOLD_PBAND_DEFAULT_C  25.0f
#define HOLD_PBAND_MIN_C       5.0f
#define HOLD_PBAND_MAX_C     100.0f

// P alone parks below setpoint -- holding a temperature needs real power, and
// a proportional term commands none at zero error. This trims that out.
//
// It integrates ONLY inside the proportional band. Outside it P already
// commands the extreme, so accumulating there is windup and nothing else --
// the exact defect this pair of constants exists to remove. 0.25 levels per
// 8 s control interval reaches the 2-3 levels an equilibrium needs in about a
// minute and a half, which is slow against the oven and cannot itself drive
// an overshoot.
// Trim of exactly 0 is allowed, and is the useful diagnostic: it runs the
// regulator proportional-only, which parks below setpoint by however much
// power the equilibrium needs. That droop IS the measurement of it.
#define HOLD_TRIM_DEFAULT      0.25f
#define HOLD_TRIM_MIN          0.0f
#define HOLD_TRIM_MAX          2.0f

// A heating step that times out on its slow bound this far short of target has
// not merely run slow -- the element is not heating. The PID build had no such
// check: its state machine advanced on the computed setpoint, so a dead element
// ran the whole profile through to Complete without ever warming the oven.
#define STEP_MISS_C         15.0f

// --- Step extension -------------------------------------------------------
//
// A heating step that runs out its slow bound short of target, but is still
// genuinely climbing, is EXTENDED rather than advanced: it keeps running, with
// its rate bounds still in force and only its duration watchdog suspended,
// until it actually reaches the target.
//
// This closes the gap between the two outcomes that used to be the only ones.
// Miss by more than STEP_MISS_C and the oven latched dead; miss by 14 degC and
// the step advanced in silence, with the deficit rebased into the next step at
// step entry -- no fault, and a log that looks perfect (REWORK_NOTES 7.7).

// Dead-element watchdog: heat demanded, nothing happening.
//
// The STEP_MISS_C check below only fires at maxDuration, which on the default
// rate-bounded step 0 is 130/0.72 = 181 s. Three minutes of a commanded-on
// mains relay before anyone is told is far too long to find out the element,
// the relay or the wiring is dead -- and it is the most likely single fault in
// the whole machine.
//
// So: while the controller is asking for real power on a heating step, the
// oven has to actually respond. Any working oven at half power moves several
// degrees in half a minute; the default profile's own slow bound is
// 0.72 degC/s, which is 21 degC in that window. Requiring 2 degC is ~10x
// slack, and the readout's noise over 30 s is under 0.1 degC, so there is no
// plausible false positive between the two.
//
// Gated on demand, not on measured rise alone, because a step legitimately
// sits flat when it is at temperature -- the clamp cuts power on approach and
// a dwell holds. Only "asking for heat and getting none" is a fault.
#define HEAT_STALL_WINDOW_MS   30000
#define HEAT_STALL_MIN_RISE_C      2.0f
// Half of full demand, rounded down. At POWER_LEVELS 9 that is level 4 of 8.
#define HEAT_STALL_MIN_LEVEL   ((POWER_LEVELS - 1) / 2)

// There is deliberately no distance gate on entering an extension.
//
// There used to be one: STEP_EXTEND_ENTRY_C, set to STEP_MISS_C, so only a
// step within 15 degC of target could be extended. The argument was that
// anything further out is the dead-or-weak element case and must keep faulting
// on its original timing. That argument was wrong twice over, and the
// 2026-09-03 run is what showed it -- step 3 was climbing at 0.27 degC/s,
// 25 degC short, and got "Oven not heating" while it was demonstrably heating.
//
// Wrong first because distance-to-target does not identify a dead element;
// gaining does, and the gain test below already applies it. A dead element is
// not gaining at any distance, so the gate rejected nothing that the gain test
// would have let through. Wrong second because HEAT_STALL_WINDOW_MS above is
// the actual dead-element detector: 30 s of demanded power with under 2 degC
// of rise, independent of step timing and confirmed on hardware at 30.1 s. The
// gate was guarding a case that was already covered twice.
//
// What bounds an extension is therefore time, not distance:
// A heating step ends on its target, not on a clock. There is no extension
// budget: see the block at `stepExtending` in the control loop for why a
// budget was the wrong question, and what replaced it.
//
// LIQUIDUS_C is still used -- the profile is sized around time above it, and
// the peak dwell is what delivers that time.
#define LIQUIDUS_C              217.0f

#define STEP_EXTEND_GAIN_WINDOW_MS 20000

// An absolute floor, and only an absolute floor.
//
// It used to be fmaxf(0.20, 0.5*|delta|/maxDuration) -- "at least half the rate
// the step itself demands" -- which came out at 0.36-0.41 degC/s on the default
// profile's heating steps. That asked the wrong question at this decision
// point. By the time a step is overrun and short we have already established
// that it is not meeting the profile's demand; what remains to decide is
// whether the oven is still climbing or has stalled, and the demanded rate
// says nothing about that. On the 2026-09-03 run step 3 was rising a genuine
// 0.27 degC/s and the relative term called it stalled.
//
// 0.20 degC/s is measured across STEP_EXTEND_GAIN_WINDOW_MS, not
// instantaneously. Over a 20 s window the endpoint noise is about 0.5 degC,
// so 0.025 degC/s of sigma -- the floor sits at roughly 8 sigma, while still
// being an order of magnitude below any rate the profile asks for.
//
// THIS IS NOW THE ONLY THING THAT ENDS A HEATING STEP SHORT. It used to be a
// weak limiter backed by the extension caps, and its own note said not to size
// it as the primary test. The caps are gone, so it has been resized for the
// job it actually has.
//
// 0.20 was too impatient once it stood alone: a step 3 degC short and climbing
// 0.15 degC/s would be abandoned despite arriving twenty seconds later. 0.10
// roughly doubles the patience and still sits about 4 sigma clear of the
// noise -- endpoint noise over a 20 s window is ~0.5 degC, so ~0.025 degC/s.
//
// It cannot go much lower. At full power the oven keeps gaining right up to
// thermal equilibrium, the rate decaying through 0.4, 0.2, 0.1, and a floor
// buried in the noise would wait out an asymptote it can never reach. 0.10 is
// the point where "still climbing" stops meaning "will arrive".
#define STEP_EXTEND_MIN_RATE_C_S   0.10f

// A profile may not start from a warm chamber.
//
// A hot oven is a hazard to load -- reaching into it is how you get burnt, and
// the door has to be open to do it. 50 degC is warm to the touch and no worse.
//
// BOUND_RATE now removes the profile-consistency half of this for any step
// that declares itself rate-bounded, which the default step 0 does. What is
// left is the plain hazard: an oven at reflow temperature is not something to
// reach into, and it has to be open to be loaded.
#define PROFILE_START_MAX_C   50

// The door-open prompt waits for the coast to finish.
//
// A heating step ends when arrival is *projected*, which is deliberately while
// the oven is still climbing and short of target -- the stored heat delivers
// the rest. Beeping on that transition asks for the door at the exact moment
// the coast is doing the work of reaching peak temperature, and an operator
// standing ready would take 7 degC off the peak. So the prompt waits until the
// climb has actually stopped and stayed stopped, with a cap so it cannot wait
// for a peak that never comes.
#define COOLDOWN_BEEP_SETTLE_MS    3000
#define COOLDOWN_BEEP_MAX_WAIT_MS 60000

// Power ceiling during a cooling step, as a level index.
//
// The corridor law can hold heat to stop a descent running away, which is what
// thermally shocks joints -- but cooling a small oven means opening the door,
// and 0 here is the only thing keeping the element dead while a hand is in it.
// The capability is built and unreachable on purpose. Raising this to 1 (25%)
// enables controlled cooldown; do not do it until there is an interlock or a
// deliberate decision that there does not need to be one.
#define COOLDOWN_MAX_LEVEL  0

// Name of both the setup access point and the mDNS host, so the oven is
// reachable at http://REFLOW_HOSTNAME.local/ once it has joined a network.
// Over-the-air firmware update.
//
// The default partition table already carries a second 1.25 MiB app slot and
// the otadata selector, so nothing here needs a repartition -- OTA space was
// always allocated, just never used.
//
// Fixed HTTP basic-auth username; only the password is configurable. There is
// no value in making both secret, and one field is one less thing to mistype
// into an oven you cannot see.
#define OTA_USER            "ota"
// Short enough not to be a nuisance, long enough not to be walked in a few
// thousand requests over a LAN.
#define OTA_PASS_MIN_LEN    8

#define REFLOW_HOSTNAME "ReflowController"
// How long to hold the setup portal open before giving up and carrying on. The
// oven must not sit in the portal forever: with no display there is nothing to
// show that it is stuck there, and loop() has a relay and a sensor to service.
#define WIFI_PORTAL_TIMEOUT_S 180

//includes
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <Update.h>
#include <SPI.h>
#include <Ticker.h>
#include <driver/adc.h>
#include <driver/ledc.h>
#include <esp_adc_cal.h>

#include "root_html.h"

//structs
// One profile segment: reach targetTemp no sooner than its fast bound and no
// later than its slow bound. Those two are the corridor, and each step says
// whether it states them as durations or as rates -- see BOUND_DURATION. This
// is how a solder paste datasheet states a profile ("ramp to 150degC in
// 60-90s", "1-3 degC/s"), so one can be transcribed rather than converted.
//
// A step's corridor is rebased on the measured temperature when the step
// begins, not on the previous step's target, so a warm oven or an overshoot
// carries forward as a gentler rate rather than an impossible one.
// How a step's two bounds are stated. Which invariant is correct differs by
// step, which is why it is per-step rather than a property of the profile:
//
// A preheat from ambient wants a fixed *rate*. Its start temperature is
// whatever the chamber happens to be, so pinning the duration makes the same
// profile a different thermal history every time -- from 25 degC the default
// step 0 needs 0.81-1.05 degC/s, from 50 degC only 0.67-0.87, and the slower
// ramp spends far longer drying the flux out. Stated as a rate, a warm start
// shortens the step instead of flattening it.
//
// A soak or a peak wants a fixed *duration*, because time at temperature is
// what drives flux activation and intermetallic growth. Its start temperature
// is the previous step's target, so it does not drift anyway.
//
// Paste datasheets state both forms -- "1-3 degC/s" and "60-90 s" -- for
// exactly this reason. Forcing everything into durations was what made a warm
// start silently change the profile.
#define BOUND_DURATION 0
#define BOUND_RATE     1

typedef struct step_s {
  int16_t targetTemp;   // degC
  // Both bounds are stated in ascending order of the unit they use, which is
  // the order a datasheet prints and a person types. Note that puts "fastest"
  // at opposite ends: the shortest duration is the fastest, but so is the
  // *highest* rate. deriveStepDurations() is the one place that flips it.
  int16_t boundLo;      // BOUND_DURATION: min seconds. BOUND_RATE: min centi-degC/s
  int16_t boundHi;      // BOUND_DURATION: max seconds. BOUND_RATE: max centi-degC/s
  uint8_t boundIsRate;  // BOUND_DURATION or BOUND_RATE
} Step_t;

// Rates are stored as hundredths of a degree per second so the whole profile
// stays integral in Preferences. 0.72 degC/s is 72.
#define RATE_SCALE      100.0f
#define RATE_MIN_CDS      1     // 0.01 degC/s
#define RATE_MAX_CDS   2000     // 20.00 degC/s
#define DURATION_MIN_S    1
#define DURATION_MAX_S  999

// data type for the values used in the reflow profile
typedef struct profileValues_s {
  char    name[PROFILE_NAME_LENGTH];
  uint8_t stepCount;
  Step_t  steps[MAX_STEPS];
} Profile_t;

typedef enum {
  None     = 0,
  Ready    = 1,   // idle, accepting commands
  Manual   = 2,   // manual heating, power set from the web UI

  // Everything above this is an active oven cycle. Ordering is load-bearing:
  // several checks are written as comparisons against it.
  ProcessStart = 9,

  // The five fixed phases are gone: a profile is now an arbitrary list of steps,
  // so the phase is a step index and there is one running state.
  Running = 10,

  // Regulate to a fixed temperature and stay there. Above ProcessStart so it
  // counts as an active cycle and the mutation lockout applies.
  Hold = 15,

  Complete = 20,

  // Self-measurement of the thermal lag. Above ProcessStart, so it counts as an
  // active cycle and the same mutation lockout applies.
  MeasureLag = 30,
} State;


//prototypes
void factoryReset();


//Varables
const char * ver = "4.0";

SPIClass RGBLED(VSPI); 
esp_adc_cal_characteristics_t adcChars;
esp_timer_handle_t  RelayTimer;
Preferences PREF;
// OTA password, persisted. Empty means OTA is refused outright: an
// unauthenticated firmware endpoint on a mains heater is not a default worth
// shipping, and "off until configured" is the only safe starting position.
String otaPassword = "";
// Why the in-flight upload was refused, or nullptr. Set by the upload
// callback and read by the completion handler, because those are two separate
// invocations of two separate lambdas over one request.
const char *otaReject = nullptr;
WiFiManager wm;

WebServer server(80);
WebServer serverAction(8080);

// Latched fault. Once set the relay is held off and no cycle can start; the
// reason is kept so /status can report it instead of the oven just going quiet.
volatile boolean globalError=false;
const char * globalErrorText = "";
// Non-fatal counterpart. Deliberately separate from globalError: nothing gates
// a start or stops a run on this.
volatile boolean globalWarning = false;
const char * globalWarningText = "";

// Heater demand, 0..255, consumed by relayDriver().
volatile uint8_t  powerHeater=0;
// Incremented by relayDriver() each time it latches a new window. The control
// loop clocks its level updates off this rather than off wall time, so a
// demand is always held for a whole number of windows -- see CONTROL_WINDOWS.
volatile uint32_t relayWindowSeq=0;
// What relayDriver() last actually did to the contact, as opposed to what was
// demanded of it. The two differ whenever a fault or a stale heartbeat holds
// the relay off, and they differ constantly during normal running: demand is
// an average over the window, this is the instantaneous state. /status reports
// it so the readout can show the element rather than the intent.
volatile bool     heaterOn=false;
// Last time the control loop ran, ms. relayDriver() will not close the relay
// without a recent one -- see RELAY_HEARTBEAT_HOLD_MS.
volatile uint64_t controlHeartbeat_ms=0;

float aktSystemTemperature;
float aktSystemTemperatureRamp; //°C/s  1 s difference -- for display only

// The rate the control law steers on, degC/s, differenced across one whole
// relay window instead of one second.
//
// Measured on the 2026-09-03 run: with the duty pinned at 100% and the true
// rate constant at 0.88 degC/s by endpoint fit, aktSystemTemperatureRamp had a
// mean of 0.92 and a standard deviation of 0.48 -- noise at 53% of signal,
// with excursions to 2.40. Three separate decisions read that number (the
// approach clamp, the fast-bound ceiling and the in-corridor rate check), so
// each of them was acting on noise roughly half the time. The clamp sets
// powerLevel to zero outright while recovery is one level per control
// interval, so a single false trigger cost 16-32 s of climbing back.
//
// A median was the wrong instinct: the residuals are ordinary noise on the
// temperature (about 0.35 degC of sigma, which differenced over 1 s gives the
// 0.48 observed), not isolated outliers, and a median does nothing about that.
// A longer baseline does -- the noise falls as 1/T while the signal does not.
//
// RELAY_WINDOW_MS is the right T for a second reason: differencing across
// exactly one time-proportional cycle means partial duty cannot alias into the
// rate at all.
//
// Replayed against that same recorded stretch, 4 s gives mean 0.86, sigma 0.30
// and a range of 0.21..1.42 -- sigma cut 1.6x and the range no longer reaching
// anywhere near the old 2.40. Note 1.6x, not the 4x that independent endpoint
// noise would predict: a good part of the residual is real short-term
// structure in the oven, not measurement noise, and no filter length removes
// that. What matters downstream is the projection it feeds, which at a 5 s lag
// and the tightened PROJECTION_MAX_RATE_C_S goes from sigma 2.4 degC peaking
// at +12.0 to sigma 1.5 peaking at +7.1.
//
// The cost is that a difference is centred half its baseline back, so the
// estimate carries ~2 s of lag -- which matters only while the rate is
// changing, not on a steady ramp.
float controlRamp = 0.0f;

int activeProfileId = 0;
Profile_t activeProfile; // the one and only instance

State currentState  = Ready;
uint64_t stateChangedTicks = 0;

// Manual heating power, 0..100%, set from the web UI.
uint8_t manualPower = 0;

// Corridor state for the step being run. heaterSetpoint is the middle of the
// corridor -- it no longer drives anything, but /status still reports it and
// the web chart still plots it, and the corridor centre is what that trace
// meant all along.
// Thermal lag in seconds, measured or entered, persisted in Preferences.
float thermalLagSec = THERMAL_LAG_DEFAULT_S;
// Temperature /measurelag heats to before cutting the power.
int16_t measureTempC = MEASURE_TEMP_DEFAULT_C;
int16_t  holdTempC   = 0;      // Hold setpoint, degC
uint64_t holdSetAt_ms = 0;     // when this hold (or its latest setpoint) began
float    holdTrim    = 0.0f;   // integral term, in power levels
float    holdPBandC  = HOLD_PBAND_DEFAULT_C;  // runtime-tunable, see /oven
float    holdTrimStep = HOLD_TRIM_DEFAULT;
float    calC2 = TEMP_CAL_C2_DEFAULT;   // sensor calibration, see /oven
float    calC1 = TEMP_CAL_C1_DEFAULT;
float    calC0 = TEMP_CAL_C0_DEFAULT;

// Rejects a calibration that is not usable as a thermometer: it must rise
// everywhere a reading can land, and must not move the reading absurdly far.
static bool calSane(float c2, float c1, float c0)
{
  float prev = c2*0.0f*0.0f + c1*0.0f + c0;
  for (int r = 1; r <= TEMP_CAL_CHECK_MAX_C; r++)
  {
    float v = c2*(float)r*(float)r + c1*(float)r + c0;
    if (v <= prev) return false;                                  // must rise
    if (fabsf(v - (float)r) > TEMP_CAL_MAX_DELTA_C) return false; // sanity
    prev = v;
  }
  return true;
}
// Result of the last measurement, 0 if none this power-up.
float measuredLagSec = 0.0f;
// Set when the oven has peaked and wants its door opened. The buzzer says so
// too, but the operator may be at the browser rather than at the oven -- and
// on this profile the difference between opening on the prompt and opening
// nine seconds early is several degrees of peak.
bool openDoorPrompt = false;

float heaterSetpoint;
float corridorLow;      // degC, coolest the oven may be right now
float corridorHigh;     // degC, hottest it may be
uint8_t activeStep = 0; // index into activeProfile.steps
// True while the active step is running past its maxDuration because it is
// still climbing toward target. File scope rather than a control-loop static
// because the /status handler is a lambda in setup() and cannot see those --
// and an extension has to show up in a log, or it is just an inexplicably
// long step.
bool stepExtending = false;
// Run-scoped extension budgets, seconds. File scope for the same reason as
// stepExtending: /status reports the total, because an extension is a
// departure from the profile the step declared and must never be silent even
// when nothing latches a fault.
float runExtendUsed_s = 0.0f;   // total extension time this run
// The controller's own die temperature, degC.
//
// The electronics sit in an enclosure inside the oven, one sheet of metal from
// the element, so "are my electronics cooking" is a real question and this is
// the only sensor aboard that can answer it. Reported to the operator and
// nothing else: it never gates a start and never calls reportError(). An
// undocumented blob function with an RF-dependent offset has no business
// latching a fault that needs a power cycle to clear -- that would be a worse
// failure than the one it is watching for. The thresholds that colour it live
// in the UI, since the firmware takes no action on them.
float mcuTemp_C = 0.0f;
float stepStartTemp;    // measured temperature when the step began
uint8_t powerLevel = 0; // 0..POWER_LEVELS-1, what the corridor law is asking for

uint64_t cycleStartTime=0;



//Funcktions
float Hue_2_RGB( float v1, float v2, float vH )            
{
  float r;
  if ( vH <= 0 ) 
    vH += 1;
  if ( vH > 1 ) 
    vH -= 1;
  if ( ( 6 * vH ) < 1 )
  {
    r= v1 + ( v2 - v1 ) * 6 * vH ;
  }
  else if ( ( 2 * vH ) < 1 ) 
  {
    r=  v2 ;
  }
  else if ( ( 3 * vH ) < 2 ) 
  {
    r=  v1 + ( v2 - v1 ) * (.66-vH) * 6;
  }
  else
  {
    r=v1;
  }
  if(r<0)
  {
    r=0;
  }
  return r;
}  

// Latches a fault: the relay is held off by relayDriver() and no cycle can be
// started while globalError is set.
//
// The original spun forever here painting the TFT red. Headless that would take
// the web server down with it, leaving no way to find out what went wrong, so
// this returns and lets loop() keep serving /status. The RGB LED flashes red
// for anyone standing at the oven.
// A condition the operator should know about that does NOT justify ending the
// run.
//
// The distinction is whether stopping helps. A dead thermocouple or a welded
// contact must stop the oven: continuing is either blind or damaging. A step
// that heated too slowly is neither. Nobody can replace a heater with a board
// in the chamber, so aborting there converts "this might still be a good
// board" into "this is definitely a ruined board" -- and that is exactly what
// happened on 2026-09-04, when step 0 came up 3 degC short of 170 and the run
// stopped with the paste half soaked and no way to restart.
//
// So these latch, surface in /status and the UI, and the profile carries on.
// The run finishes and the operator decides what the board is worth.
void reportWarning(const char *text)
{
  if (globalWarning) return;  // first cause, same as reportError

  globalWarning = true;
  globalWarningText = text;

  Serial.print("Warning: ");
  Serial.println(text);
}

void reportError(const char *text)
{
  if (globalError) return; // keep the first cause, not the last

  globalError=true;
  globalErrorText=text;

  //Turn off heater
  digitalWrite(HEATER,LOW);

  Serial.print("Report Error: ");
  Serial.println(text);
}

// Why an OTA must be refused right now, or nullptr if it may proceed.
//
// Ready and Complete are the only states with the element guaranteed idle.
// Manual is emphatically not one of them -- its entire purpose is to drive the
// heater from the web UI -- and because Manual sits *below* ProcessStart, the
// usual `currentState > ProcessStart` busy test would wave it straight
// through. Hence naming the two permitted states rather than excluding the
// active ones.
//
// A latched fault now lands in Complete with the relay held off, so a faulted
// oven can still be updated. That is deliberate: a firmware bug is one of the
// things you might be trying to fix.
const char *otaBlockedReason()
{
  if (otaPassword.length() == 0)
    return "OTA disabled: set a password first";
  if (currentState != Ready && currentState != Complete)
    return "Oven busy: stop the cycle before updating firmware";
  return nullptr;
}

void setLEDRGBBColor(uint8_t r, uint8_t g, uint8_t b)
{
  RGBLED.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  RGBLED.transfer(b);
  RGBLED.transfer(g);
  RGBLED.transfer(r);
  RGBLED.endTransaction();
}


// Reads the AD595 on ADC1, oversampled to knock down the ESP32 ADC's
// considerable sample noise, and converted through the chip's factory eFuse
// calibration rather than a nominal full-scale assumption.
//
// Returns degrees C. An open thermocouple drives the AD595 to the rail, which
// shows up here as an implausibly high reading; the caller treats that as a
// sensor fault.
float readTemperature() {
  uint32_t sum = 0;
  const int64_t t0 = esp_timer_get_time();

  for (uint8_t i = 0; i < TEMP_OVERSAMPLE; i++) {
    // Each sample is due at a fixed offset from the start of the burst, not a
    // fixed delay after the previous one. Scheduling against t0 means a slow
    // read is absorbed by the next slot instead of stretching the window --
    // and the window has to stay one mains period for any of this to work.
    const int64_t due = t0 + ((int64_t)TEMP_SAMPLE_SPAN_US * i) / TEMP_OVERSAMPLE;
    while (esp_timer_get_time() < due) { /* ~300us, not worth a yield */ }

    sum += adc1_get_raw(TEMP_ADC_CH);
  }

  // Keep the fraction the oversample earned. Averaging 64 samples and then
  // integer-dividing hands back a plain 12-bit number, and esp_adc_cal returns
  // whole millivolts on top of that -- about 0.15 degC per count through the
  // 1.5:1 divider. So the oversample bought noise rejection and then threw the
  // extra resolution away, with a half-count downward bias for good measure.
  //
  // That matters more than 0.15 degC sounds, because the rate term is a
  // difference of two averages and the projection multiplies it by the lag: a
  // staircase in temperature becomes a staircase in degC/s, amplified. And
  // heavy oversampling makes it worse rather than better -- it removes the
  // very noise that would otherwise dither the outer rolling average across
  // the quantisation and let it interpolate.
  //
  // esp_adc_cal_raw_to_voltage is linear over one count, so interpolating
  // between the two neighbouring conversions recovers both lost fractions.
  uint32_t whole = sum / TEMP_OVERSAMPLE;
  uint32_t frac  = sum % TEMP_OVERSAMPLE;
  uint32_t mvLo  = esp_adc_cal_raw_to_voltage(whole,     &adcChars);
  uint32_t mvHi  = esp_adc_cal_raw_to_voltage(whole + 1, &adcChars);
  float    mv    = mvLo + (float)(mvHi - mvLo) * frac / TEMP_OVERSAMPLE;

  float raw = (mv * TEMP_DIVIDER_RATIO) / AD595_MV_PER_C;

  // Sensor calibration. Applied here so every consumer -- the control loop,
  // the faults, /status, the chart -- sees one temperature and there is no
  // second place where a raw reading could leak through.
  //
  // Note this shifts TEMP_PLAUSIBLE_MAX_C slightly in raw terms: the 300 degC
  // fault now trips at about 306 raw, because the correction subtracts ~6
  // there. That is well inside the margin the threshold was chosen with.
  return calC2 * raw * raw + calC1 * raw + calC0;
}

// Drives the mechanical relay with slow time-proportional control.
//
// Runs from a periodic esp_timer every RELAY_TICK_MS so it is unaffected by
// anything that blocks loop(). The demand is latched once per window, so the
// on-pulse stays contiguous even if powerHeater moves mid-window.
void relayDriver()
{
  static uint32_t windowElapsed = RELAY_WINDOW_MS; // force a latch on first tick
  static uint32_t onTime = 0;

  // Is loop() still alive? Nothing else can lower powerHeater, so without this
  // a stall in the web server or the WiFi stack would leave the last demand
  // driving the element for as long as the board stays powered.
  uint64_t now_ms = esp_timer_get_time() / 1000;
  uint64_t sinceHeartbeat = (controlHeartbeat_ms && now_ms > controlHeartbeat_ms)
                            ? now_ms - controlHeartbeat_ms : 0;
  bool heartbeatStale = (controlHeartbeat_ms == 0) ||
                        (sinceHeartbeat > RELAY_HEARTBEAT_HOLD_MS);

  if (sinceHeartbeat > RELAY_HEARTBEAT_FAULT_MS)
  {
    // Dispatched on the timer *task*, not in an ISR, so this is safe here.
    reportError("Control loop stopped: heater held off");
  }

  if (windowElapsed >= RELAY_WINDOW_MS)
  {
    windowElapsed = 0;
    relayWindowSeq++;
    onTime = ((uint32_t)powerHeater * RELAY_WINDOW_MS) / 255;

    // Never ask the relay for a pulse (or a gap) too short to be worth a
    // contact cycle: round it away to fully off or fully on.
    if (onTime < RELAY_MIN_ON_MS) onTime = 0;
    if (onTime > RELAY_WINDOW_MS - RELAY_MIN_OFF_MS) onTime = RELAY_WINDOW_MS;
  }

  heaterOn = (!globalError && !heartbeatStale && windowElapsed < onTime);
  digitalWrite(HEATER, heaterOn ? HIGH : LOW);
  windowElapsed += RELAY_TICK_MS;
}


// A step's effective duration window, in seconds.
//
// A duration-bounded step states it. A rate-bounded step derives it from how
// far it actually has to go, which is the whole point: cover less ground and
// the step gets shorter rather than gentler. delta is signed; only its
// magnitude matters here.
//
// The high rate is the *fast* bound and so yields the *minimum* duration.
static void deriveStepDurations(const Step_t &step, float delta,
                                float *minDur, float *maxDur)
{
  if (step.boundIsRate == BOUND_RATE)
  {
    const float absDelta = fabsf(delta);
    const float rateFast = step.boundHi / RATE_SCALE;
    const float rateSlow = step.boundLo / RATE_SCALE;
    *minDur = (rateFast > 0.0f) ? absDelta / rateFast : 0.0f;
    *maxDur = (rateSlow > 0.0f) ? absDelta / rateSlow : 0.0f;
    // Already at the target is a satisfied rate step, not a zero-length wait
    // for something to happen -- but never let the watchdog precede the floor.
    if (*maxDur < *minDur) *maxDur = *minDur;
  }
  else
  {
    *minDur = step.boundLo;
    *maxDur = step.boundHi;
  }
}


const char * currentStateToString()
{
  #define casePrintState(state) case state: return #state;
  switch (currentState) {
    casePrintState(Running);
    casePrintState(Complete);
    casePrintState(Manual);
    casePrintState(Hold);
    casePrintState(MeasureLag);
    default: return "Ready";
  }
}


void saveProfile(unsigned int targetProfile) {
  activeProfileId = targetProfile;
  saveParameters(activeProfileId);
  saveLastUsedProfile();
}

void loadProfile(unsigned int targetProfile) {
  loadParameters(targetProfile);

  // save in any way, as we have no undo
  activeProfileId = targetProfile;
  saveLastUsedProfile();
}

// The lead-free profile decoded from the source project (PORTING_ANALYSIS.md
// section 6), which was run against this oven, relay and probe. These durations
// are measured behaviour of this specific oven, not a datasheet ideal, so they
// are the right thing to start from -- and the reason the corridor law arrives
// already tuned.
//
// Steps 4-6 are the controlled cooldown. They are stored and run, but
// COOLDOWN_MAX_LEVEL holds their power at zero, so today they act as timed
// coast-down segments.
void makeDefaultProfile() {
  snprintf(activeProfile.name,PROFILE_NAME_LENGTH,"%s","LeadFree");
  //
  // Step 0 is rate-bounded and the rest are duration-bounded, which is the
  // distinction BOUND_RATE exists for. 0.72-0.94 degC/s is exactly what
  // "40 -> 170 degC in 138-180 s" means, so this is the same measured
  // behaviour restated in the invariant that survives a warm chamber. The
  // remaining steps start from the previous step's target rather than from
  // ambient, and their durations are the thing that matters chemically, so
  // they keep the measured seconds.
  //
  // The peak is a plateau, not an apex.
  //
  // It used to be a single 243 degC step followed straight by the descent, so
  // the oven touched peak and turned around. With the probe in the air the
  // board lags the reading in magnitude AND in time, so a triangular apex
  // gives the joints the least of both: the air peaks and falls while the
  // board is still climbing toward a number it never reaches. The 2026-09-03
  // run ended with the air at 221 degC and the solder unmelted.
  //
  // A dwell trades apex height for time at temperature, which is what
  // actually transfers heat into a board, so the peak gained a hold.
  //
  // 240 -> 245 on 2026-09-04, after the first run that actually reflowed. It
  // did, but only in the last few seconds, which is no margin at all: the peak
  // measured 237 against a 240 target, and every degree between the joints and
  // liquidus is time the paste is not flowing. 245 buys ~8 degC over what that
  // run delivered.
  //
  // This is now a BOARD temperature and a calibrated one, so 245 means 245 at
  // the joints -- not an air reading that flattered them by 20 degC. Parts
  // rated 245-260 peak have real but not generous margin here, which is why
  // this went to 245 and not higher.
  //
  // Time above liquidus is the constraint that bounds all of this: 217 degC,
  // 60-90 s for SAC305.
  //
  // Sizing it by summing the steps' declared durations does not work, and the
  // 2026-09-04 run is the proof -- that method predicted 60.2-76.8 s and the
  // oven delivered 47.4 s. Two reasons, both the same mistake:
  //
  //   - the 220 step was credited 1.9-3.8 s above 217 and contributed ZERO.
  //     It advanced on the projection and left at 213.1 degC, never crossing
  //     liquidus at all. (Fixed since -- a step leading into a hotter one now
  //     advances on the thermometer -- which is worth 3.3 s here.)
  //   - the 240->220 descent was credited its full 21-28 s and contributed
  //     11.1 s. With the door open the oven falls at up to 4.3 degC/s and is
  //     through 217 in eleven seconds, then spends the rest of its minDuration
  //     continuing down to 180. Cooling steps are timers; their declared
  //     duration says nothing about where the temperature is.
  //
  // So the only honest method is measurement, and the only DETERMINISTIC
  // contribution is the dwell -- everything else depends on a trajectory. From
  // the run: 32.3 s of measured non-dwell time, plus the 3.3 s the 220 step now
  // earns, is a 35.6 s base. The 30 s dwell puts the total near 65 s, mid-spec.
  //
  // Nothing caps it any more. The extension budget that used to hold a fully
  // extended run near 78 s is gone, so a slow oven inching toward its targets
  // can spend considerably longer than that above liquidus.
  //
  // The dwell contributes exactly its minDuration, not a range: `reached` is
  // unconditionally true for a dwell step (delta is zero, so isDwell), and the
  // step therefore advances the moment minMet. Its maxDuration is a watchdog
  // and nothing else. Measured at 14.1 s against a nominal 15.
  //
  // Note a 30 s dwell will not be free the way the 15 s one was: that held on
  // residual heat alone at 0% duty and was already declining by the end
  // (241.57 -> 240.73 -> 240.40), so the dwell regulator will now have to
  // supply real power partway through.
  //
  // THE SOAK, added 2026-09-04 after the first clean run.
  //
  // That run reflowed but the largest components went last, and the board read
  // cooler than the probe. Neither is a heat shortage: it spent roughly 120 s
  // above liquidus, well past the 60-90 s spec, and the heavy parts still only
  // just made it. It is a gradient. A part tied to a ground plane carries far
  // more mass than the laminate the probe sits on, so it lags, and the profile
  // gave it nowhere to catch up -- 25 to 170 to 220 to 245 with no plateau
  // anywhere.
  //
  // Time closes a thermal gradient; temperature does not. Raising the peak
  // further would cook the light parts and still leave the heavy ones behind.
  // A soak is the standard answer and this profile simply lacked one.
  //
  // It has to sit BELOW liquidus or it is not a soak. The obvious change --
  // lengthening the old 220 step -- would have held the board ABOVE 217 and
  // made the time-above-liquidus problem worse, so the 220 step becomes 200
  // and gains a dwell.
  //
  // 200 rather than the more usual 150-180: the smaller the gap left to the
  // liquidus crossing, the less gradient can redevelop on the final ramp, and
  // 200 is the top of the standard soak band with 17 degC of margin.
  //
  // Eight steps is MAX_STEPS exactly. There is no room for another.
  static const Step_t defaultSteps[] = {
    { 170,  72,  94, BOUND_RATE     },  // 0.72-0.94 degC/s
    { 200,  40,  70, BOUND_DURATION },  // 0.43-0.75 degC/s into the soak
    { 200,  60,  90, BOUND_DURATION },  // SOAK: equalise below liquidus
    { 245,  20,  45, BOUND_DURATION },  // 1.00-2.25 degC/s to peak.
                                        //
                                        // Measured 2026-09-04 with the door
                                        // insulated: 2.2 degC/s sustained at
                                        // full power through 196-217, against
                                        // the 0.88 on record before it. The
                                        // 55-90 s this replaces was derived
                                        // from that old figure and demanded
                                        // 0.50-0.82 -- throttling an element
                                        // that can do four times as much, and
                                        // holding the board above liquidus
                                        // while it did.
                                        //
                                        // Do NOT read the ~0.6 degC/s that a
                                        // naive reading of the 230-240 band
                                        // gives. Power there had been cut and
                                        // restored, and the element takes
                                        // ~10 s to answer: the temperature
                                        // FELL for ten seconds at 100% duty.
                                        // That is recovery lag, not ceiling.
    { 245,  30,  45, BOUND_DURATION },  // dwell at peak; holds for 30 s
    { 220,  21,  28, BOUND_DURATION },
    { 150,  30,  45, BOUND_DURATION },
    {  30,  20,  30, BOUND_DURATION },
  };
  activeProfile.stepCount = sizeof(defaultSteps)/sizeof(defaultSteps[0]);
  memcpy(activeProfile.steps, defaultSteps, sizeof(defaultSteps));
  for (uint8_t i = activeProfile.stepCount; i < MAX_STEPS; i++) {
    activeProfile.steps[i] = (Step_t){0,0,0,BOUND_DURATION};
  }
}

void getProfileKey(uint8_t profile, char * buffer){
  sprintf(buffer,"P%03d",profile);
}

bool saveParameters(uint8_t profile) {
  char buffer[10];
  getProfileKey(profile, buffer);
  Serial.print("Save Profile: ");
  Serial.println(buffer);
  PREF.putBytes(buffer, (uint8_t*)&activeProfile, sizeof(Profile_t));  

  return true;
}

bool loadParameters(uint8_t profile) {
  char buffer[10];
  getProfileKey(profile, buffer);
  Serial.print("Load Profile: ");
  Serial.println(buffer);

  size_t length = PREF.getBytesLength(buffer);
  
  if(length!=sizeof(Profile_t)){
    Serial.println("load default PROFILE!");
    makeDefaultProfile();  
  }
  else
  {
    PREF.getBytes(buffer, (uint8_t*)&activeProfile, length);

    // stepCount indexes steps[] in the control loop, and it came off flash.
    // A size match is not a validity guarantee -- a struct change that happened
    // to keep the same size would read as garbage -- so bound it here rather
    // than trusting it at the point where it is used to subscript.
    bool sane = (activeProfile.stepCount >= 1 &&
                 activeProfile.stepCount <= MAX_STEPS);
    for (uint8_t i = 0; sane && i < activeProfile.stepCount; i++) {
      const Step_t &st = activeProfile.steps[i];
      // boundIsRate selects how boundLo/Hi are interpreted, and a rate step
      // divides by them, so an unrecognised value or a zero bound has to be
      // caught here rather than in the control loop.
      if (st.boundIsRate > BOUND_RATE) sane = false;
      if (st.boundLo < 1 || st.boundHi < st.boundLo) sane = false;
    }
    if (!sane) {
      Serial.println("PROFILE implausible, loading default");
      makeDefaultProfile();
    }
  }  

  return true;
}

void loadProfileName(uint8_t id, char * buffer){
  Profile_t temp;
  char profilestring[10];
  getProfileKey(id, profilestring);
  Serial.println(profilestring);

  size_t length = PREF.getBytesLength(profilestring);
  
  if(length!=sizeof(Profile_t)){
    snprintf(buffer,PROFILE_NAME_LENGTH,"%s","--FREE--");
  }
  else
  {
    PREF.getBytes(profilestring, (uint8_t*)&temp, length);
    snprintf(buffer,PROFILE_NAME_LENGTH,"%s",temp.name);    
  }  
}

// The lag and the measure temperature are oven properties rather than profile
// properties, so they live in their own key and survive a profile change.
typedef struct {
  float   thermalLagSec;
  int16_t measureTempC;
  // Hold-regulator gains. Runtime-settable because tuning them by rebuilding
  // and reflashing costs a firmware cycle per iteration, and they are the only
  // gains in this controller -- the corridor law still has none.
  float   holdPBandC;
  float   holdTrimStep;
  float   calC2, calC1, calC0;
} Oven_t;

bool saveOven() {
  Oven_t o = { thermalLagSec, measureTempC, holdPBandC, holdTrimStep,
               calC2, calC1, calC0 };
  PREF.putBytes("OVEN", (uint8_t*)&o, sizeof(o));
  return true;
}

bool loadOven() {
  Oven_t o;
  if (PREF.getBytesLength("OVEN") != sizeof(o)) {
    Serial.println("load default OVEN");
    thermalLagSec = THERMAL_LAG_DEFAULT_S;
    measureTempC  = MEASURE_TEMP_DEFAULT_C;
    holdPBandC    = HOLD_PBAND_DEFAULT_C;
    holdTrimStep  = HOLD_TRIM_DEFAULT;
    calC2 = TEMP_CAL_C2_DEFAULT; calC1 = TEMP_CAL_C1_DEFAULT; calC0 = TEMP_CAL_C0_DEFAULT;
    return true;
  }
  PREF.getBytes("OVEN", (uint8_t*)&o, sizeof(o));

  // Both are read back off flash and both feed the control loop -- the lag
  // multiplies the rate term in the projection, so a garbage value here is a
  // profile that races to Complete. Bound them rather than trust them.
  thermalLagSec = (o.thermalLagSec >= THERMAL_LAG_MIN_S &&
                   o.thermalLagSec <= THERMAL_LAG_MAX_S)
                  ? o.thermalLagSec : THERMAL_LAG_DEFAULT_S;
  measureTempC  = (o.measureTempC >= MEASURE_TEMP_MIN_C &&
                   o.measureTempC <= MEASURE_TEMP_MAX_C)
                  ? o.measureTempC : MEASURE_TEMP_DEFAULT_C;
  holdPBandC    = (o.holdPBandC >= HOLD_PBAND_MIN_C &&
                   o.holdPBandC <= HOLD_PBAND_MAX_C)
                  ? o.holdPBandC : HOLD_PBAND_DEFAULT_C;
  holdTrimStep  = (o.holdTrimStep >= HOLD_TRIM_MIN &&
                   o.holdTrimStep <= HOLD_TRIM_MAX)
                  ? o.holdTrimStep : HOLD_TRIM_DEFAULT;
  // Same validation the setter uses. This came off flash and feeds every
  // temperature in the system, so it is not taken on trust either.
  if (calSane(o.calC2, o.calC1, o.calC0))
  { calC2 = o.calC2; calC1 = o.calC1; calC0 = o.calC0; }
  else
  {
    Serial.println("stored calibration rejected -- using default");
    calC2 = TEMP_CAL_C2_DEFAULT; calC1 = TEMP_CAL_C1_DEFAULT; calC0 = TEMP_CAL_C0_DEFAULT;
  }
  return true;
}


void factoryReset() {
  Serial.println("Factory reset");

  // Also drop the WiFi credentials. With no display and no encoder this is the
  // only way back to the setup portal if the oven is moved to a new network.
  wm.resetSettings();

  PREF.clear(); 
  // PREF.clear() takes the stored copy; this takes the live one, which would
  // otherwise keep working until the next boot.
  otaPassword = "";

  activeProfileId = 0;
  makeDefaultProfile();
  thermalLagSec = THERMAL_LAG_DEFAULT_S;
  measureTempC  = MEASURE_TEMP_DEFAULT_C;
  holdPBandC    = HOLD_PBAND_DEFAULT_C;
  holdTrimStep  = HOLD_TRIM_DEFAULT;
  calC2 = TEMP_CAL_C2_DEFAULT; calC1 = TEMP_CAL_C1_DEFAULT; calC0 = TEMP_CAL_C0_DEFAULT;
  measuredLagSec = 0.0f;
}

void saveLastUsedProfile() {
  PREF.putUChar("ProfileID",activeProfileId);
}

void loadLastUsedProfile() {
  activeProfileId = PREF.getUChar("ProfileID", 0);
  loadParameters(activeProfileId);
}

// Escapes the few characters that would otherwise break out of a JSON string.
// Profile names are user-supplied and go straight into /profiles output.
static String jsonEscape(const char *in) {
  String out;
  for (const char *c = in; *c; c++) {
    if (*c == '"' || *c == '\\') { out += '\\'; out += *c; }
    else if (*c >= 0x20)           { out += *c; }
  }
  return out;
}

void setup() {
  //Debug
  Serial.begin(115200);

  // First thing on the wire, so a serial monitor attached for the IP address
  // also answers "which build is this".
  Serial.println();
  Serial.println("ReflowController " FW_VERSION "  build " FW_BUILD);
  
  //INIT Temps
  // 12dB attenuation, specified linear to ~2450mV. With the 1.5:1 divider (see
  // TEMP_DIVIDER_RATIO) that covers ~367degC, so the whole reflow range sits
  // comfortably inside the linear region.
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(TEMP_ADC_CH, TEMP_ADC_ATTEN);
  esp_adc_cal_characterize(ADC_UNIT_1, TEMP_ADC_ATTEN, ADC_WIDTH_BIT_12, 1100, &adcChars);
  
  //init relay output
  pinMode(HEATER, OUTPUT);
  digitalWrite(HEATER,LOW);
  esp_timer_create_args_t _timerConfig;
  _timerConfig.callback = (void (*)(void*))relayDriver;
  _timerConfig.dispatch_method = ESP_TIMER_TASK;
  _timerConfig.name = "relayDriver";
  esp_timer_create(&_timerConfig, &RelayTimer);
  esp_timer_start_periodic(RelayTimer, RELAY_TICK_MS * 1000);

  //beep
  pinMode(BUZZER,OUTPUT);
  digitalWrite(BUZZER,LOW);
  // The third argument is duty resolution in bits, not a channel index. It was
  // 0, which is out of range, so this call failed and configured nothing --
  // harmless only because ledcWriteTone() re-runs the timer config itself. 10
  // bits is what ledcWriteTone() uses, so matching it here means the boot
  // configuration and the beep agree and no tone has to reconfigure anything.
  ledcSetup(0,1000,BUZZER_LEDC_BITS);
  // ledcSetup() configures the *timer* only; the channel stays uninitialised.
  // The first thing beep() does is ledcAttachPin(), which reads the channel's
  // duty back before configuring it -- on an uninitialised channel that read
  // is what logged "ledc_get_duty(745): LEDC is not initialized". There is no
  // Arduino entry point that configures a channel without that read, so go to
  // the IDF driver once here. Attaching the pin at duty 0 is silent, and every
  // later ledcAttachPin() then finds a channel it can legitimately read.
  ledc_channel_config_t buzzerChannel = {};
  buzzerChannel.speed_mode = BUZZER_LEDC_MODE;
  buzzerChannel.channel    = BUZZER_LEDC_CHANNEL;
  buzzerChannel.intr_type  = LEDC_INTR_DISABLE;
  buzzerChannel.timer_sel  = BUZZER_LEDC_TIMER;
  buzzerChannel.gpio_num   = BUZZER;
  buzzerChannel.duty       = 0;
  buzzerChannel.hpoint     = 0;
  ledc_channel_config(&buzzerChannel);

  //LEDs:
  RGBLED.begin(RGB_CLK,RGB_SDO,RGB_SDO,0);  
  setLEDRGBBColor(0,0,0);
  
  //Preferences init
  PREF.begin("REFLOW");
  otaPassword = PREF.getString("otapw", "");
  loadLastUsedProfile();
  loadOven();
  
  //init Wifi:
  // Provisioning used to be an encoder-driven network scan with an on-screen
  // keyboard. Headless that is unreachable, so WiFiManager raises its own
  // access point when it has no working credentials: join REFLOW_HOSTNAME and
  // pick a network from the captive portal.
  WiFi.mode(WIFI_STA);
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  wm.setHostname(REFLOW_HOSTNAME);

  if (wm.autoConnect(REFLOW_HOSTNAME)) {
    Serial.println();
    Serial.print("WiFi connected to: "); Serial.println(WiFi.SSID());
    // With no display, serial and mDNS are the only ways to find the oven.
    Serial.print("Web UI:  http://"); Serial.println(WiFi.localIP());
    Serial.print("     or: http://" REFLOW_HOSTNAME ".local/");
    Serial.println();
  }
  else {
    Serial.println();
    Serial.println("WiFi setup portal timed out -- no network.");
    Serial.println("The oven cannot be controlled until it joins one:");
    Serial.println("power cycle to reopen the portal at SSID " REFLOW_HOSTNAME);
    Serial.println();
  }
 
  //init Webserver
  if (MDNS.begin(REFLOW_HOSTNAME)) {
      Serial.println("MDNS responder started");
  }
  server.on("/", []() {
    server.sendHeader("Cache-Control","no-cache");
    server.send(200, "text/html", ROOT_HTML);
  });
  server.on("/status", []() {
    server.sendHeader("Cache-Control","no-cache");
    char buffer[640];   // grew for "warning"
    unsigned long time = (esp_timer_get_time()-cycleStartTime)/1000;
    snprintf(buffer,sizeof(buffer),
      "{\"time\": %lu, \"temp\": %.2f, \"dt\": %.2f, \"setpoint\": %.2f,"
      " \"low\": %.2f, \"high\": %.2f, \"power\": %.2f, \"step\": %d,"
      " \"steps\": %d, \"lag\": %.1f, \"openDoor\": %d, \"heating\": %d,"
      " \"extending\": %d, \"extended\": %.0f, \"mcu\": %.1f,"
      " \"state\": \"%s\", \"fault\": \"%s\", \"warning\": \"%s\"}",
      time, aktSystemTemperature, aktSystemTemperatureRamp, heaterSetpoint,
      corridorLow, corridorHigh,
      (float)powerHeater*100.0f/255.0f, activeStep, activeProfile.stepCount,
      thermalLagSec, openDoorPrompt ? 1 : 0, heaterOn ? 1 : 0,
      stepExtending ? 1 : 0, runExtendUsed_s, mcuTemp_C,
      currentStateToString(), globalErrorText, globalWarningText);
    server.send(200, "application/json", buffer);
  });
  // Slot directory for the profile picker: every slot, named or free.
  server.on("/profiles", []() {
    server.sendHeader("Cache-Control","no-cache");
    String out = "{\"active\": " + String(activeProfileId) + ", \"profiles\": [";
    for (uint8_t i = 0; i <= MAX_PROFILES; i++) {
      char name[PROFILE_NAME_LENGTH];
      loadProfileName(i, name);
      if (i) out += ",";
      out += "{\"id\": " + String(i) + ", \"name\": \"" + jsonEscape(name) + "\"}";
    }
    out += "]}";
    server.send(200, "application/json", out);
  });
  // Everything the menu used to let you edit, in one document.
  // Everything the menu used to let you edit, in one document. The PID gains
  // and autotune parameters that used to live here are gone with the PID.
  // Firmware upload. On the page's own server, not the action server on 8080,
  // so the POST is same-origin: no preflight, and the Authorization header
  // rides along without CORS having to be talked into allowing credentials.
  //
  // Note what deliberately does NOT happen during an upload: the control loop
  // stops being serviced, so the relay heartbeat goes stale and relayDriver()
  // holds the contact open within RELAY_HEARTBEAT_HOLD_MS. That is the correct
  // behaviour and is left alone -- the element must not be driven while the
  // firmware driving it is being replaced.
  server.on("/update", HTTP_POST,
    []() {   // request complete
      if (otaReject)
      {
        server.send(409, "text/plain", otaReject);
        otaReject = nullptr;
        return;
      }
      if (Update.hasError())
      {
        server.send(500, "text/plain", Update.errorString());
        return;
      }
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", "OK -- rebooting into the new firmware");
      Serial.println("OTA complete, restarting");
      delay(200);
      ESP.restart();
    },
    []() {   // one call per chunk, plus a START and an END
      HTTPUpload &up = server.upload();

      if (up.status == UPLOAD_FILE_START)
      {
        otaReject = nullptr;

        // Both gates belong HERE and not in the completion handler above.
        // This callback runs first and writes flash as it goes, so a check
        // deferred to completion would refuse the request only after the
        // running firmware had already been overwritten.
        //
        // Order matters too: the blocked-reason test covers the no-password
        // case, and it has to run before authenticate(), which would happily
        // match an empty password against a client sending "ota:".
        const char *why = otaBlockedReason();
        if (why) { otaReject = why; return; }

        if (!server.authenticate(OTA_USER, otaPassword.c_str()))
        {
          otaReject = "Bad OTA password";
          Serial.println("OTA refused: bad password");
          return;
        }

        Serial.printf("OTA start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
        {
          otaReject = Update.errorString();
          return;
        }
      }
      else if (up.status == UPLOAD_FILE_WRITE)
      {
        if (otaReject) return;   // still drained by the parser, just not written
        if (Update.write(up.buf, up.currentSize) != up.currentSize)
        {
          otaReject = Update.errorString();
          Update.abort();
        }
      }
      else if (up.status == UPLOAD_FILE_END)
      {
        if (otaReject) { Update.abort(); return; }
        if (!Update.end(true)) otaReject = Update.errorString();
      }
      else if (up.status == UPLOAD_FILE_ABORTED)
      {
        Update.abort();
        otaReject = "Upload aborted";
      }
    });

  server.on("/config", []() {
    server.sendHeader("Cache-Control","no-cache");
    String out = "{\"profile\": {\"id\": " + String(activeProfileId) +
                 ", \"name\": \"" + jsonEscape(activeProfile.name) +
                 "\", \"steps\": [";
    for (uint8_t i = 0; i < activeProfile.stepCount; i++) {
      if (i) out += ",";
      const Step_t &st = activeProfile.steps[i];
      bool isRate = (st.boundIsRate == BOUND_RATE);
      out += "{\"targetTemp\": " + String(st.targetTemp) +
             ", \"bound\": \"" + (isRate ? "rate" : "duration") + "\"" +
             ", \"lo\": " + (isRate ? String(st.boundLo / RATE_SCALE, 2)
                                     : String(st.boundLo)) +
             ", \"hi\": " + (isRate ? String(st.boundHi / RATE_SCALE, 2)
                                     : String(st.boundHi)) + "}";
    }
    out += "]}, \"maxSteps\": " + String(MAX_STEPS) +
           ", \"oven\": {\"thermalLag\": " + String(thermalLagSec, 1) +
           ", \"measureTemp\": " + String(measureTempC) +
           ", \"measuredLag\": " + String(measuredLagSec, 1) +
           ", \"startMaxTemp\": " + String(PROFILE_START_MAX_C) +
           ", \"holdPBand\": " + String(holdPBandC, 1) +
           ", \"holdTrim\": " + String(holdTrimStep, 2) +
           ", \"calC2\": " + String(calC2, 9) +
           ", \"calC1\": " + String(calC1, 6) +
           ", \"calC0\": " + String(calC0, 4) +
           "}, \"otaSet\": " + String(otaPassword.length() ? 1 : 0) +
           ", \"version\": \"" FW_VERSION "\"" +
           ", \"build\": \"" FW_BUILD "\"}";
    server.send(200, "application/json", out);
  });
  serverAction.on("/start", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    if(currentState != Ready || globalError)
    {
      serverAction.send(409, "text/plain", "Controller busy");
      return;
    }
    // A hot chamber is not something to reach into, and it has to be open to
    // be loaded. The profile-consistency half of this is BOUND_RATE's job now,
    // not the lockout's -- see PROFILE_START_MAX_C.
    if(aktSystemTemperature > PROFILE_START_MAX_C)
    {
      char msg[96];
      snprintf(msg, sizeof(msg),
               "Chamber too hot to start: %.0fC, must be below %dC",
               aktSystemTemperature, PROFILE_START_MAX_C);
      serverAction.send(409, "text/plain", msg);
      return;
    }
    //Start Reflow!
    cycleStartTime = esp_timer_get_time();
    currentState = Running;
    serverAction.send(200, "text/plain", "OK");
  });
  serverAction.on("/stop", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    // The old firmware had a CoolDown phase to hand off to, so a first /stop
    // aborted into it and a second acknowledged completion. Cooling is now just
    // the profile's trailing steps, so there is nothing to hand off to: stop
    // means stop, with the heater off either way.
    bool ok=false;
    if (currentState == Complete)
    {
      currentState = Ready;
      ok=true;
    }
    else if (currentState > ProcessStart)
    {
      currentState = Complete;
      ok=true;
    }
    if(ok){
      serverAction.send(200, "text/plain", "OK");      
    }
    else
    {
      serverAction.send(200, "text/plain", "ERROR");
    }
  });
  // These replace menu items that the display rework removed. Without them
  // manual heating, profile switching and factory reset would have no trigger
  // at all. Richer profile editing follows on the main server.
  // Set or change the OTA password.
  //
  // Changing an existing one requires the current one. The very first set
  // cannot be authenticated -- there is nothing yet to authenticate against --
  // so do it once, deliberately, on a network you trust. Until then OTA is
  // refused outright, which is the safe direction for the failure.
  serverAction.on("/otapass", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");

    if (otaPassword.length() > 0 && serverAction.arg("old") != otaPassword)
    {
      serverAction.send(403, "text/plain", "Wrong current password");
      return;
    }

    String next = serverAction.arg("pass");
    if (next.length() < OTA_PASS_MIN_LEN)
    {
      char msg[64];
      snprintf(msg, sizeof(msg), "Password must be at least %d characters",
               OTA_PASS_MIN_LEN);
      serverAction.send(400, "text/plain", msg);
      return;
    }

    otaPassword = next;
    PREF.putString("otapw", otaPassword);
    Serial.println("OTA password set");
    serverAction.send(200, "text/plain", "OK");
  });

  serverAction.on("/manual", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    if(globalError || (currentState != Ready && currentState != Manual))
    {
      serverAction.send(409, "text/plain", "ERROR");
      return;
    }
    long power = serverAction.arg("power").toInt();
    if(power < 0) power = 0;
    if(power > 100) power = 100;
    manualPower = power;
    currentState = (power > 0) ? Manual : Ready;
    serverAction.send(200, "text/plain", "OK");
  });
  serverAction.on("/profile/load", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    long id = serverAction.arg("id").toInt();
    if(currentState != Ready || id < 0 || id > MAX_PROFILES)
    {
      serverAction.send(409, "text/plain", "ERROR");
      return;
    }
    loadProfile(id);
    serverAction.send(200, "text/plain", "OK");
  });
  serverAction.on("/profile/save", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    long id = serverAction.arg("id").toInt();
    if(currentState != Ready || id < 0 || id > MAX_PROFILES)
    {
      serverAction.send(409, "text/plain", "ERROR");
      return;
    }
    saveProfile(id);
    serverAction.send(200, "text/plain", "OK");
  });
  // Sets the oven's own parameters, as opposed to a profile's. Persisted
  // immediately: unlike /profile/edit there is no slot to save them into.
  serverAction.on("/oven", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    if(currentState != Ready)
    {
      serverAction.send(409, "text/plain", "ERROR");
      return;
    }
    if(serverAction.hasArg("thermalLag"))
    {
      float v = serverAction.arg("thermalLag").toFloat();
      if (v < THERMAL_LAG_MIN_S) v = THERMAL_LAG_MIN_S;
      if (v > THERMAL_LAG_MAX_S) v = THERMAL_LAG_MAX_S;
      thermalLagSec = v;
    }
    if(serverAction.hasArg("measureTemp"))
    {
      long v = serverAction.arg("measureTemp").toInt();
      if (v < MEASURE_TEMP_MIN_C) v = MEASURE_TEMP_MIN_C;
      if (v > MEASURE_TEMP_MAX_C) v = MEASURE_TEMP_MAX_C;
      measureTempC = (int16_t)v;
    }
    if(serverAction.hasArg("holdPBand"))
    {
      float v = serverAction.arg("holdPBand").toFloat();
      if (v < HOLD_PBAND_MIN_C) v = HOLD_PBAND_MIN_C;
      if (v > HOLD_PBAND_MAX_C) v = HOLD_PBAND_MAX_C;
      holdPBandC = v;
    }
    if(serverAction.hasArg("holdTrim"))
    {
      float v = serverAction.arg("holdTrim").toFloat();
      if (v < HOLD_TRIM_MIN) v = HOLD_TRIM_MIN;
      if (v > HOLD_TRIM_MAX) v = HOLD_TRIM_MAX;
      holdTrimStep = v;
    }
    // Sensor calibration. All three together and all-or-nothing: the
    // coefficients only mean anything as a set, and half-applying them would
    // leave a curve nobody chose. Unspecified terms keep their current value,
    // so one can be nudged without restating the others.
    if(serverAction.hasArg("calC2") || serverAction.hasArg("calC1") ||
       serverAction.hasArg("calC0"))
    {
      float n2 = serverAction.hasArg("calC2") ? serverAction.arg("calC2").toFloat() : calC2;
      float n1 = serverAction.hasArg("calC1") ? serverAction.arg("calC1").toFloat() : calC1;
      float n0 = serverAction.hasArg("calC0") ? serverAction.arg("calC0").toFloat() : calC0;
      if (!calSane(n2, n1, n0))
      {
        // Refused rather than clamped. There is no safe way to nudge a bad
        // calibration into a good one, and a silently clamped curve reading
        // low would run the oven hot without saying so.
        serverAction.send(409, "text/plain",
          "Calibration rejected: must rise across 0-350C and stay within 30C "
          "of the raw reading");
        return;
      }
      calC2 = n2; calC1 = n1; calC0 = n0;
    }
    saveOven();
    serverAction.send(200, "text/plain", "OK");
  });
  // Runs the lag measurement cycle. This heats the oven to measureTemp and
  // then deliberately lets it overshoot, so it is a real oven cycle with the
  // same lockout as a profile run.
  serverAction.on("/measurelag", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    if(currentState != Ready || globalError)
    {
      serverAction.send(409, "text/plain", "Controller busy");
      return;
    }
    // Gated on the same limit as /start. The coast depends on how long the
    // element has been driven, so measuring from a warm chamber measures
    // something other than what a profile run will experience.
    if(aktSystemTemperature > PROFILE_START_MAX_C)
    {
      char msg[96];
      snprintf(msg, sizeof(msg),
               "Chamber too hot to measure: %.0fC, must be below %dC",
               aktSystemTemperature, PROFILE_START_MAX_C);
      serverAction.send(409, "text/plain", msg);
      return;
    }
    cycleStartTime = esp_timer_get_time();
    currentState = MeasureLag;
    serverAction.send(200, "text/plain", "OK");
  });
  // Hold a fixed temperature until stopped. See HOLD_TEMP_MIN_C.
  //
  // Accepted from Hold as well as Ready, and that is the point: a probe is
  // characterised by stepping through levels, so changing the setpoint must
  // not require stopping and re-entering from a cold chamber.
  //
  // Deliberately NOT gated on PROFILE_START_MAX_C. That limit exists because
  // a profile run must not start from a chamber whose stored heat it has not
  // accounted for, and because loading a board means reaching into the oven.
  // Neither applies here: nothing is loaded mid-sweep, and a hold has no
  // schedule to invalidate -- the second level of any sweep is entered warm
  // by design.
  serverAction.on("/hold", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    if((currentState != Ready && currentState != Hold) || globalError)
    {
      serverAction.send(409, "text/plain", "Controller busy");
      return;
    }
    if(!serverAction.hasArg("temp"))
    {
      serverAction.send(400, "text/plain", "temp required");
      return;
    }
    long v = serverAction.arg("temp").toInt();
    if(v < HOLD_TEMP_MIN_C || v > HOLD_TEMP_MAX_C)
    {
      char msg[96];
      snprintf(msg, sizeof(msg), "Hold temp must be %d-%dC",
               HOLD_TEMP_MIN_C, HOLD_TEMP_MAX_C);
      serverAction.send(409, "text/plain", msg);
      return;
    }
    holdTempC    = (int16_t)v;
    holdSetAt_ms = esp_timer_get_time()/1000;   // restart the timeout
    if(currentState != Hold)
    {
      // Fresh entry only. Stepping the setpoint mid-sweep keeps the trim,
      // because the power an equilibrium needs at the last level is a better
      // starting guess for the next one than zero is.
      holdTrim       = 0.0f;
      cycleStartTime = esp_timer_get_time();
      currentState   = Hold;
    }
    Serial.printf("Hold at %dC\n", holdTempC);
    serverAction.send(200, "text/plain", "OK");
  });
  serverAction.on("/factoryreset", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    if(currentState != Ready)
    {
      serverAction.send(409, "text/plain", "ERROR");
      return;
    }
    factoryReset();
    serverAction.send(200, "text/plain", "OK");
  });
  // Edits the in-memory profile. Not persisted until /profile/save, matching
  // how the menu behaved: edit freely, then choose a slot to write to.
  //
  // Steps arrive as one "target,lo,hi[,unit]" record per step, semicolon
  // separated. The optional unit is "s" for seconds (the default) or "r" for
  // degC/s, which is what selects the step's bound type:
  //   steps=170,0.72,0.94,r;220,31,64;240,21,28;240,15,25
  // Both bounds ascend in whatever unit they use, as a datasheet prints them.
  // The whole list is replaced or none of it is -- a half-applied profile would
  // be a corridor that no longer joins up.
  serverAction.on("/profile/edit", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    if(currentState != Ready)
    {
      serverAction.send(409, "text/plain", "ERROR");
      return;
    }
    if(serverAction.hasArg("name"))
    {
      snprintf(activeProfile.name, PROFILE_NAME_LENGTH, "%s", serverAction.arg("name").c_str());
    }
    if(serverAction.hasArg("steps"))
    {
      Step_t parsed[MAX_STEPS];
      uint8_t count = 0;
      String in = serverAction.arg("steps");
      int pos = 0;

      while (pos < (int)in.length() && count < MAX_STEPS)
      {
        int end = in.indexOf(';', pos);
        if (end < 0) end = in.length();
        String field = in.substring(pos, end);
        pos = end + 1;

        field.trim();
        if (field.length() == 0) continue;

        int c1 = field.indexOf(',');
        int c2 = (c1 < 0) ? -1 : field.indexOf(',', c1 + 1);
        if (c1 < 0 || c2 < 0)
        {
          serverAction.send(400, "text/plain", "ERROR");
          return;
        }
        int c3 = field.indexOf(',', c2 + 1);   // optional unit

        String loStr = field.substring(c1 + 1, c2);
        String hiStr = (c3 < 0) ? field.substring(c2 + 1)
                                : field.substring(c2 + 1, c3);
        String unit  = (c3 < 0) ? String("s") : field.substring(c3 + 1);
        unit.toLowerCase();

        bool isRate = unit.startsWith("r");
        if (!isRate && !unit.startsWith("s"))
        {
          serverAction.send(400, "text/plain",
                            "Step unit must be 's' (seconds) or 'r' (degC/s)");
          return;
        }

        long target = field.substring(0, c1).toInt();
        long lo, hi;
        if (isRate)
        {
          // degC/s on the wire, hundredths in the struct.
          lo = lroundf(loStr.toFloat() * RATE_SCALE);
          hi = lroundf(hiStr.toFloat() * RATE_SCALE);
          if (lo < RATE_MIN_CDS) lo = RATE_MIN_CDS;
          if (lo > RATE_MAX_CDS) lo = RATE_MAX_CDS;
          if (hi < lo) hi = lo;              // an inverted corridor has no inside
          if (hi > RATE_MAX_CDS) hi = RATE_MAX_CDS;
        }
        else
        {
          lo = loStr.toInt();
          hi = hiStr.toInt();
          if (lo < DURATION_MIN_S) lo = DURATION_MIN_S;
          if (lo > DURATION_MAX_S) lo = DURATION_MAX_S;
          if (hi < lo) hi = lo;
          if (hi > DURATION_MAX_S) hi = DURATION_MAX_S;
        }

        // Held below TEMP_PLAUSIBLE_MAX_C: a profile must not be able to ask
        // for a temperature that would trip the sensor fault on the way to
        // reaching it.
        if (target <   0) target =   0;
        if (target > 280) target = 280;

        parsed[count].targetTemp  = (int16_t)target;
        parsed[count].boundLo     = (int16_t)lo;
        parsed[count].boundHi     = (int16_t)hi;
        parsed[count].boundIsRate = isRate ? BOUND_RATE : BOUND_DURATION;
        count++;
      }

      if (count == 0)
      {
        serverAction.send(400, "text/plain", "ERROR");
        return;
      }

      activeProfile.stepCount = count;
      for (uint8_t i = 0; i < MAX_STEPS; i++) {
        activeProfile.steps[i] = (i < count) ? parsed[i] : (Step_t){0,0,0,BOUND_DURATION};
      }
    }
    serverAction.send(200, "text/plain", "OK");
  });
  server.onNotFound([](){
    server.send(404, "text/plain", "404 :-(");
  });
  serverAction.onNotFound([](){
    serverAction.send(404, "text/plain", "404 :-(");
  });
  
  server.begin();  
  serverAction.begin();  
  
  currentState = Ready;
}

void loop()
{
  uint64_t time_ms = esp_timer_get_time()/1000;
  static uint64_t lastbeep=time_ms;
  static uint64_t lastreadTemp=time_ms;
  static uint64_t lastMcuTemp=0;   // 0 means "never sampled", see below
  static uint64_t lastRGBupdate=time_ms;
  static uint64_t lastControlloopupdate=time_ms;

  static uint8_t  beepcount =1; //start beep

  // --------------------------------------------------------------------------
  // Handle internet requests
  //
  server.handleClient();  
  serverAction.handleClient();  

  // --------------------------------------------------------------------------
  // Do the beep if needed
  //
  if(time_ms >= (lastbeep + 500))
  {
    static boolean isbeeping=false;
    lastbeep=time_ms;
    if(isbeeping==false)
    {
      if(beepcount > 0)
      {
        beepcount--;
        ledcAttachPin(BUZZER, 0);
        ledcWriteTone(0, 1000);
        isbeeping=true;
      }
    }
    else
    {
      ledcDetachPin(BUZZER);
      isbeeping=false;
    }
  }

  // --------------------------------------------------------------------------
  // Controller die temperature
  //
  // The lastMcuTemp==0 arm fires on the very first pass so /status never
  // publishes the initialiser. Seeding lastMcuTemp to
  // time_ms - MCU_TEMP_INTERVAL_MS would do the same job but underflows the
  // unsigned subtraction if setup() ever finishes in under 2 s, and the
  // comparison would then never come true again.
  if(lastMcuTemp == 0 || time_ms >= (lastMcuTemp + MCU_TEMP_INTERVAL_MS))
  {
    lastMcuTemp = time_ms;
    mcuTemp_C   = temperatureRead();
  }

  // --------------------------------------------------------------------------
  // Temp messurment and averageing
  //
  if(time_ms >= (lastreadTemp + READ_TEMP_INTERVAL_MS))
  {
    lastreadTemp+=READ_TEMP_INTERVAL_MS; //interval should be regularly
    float reading = readTemperature();

    static float average[READ_TEMP_AVERAGE_COUNT];
    static bool  faulted[READ_TEMP_AVERAGE_COUNT];
    static uint8_t pointer =0;
    static bool primed = false;

    // Prime both filters from the first real reading rather than letting them
    // fill from zero.
    //
    // Zero-filled, the rolling average reads a tenth of the true temperature
    // on the first pass and climbs to it over a second, and the ramp ring
    // reports that whole climb as a rate. The ramp is only cosmetic for that
    // second -- the projection clamps it -- but the *temperature* is not: the
    // 50 degC start interlock reads it, and a low reading opens the gate rather
    // than closing it. The sequence that matters is a hot oven, a power cycle
    // to clear a fault, the browser reconnecting, and Start pressed straight
    // away; without this the oven would read cold and accept it.
    if (!primed)
    {
      primed = true;
      for (int i=0;i<READ_TEMP_AVERAGE_COUNT;i++) { average[i]=reading; faulted[i]=false; }
      aktSystemTemperature = reading;
    }

    average[pointer]=reading;
    faulted[pointer]=(reading > TEMP_PLAUSIBLE_MAX_C);
    pointer=(pointer+1)%READ_TEMP_AVERAGE_COUNT;

    float sum =0;
    uint8_t fault_count=0;
    for (int i=0;i<READ_TEMP_AVERAGE_COUNT;i++)
    {
      sum +=average[i];
      if (faulted[i])
      {
        fault_count++;
      }
    }

    // Majority vote, as before: one noisy sample must not abort a reflow.
    if (fault_count>READ_TEMP_AVERAGE_COUNT/2) {
      reportError("Temp Sensor: Open Circuit or out of range");
    }

    aktSystemTemperature = sum/READ_TEMP_AVERAGE_COUNT;


    static float averagees[1000/READ_TEMP_INTERVAL_MS];
    static uint16_t p=0;
    static bool rampPrimed = false;

    if (!rampPrimed)
    {
      rampPrimed = true;
      for (int i=0;i<1000/READ_TEMP_INTERVAL_MS;i++) averagees[i]=aktSystemTemperature;
    }

    aktSystemTemperatureRamp = aktSystemTemperature - averagees[p];

    averagees[p]=aktSystemTemperature;
    p=(p+1)%(1000/READ_TEMP_INTERVAL_MS);

    // The same difference taken across a whole relay window. See controlRamp.
    #define CTRL_RAMP_SLOTS (RELAY_WINDOW_MS/READ_TEMP_INTERVAL_MS)
    static float    ctrlRing[CTRL_RAMP_SLOTS];
    static uint16_t cp = 0;
    static bool     ctrlPrimed = false;

    if (!ctrlPrimed)
    {
      ctrlPrimed = true;
      for (int i=0;i<CTRL_RAMP_SLOTS;i++) ctrlRing[i]=aktSystemTemperature;
    }

    controlRamp = (aktSystemTemperature - ctrlRing[cp])
                  / (RELAY_WINDOW_MS / 1000.0f);
    ctrlRing[cp]=aktSystemTemperature;
    cp=(cp+1)%CTRL_RAMP_SLOTS;
    
  }

  // --------------------------------------------------------------------------
  // Show temp as RGB color
  //
  if(time_ms >= (lastRGBupdate + 1000))
  {
    lastRGBupdate=time_ms;

    if (globalError)
    {
      // Flash red: the only local indication left that the oven has faulted.
      static bool on=false;
      on=!on;
      setLEDRGBBColor(on?RGB_LED_BRITHNESS_1TO255:0,0,0);
    }
    else
    {
      float t=(300-aktSystemTemperature)/300.0/2;
      setLEDRGBBColor(RGB_LED_BRITHNESS_1TO255 * Hue_2_RGB( 0, 1, t+0.33 ),RGB_LED_BRITHNESS_1TO255 * Hue_2_RGB( 0, 1, t ),RGB_LED_BRITHNESS_1TO255 * Hue_2_RGB( 0, 1, t-0.33 ));
    }
  }

  // --------------------------------------------------------------------------
  // control loop
  //
  if(time_ms >= (lastControlloopupdate + 100))
  {
    lastControlloopupdate+=100; 

    // Tell relayDriver() we are still here. Stamped before the work rather
    // than after, so a fault raised below still leaves a fresh heartbeat and
    // reports as itself instead of as a dead control loop.
    controlHeartbeat_ms = time_ms;

    static State previousState= None; // sentinel: never equals a real state
    boolean stateChanged=false;
    if (currentState != previousState)
    {
      stateChanged = true;
      previousState = currentState;
      // Any transition ends an extension. This has to live here rather than in
      // the Running block, because /stop assigns currentState = Complete
      // directly and never passes through it -- which would otherwise leave
      // /status claiming an extension that stopped when the run did.
      stepExtending = false;
    }
    static uint64_t stepStartedTime_ms = time_ms;
    static uint32_t lastLevelWindow     = 0;
    static bool     cooldownAnnounced   = false;
    static bool     awaitingPeakBeep    = false;
    static uint64_t coolStepEntered_ms  = 0;
    static uint64_t notClimbingSince_ms = 0;
    // Step extension bookkeeping. The gain reference rolls for the whole step,
    // not just during an extension, so the eligibility decision at maxDuration
    // already has a full window of history behind it rather than needing a
    // separate and weaker entry test.
    static uint64_t heatStallRef_ms     = 0;
    static float    heatStallRefTemp    = 0.0f;
    static uint64_t stepGainRef_ms      = 0;
    static float    stepGainRefTemp     = 0.0f;
    static bool     stepGaining         = true;  // verdict, one per window

    // A latched fault must not be able to reach Complete along the success
    // path. reportError() deliberately does not touch currentState -- it is
    // also called from relayDriver(), which runs on the esp_timer task, so
    // writing state from there would be a cross-task write. The consequence
    // was that a faulted run stayed in Running, marched its remaining steps,
    // fired the ordinary three-beep door prompt, and finished with the single
    // beep a good run gives: away from the browser there was nothing to tell
    // success and failure apart but the RGB LED.
    //
    // Ahead of the Running block rather than inside it, so the step machinery
    // does not get one more pass after the decision to stop has been made.
    if (globalError && (currentState == Running || currentState == Hold))
    {
      powerLevel   = 0;
      currentState = Complete;
      beepcount    = 6;   // distinct from the single success beep
    }

    if (currentState == Running)
    {
      if (stateChanged)
      {
        // Warnings are per-run. Unlike globalError they are not a lockout, so
        // clearing them here costs nothing and stops a previous run's message
        // being read as this one's.
        globalWarning     = false;
        globalWarningText = "";
        activeStep         = 0;
        stepStartTemp      = aktSystemTemperature;
        stepStartedTime_ms = time_ms;
        lastLevelWindow    = relayWindowSeq;
        // Start at full duty, not at zero.
        //
        // The oven's thermal inertia is highest when everything in it is cold,
        // and the corridor law used to have to discover that: from level 0 it
        // nudged up over two windows before the element saw full power, with
        // the first of those windows spent entirely off. Beginning at the top
        // costs nothing -- the very first thing any profile does is a ramp from
        // ambient, which wants all the heat there is -- and the approach clamp
        // still takes it straight back to zero the moment arrival is assured.
        powerLevel         = POWER_LEVELS - 1;
        cooldownAnnounced  = false;
        awaitingPeakBeep   = false;
        stepGainRef_ms     = time_ms;
        stepGainRefTemp    = aktSystemTemperature;
        stepGaining        = true;
        // Run-scoped, so these reset here and nowhere else.
        runExtendUsed_s    = 0.0f;
      }

      const Step_t &step = activeProfile.steps[activeStep];

      // Signed size of the step. dir is +1 for a heating step and -1 for a
      // cooling one, and is the only place the two differ: everything below is
      // computed as a fraction of delta, which normalises the sign away.
      float delta = (float)step.targetTemp - stepStartTemp;
      float dir   = (delta >= 0.0f) ? 1.0f : -1.0f;

      float elapsed_s = (time_ms - stepStartedTime_ms) / 1000.0f;

      // The step's duration window, stated or derived from its rate bounds.
      // Recomputed each pass, but delta only changes at step entry, so this is
      // constant across a step.
      float minDur, maxDur;
      deriveStepDurations(step, delta, &minDur, &maxDur);

      // Where the corridor says the oven should be by now, as a fraction of the
      // step. Reaching the target sooner than minDur is "ahead", later than
      // maxDur is "behind".
      float aheadFrac  = (minDur > 0.0f) ? elapsed_s / minDur : 1.0f;
      float behindFrac = (maxDur > 0.0f) ? elapsed_s / maxDur : 1.0f;
      if (aheadFrac  > 1.0f) aheadFrac  = 1.0f;
      if (behindFrac > 1.0f) behindFrac = 1.0f;

      float aheadTemp  = stepStartTemp + delta * aheadFrac;
      float behindTemp = stepStartTemp + delta * behindFrac;

      corridorLow    = (aheadTemp < behindTemp) ? aheadTemp : behindTemp;
      corridorHigh   = (aheadTemp < behindTemp) ? behindTemp : aheadTemp;
      heaterSetpoint = (corridorLow + corridorHigh) / 2.0f;

      // A step with no temperature change (a dwell) has no corridor to be
      // inside; it regulates on the projection instead, below.
      bool isDwell = (fabsf(delta) < 0.5f);

      // Where the oven ends up if the relay opens now. This is what the
      // controller steers, not the present temperature: with a mechanical
      // relay and an element hotter than the air, by the time the thermometer
      // reads the target the heat that overshoots it has already been
      // delivered. See thermalLagSec and THERMAL_LAG_DEFAULT_S.
      float projectionRate = controlRamp;
      if (projectionRate >  PROJECTION_MAX_RATE_C_S) projectionRate =  PROJECTION_MAX_RATE_C_S;
      if (projectionRate < -PROJECTION_MAX_RATE_C_S) projectionRate = -PROJECTION_MAX_RATE_C_S;
      float projectedTemp = aktSystemTemperature + projectionRate * thermalLagSec;

      // Re-evaluate once per relay window. The level is what the actuator can
      // actually express, so moving it faster than the window would just slew
      // it across the whole range before one change reached the oven.
      if (relayWindowSeq - lastLevelWindow >= CONTROL_WINDOWS)
      {
        lastLevelWindow = relayWindowSeq;

        int8_t nudge = 0; // in corridor terms: +1 means "make more progress"

        if (isDwell)
        {
          // Hold the target against the projection. Without this a dwell step
          // simply froze the level wherever the previous step left it and let
          // the durations run out, which is no regulation at all.
          if (projectedTemp > step.targetTemp + DWELL_BAND_C)      nudge = -1;
          else if (projectedTemp < step.targetTemp - DWELL_BAND_C) nudge =  1;
        }
        else
        {
          float progress = (aktSystemTemperature - stepStartTemp) / delta;

          if (progress < behindFrac)
          {
            // Behind the corridor. Two levels if badly behind, so a cold start
            // or a late step recovers instead of creeping.
            nudge = (dir * (behindTemp - aktSystemTemperature) > CORRIDOR_HARD_C)
                    ? CORRIDOR_HARD_NUDGE : 1;
          }
          else if (progress > aheadFrac)
          {
            nudge = (dir * (aktSystemTemperature - aheadTemp) > CORRIDOR_HARD_C)
                    ? -CORRIDOR_HARD_NUDGE : -1;
          }
          else
          {
            // Inside the corridor: hold position, but keep the rate in band so
            // the oven arrives at the far end still under control rather than
            // drifting to one wall and riding it.
            //
            // aktSystemTemperatureRamp is a 1s difference of the 1s rolling
            // average, in degC/s and in float. The source project computed its
            // rate term in integer arithmetic that truncated to 1 degC/s against
            // profile rates of 0.72-1.61 -- barely one bit -- so this term
            // actually contributes here in a way it never did there.
            // Only the slow half lives here. The fast bound is hoisted below,
            // because it has to govern every branch and not just this one.
            float normRate = controlRamp / delta; // fraction of step per second
            if (maxDur > 0.0f && normRate < 1.0f / maxDur) nudge = 1;
          }
        }

        // The step's fast rate bound, in force in EVERY branch above.
        //
        // It used to live inside the "inside the corridor" arm, which made it
        // unreachable exactly when it matters most: aheadFrac and behindFrac
        // are clamped to 1.0, so once elapsed_s reaches maxDuration a step
        // short of target is permanently "behind" and takes the first branch.
        // A step extension would then have been a full-power dash at whatever
        // rate the element managed.
        //
        // Being behind on the clock is not a licence to exceed the ramp the
        // profile declared: that rate is a property of the paste, not of the
        // schedule. minDur comes from deriveStepDurations(), so for a
        // rate-bounded step 1/minDur is literally boundHi -- the profile's own
        // maximum ramp, restated in fractions of the step.
        //
        // Backing off by one and not merely to zero: clamping the nudge to 0
        // would freeze the level at whatever produced the over-rate, so the
        // over-rate would persist. The -1 is what actually walks it down, and
        // it is what the in-corridor arm did before the hoist. The nudge > -1
        // guard keeps an existing hard backoff (-CORRIDOR_HARD_NUDGE) from
        // being weakened.
        if (!isDwell && minDur > 0.0f && nudge > -1)
        {
          float rateFrac = controlRamp / delta; // fraction of step per second
          if (rateFrac > 1.0f / minDur) nudge = -1;
        }

        // Corridor terms into power terms. On a cooling step making more
        // progress means less heat, which is exactly what dir flips.
        int16_t next = (int16_t)powerLevel + (int16_t)(dir * nudge);
        if (next < 0) next = 0;
        if (next > POWER_LEVELS - 1) next = POWER_LEVELS - 1;

        powerLevel = (uint8_t)next;
      }

      // Reductions are not on the control interval. Increases are.
      //
      // Slowing the integration to CONTROL_WINDOWS was the point, but these
      // two are overshoot protection, and deferring protection by a whole
      // interval is not a trade worth making: at these ramp rates one interval
      // is 7.5 degC of extra drive on step 0, 12.9 degC on step 1 and 8.8 degC
      // on step 2 -- worst placed exactly at the peak. So the loop integrates
      // slowly on the way up and cuts immediately.
      //
      // Arrival is assured: stop driving and coast in.
      //
      // This overrides the corridor, deliberately. The corridor can still be
      // asking for heat -- being behind on time is exactly when it does -- but
      // if the heat already in the element is enough to reach the target, any
      // more of it is overshoot. Reaching the target late is a profile that ran
      // slow; reaching it 20 degC hot is a reflow that cooked the board, so the
      // corridor loses this argument.
      //
      // The step advance below tests the same projection, so most of the time
      // the step simply ends here. What this clamp catches is the case the
      // advance cannot: a step that projects to arrive *before* minDuration has
      // elapsed. The advance is blocked by minMet, the oven is running hot and
      // early, and without this it would keep driving into the wait.
      //
      // Still a clamp and not a latch: if the climb decays and the projection
      // falls back below the target, the corridor gets its authority back at
      // the next control interval and drives again.
      //
      // EXCEPT where the next step is hotter, and this is not a detail. The
      // clamp exists to stop the oven sailing past a ceiling, and a step
      // leading into a hotter one has no ceiling -- the same reasoning that
      // already makes such a step advance on the thermometer rather than on
      // the projection.
      //
      // Left ungated the two rules fight, and the clamp wins: the step waits
      // for the measurement to cross the target while the clamp refuses to
      // heat toward it, so the only thing that closes the gap is coasting.
      // Measured on the 2026-09-04 run -- 57 s parked a degree under 170 and
      // 45 s parked a degree under 220, over a hundred seconds of a reflow
      // profile with the element off and the corridor asking for power the
      // whole time. The flux pays for that and the joints get nothing.
      //
      // The fast rate bound is hoisted below and still governs every branch,
      // so this cannot run away: a step may arrive hot, but not faster than
      // the ramp the profile declared.
      bool nextHotter = (activeStep + 1 < activeProfile.stepCount) &&
                        (activeProfile.steps[activeStep + 1].targetTemp
                           > step.targetTemp);
      if (!isDwell && dir > 0.0f && !nextHotter &&
          projectedTemp >= step.targetTemp)
      {
        powerLevel = 0;
      }

      // Cooling steps are capped, and by default that cap is zero. See
      // COOLDOWN_MAX_LEVEL: the descent is a coast, not a controlled one, until
      // someone decides the door interlock question.
      //
      // Also out here because a step entry resets the control clock: entering a
      // cooling step used to leave the previous step's demand driving the
      // element for a whole interval, and the transition that matters is peak
      // to cooldown.
      if (delta < 0.0f && powerLevel > COOLDOWN_MAX_LEVEL)
      {
        powerLevel = COOLDOWN_MAX_LEVEL;
      }

      // The level will often alternate between two neighbours rather than
      // settling -- the oven usually wants something between 50% and 75%, and
      // POWER_LEVELS cannot express it. That is dithering, not instability:
      // alternating each window averages to the level in between, so the
      // quantisation loses less resolution than its step size suggests, and
      // the oven's thermal mass integrates the ripple away. Do not "fix" it
      // with hysteresis; that would throw the recovered resolution away.

      // Step advance: arrival has to be *guaranteed*, with maxDuration as the
      // watchdog.
      //
      // The PID build advanced when its *computed setpoint* reached the target
      // and never consulted the thermometer, so an oven that could not keep up
      // was abandoned mid-ramp while the state machine marched on.
      //
      // Guaranteed, not achieved: a heating step ends once the heat already in
      // the element is enough to carry the oven to the target, because that is
      // the moment there is nothing left to drive. Waiting for the thermometer
      // to actually read the target would mean holding the relay closed
      // through the entire coast and arriving hot -- and the step would sit
      // there driving a target it had already committed to overshooting.
      //
      // Cooling steps stay on the measured temperature. The projection exists
      // to stop the *actuator* overshooting, and nothing drives a descent.
      //
      // ...but "arriving hot" is only a hazard where the target is a ceiling.
      // A step that leads into a HOTTER step has no ceiling to protect: every
      // degree of overshoot is progress toward the next target, while cutting
      // early just hands the shortfall forward. Measured on the 2026-09-04
      // run, where both preheat steps did exactly that -- 170 left at 164.5
      // and 220 left at 213.1 -- so the board soaked at neither, and the peak
      // ramp began 7 degC down. Those steps now advance on the thermometer.
      //
      // The peak keeps the projection, which is the whole reason it exists:
      // there, and on every descent, the target really is a limit. In the
      // default profile that makes this exactly the 170 and 220 steps, and
      // changes nothing about how 240 is approached.
      //
      // Note the two fixes converge: once a step genuinely reaches its target,
      // the corridor anchoring below has nothing left to carry forward, and
      // stepStartTemp is the measurement again.
      // nextHotter is computed above, at the approach clamp, which is the
      // other half of this same rule.
      bool reached  = isDwell ||
                      (dir > 0.0f
                         ? (nextHotter
                              ? (aktSystemTemperature >= (float)step.targetTemp)
                              : (projectedTemp        >= (float)step.targetTemp))
                         : (aktSystemTemperature <= step.targetTemp));
      bool minMet  = elapsed_s >= minDur;
      bool overrun = (maxDur > 0.0f) && (elapsed_s >= maxDur);

      // Progress across a window, rolled through the whole step.
      //
      // The verdict is taken ONLY when a whole window has elapsed, and held
      // between times. Testing it continuously against a window that grows
      // from zero would defeat the point: one second in it asks for 0.36 degC
      // in 1 s, which is an instantaneous rate test carrying exactly the noise
      // the window length was chosen to reject. Held this way, an extension is
      // re-judged roughly twice inside its cap, on evidence that has actually
      // accumulated.
      //
      // It starts optimistic, which is now load-bearing rather than merely
      // convenient: it is what stops a step giving up inside its first window,
      // however slowly the oven happens to be moving when the window opens.
      float gainWindow_s = (time_ms - stepGainRef_ms) / 1000.0f;
      float gainFloor    = STEP_EXTEND_MIN_RATE_C_S;
      if (gainWindow_s * 1000.0f >= STEP_EXTEND_GAIN_WINDOW_MS)
      {
        stepGaining     = (aktSystemTemperature - stepGainRefTemp)
                          >= gainFloor * gainWindow_s;
        stepGainRef_ms  = time_ms;
        stepGainRefTemp = aktSystemTemperature;
      }
      bool gaining = stepGaining;

      float miss_C = (float)step.targetTemp - aktSystemTemperature;

      // A HEATING STEP HAS NO TIME LIMIT. It ends when it reaches its target,
      // or when the oven proves it cannot.
      //
      // There used to be an extension budget here -- 45 s per step, 60 s per
      // run, 12 s of that above liquidus -- and it was the wrong shape of
      // rule. A budget answers "how long am I allowed to keep trying", and
      // nothing useful turns on the answer: the operator cannot replace a
      // heater with a board in the chamber, so a step that runs out of budget
      // hands forward a shortfall nobody can act on. Every downstream step
      // then starts cold, and the peak arrives short. On 2026-09-04 that ended
      // a run 3 degC below a 170 degC target with the paste half soaked.
      //
      // The question worth asking is physical, and it was already being
      // computed: IS THE OVEN STILL CLIMBING. While it is, waiting costs time
      // and gains temperature, which is the trade the profile wants. Once it
      // has plateaued below target at power, waiting costs time and gains
      // nothing -- the oven cannot get there and only the board is paying. So
      // that, and not a clock, is what ends the attempt.
      //
      // `gaining` is a temperature difference across STEP_EXTEND_GAIN_WINDOW_MS
      // and starts optimistic at each step, so a step cannot give up inside
      // its first window however slowly it is moving.
      //
      // KNOWN COST, deliberately accepted: RUN_EXTEND_LIQ_S also bounded time
      // above liquidus, and nothing bounds it now. A slow oven can sit above
      // 217 degC for as long as it keeps inching upward, which damages joints
      // by soak rather than by peak. The judgement is that a board which never
      // reaches peak was not going to reflow anyway.
      //
      // Cooling steps and dwells keep their timers. Nothing drives a descent,
      // so a cooling step that waited for its target would wait forever, and a
      // dwell's duration is the whole point of it.
      stepExtending = overrun && !reached && !isDwell && dir > 0.0f && gaining;
      if (stepExtending) runExtendUsed_s += 0.1f;   // reported, not budgeted

      // Plateaued below target with the clock already past maxDuration: the
      // oven has given what it can and there is nothing left to wait for.
      bool gaveUp = overrun && !reached && !isDwell && dir > 0.0f && !gaining;

      bool timedOut = overrun && (isDwell || dir <= 0.0f);

      if ((minMet && reached) || timedOut || gaveUp)
      {
        // Timed out a long way short on a heating step. Without a check here
        // a dead element runs the whole profile through to Complete having
        // never warmed anything.
        //
        // But "short of target" and "not heating" are different claims, and
        // the old message asserted the second from evidence for only the
        // first. On the 2026-09-03 run it reported "Oven not heating" about an
        // oven climbing at 0.27 degC/s -- the element was fine; the controller
        // had spent 21 s of a 28 s step getting the power back up, and then
        // refused to wait. Wrong diagnosis, and it sent the operator looking
        // at the element.
        //
        // Only reachable by gaveUp now, a heating step having no timeout:
        // short of target AND no longer climbing. That is the one shape of
        // evidence that says something about the element rather than about
        // the schedule.
        if (gaveUp && (step.targetTemp - aktSystemTemperature) > STEP_MISS_C)
        {
          reportWarning("Oven stopped climbing short of target");
        }

        activeStep++;
        if (activeStep >= activeProfile.stepCount)
        {
          currentState = Complete;
          powerLevel   = 0;
          beepcount    = 1; // Beep! We are done!!!
        }
        else
        {
          // Anchor a step that ramps UP at the profile's own target rather
          // than at whatever the thermometer happens to read here.
          //
          // Rebasing on the measurement let every step redraw its corridor
          // from the previous step's shortfall, so an undershoot was not made
          // up -- it was adopted. progress is (temp - stepStartTemp)/delta, so
          // a rebased start reads 0.0 and perfectly on schedule while the oven
          // sits 15 degC below the profile: the corridor cannot see a deficit
          // it was just re-zeroed against. STEP_MISS_C then measures each step
          // against its OWN target only, so every heating step may quietly
          // shed up to 15 degC and the losses ADD -- three of them on the
          // default profile, and a peak reached 45 degC cold with no fault
          // raised and a corridor that read clean the whole way. This is
          // REWORK_NOTES.md 7.7's "no fault, and a log that looks perfect",
          // and it is also why the setpoint trace stepped down to meet the
          // temperature at each transition instead of holding the profile.
          //
          // Anchoring carries the deficit forward instead: the step opens with
          // progress negative, the badly-behind arm asks for two levels, and
          // the oven is driven to make it up -- still under the step's own
          // fast rate bound, which is now enforced in every branch.
          //
          // The comment on defaultSteps above already asserted this ("the
          // remaining steps start from the previous step's target"); only the
          // code disagreed.
          //
          // Upward steps only, and only while the anchor is at or above the
          // measurement -- that is precisely the undershoot case, where the
          // oven is short of where the last step was meant to leave it and
          // the shortfall is the step's to make up. The second test is what
          // stops the anchor cutting the other way: after a passive cooling
          // step that never got down to its target, anchoring an up-ramp at
          // that target would inflate delta instead of holding it, handing the
          // step exactly the undeclared time this is meant to deny it.
          // Cooling steps never anchor at all; the open door is the actuator
          // and the controller cannot be held to a corridor it cannot drive.
          //
          // A step repeating the previous target is a dwell by intent, and it
          // anchors unconditionally so that delta is exactly zero and isDwell
          // recognises it. Taken from the measurement instead, a dwell entered
          // a few degrees low becomes a tiny ramp -- and entered a few degrees
          // high becomes a *cooling* step, with dir negative and
          // COOLDOWN_MAX_LEVEL holding the power down -- so the dwell
          // regulator that holds the peak would never run at all.
          const int16_t nextTarget = activeProfile.steps[activeStep].targetTemp;
          if (nextTarget == step.targetTemp ||
              (nextTarget > step.targetTemp &&
               (float)step.targetTemp >= aktSystemTemperature))
            stepStartTemp    = (float)step.targetTemp;
          else
            stepStartTemp    = aktSystemTemperature;
          stepStartedTime_ms = time_ms;
          lastLevelWindow    = relayWindowSeq;
          stepExtending      = false;

          // Enter a heating step at full duty instead of inheriting whatever
          // the previous step was left holding.
          //
          // The approach clamp sets powerLevel to zero at the end of every
          // heating step -- correctly; that is the deliberate early cut -- and
          // the next step then began from that zero and climbed back one level
          // per control interval. Measured on the 2026-09-03 run: 21 s to
          // regain full power on a step whose entire maximum duration was
          // 28 s. The power ramp was longer than the step, so a short step
          // could not be met however capable the element was.
          //
          // Safe for the same reason the full-duty entry at run start is: the
          // approach clamp is ungated, re-tests every 100 ms, and takes the
          // power straight back off the moment arrival is assured.
          //
          // Keyed on the profile's intent rather than on the measurement,
          // because a dwell must NOT get this. The approach clamp excludes
          // dwell steps, so nothing would take the power back off inside the
          // 100 ms pass -- the dwell regulator would have to walk it down at
          // one level per control interval, and 8 s of full duty at the peak
          // is 8 degC of overshoot on the step that can least afford it.
          if (nextTarget > step.targetTemp)
          {
            powerLevel = POWER_LEVELS - 1;
          }
          stepGainRef_ms     = time_ms;
          stepGainRefTemp    = aktSystemTemperature;
          stepGaining        = true;

          // First cooling step: the oven cannot cool itself, so ask for the
          // door -- but not yet. The coast is still carrying the oven up to
          // peak temperature, and prompting now would have the operator open
          // the door into it. Arm the prompt and let the peak arrive first.
          if (!cooldownAnnounced &&
              activeProfile.steps[activeStep].targetTemp < step.targetTemp)
          {
            cooldownAnnounced   = true;
            awaitingPeakBeep    = true;
            coolStepEntered_ms  = time_ms;
            notClimbingSince_ms = 0;
          }
        }
      }

      // The armed door prompt, released once the oven has actually peaked.
      // Same test /measurelag uses: the climb has to have stopped and stayed
      // stopped, so one noisy sample cannot release it early.
      if (awaitingPeakBeep)
      {
        if (aktSystemTemperatureRamp > 0.0f) notClimbingSince_ms = 0;
        else if (notClimbingSince_ms == 0)   notClimbingSince_ms = time_ms;

        bool peaked = notClimbingSince_ms &&
                      (time_ms - notClimbingSince_ms >= COOLDOWN_BEEP_SETTLE_MS);
        bool waited = (time_ms - coolStepEntered_ms >= COOLDOWN_BEEP_MAX_WAIT_MS);

        if (peaked || waited)
        {
          awaitingPeakBeep = false;
          openDoorPrompt   = true;
          beepcount        = 3; // Beep! We need the door open!!!
          Serial.printf("Peak %.1fC reached, open the door\n", aktSystemTemperature);
        }
      }

      // Heat arriving that nothing asked for: far above the corridor AND
      // still climbing. See CORRIDOR_ABORT_C for why the climb is load-
      // bearing and the gap on its own is not.
      static uint64_t corridorAbortSince_ms = 0;
      if (aktSystemTemperature > corridorHigh + CORRIDOR_ABORT_C &&
          controlRamp > 0.0f)
      {
        if (corridorAbortSince_ms == 0) corridorAbortSince_ms = time_ms;
        else if (time_ms - corridorAbortSince_ms >= CORRIDOR_ABORT_SETTLE_MS)
        {
          // Names the observation and the part to look at. The old text
          // ("Temperature is Way to HOT!!!!!") named neither, so an operator
          // reading it on a cooling oven had nothing to act on.
          reportError("Oven still heating with the relay open -- "
                      "check for a welded relay contact");
        }
      }
      else corridorAbortSince_ms = 0;

      // The opposite fault, and the one that actually happens: heat demanded
      // and nothing coming back. See HEAT_STALL_WINDOW_MS.
      //
      // The window restarts whenever demand drops below the threshold, so it
      // only ever measures a stretch of sustained heating -- the approach
      // clamp dropping to zero on arrival resets it rather than accumulating
      // toward a fault.
      if (dir > 0.0f && !isDwell && powerLevel >= HEAT_STALL_MIN_LEVEL)
      {
        if (heatStallRef_ms == 0)
        {
          heatStallRef_ms  = time_ms;
          heatStallRefTemp = aktSystemTemperature;
        }
        else if (time_ms - heatStallRef_ms >= HEAT_STALL_WINDOW_MS)
        {
          if (aktSystemTemperature - heatStallRefTemp < HEAT_STALL_MIN_RISE_C)
          {
            reportWarning("Oven not heating: no rise under power");
          }
          // Roll the window either way, so a recovered oven is not judged on
          // history and a still-dead one re-reports on the next window.
          heatStallRef_ms  = time_ms;
          heatStallRefTemp = aktSystemTemperature;
        }
      }
      else
      {
        heatStallRef_ms = 0;
      }

      powerHeater = (uint8_t)PH_FOR_LEVEL(powerLevel);
    }
    else if (currentState == MeasureLag)
    {
      // Measures the oven's thermal lag by performing the experiment the
      // approach clamp relies on: hold a fixed level to a known temperature,
      // open the relay, and see how far the oven carries on climbing.
      //
      //     lag = (peak - temp at cut) / rate at cut
      //
      // Note this deliberately measures the whole cut-to-peak behaviour,
      // including up to one relay window of residual on-time after the demand
      // drops -- relayDriver() latches per window, so the contacts can stay
      // closed briefly after the cut. That latency is present during a real
      // profile too, and the projection has to cover it, so folding it into
      // the number is right rather than something to correct out.
      static bool  coasting = false;
      static float cutTemp = 0.0f, cutRate = 0.0f, peakTemp = 0.0f;
      static uint64_t phaseStart_ms = time_ms;
      static uint64_t fallingSince_ms = 0;

      if (stateChanged)
      {
        coasting        = false;
        fallingSince_ms = 0;
        phaseStart_ms = time_ms;
        peakTemp      = aktSystemTemperature;
        measuredLagSec = 0.0f;
        powerLevel    = MEASURE_LEVEL;
      }

      if (!coasting)
      {
        powerLevel = MEASURE_LEVEL;

        if (aktSystemTemperature >= measureTempC)
        {
          if (aktSystemTemperatureRamp < MEASURE_MIN_RATE_C_S)
          {
            // Arrived, but not climbing: nothing to divide by, and the answer
            // would be meaningless rather than merely wrong.
            reportError("Lag measurement: oven not climbing at cut");
          }
          else
          {
            cutTemp       = aktSystemTemperature;
            cutRate       = aktSystemTemperatureRamp;
            peakTemp      = aktSystemTemperature;
            coasting      = true;
            powerLevel    = 0;
            phaseStart_ms = time_ms;
            Serial.printf("Lag measurement: cut at %.1fC rising %.2fC/s\n",
                          cutTemp, cutRate);
          }
        }
        else if (time_ms - phaseStart_ms > (uint64_t)MEASURE_HEAT_TIMEOUT_S * 1000)
        {
          reportError("Lag measurement: oven never reached measure temp");
        }
      }
      else
      {
        powerLevel = 0;

        if (aktSystemTemperature > peakTemp) peakTemp = aktSystemTemperature;

        // Require the climb to have stopped and stayed stopped, so one noisy
        // sample cannot be mistaken for the peak.
        if (aktSystemTemperatureRamp > 0.0f) fallingSince_ms = 0;
        else if (fallingSince_ms == 0)       fallingSince_ms = time_ms;

        bool peaked  = fallingSince_ms &&
                       (time_ms - fallingSince_ms >= MEASURE_PEAK_SETTLE_MS) &&
                       (time_ms - phaseStart_ms > 2000); // let the cut register
        bool gaveUp  = (time_ms - phaseStart_ms >
                        (uint64_t)MEASURE_COAST_TIMEOUT_S * 1000);

        if (peaked || gaveUp)
        {
          measuredLagSec = (peakTemp - cutTemp) / cutRate;
          if (measuredLagSec < THERMAL_LAG_MIN_S) measuredLagSec = THERMAL_LAG_MIN_S;
          if (measuredLagSec > THERMAL_LAG_MAX_S) measuredLagSec = THERMAL_LAG_MAX_S;

          thermalLagSec = measuredLagSec;
          saveOven();

          Serial.printf("Lag measurement: peak %.1fC, coast %.1fC, lag %.1fs%s\n",
                        peakTemp, peakTemp - cutTemp, measuredLagSec,
                        gaveUp ? " (COAST TIMED OUT -- treat as a lower bound)" : "");

          currentState = Complete;
          powerLevel   = 0;
          beepcount    = 2;
        }
      }

      powerHeater = (uint8_t)PH_FOR_LEVEL(powerLevel);
    }
    else if (currentState == Hold)
    {
      // Same law as everywhere else: steer the PROJECTION, not the present
      // temperature. With a mechanical relay and an element hotter than the
      // air, heat already committed arrives after the thermometer reads the
      // target -- which is what makes a naive setpoint controller oscillate
      // here regardless of its gains.
      float projectionRate = controlRamp;
      if (projectionRate >  PROJECTION_MAX_RATE_C_S) projectionRate =  PROJECTION_MAX_RATE_C_S;
      if (projectionRate < -PROJECTION_MAX_RATE_C_S) projectionRate = -PROJECTION_MAX_RATE_C_S;
      float projectedTemp = aktSystemTemperature + projectionRate * thermalLagSec;

      heaterSetpoint = holdTempC;
      corridorLow    = holdTempC - DWELL_BAND_C;
      corridorHigh   = holdTempC + DWELL_BAND_C;

      // Runaway guard. Unlike the corridor abort this compares against a
      // constant, so being far above it means one thing only.
      if (aktSystemTemperature > holdTempC + HOLD_ABORT_C)
      {
        reportError("Hold: oven far above setpoint -- "
                    "check for a welded relay contact");
      }

      // Unattended element, no profile to end the cycle.
      if (time_ms - holdSetAt_ms >= (uint64_t)HOLD_TIMEOUT_S * 1000ull)
      {
        Serial.printf("Hold at %dC timed out after %d s\n",
                      holdTempC, HOLD_TIMEOUT_S);
        currentState = Complete;
        powerLevel   = 0;
        beepcount    = 2;
      }
      else if (relayWindowSeq - lastLevelWindow >= CONTROL_WINDOWS)
      {
        lastLevelWindow = relayWindowSeq;

        const float TOP = (float)(POWER_LEVELS - 1);
        float err = (float)holdTempC - projectedTemp;

        // Proportional. Computed, never accumulated -- see HOLD_PBAND_C.
        float p = (err / holdPBandC) * TOP;
        if (p < 0.0f) p = 0.0f;
        if (p > TOP)  p = TOP;

        // Integral trim, gated to the band and to outside the deadband so it
        // does not hunt once the oven is where it was asked to be.
        float mag = fabsf(err);
        if (mag < holdPBandC && mag > DWELL_BAND_C)
        {
          holdTrim += (err > 0.0f) ? holdTrimStep : -holdTrimStep;
          if (holdTrim < 0.0f) holdTrim = 0.0f;
          if (holdTrim > TOP)  holdTrim = TOP;
        }

        float lvl = p + holdTrim;
        if (lvl < 0.0f) lvl = 0.0f;
        if (lvl > TOP)  lvl = TOP;
        powerLevel = (uint8_t)(lvl + 0.5f);
      }

      powerHeater = (uint8_t)PH_FOR_LEVEL(powerLevel);
    }
    else if(currentState == Manual)
    {
      powerHeater=((uint16_t)manualPower*255)/100;
    }
    else
    {
      powerLevel  = 0;
      powerHeater = 0;
    }

    if (currentState != Running && currentState != Complete)
    {
      // The prompt is a per-cycle thing. Complete keeps it: the board is still
      // hot and the door still wants to be open.
      openDoorPrompt = false;
    }

    if (currentState != Running && currentState != Hold)
    {
      // Nothing defines a corridor outside a profile run, and leaving the last
      // one in place puts a stale band on the chart that looks like the oven
      // is being held somewhere it is not. Hold is excluded because it does
      // define one -- its deadband, set in its own branch.
      corridorLow = corridorHigh = heaterSetpoint = aktSystemTemperature;
    }
  }

}
