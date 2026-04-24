#ifndef IMU_CONTROLS_H
#define IMU_CONTROLS_H

#include <Arduino.h>

// =============================================================================
// Button Controls — BOOT button only (no IMU/tilt gestures)
// =============================================================================

// Event types emitted to the application
enum ControlEvent {
    EVT_NONE = 0,
    EVT_PLAY_PAUSE,      // Short press BOOT button (<500ms)
    EVT_MENU,            // Long press BOOT button (>1s)
    EVT_NEXT_CHAPTER,    // Double press BOOT button
    EVT_PREV_CHAPTER,    // Triple press BOOT button
    EVT_SELECT,          // Long press (in menu context)
    EVT_NEXT_ITEM,       // Short press (in menu context)
};

// Callback invoked when a control event fires
typedef void (*control_event_cb_t)(ControlEvent evt);

// Initialize BOOT button pin mode
void imu_init();

// Poll button state machine (call from Arduino loop())
void imu_tick();

// Register event callback
void imu_set_callback(control_event_cb_t cb);

// Set menu mode. When true, short press → EVT_NEXT_ITEM, long press → EVT_SELECT.
void imu_set_menu_mode(bool in_menu);

#endif // IMU_CONTROLS_H
