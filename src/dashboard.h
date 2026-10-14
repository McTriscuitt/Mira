#pragma once
//
// dashboard.h — all Railway HTTP lives on its own Core-0 task (netTask, carried
// over from 2.x N1 Stage 5): queued log lines, the latest status snapshot, and the
// /api/command poll. A slow or unreachable dashboard can never stall the lights.
//
#include <Arduino.h>
#include "lights.h"

// Everything a status POST carries, snapshotted on the main task.
struct StatusSnapshot {
    char      state[12];  // 2.x wire name — interim, so the current dashboard keeps rendering
    char      mode[8];    // 3.0 name: ASLEEP / WAKE / ACTIVE / HOLD / NAP
    float     lux;        // reporting only; -1 when the sensor is missing
    float     bri;        // the in-line level right now
    int       ct;
    bool      ceilingOn;
    int       wakeStep;
    int       wakeTotal;
    long      pauseRemainingS;  // nap or hold
    LightMask out;              // out for today (shown in the old "excluded" chips)
    LightMask on;               // what the bridge says is on
    uint32_t  heapFree;
    uint32_t  sseStackFree;
};

void dashboardStart();                       // queues + netTask (needs evQueue)
void dashLog(const char* line);              // non-blocking; drops the oldest line when full
void dashStatus(const StatusSnapshot& s);    // non-blocking; latest snapshot wins
