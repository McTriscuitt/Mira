#pragma once
//
// events.h — the one queue both background tasks feed and the main loop drains.
// Producers: the SSE task (bridge.cpp) and the dashboard task (dashboard.cpp).
// Fixed-size POD — nothing heap-backed crosses a task boundary.
//
#include <Arduino.h>
#include "lights.h"

enum class EvType : uint8_t {
    LightEdge,       // a light went on/off · flags: bit0 on, bit1 by you (not Mira's echo) · f: brightness
    UserBrightness,  // a brightness change Mira didn't make · f: brightness · i: CT
    StaleRevert,     // the bridge walked back a Mira command · flags bit0: on now
    DashCmd          // a dashboard command · a: Director::Cmd · i: value
};

struct Event {
    EvType   type  = EvType::LightEdge;
    uint8_t  a     = 0;      // light index / command
    uint8_t  flags = 0;
    int32_t  i     = 0;
    float    f     = 0.0f;
    uint32_t tMs   = 0;      // millis() at enqueue
};

extern QueueHandle_t evQueue;  // created in setup() before either task starts
bool postEvent(const Event& ev);  // non-blocking; false when the queue is full
