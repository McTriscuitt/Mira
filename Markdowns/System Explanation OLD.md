Good — 713 lines total. I'll walk through it in logical sections. This will be a long reply; follow along at 
  your own pace.                                                        
                                                                                                               
  ---
  Section 1: Includes & Global State (lines 1–68)                                                                                                                                                                             
  #include <Arduino.h>                                                                                           #include <WiFi.h>
  #include <HTTPClient.h>                                                                                        #include <ArduinoJson.h>                                                                                       #include <NTPClient.h>                                                                                         #include <WiFiUDP.h>                                                                                           #include <Preferences.h>                                                                                       #include <Adafruit_VEML7700.h>                                                                                 #include "config.h"                                                                                            #include "lightcurve.h"                                                                                                                                                                                                       Angle-bracket includes are library headers resolved by PlatformIO; quoted includes are your own files found    relative to src/. config.h holds all the magic numbers and credentials. lightcurve.h owns the lux→bri/ct math
   — that separation keeps main.cpp free of curve constants.
  struct LightState { bool on; int bri; int ct; };
  A plain data struct — no methods. Its only job is to carry a snapshot of what the bridge reports back.
  LightTarget sentTarget    = {255, 0};
  LightTarget prevSentTarget = {255, 0};
  Both initialized to {255, 0} — values deliberately outside the valid Hue range (bri 1–254, ct 153–447). This
  means the first shouldUpdate check in loop() will always be true, forcing an immediate PUT on the very first
  tick even if the computed target happens to match.
  ★ Insight ─────────────────────────────────────
  The sentinel {255, 0} is a clever initialization trick: rather than adding a firstRun boolean, the invalid
  values guarantee the delta check fires on tick 1. The same approach is used for prevSentTarget — if
  checkOverride fires before any PUT has ever gone out, the prev-target comparison will never falsely match a
  real bulb state.
  ─────────────────────────────────────────────────

  bool overheadsOn = false;

  Global — shared by tickNormal, tickWakeRamp, tickWindDown, checkOverride, and sendDashboardStatus. It's the  
  authoritative "are the ceiling lights on?" flag and must be kept in sync with every PUT that touches
  CEIL_1/2.

  struct ButtonState { ... };
  ButtonState btnMode;
  ButtonState btnCycle;

  ButtonState bundles all the debounce and press-classification state for one button. Having it as a struct    
  means pollButton() can take a reference and handle any button generically without code duplication.

  float lastLux = 1.0f;

  Saved each tick so the cycle button's forceState(WAKE) can call triggerWake(lastLux) from inside the
  30-second wait loop where no fresh lux read has happened.

  enum class State { LOCKED_OUT, NORMAL, WAKE, WIND_DOWN, SOFT_PAUSE, HARD_OFF };
  State state = State::LOCKED_OUT;

  enum class (scoped enum) means State::NORMAL instead of just NORMAL — no namespace pollution, and the        
  compiler will catch any integer-to-State implicit conversions. Starts at LOCKED_OUT — the system is inert    
  until it sees a manual bedside flip.

  The remaining globals form three clusters:

  - Wind-down: stableLuxCount, windDownStep, windDownStartBri
  - Wake: wakeStep, wakeStartTarget, wakeEndTarget
  - Soft pause: softPauseStart, skipOverrideCheck, pauseResumeActive, pauseResumeStep, pauseResumeStartTarget  

  lastBedsideOn stands alone — it's the edge-detector seed for checkBedsideState.

  ---
  Section 2: Hardware Helpers (lines 74–92)

  void setRGB(bool r, bool g, bool b) {
      digitalWrite(LEDR, r ? LOW : HIGH);
      ...
  }

  The Nano ESP32-S3 onboard RGB is active LOW — LOW turns the color on. setRGB(true, false, false) = red on,   
  all others off. The inversion is encapsulated here so every call site thinks in "true = on" terms.

  void connectWiFi() {
      ...
      while (WiFi.status() != WL_CONNECTED) {
          setRGB(true, false, false); delay(300);
          setRGB(false, false, false); delay(300);
      }
      setRGB(false, true, false);
      ...
  }

  This is a blocking spin during setup() — totally fine because nothing else needs to run yet. Red blink =     
  trying, solid green = connected. The 800ms delay after is just a visual beat so the LED state is readable.   

  ---
  Section 3: Hue API Layer (lines 94–139)

  LightState getLightState(int id) {
      ...
      if (http.GET() == 200) {
          JsonDocument doc;
          deserializeJson(doc, http.getString());
          result.on  = doc["state"]["on"].as<bool>();
          result.bri = doc["state"]["bri"].as<int>();
          result.ct  = doc["state"]["ct"].as<int>();
      }
      http.end();
      return result;
  }

  Returns a zeroed {false, 0, 0} if the GET fails — callers must be aware. In triggerWake, there's a guard:    
  (bedside.bri > 0) ? bedside.bri : S4_FLOOR_BRI handles the failure case gracefully.

  ★ Insight ─────────────────────────────────────
  http.getString() reads the full response body into a String before parsing. On a constrained device this     
  costs RAM, but ArduinoJson also supports streaming parse from the response stream — that would avoid the     
  intermediate String. For 4 bulbs at ~200 bytes each, it's fine, but worth knowing if heap ever gets tight.   
  ─────────────────────────────────────────────────

  void setLight(int id, bool on, int bri, int ct, int transitiontime) {
      String body = "{\"on\":" + String(on ? "true" : "false") + ...}
      http.PUT(body);
  }

  Manual JSON string building instead of JsonDocument — faster and avoids an extra allocation for a body this  
  simple. transitiontime is in 100ms units per the Hue API: 300 = 30 seconds, matching the poll interval so    
  transitions are seamless.

  setLightColor is structurally identical but sends hue/sat instead of ct — used only for the startup purple   
  flash in setup().

  ---
  Section 4: Utilities (lines 141–181)

  String getTimeString() {
      int hour12 = h % 12;
      if (hour12 == 0) hour12 = 12;  // midnight/noon fix
      const char* ampm = h < 12 ? "AM" : "PM";
      return t + " " + days[day];
  }

  The hour12 == 0 guard handles midnight (00:xx → 12:xx AM) and noon (12:xx → stays 12:xx PM) correctly. Shared
   between printStatus and every sendLog call.

  printStatus() duplicates the time formatting logic that getTimeString() already handles — it was probably    
  written first, then getTimeString() was factored out for log messages. Minor redundancy; low priority.       

  ---
  Section 5: Dashboard Communication (lines 183–270)

  void sendDashboardLog(const String& message) {
      http.addHeader("Authorization", "Bearer " + String(ESP32_API_KEY));
      doc["message"] = message;
      ...
      http.POST(body);
  }

  void sendLog(const String& message) {
      sendDashboardLog(message);
  }

  sendLog is a thin wrapper — it exists so callers don't need to know the underlying function name, and you    
  could add a second sink (serial, SD card) here without touching every call site.

  void sendDashboardStatus(float lux) {
      doc["state"]       = stateName();
      doc["lux"]         = lux;
      doc["bri"]         = sentTarget.bri;
      doc["ct"]          = sentTarget.ct;
      doc["overhead_on"] = overheadsOn;
      doc["wind_down_step"] = windDownStep;
      doc["wake_step"]      = wakeStep;
      doc["wake_total"]     = WAKE_RAMP_TICKS;
      long pauseRemaining = (state == State::SOFT_PAUSE) ?
          max(0L, ((long)SOFT_PAUSE_MS - (long)(millis() - softPauseStart)) / 1000L) : 0L;
      doc["soft_pause_remaining_s"] = pauseRemaining;
  }

  This is the heartbeat — posted at the end of every tick. The (long) casts before the subtraction are
  important: millis() returns unsigned long, and SOFT_PAUSE_MS is unsigned long; without the casts, the        
  subtraction wraps on underflow instead of going negative, making max(0L, ...) useless.

  ★ Insight ─────────────────────────────────────
  millis() wraps at ~49.7 days (UINT32_MAX / 1000). The soft pause remaining calculation is technically        
  vulnerable to a millis() rollover mid-pause — but given the system's design (it reboots via power cycles     
  regularly), this is a non-issue in practice.
  ─────────────────────────────────────────────────

  void pollDashboardCommand() {
      ...
      if (cmd == "WIND_DOWN") {
          forceState(State::WIND_DOWN);
          if (cmdValue >= 0) windDownStep = min(cmdValue, 119);
      }
      ...
      if (cmdId >= 0) {
          ack.POST("");  // acknowledge
      }
  }

  Note the wind-down seek cap: min(cmdValue, 119) not 120 — step 120 is the completion step that fires state = 
  LOCKED_OUT, so the dashboard can't seek past 119. There's a forward declaration for forceState at line 225   
  since pollDashboardCommand calls it before it's defined.

  ---
  Section 6: Persistence (lines 272–277)

  void saveLastState(uint8_t bri, uint16_t ct) {
      prefs.begin("mira", false);
      prefs.putUChar("savedBri", bri);
      prefs.putUShort("savedCt", ct);
      prefs.end();
  }

  Writes to ESP32 NVS (Non-Volatile Storage) via the Preferences library. The false arg means read-write mode. 
  Called on every bulb update and at wind-down completion. The saved values are no longer read at boot —       
  triggerWake polls actual bedside state instead. So this is currently write-only persistence, essentially     
  unused on the read side.

  ---
  Section 7: State Tick Functions (lines 279–478)

  tickWakeRamp (279–311)

  void tickWakeRamp(float lux) {
      wakeStep++;
      float t = min(wakeStep / (float)WAKE_RAMP_TICKS, 1.0f);
      int rampBri = wakeStartTarget.bri + t * (wakeEndTarget.bri - wakeStartTarget.bri);
      int rampCt  = wakeStartTarget.ct  + t * (wakeEndTarget.ct  - wakeStartTarget.ct);

  Linear interpolation: t goes 0→1 over 40 ticks (20 min). The min(..., 1.0f) clamps t so step 41+ doesn't     
  overshoot.

      if (wakeStep == WAKE_RAMP_TICKS / 4 || ...) sendLog(...);
      if (t >= 1.0f) {
          state = State::NORMAL;
          skipOverrideCheck = true;
          ...
      }
      sentTarget = {(uint8_t)rampBri, (uint16_t)rampCt};

  skipOverrideCheck = true at wake completion: the bulbs just changed — if checkOverride fires immediately on  
  the first NORMAL tick, it would see the newly ramped values and potentially flag them as a manual change. The
   skip consumes the flag for one tick only (see checkOverride).

  ★ Insight ─────────────────────────────────────
  sentTarget is updated after the PUTs, not before — so prevSentTarget (used by override detection) always     
  reflects what was sent in the previous tick, not the current one. The ordering matters: if you moved the     
  assignment before the PUT, override detection would lose its one-tick lag buffer.
  ─────────────────────────────────────────────────

  tickWindDown (313–343)

  void tickWindDown() {
      windDownStep++;
      if (overheadsOn) {
          setLight(LIGHT_CEIL_1, false, 0, 0, 10);
          setLight(LIGHT_CEIL_2, false, 0, 0, 10);
          overheadsOn = false;
      }
      float windDownBri = windDownStartBri - (windDownStartBri - S4_FLOOR_BRI) * (windDownStep / 120.0f);      

  Overheads are shut off immediately on the first wind-down tick, regardless of step. The dim formula runs from
   windDownStartBri down to S4_FLOOR_BRI over exactly 120 steps (60 min at 30s/tick).

      if (windDownStep < 120) {
          setLight(LIGHT_DESK, true, wdBri, wdCt, 300);
      } else {
          setLight(LIGHT_DESK, false, wdBri, wdCt, 100);
          state = State::LOCKED_OUT;
          saveLastState((uint8_t)S4_FLOOR_BRI, (uint16_t)CT_WARM);
      }

  Desk dims alongside bedside for 120 steps, then turns off at step 120 when the state transitions. Bedside is 
  left on at S4_FLOOR_BRI — it stays on all night at minimum brightness until the user turns it off manually,  
  which re-arms the lockout.

  isManualOverride (345–355)

  bool isManualOverride(LightState ls, bool expectedOn) {
      if (expectedOn && !ls.on) return true;        // turned off
      if (ls.on) {
          bool matchesCurrent = abs(ls.bri - sentTarget.bri) <= STATE_TOLERANCE && ...;
          bool matchesPrev    = abs(ls.bri - prevSentTarget.bri) <= STATE_TOLERANCE && ...;
          if (!matchesCurrent && !matchesPrev) return true;  // value drifted
      }
      return false;
  }

  Two-window comparison: matches either the current sent target or the previous one. This absorbs Hue RF lag — 
  the bridge might still be reporting the old value one tick after a PUT. Without matchesPrev, any PUT that    
  coincides with a simultaneous lux change would falsely trigger soft pause.

  checkOverride (357–391)

  void checkOverride() {
      if (skipOverrideCheck) { skipOverrideCheck = false; return; }
      ...
      bool overridden = isManualOverride(bedside, true) || isManualOverride(desk, true);
      if (overheadsOn) { ... overridden = overridden || ...; }
      if (overridden) {
          state = State::SOFT_PAUSE;
          softPauseStart = millis();
          pauseResumeActive = false;
      }
  }

  Always polls bedside + desk; only polls overheads if overheadsOn. This saves 2 HTTP GETs per tick when the   
  ceilings are off. The skipOverrideCheck flag is consumed on use — it's a one-shot, not a persistent suppress.

  tickNormal (393–466)

  This is the most complex tick function. Order of operations:

  1. checkOverride() — may flip state to SOFT_PAUSE
  2. Early return if SOFT_PAUSE (override just triggered)
  3. Overhead edge detection — force shouldUpdate true on crossing
  4. Pause resume ramp — if active, interpolates from pre-pause values to current ambient, then return (skips  
  rest of normal logic)
  5. Stable lux counter — increment if lux ≤ 10 after 9 PM, else decrement (floor 0)
  6. Wind-down trigger at count ≥ 60
  7. Conditional PUT if shouldUpdate

  ★ Insight ─────────────────────────────────────
  The stable lux counter at line 433 uses weighted decay — it decrements (not resets) when the condition       
  breaks. This means brief lux spikes (a lamp turning on briefly) don't reset the 30-minute accumulation; the  
  system is resilient to transient disturbances. 60 readings × 30s = 30 min minimum, but effectively ~60 min   
  with the decrement logic because each non-qualifying tick costs you a tick of progress.
  ─────────────────────────────────────────────────

  if (shouldUpdate) {
      ...
      prevSentTarget = sentTarget;
      sentTarget = target;
      saveLastState(...);
  }

  prevSentTarget is updated only on a PUT — not every tick. This is correct: the override detector needs to    
  know what the bulb was last told to be at, not what the curve currently computes.

  tickSoftPause (468–478)

  void tickSoftPause() {
      if (millis() - softPauseStart >= SOFT_PAUSE_MS) {
          pauseResumeStartTarget = sentTarget;
          pauseResumeActive      = true;
          pauseResumeStep        = 0;
          state                  = State::NORMAL;
          skipOverrideCheck      = true;
      }
  }

  On expiry: seeds the resume ramp from sentTarget (last known bulb state), arms pauseResumeActive, jumps to   
  NORMAL. The skipOverrideCheck here prevents the first NORMAL tick from immediately detecting the resume      
  ramp's intermediate values as an override.

  ---
  Section 8: Wake & Lockout (lines 480–512)

  triggerWake (480–491)

  void triggerWake(float ambientLux) {
      LightState bedside = getLightState(LIGHT_BEDSIDE);
      uint8_t startBri = (bedside.on && bedside.bri > 0) ? bedside.bri : S4_FLOOR_BRI;
      uint16_t startCt = (bedside.ct > 0) ? bedside.ct : CT_WARM;
      wakeStartTarget  = {startBri, startCt};
      wakeEndTarget    = luxToTarget(ambientLux);
      wakeStep         = 0;
      state            = State::WAKE;
  }

  Reads actual bedside state at trigger time — not NVS. This means if someone manually set the bedside to a    
  custom color, the ramp starts from exactly there rather than wherever Mira last set it. ct > 0 guard handles 
  the HTTP failure fallback.

  checkBedsideState (493–512)

  void checkBedsideState(float lux) {
      LightState bedside = getLightState(LIGHT_BEDSIDE);

      if (state == State::LOCKED_OUT && !lastBedsideOn && bedside.on)
          triggerWake(lux);  // rising edge → wake

      if (lastBedsideOn && !bedside.on && timeClient.getHours() >= LOCKOUT_RESET_HOUR) {
          // falling edge after 9 PM — check if all lights are off
          LightState desk = ...; LightState ceil1 = ...; LightState ceil2 = ...;
          if (!desk.on && !ceil1.on && !ceil2.on) {
              state = State::LOCKED_OUT;
              stableLuxCount = 0;
              windDownStep   = 0;
          }
      }
      lastBedsideOn = bedside.on;
  }

  ★ Insight ─────────────────────────────────────
  The re-arm check fires on bedside falling edge, not on a timer. This means you can turn off the bedside light
   at 9:01 PM or 11:59 PM and it re-arms either way. The LOCKOUT_RESET_HOUR check prevents accidental re-arming
   if you turn the bedside off briefly during the day. The 4-light confirmation prevents partial shutdowns from
   re-arming too early.
  ─────────────────────────────────────────────────

  checkBedsideState is called in LOCKED_OUT, NORMAL, and WIND_DOWN — not in WAKE, SOFT_PAUSE, or HARD_OFF.     
  During NORMAL this is primarily for re-arming; during WIND_DOWN it's to catch an early manual shutoff.       

  ---
  Section 9: Button Handling (lines 514–619)

  pollButton (514–539)

  void pollButton(ButtonState& btn, int pin) {
      bool raw = (digitalRead(pin) == LOW);
      unsigned long now = millis();

      if (raw == btn.debounced) {
          btn.lastDebounceMs = now;  // keep timer fresh
      } else if (now - btn.lastDebounceMs >= DEBOUNCE_MS) {
          btn.debounced = raw;
          if (raw) { btn.held = true; btn.pressStartMs = now; }
          else {
              btn.held = false;
              if (!btn.longFired) btn.shortPress = true;
              btn.longFired = false;
          }
      }

      if (btn.held && !btn.longFired && now - btn.pressStartMs >= LONG_PRESS_MS) {
          btn.longPress = true;
          btn.longFired = true;
      }
  }

  ★ Insight ─────────────────────────────────────
  This debouncer resets lastDebounceMs while the signal is stable (not just when it changes). The effect: the  
  debounce timer measures "how long has the signal been different from last committed state." The timer resets 
  every stable poll so it only accumulates when signal is bouncing. This is the correct pattern — many naive   
  implementations accidentally reset the timer on every transition instead.

  Short press fires on release (not on press-down). This is the right UX choice: the user can change their mind
   during the press by holding longer. longFired prevents double-firing — once a long press fires, the
  subsequent release does not also emit a short press.
  ─────────────────────────────────────────────────

  forceState (541–571)

  void forceState(State next) {
      switch (next) {
          case State::NORMAL:
              sentTarget        = {255, 0};  // sentinel — force first PUT
              skipOverrideCheck = true;
              stableLuxCount    = 0;
              ...
          case State::WAKE:
              triggerWake(lastLux);  // sets state internally
              break;
          case State::WIND_DOWN:
              windDownStartBri = (sentTarget.ct > 0) ? sentTarget.bri : luxToTarget(lastLux).bri;

  sentTarget.ct > 0 in the WIND_DOWN case: if sentTarget is still the {255, 0} sentinel (i.e., no PUT has ever 
  gone out), fall back to luxToTarget(lastLux).bri. This prevents windDownStartBri = 255, which would produce a
   very dramatic ramp.

  handleButtonEvents (584–619)

  Long press toggles HARD_OFF. Short press: from NORMAL → SOFT_PAUSE; from anything else → NORMAL. Hard off is 
  exempt from short press — you can only exit HARD_OFF via long press.

  Note that the short press to NORMAL path at line 612 manually sets sentTarget = {255, 0} and
  skipOverrideCheck = true, mirroring forceState(NORMAL) — it's a manual inline rather than calling forceState.
   This slight duplication is a minor inconsistency.

  ---
  Section 10: setup() (lines 621–657)

  void setup() {
      // RGB init → connect WiFi → NTP sync → VEML init
      ...
      LightState saved = getLightState(LIGHT_BEDSIDE);
      setLightColor(LIGHT_BEDSIDE, true, 200, 48000, 200, 10); // purple flash
      delay(3000);
      setLight(LIGHT_BEDSIDE, saved.on, saved.bri, saved.ct, 10); // restore

      lastBedsideOn = getLightState(LIGHT_BEDSIDE).on;
      overheadsOn   = getLightState(LIGHT_CEIL_1).on;
      sendLog("Online — " + getTimeString());
  }

  ★ Insight ─────────────────────────────────────
  The startup sequence seeds both lastBedsideOn and overheadsOn from actual bridge state before the first      
  loop() tick. Without this, if the bedside was already on at boot, the edge detector would see false → true on
   tick 1 and trigger a spurious wake. Similarly, if the overheads were already on, checkOverride would report 
  them as unexpected. Both seeds prevent false-positive events on the very first loop iteration.
  ─────────────────────────────────────────────────

  ---
  Section 11: loop() (lines 659–713)

  void loop() {
      timeClient.update();
      printStatus();

      float lux = veml.readLux(VEML_LUX_AUTO);
      lastLux = lux;
      LightTarget target = luxToTarget(lux);

      bool shouldUpdate = abs(target.bri - sentTarget.bri) > STATE_TOLERANCE ||
      bool shouldUpdate = abs(target.bri - sentTarget.bri) > STATE_TOLERANCE ||
                          abs(target.ct  - sentTarget.ct)  > STATE_TOLERANCE;

      pollDashboardCommand();

      switch (state) { ... }

      switch (state) { ... }
      sendDashboardStatus(lux);

      unsigned long tickStart = millis();
      setLightColor(LIGHT_BEDSIDE, true, 200, 48000, 200, 10); // purple flash
      delay(3000);
      setLight(LIGHT_BEDSIDE, saved.on, saved.bri, saved.ct, 10); // restore

      lastBedsideOn = getLightState(LIGHT_BEDSIDE).on;
      overheadsOn   = getLightState(LIGHT_CEIL_1).on;
      sendLog("Online — " + getTimeString());
  }

  ★ Insight ─────────────────────────────────────
  The startup sequence seeds both lastBedsideOn and overheadsOn from actual bridge state before the first loop()   
  tick. Without this, if the bedside was already on at boot, the edge detector would see false → true on tick 1 and
   trigger a spurious wake. Similarly, if the overheads were already on, checkOverride would report them as        
  unexpected. Both seeds prevent false-positive events on the very first loop iteration.
  ─────────────────────────────────────────────────

  ---
  Section 11: loop() (lines 659–713)

  void loop() {
      timeClient.update();
      printStatus();

      float lux = veml.readLux(VEML_LUX_AUTO);
      lastLux = lux;
      LightTarget target = luxToTarget(lux);
      lastLux = lux;
      LightTarget target = luxToTarget(lux);

      bool shouldUpdate = abs(target.bri - sentTarget.bri) > STATE_TOLERANCE ||
                          abs(target.ct  - sentTarget.ct)  > STATE_TOLERANCE;

      pollDashboardCommand();

      switch (state) { ... }

      sendDashboardStatus(lux);

      unsigned long tickStart = millis();

  Long press toggles HARD_OFF. Short press: from NORMAL → SOFT_PAUSE; from anything else → NORMAL. Hard off is     
  exempt from short press — you can only exit HARD_OFF via long press.

  Note that the short press to NORMAL path at line 612 manually sets sentTarget = {255, 0} and skipOverrideCheck = 
  true, mirroring forceState(NORMAL) — it's a manual inline rather than calling forceState. This slight duplication
   is a minor inconsistency.

  ---
  Section 10: setup() (lines 621–657)
  void setup() {
      // RGB init → connect WiFi → NTP sync → VEML init
      ...
      LightState saved = getLightState(LIGHT_BEDSIDE);
      setLightColor(LIGHT_BEDSIDE, true, 200, 48000, 200, 10); // purple flash
      delay(3000);
      setLight(LIGHT_BEDSIDE, saved.on, saved.bri, saved.ct, 10); // restore

      lastBedsideOn = getLightState(LIGHT_BEDSIDE).on;
      overheadsOn   = getLightState(LIGHT_CEIL_1).on;
      sendLog("Online — " + getTimeString());
  }

  ★ Insight ─────────────────────────────────────
  The startup sequence seeds both lastBedsideOn and overheadsOn from actual bridge state before the first loop()   
  tick. Without this, if the bedside was already on at boot, the edge detector would see false → true on tick 1 and
   trigger a spurious wake. Similarly, if the overheads were already on, checkOverride would report them as        
  unexpected. Both seeds prevent false-positive events on the very first loop iteration.
  ─────────────────────────────────────────────────

  ---
  Section 11: loop() (lines 659–713)

  void loop() {
      timeClient.update();
      printStatus();

      float lux = veml.readLux(VEML_LUX_AUTO);
      lastLux = lux;
      LightTarget target = luxToTarget(lux);

      bool shouldUpdate = abs(target.bri - sentTarget.bri) > STATE_TOLERANCE ||
                          abs(target.ct  - sentTarget.ct)  > STATE_TOLERANCE;

      pollDashboardCommand();

      switch (state) { ... }

      sendDashboardStatus(lux);

      bool shouldUpdate = abs(target.bri - sentTarget.bri) > STATE_TOLERANCE ||
                          abs(target.ct  - sentTarget.ct)  > STATE_TOLERANCE;

      pollDashboardCommand();

      switch (state) { ... }

      sendDashboardStatus(lux);

      unsigned long tickStart = millis();
      while (millis() - tickStart < 30000UL) {
          pollButton(btnMode,  BTN_MODE);
          pollButton(btnCycle, BTN_CYCLE);
          handleButtonEvents();
          handleCycleButton();
          delay(50);
      }
  }

  The full tick flow every 30 seconds:

  1. NTP update + status print
  2. Lux read → curve lookup → shouldUpdate delta check
  3. Dashboard command poll (remote control)
  4. State dispatch
  5. Dashboard status post
  6. 600 button polls (30,000 ms ÷ 50 ms) — the system stays responsive to physical input throughout the wait  

  ★ Insight ─────────────────────────────────────
  shouldUpdate is computed once from the raw curve output and passed into tickNormal. But tickNormal can force 
  shouldUpdate = true on overhead edge transitions regardless of lux change. This is the correct design — it   
  avoids duplicating the delta logic and makes the override site explicit. Note that shouldUpdate is a local   
  variable, so the forced-true inside tickNormal doesn't "leak" back to loop.

  pollDashboardCommand() runs before the state dispatch. This means a command received this tick is acted upon 
  immediately in the same tick — no one-tick lag. The forceState it calls might change state, which the        
  subsequent switch will see correctly.
  ─────────────────────────────────────────────────

  ---
  That's the full file. A few things worth flagging as potential follow-ups if you want them:

  - printStatus duplicates time formatting that getTimeString already does (lines 165–181)
  - saveLastState is write-only — the saved values are never read back
  - handleButtonEvents manually mirrors forceState(NORMAL) instead of calling it (lines 612–615)

  Want to go deeper on any specific section, or talk through any of the design decisions?