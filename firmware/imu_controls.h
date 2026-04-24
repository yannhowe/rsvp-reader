#ifndef IMU_CONTROLS_H
#define IMU_CONTROLS_H

#include <Arduino.h>

// =============================================================================
// IMU Controls — BOOT button + QMI8658 tilt gestures
// =============================================================================

// Event types emitted to the application
enum ControlEvent {
    EVT_NONE = 0,
    EVT_PLAY_PAUSE,      // Short press BOOT button (<500ms)
    EVT_MENU,            // Long press BOOT button (>1s)
    EVT_NEXT_CHAPTER,    // Double press BOOT or tilt forward
    EVT_PREV_CHAPTER,    // Tilt backward
    EVT_WPM_UP,          // Tilt right (+25 WPM)
    EVT_WPM_DOWN,        // Tilt left (-25 WPM)
    EVT_SELECT,          // Long press (in menu context)
    EVT_NEXT_ITEM,       // Short press (in menu context)
};

// Screen orientation
enum Orientation {
    ORIENT_PORTRAIT = 0,   // 320×820 (default, natural)
    ORIENT_LANDSCAPE = 1,  // 820×320 (rotated 90° CW)
};

// Callback invoked when a control event fires
typedef void (*control_event_cb_t)(ControlEvent evt);

// Callback invoked when orientation changes
typedef void (*orientation_cb_t)(Orientation orient);

// Initialize BOOT button pin mode and QMI8658 over I2C
void imu_init();

// Poll button state machine and IMU (call from Arduino loop())
void imu_tick();

// Register event callback
void imu_set_callback(control_event_cb_t cb);

// Register orientation change callback
void imu_set_orientation_callback(orientation_cb_t cb);

// Enable or disable tilt gesture detection (disable while in menus)
void imu_gestures_enable(bool enable);

// Returns true if the application is currently in a menu context.
// When true, short press → EVT_NEXT_ITEM, long press → EVT_SELECT.
void imu_set_menu_mode(bool in_menu);

// Get current orientation
Orientation imu_get_orientation();

// Lock orientation (disable auto-rotate). Pass ORIENT_PORTRAIT or ORIENT_LANDSCAPE.
void imu_lock_orientation(Orientation orient);

// Unlock orientation (re-enable auto-rotate)
void imu_unlock_orientation();

#endif // IMU_CONTROLS_H
