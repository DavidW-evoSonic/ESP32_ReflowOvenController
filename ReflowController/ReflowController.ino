// ----------------------------------------------------------------------------
// Reflow Oven Controller
// (c) 2019      Patrick Knöbel
// (c) 2014 Karl Pitrich <karl@pitrich.com>
// (c) 2012-2013 Ed Simmons
// ----------------------------------------------------------------------------

//Devdefins
//#define NOEDGEERRORREPORT 

//Pin Mapping
// One of the two original heater pins, so it is already routed for heater duty
// on the board. Non-strapping and output-capable.
//
// Note 25/26 -- suggested in the original rework notes -- are NOT available:
// they stay claimed by BUZZER and RGB_SDO. Still free if this ever has to
// move: 17 (the other original heater pin), 27, 23, 22, 4, 33.
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
#define POWER_LEVELS   5

// How often the power level is re-evaluated. One relay window: nudging faster
// than the actuator can express would slew the level across its whole range
// before a single change reached the oven.
#define CONTROL_INTERVAL_MS  RELAY_WINDOW_MS

// Outside the corridor by more than this, the level moves by two rather than
// one. Recovers from a badly-placed start without making normal tracking jumpy.
#define CORRIDOR_HARD_C     10.0f

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
#define PROJECTION_MAX_RATE_C_S  5.0f

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

// Abort if the oven runs this far above the corridor: a welded relay contact or
// a shorted drive transistor looks like this, and nothing else does.
#define CORRIDOR_ABORT_C   100.0f

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

// Extension is for the "nearly made it" case only.
//
// A step still more than STEP_MISS_C short when its slow bound expires is not
// a slow oven, it is the dead-or-weak element case, and it must keep faulting
// on exactly the timing it always did. Without this gate a partially failed
// element -- one element open, low mains, a door ajar -- would be extended
// instead of faulted, because it does still climb, just not enough. On the
// default step 0 that would push detection from 180 s out to 542 s of a
// half-failed mains heater at full power.
//
// Gating here means the extension covers precisely the 0..15 degC window that
// used to advance silently, and nothing else.
#define STEP_EXTEND_ENTRY_C     STEP_MISS_C

// Absolute per-step cap, seconds.
//
// Deliberately not a multiple of maxDuration: that unit is largest exactly
// where extension matters least (361 s on the default step 0, against 28 s on
// the peak) and is bounded by nothing chemical. Derived instead from the work
// to be done -- closing a STEP_EXTEND_ENTRY_C gap at the rate floor below
// takes 37-42 s on every heating step of the default profile.
#define STEP_EXTEND_CAP_S       45.0f

// Run-wide extension budget, seconds. Per-step caps compose with the step
// count, and MAX_STEPS is 8, so a per-step cap on its own bounds nothing
// useful. A step that ends short also rebases the next step's start on the
// measured temperature, shrinking its delta while its durations stay fixed --
// so an extension makes the following step harder, and more likely to extend
// in turn. This is what stops that cascading.
#define RUN_EXTEND_BUDGET_S     60.0f

// Run-wide budget for extension time spent above liquidus, seconds.
//
// This, not the per-step cap, is the constraint that actually limits the
// feature. Time above liquidus is what grows intermetallics and damages parts,
// and the default profile already spends 61.8 s above 217 degC when its steps
// run to their slow bounds -- against a 60-90 s paste spec, so about 28 s of
// headroom for the whole run. Charging only above-liquidus extension time
// against 25 s keeps even a fully extended run near 87 s, still inside spec.
//
// Uncapped, extensions on the 220 and 243 degC steps would take it to 154 s.
#define LIQUIDUS_C              217.0f
#define RUN_EXTEND_LIQ_S        25.0f

// "Still gaining" -- measured as a temperature DIFFERENCE ACROSS A WINDOW, and
// never as an instantaneous rate.
//
// aktSystemTemperatureRamp is a 1 s difference of a 1 s rolling mean, so it
// carries roughly sqrt(2)/sqrt(10) of the per-read noise: about 0.045 degC/s
// of sigma on this hardware (REWORK_NOTES 7.1). Thresholding that directly is
// a coin flip evaluated at 10 Hz, and across a 45 s extension noise alone
// would hold the test open indefinitely.
//
// Differencing the temperature across a whole window telescopes instead: an
// ADC excursion that inflates one endpoint inflates the next window's start by
// the same amount, so a spike can shift one boundary but cannot manufacture
// sustained gain. Only a steady bias of this size could, and a bias that large
// would walk the absolute reading into TEMP_PLAUSIBLE_MAX_C on its own.
//
// Do NOT reuse MEASURE_MIN_RATE_C_S (0.05) here. That is about one sigma of
// this noise, and it answers a different question at a different operating
// point.
#define STEP_EXTEND_GAIN_WINDOW_MS 20000

// The floor scales with the step's own slow bound -- "at least half the rate
// the step itself demands" -- under an absolute 0.20 degC/s, which is 4-5
// sigma of the ramp noise. That yields 0.36, 0.39 and 0.41 degC/s on the
// default profile's three heating steps.
//
// Note this test is a weak limiter by nature: at full power the oven really is
// gaining right up to thermal equilibrium, with the rate decaying through 0.4,
// 0.2, 0.1 degC/s. The caps above do most of the safety work, so do not size
// them on the assumption that gaining will end most extensions early.
#define STEP_EXTEND_MIN_RATE_C_S   0.20f

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

// Heater demand, 0..255, consumed by relayDriver().
volatile uint8_t  powerHeater=0;
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
float aktSystemTemperatureRamp; //°C/s

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
float runExtendLiq_s  = 0.0f;   // ... of which was spent above liquidus
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

  return (mv * TEMP_DIVIDER_RATIO) / AD595_MV_PER_C;
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
// Steps 3-5 are the controlled cooldown. They are stored and run, but
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
  static const Step_t defaultSteps[] = {
    { 170,  72,  94, BOUND_RATE     },  // 0.72-0.94 degC/s
    { 220,  31,  64, BOUND_DURATION },
    { 243,  21,  28, BOUND_DURATION },
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
} Oven_t;

bool saveOven() {
  Oven_t o = { thermalLagSec, measureTempC };
  PREF.putBytes("OVEN", (uint8_t*)&o, sizeof(o));
  return true;
}

bool loadOven() {
  Oven_t o;
  if (PREF.getBytesLength("OVEN") != sizeof(o)) {
    Serial.println("load default OVEN");
    thermalLagSec = THERMAL_LAG_DEFAULT_S;
    measureTempC  = MEASURE_TEMP_DEFAULT_C;
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
    char buffer[512];
    unsigned long time = (esp_timer_get_time()-cycleStartTime)/1000;
    snprintf(buffer,sizeof(buffer),
      "{\"time\": %lu, \"temp\": %.2f, \"dt\": %.2f, \"setpoint\": %.2f,"
      " \"low\": %.2f, \"high\": %.2f, \"power\": %.2f, \"step\": %d,"
      " \"steps\": %d, \"lag\": %.1f, \"openDoor\": %d, \"heating\": %d,"
      " \"extending\": %d, \"extended\": %.0f,"
      " \"state\": \"%s\", \"fault\": \"%s\"}",
      time, aktSystemTemperature, aktSystemTemperatureRamp, heaterSetpoint,
      corridorLow, corridorHigh,
      (float)powerHeater*100.0f/255.0f, activeStep, activeProfile.stepCount,
      thermalLagSec, openDoorPrompt ? 1 : 0, heaterOn ? 1 : 0,
      stepExtending ? 1 : 0, runExtendUsed_s,
      currentStateToString(), globalErrorText);
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
           "}, \"otaSet\": " + String(otaPassword.length() ? 1 : 0) + "}";
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
  //   steps=170,0.72,0.94,r;220,31,64;243,21,28
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
    static uint64_t lastLevelUpdate_ms  = time_ms;
    static bool     cooldownAnnounced   = false;
    static bool     awaitingPeakBeep    = false;
    static uint64_t coolStepEntered_ms  = 0;
    static uint64_t notClimbingSince_ms = 0;
    // Step extension bookkeeping. The gain reference rolls for the whole step,
    // not just during an extension, so the eligibility decision at maxDuration
    // already has a full window of history behind it rather than needing a
    // separate and weaker entry test.
    static uint64_t stepGainRef_ms      = 0;
    static float    stepGainRefTemp     = 0.0f;
    static float    stepExtendStart_s   = 0.0f;  // elapsed_s when it began
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
    if (globalError && currentState == Running)
    {
      powerLevel   = 0;
      currentState = Complete;
      beepcount    = 6;   // distinct from the single success beep
    }

    if (currentState == Running)
    {
      if (stateChanged)
      {
        activeStep         = 0;
        stepStartTemp      = aktSystemTemperature;
        stepStartedTime_ms = time_ms;
        lastLevelUpdate_ms = time_ms;
        powerLevel         = 0;
        cooldownAnnounced  = false;
        awaitingPeakBeep   = false;
        stepGainRef_ms     = time_ms;
        stepGainRefTemp    = aktSystemTemperature;
        stepGaining        = true;
        // Run-scoped, so these reset here and nowhere else.
        runExtendUsed_s    = 0.0f;
        runExtendLiq_s     = 0.0f;
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
      float projectionRate = aktSystemTemperatureRamp;
      if (projectionRate >  PROJECTION_MAX_RATE_C_S) projectionRate =  PROJECTION_MAX_RATE_C_S;
      if (projectionRate < -PROJECTION_MAX_RATE_C_S) projectionRate = -PROJECTION_MAX_RATE_C_S;
      float projectedTemp = aktSystemTemperature + projectionRate * thermalLagSec;

      // Re-evaluate once per relay window. The level is what the actuator can
      // actually express, so moving it faster than the window would just slew
      // it across the whole range before one change reached the oven.
      if (time_ms - lastLevelUpdate_ms >= CONTROL_INTERVAL_MS)
      {
        lastLevelUpdate_ms = time_ms;

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
            nudge = (dir * (behindTemp - aktSystemTemperature) > CORRIDOR_HARD_C) ? 2 : 1;
          }
          else if (progress > aheadFrac)
          {
            nudge = (dir * (aktSystemTemperature - aheadTemp) > CORRIDOR_HARD_C) ? -2 : -1;
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
            float normRate = aktSystemTemperatureRamp / delta; // fraction of step per second
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
        // guard keeps an existing -2 hard backoff from being weakened.
        if (!isDwell && minDur > 0.0f && nudge > -1)
        {
          float rateFrac = aktSystemTemperatureRamp / delta; // fraction of step per second
          if (rateFrac > 1.0f / minDur) nudge = -1;
        }

        // Corridor terms into power terms. On a cooling step making more
        // progress means less heat, which is exactly what dir flips.
        int16_t next = (int16_t)powerLevel + (int16_t)(dir * nudge);
        if (next < 0) next = 0;
        if (next > POWER_LEVELS - 1) next = POWER_LEVELS - 1;

        // Arrival is assured: stop driving and coast in.
        //
        // This overrides the corridor, deliberately. The corridor can still be
        // asking for heat -- being behind on time is exactly when it does --
        // but if the heat already in the element is enough to reach the target,
        // any more of it is overshoot. Reaching the target late is a profile
        // that ran slow; reaching it 20 degC hot is a reflow that cooked the
        // board, so the corridor loses this argument.
        //
        // The step advance below tests the same projection, so most of the time
        // the step simply ends here. What this clamp catches is the case the
        // advance cannot: a step that projects to arrive *before* minDuration
        // has elapsed. The advance is blocked by minMet, the oven is running
        // hot and early, and without this it would keep driving into the wait.
        //
        // It is a clamp and not a latch: if the climb decays and the
        // projection falls back below the target, the corridor gets its
        // authority back on the next window and drives again.
        if (!isDwell && dir > 0.0f && projectedTemp >= step.targetTemp)
        {
          next = 0;
        }

        // Cooling steps are capped, and by default that cap is zero. See
        // COOLDOWN_MAX_LEVEL: the descent is a coast, not a controlled one,
        // until someone decides the door interlock question.
        if (delta < 0.0f && next > COOLDOWN_MAX_LEVEL) next = COOLDOWN_MAX_LEVEL;

        powerLevel = (uint8_t)next;
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
      bool reached  = isDwell ||
                      (dir > 0.0f ? (projectedTemp >= step.targetTemp)
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
      // It starts optimistic, so a step whose maxDuration is shorter than the
      // window can enter an extension on no evidence. That is bounded: the
      // entry gate has already established the oven is within
      // STEP_EXTEND_ENTRY_C, and the first real verdict lands 20 s in, well
      // inside STEP_EXTEND_CAP_S and the liquidus budget.
      float gainWindow_s = (time_ms - stepGainRef_ms) / 1000.0f;
      float gainFloor    = fmaxf(STEP_EXTEND_MIN_RATE_C_S,
                                 (maxDur > 0.0f)
                                   ? 0.5f * fabsf(delta) / maxDur : 0.0f);
      if (gainWindow_s * 1000.0f >= STEP_EXTEND_GAIN_WINDOW_MS)
      {
        stepGaining     = (aktSystemTemperature - stepGainRefTemp)
                          >= gainFloor * gainWindow_s;
        stepGainRef_ms  = time_ms;
        stepGainRefTemp = aktSystemTemperature;
      }
      bool gaining = stepGaining;

      float miss_C = (float)step.targetTemp - aktSystemTemperature;

      // Enter an extension: overrun its slow bound, short of target, heating,
      // still genuinely climbing, and nearly there.
      //
      // `reached` is the projection, so this never fights the deliberate early
      // cut: a step that projects to arrive is not short, and ends normally
      // through the ordinary advance below. A dead element is short and NOT
      // gaining, so it never gets here either -- it falls through to the
      // STEP_MISS_C check in the same window it always did.
      if (overrun && !reached && !isDwell && dir > 0.0f && gaining &&
          miss_C <= STEP_EXTEND_ENTRY_C && !stepExtending &&
          runExtendUsed_s < RUN_EXTEND_BUDGET_S &&
          runExtendLiq_s  < RUN_EXTEND_LIQ_S)
      {
        stepExtending     = true;
        stepExtendStart_s = elapsed_s;
        Serial.printf("Step %u extended at %.0fs, %.1fC short of %dC\n",
                      activeStep, elapsed_s, miss_C, step.targetTemp);
      }

      if (stepExtending)
      {
        float ext_s = elapsed_s - stepExtendStart_s;
        // Charged per 100 ms pass. Only time actually spent above liquidus
        // counts against the tighter budget -- that is the exposure that
        // damages the board, and a long sub-liquidus extension does not.
        runExtendUsed_s += 0.1f;
        if (aktSystemTemperature >= LIQUIDUS_C) runExtendLiq_s += 0.1f;

        if (ext_s >= STEP_EXTEND_CAP_S ||
            runExtendUsed_s >= RUN_EXTEND_BUDGET_S ||
            runExtendLiq_s  >= RUN_EXTEND_LIQ_S)
        {
          reportError("Extended step never reached target");
          stepExtending = false;  // fall through to the ordinary advance
        }
        else if (!gaining)
        {
          // Stopped climbing: the oven has given what it can. Faulting on a
          // miss of a degree or two would be a new false alarm where today the
          // step simply advances, and the projection catches nearly everything
          // that close anyway -- at the default 20 s lag even 0.05 degC/s of
          // climb puts projectedTemp over the target.
          stepExtending = false;
          if (miss_C > DWELL_BAND_C)
          {
            reportError("Extended step stalled short of target");
          }
        }
      }

      // Suspending the duration watchdog is the whole of what an extension
      // does. Its rate bounds stayed in force throughout, by way of the
      // hoisted fast-bound clamp above.
      bool timedOut = overrun && !stepExtending;

      if ((minMet && reached) || timedOut)
      {
        // Timed out a long way short on a heating step: this is not a slow
        // oven, it is an element that is not heating. Nothing else looks like
        // this, and without the check a dead element runs the whole profile
        // through to Complete having never warmed anything.
        if (timedOut && !reached && dir > 0.0f &&
            (step.targetTemp - aktSystemTemperature) > STEP_MISS_C)
        {
          reportError("Oven not heating: step timed out short of target");
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
          stepStartTemp      = aktSystemTemperature;
          stepStartedTime_ms = time_ms;
          lastLevelUpdate_ms = time_ms;
          stepExtending      = false;
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

      // Far above the corridor with the heat quantised and bounded: a welded
      // contact or a shorted drive looks like this and little else does.
      if (aktSystemTemperature > corridorHigh + CORRIDOR_ABORT_C)
      {
        reportError("Temperature is Way to HOT!!!!!");
      }

      powerHeater = (uint16_t)powerLevel * 255 / (POWER_LEVELS - 1);
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

      powerHeater = (uint16_t)powerLevel * 255 / (POWER_LEVELS - 1);
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

    if (currentState != Running)
    {
      // Nothing defines a corridor outside a profile run, and leaving the last
      // one in place puts a stale band on the chart that looks like the oven
      // is being held somewhere it is not.
      corridorLow = corridorHigh = heaterSetpoint = aktSystemTemperature;
    }
  }

}
