#pragma once
//
// bridge.h — everything Hue: the TLS cert pin, the light cache, Mira's commands,
// and the Core-0 SSE task that classifies every bridge event as either Mira's own
// echo or a change you made (Markdowns/MIRA_3.md §5, §8). It talks to the main
// task only through evQueue and holdPending.
//
#include <Arduino.h>
#include "lights.h"

struct LightSnapshot {
    bool  on;
    float bri;
    int   ct;
};

enum class BridgeResult : uint8_t { Sent, Failed, Aborted };

void          bridgeBegin();  // setup(): cert pin + one GET to seed the cache (blocking)
void          bridgeStart();  // setup(): open the event stream, start the SSE task
LightMask     bridgeOnMask();
LightSnapshot bridgeCached(int idx);

// One command to one light (on: brightness + CT; off: just off), carried out by
// the bulb over durMs. Aborted while holdPending is up — see below.
BridgeResult  bridgePut(int idx, bool on, float bri, uint16_t ct, uint32_t durMs);
// PRESET_OFF_LIGHTS: brightness + CT to a light that's off, leaving it off.
void          bridgePreset(int idx, float bri, uint16_t ct, uint32_t durMs);
// Boot flourish: deep purple for 3 s, then back to how it was.
void          bridgeFlourish(int idx);
// After a soft reset, re-record a command that's still running so its echoes
// (if the bridge reports progress mid-fade) aren't mistaken for your changes.
void          bridgeSeedTrajectory(int idx, float b0, float b1, uint32_t elapsedMs, uint32_t durMs);

void          bridgeDumpDiagnostics(int idx);  // Serial: what led up to a hold
uint32_t      bridgeSseStackFree();

// Set by the SSE task the instant it sees a brightness change Mira didn't make.
// bridgePut() refuses to send while it's up, so the rest of a multi-light batch
// can't land on top of your change. The main task clears it when it applies the
// hold (Director::onUserBrightness).
extern volatile bool holdPending;
