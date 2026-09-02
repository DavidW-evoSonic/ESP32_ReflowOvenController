// ----------------------------------------------------------------------------
// Reflow Oven Controller
// (c) 2019      Patrick Knöbel
// (c) 2014 Karl Pitrich <karl@pitrich.com>
// (c) 2012-2013 Ed Simmons
// ----------------------------------------------------------------------------

//Devdefins
//#define NOEDGEERRORREPORT 

//Pin Mapping
// TODO: not yet decided -- change this one line when the hardware is fixed.
// Must be non-strapping and output-capable. Note 25/26 (suggested in the rework
// notes) are NOT available: they stay claimed by BUZZER and RGB_SDO. Freed by
// this rework and safe to use: 17, 16 (the original heater pins, already routed
// for heater duty), 27, 23, 22, 4, 33. Defaulting to 17.
#define HEATER      17

// AD595 analog output. Must be an ADC1 channel (32-39); ADC2 is unusable while
// WiFi is active. 34 is input-only, which suits a sensor input.
#define TEMP_ADC_GPIO   34
#define TEMP_ADC_CH     ADC1_CHANNEL_6

#define BUZZER      25

#define RGB_CLK     21
#define RGB_SDO     26

//constance
// Time-proportional relay output. A mechanical relay cannot be phase-fired, so
// PID demand is expressed as on-time within a fixed window. The min on/off
// clamps suppress pulses too short to be worth a contact cycle.
#define RELAY_WINDOW_MS   4000
#define RELAY_TICK_MS       50
#define RELAY_MIN_ON_MS    500
#define RELAY_MIN_OFF_MS   500
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
// the PID back off, never overheat -- but it will fault out rather than run, so
// the symptom is a dead oven, not a damaged one.
#define AD595_MV_PER_C        10.0f
#define TEMP_DIVIDER_RATIO     1.5f
#define TEMP_OVERSAMPLE         64
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

//includes
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPI.h>
#include <Ticker.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>

#include "src/PID_v1/PID_v1.h"
#include "src/PID_AutoTune_v0/PID_AutoTune_v0.h"

#include "root_html.h"

//structs
// data type for the values used in the reflow profile
typedef struct profileValues_s {
  char    name[PROFILE_NAME_LENGTH];
  int16_t soakTemp;
  int16_t soakDuration;
  int16_t peakTemp;
  int16_t peakDuration;
  float  rampUpRate;
  float  rampDownRate;
} Profile_t;

typedef enum {
  None     = 0,
  Ready    = 1,   // idle, accepting commands
  Manual   = 2,   // manual heating, power set from the web UI

  // Everything above this is an active oven cycle. Ordering is load-bearing:
  // several checks are written as comparisons against it.
  ProcessStart = 9,

  RampToSoak = 10,
  Soak,
  RampUp,
  Peak,
  CoolDown,

  Complete = 20,

  PreTune = 30,
  Tune,
} State;


//prototypes
void factoryReset();


//Varables
const char * ver = "4.0";

SPIClass RGBLED(VSPI); 
esp_adc_cal_characteristics_t adcChars;
esp_timer_handle_t  RelayTimer;
Preferences PREF;

WebServer server(80);
WebServer serverAction(8080);

// Latched fault. Once set the relay is held off and no cycle can start; the
// reason is kept so /status can report it instead of the oven just going quiet.
volatile boolean globalError=false;
const char * globalErrorText = "";

// Heater demand, 0..255, consumed by relayDriver().
volatile uint8_t  powerHeater=0;

float aktSystemTemperature;
float aktSystemTemperatureRamp; //°C/s

int16_t tuningHeaterOutput=30;
int16_t tuningNoiseBand=1;
int16_t tuningOutputStep=10;
int16_t tuningLookbackSec=60;


int activeProfileId = 0;
Profile_t activeProfile; // the one and only instance

State currentState  = Ready;
uint64_t stateChangedTicks = 0;

// Manual heating power, 0..100%, set from the web UI.
uint8_t manualPower = 0;

float heaterSetpoint;
float heaterInput;
float heaterOutput;

typedef struct {
  float Kp;
  float Ki;
  float Kd;
} PID_t;

PID_t heaterPID;

PID PID(&heaterInput, &heaterOutput, &heaterSetpoint, heaterPID.Kp, heaterPID.Ki, heaterPID.Kd, DIRECT);

PID_ATune PIDTune(&heaterInput, &heaterOutput);

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
  for (uint8_t i = 0; i < TEMP_OVERSAMPLE; i++) {
    sum += adc1_get_raw(TEMP_ADC_CH);
  }

  uint32_t mv = esp_adc_cal_raw_to_voltage(sum / TEMP_OVERSAMPLE, &adcChars);

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

  if (windowElapsed >= RELAY_WINDOW_MS)
  {
    windowElapsed = 0;
    onTime = ((uint32_t)powerHeater * RELAY_WINDOW_MS) / 255;

    // Never ask the relay for a pulse (or a gap) too short to be worth a
    // contact cycle: round it away to fully off or fully on.
    if (onTime < RELAY_MIN_ON_MS) onTime = 0;
    if (onTime > RELAY_WINDOW_MS - RELAY_MIN_OFF_MS) onTime = RELAY_WINDOW_MS;
  }

  digitalWrite(HEATER, (!globalError && windowElapsed < onTime) ? HIGH : LOW);
  windowElapsed += RELAY_TICK_MS;
}


const char * currentStateToString()
{
  #define casePrintState(state) case state: return #state;
  switch (currentState) {
    casePrintState(RampToSoak);
    casePrintState(Soak);
    casePrintState(RampUp);
    casePrintState(Peak);
    casePrintState(CoolDown);
    casePrintState(Complete);
    casePrintState(PreTune);
    casePrintState(Tune);
    casePrintState(Manual);
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

void makeDefaultProfile() {
  snprintf(activeProfile.name,PROFILE_NAME_LENGTH,"%s","Default");
  activeProfile.soakTemp     = 130;
  activeProfile.soakDuration =  80;
  activeProfile.peakTemp     = 220;
  activeProfile.peakDuration =  40;
  activeProfile.rampUpRate   =   0.80;
  activeProfile.rampDownRate =   2.0;
}
void makeDefaultPID() {
  heaterPID.Kp =  0.60; 
  heaterPID.Ki =  0.01;
  heaterPID.Kd = 19.70;
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

bool savePID() {
  PREF.putBytes("PID", (uint8_t*)&heaterPID, sizeof(heaterPID));  
  return true;
}

bool loadPID() {
  
  size_t length = PREF.getBytesLength("PID");
  
  if(length!=sizeof(heaterPID)){
    makeDefaultPID();
    Serial.println("load default PID");
  }
  else
  {
    PREF.getBytes("PID", (uint8_t*)&heaterPID, length);
  }  
  return true;  
}


void factoryReset() {
  Serial.println("Factory reset");

  PREF.clear(); 

  activeProfileId = 0;
  makeDefaultProfile();
}

void saveLastUsedProfile() {
  PREF.putUChar("ProfileID",activeProfileId);
}

void loadLastUsedProfile() {
  activeProfileId = PREF.getUChar("ProfileID", 0);
  loadParameters(activeProfileId);
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
  ledcSetup(0,1000,0);

  //LEDs:
  RGBLED.begin(RGB_CLK,RGB_SDO,RGB_SDO,0);  
  setLEDRGBBColor(0,0,0);
  
  //Preferences init
  PREF.begin("REFLOW");
  loadLastUsedProfile();
  loadPID();
  
  //init Wifi:
  WiFi.begin();            
 
  //init Webserver
  if (MDNS.begin("ReflowController")) {
      Serial.println("MDNS responder started");
  }
  server.on("/", []() {
    server.sendHeader("Cache-Control","no-cache");
    server.send(200, "text/html", ROOT_HTML);
  });
  server.on("/status", []() {
    server.sendHeader("Cache-Control","no-cache");
    char buffer[320];
    unsigned long time = (esp_timer_get_time()-cycleStartTime)/1000;
    snprintf(buffer,320,"{\"time\": %lu, \"temp\": %.2f, \"dt\": %.2f, \"setpoint\":  %.2f, \"power\": %.2f, \"state\": \"%s\", \"fault\": \"%s\"}",time,aktSystemTemperature,aktSystemTemperatureRamp,heaterSetpoint,heaterOutput*100/256,currentStateToString(),globalErrorText);
    server.send(200, "application/json", buffer);
  });
  serverAction.on("/start", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    if(currentState == Ready && !globalError)
    {
      //Start Reflow!
      cycleStartTime = esp_timer_get_time();
      currentState = RampToSoak;
      serverAction.send(200, "text/plain", "OK");
    }
    else
    {
      serverAction.send(200, "text/plain", "ERROR");
    }
  });
  serverAction.on("/stop", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    bool ok=false;
    if (currentState == Complete) 
    { 
      currentState = Ready;
      ok=true;
    }
    else if (currentState == CoolDown) 
    {
      currentState = Complete;
      ok=true;
    }
    else if (currentState > ProcessStart) 
    {
      currentState = CoolDown;
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
  // autotune, manual heating, profile switching and factory reset would have no
  // trigger at all. Richer profile editing follows on the main server.
  serverAction.on("/tune", []() {
    serverAction.sendHeader("Cache-Control","no-cache");
    serverAction.sendHeader("Access-Control-Allow-Origin","*");
    if(currentState == Ready && !globalError)
    {
      cycleStartTime = esp_timer_get_time();
      currentState = PreTune; // preheat until stable, then hand off to Tune
      serverAction.send(200, "text/plain", "OK");
    }
    else
    {
      serverAction.send(409, "text/plain", "ERROR");
    }
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

    static State previousState= None; // sentinel: never equals a real state
    static uint64_t stateChangedTime_ms=time_ms;
    boolean stateChanged=false;
    if (currentState != previousState) 
    {
      stateChangedTime_ms=time_ms;
      stateChanged = true;
      previousState = currentState;
    }
    static float rampToSoakStartTemp;
    static float coolDownStartTemp;
    
    heaterInput = aktSystemTemperature; 

    switch (currentState) 
    {
      case RampToSoak:
        if (stateChanged) 
        {

          rampToSoakStartTemp=aktSystemTemperature;
          heaterSetpoint = rampToSoakStartTemp;

          PID.SetMode(AUTOMATIC);
          PID.SetControllerDirection(DIRECT);
          PID.SetTunings(heaterPID.Kp, heaterPID.Ki, heaterPID.Kd);
        }

        heaterSetpoint = rampToSoakStartTemp + (activeProfile.rampUpRate * (time_ms-stateChangedTime_ms)/1000.0);

        if (heaterSetpoint >= activeProfile.soakTemp) 
        {
          currentState = Soak;
        }
        break;

      case Soak:

        heaterSetpoint = activeProfile.soakTemp;

        if (time_ms - stateChangedTime_ms >= (uint32_t)activeProfile.soakDuration * 1000) 
        {
          currentState = RampUp;
        }
        break;

      case RampUp:

        heaterSetpoint = activeProfile.soakTemp + (activeProfile.rampUpRate * (time_ms-stateChangedTime_ms)/1000.0);

        if (heaterSetpoint >= activeProfile.peakTemp) 
        {
          currentState = Peak;
        }
        break;

      case Peak:

        heaterSetpoint = activeProfile.peakTemp;

        if (time_ms - stateChangedTime_ms >= (uint32_t)activeProfile.peakDuration * 1000) {
          currentState = CoolDown;
        }
        break;

      case CoolDown:
        if (stateChanged) {
          PID.SetMode(MANUAL);

          beepcount=3;  //Beep! We need the door open!!!

          //rampDown from the last setpoint
          coolDownStartTemp=heaterSetpoint;
        }

        heaterSetpoint = coolDownStartTemp - (activeProfile.rampDownRate * (time_ms - stateChangedTime_ms) / 1000.0);
        heaterOutput = 0;

        if (heaterSetpoint < IDLE_TEMP) {
            heaterSetpoint = IDLE_TEMP;
        }
        
        if (aktSystemTemperature < IDLE_TEMP && heaterSetpoint == IDLE_TEMP) {
          currentState = Complete;
          PID.SetMode(MANUAL);

          beepcount=1;  //Beep! We are done!!!

        }
        break;
      case PreTune:
        if(stateChanged)
        {
        	PID.SetMode(MANUAL);
        	heaterSetpoint = aktSystemTemperature;
            heaterOutput = 255*tuningHeaterOutput/100;
        }
        if(heaterSetpoint+tuningNoiseBand <aktSystemTemperature || heaterSetpoint-tuningNoiseBand >aktSystemTemperature ) {
          stateChangedTime_ms=time_ms;
          heaterSetpoint = aktSystemTemperature;
        }
        if (time_ms - stateChangedTime_ms >= tuningLookbackSec*2 * 1000) {
          currentState = Tune;
        }
        break;
      case Tune:
        if (stateChanged) 
        {
          heaterSetpoint = aktSystemTemperature;
          
          PIDTune.Cancel();
          heaterOutput = 255*tuningHeaterOutput/100;
          PIDTune.SetNoiseBand(tuningNoiseBand);
          PIDTune.SetOutputStep(255*tuningOutputStep/100);
          PIDTune.SetLookbackSec(tuningLookbackSec);
          PIDTune.SetControlType(CT_PID_NO_OVERSHOOT); //We want NO Overshoot :-)
        }

        int8_t val = PIDTune.Runtime();

        if (val != 0) 
        {
          currentState = CoolDown;
          heaterPID.Kp = PIDTune.GetKp();
          heaterPID.Ki = PIDTune.GetKi();
          heaterPID.Kd = PIDTune.GetKd();

          savePID();

          Serial.printf("Autotune done: Kp=%.2f Ki=%.2f Kd=%.2f\n",
                        heaterPID.Kp, heaterPID.Ki, heaterPID.Kd);
        }

        break;
    }

    PID.Compute();

    if (
         currentState == RampToSoak ||
         currentState == Soak ||
         currentState == RampUp ||
         currentState == Peak ||
         currentState == PreTune ||
         currentState == Tune          
       )
    {
  
      if (heaterSetpoint+100 < aktSystemTemperature) // if we're 100 degree cooler than setpoint, abort
      {
        reportError("Temperature is Way to HOT!!!!!"); 
      }
      powerHeater = (uint8_t)heaterOutput;
    } 
    else if(currentState == Manual)
    {
      powerHeater=((uint16_t)manualPower*255)/100;
    }
    else
    {
      powerHeater =0;
    }
  }

}
