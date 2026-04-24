// =============================================================================
// imu_controls.cpp
// BOOT button state machine — all input via button combos, no IMU
// =============================================================================

#include "imu_controls.h"
#include "user_config.h"

// ---------------------------------------------------------------------------
// Timing constants
// ---------------------------------------------------------------------------
static const uint32_t BTN_DEBOUNCE_MS       = 50;
static const uint32_t BTN_SHORT_MAX_MS      = 500;
static const uint32_t BTN_LONG_HOLD_MS      = 1000;
static const uint32_t BTN_DOUBLE_WINDOW_MS  = 400;
static const uint32_t BTN_TRIPLE_WINDOW_MS  = 400;

// ---------------------------------------------------------------------------
// Button state machine
// ---------------------------------------------------------------------------
enum BtnState {
    BTN_IDLE,
    BTN_PRESSED,
    BTN_WAIT_DOUBLE,
    BTN_WAIT_SECOND_RELEASE,
    BTN_WAIT_TRIPLE,
    BTN_LONG_PRESS,
    BTN_WAIT_RELEASE,
};

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static control_event_cb_t   s_callback      = nullptr;
static bool                 s_menu_mode     = false;

// Button
static BtnState             s_btn_state     = BTN_IDLE;
static uint32_t             s_btn_ts        = 0;    // Timestamp of last significant transition
static bool                 s_btn_prev_raw  = HIGH; // Previous debounced reading

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void emit(ControlEvent evt) {
    if (s_callback && evt != EVT_NONE) {
        s_callback(evt);
    }
}

// ---------------------------------------------------------------------------
// Button state machine — called every imu_tick()
// ---------------------------------------------------------------------------
static void btn_tick() {
    uint32_t now      = millis();
    bool     raw      = digitalRead(BOOT_BTN);   // LOW = pressed
    bool     pressed  = (raw == LOW);

    // Debounce: ignore if raw changed but not yet settled
    bool edge = (raw != s_btn_prev_raw);
    if (edge) {
        if ((now - s_btn_ts) < BTN_DEBOUNCE_MS) {
            return; // Bounce — discard
        }
        s_btn_prev_raw = raw;
        s_btn_ts = now;
    }

    switch (s_btn_state) {
        // -----------------------------------------------------------------
        case BTN_IDLE:
            if (pressed) {
                s_btn_state = BTN_PRESSED;
            }
            break;

        // -----------------------------------------------------------------
        case BTN_PRESSED:
            if (!pressed) {
                // Released
                uint32_t held = now - s_btn_ts;
                if (held < BTN_SHORT_MAX_MS) {
                    // Short press — could be start of a double/triple press
                    s_btn_state = BTN_WAIT_DOUBLE;
                    s_btn_ts    = now; // start double-press window
                } else {
                    // Medium release (between short and long) — treat as single short
                    ControlEvent evt = s_menu_mode ? EVT_NEXT_ITEM : EVT_PLAY_PAUSE;
                    emit(evt);
                    s_btn_state = BTN_IDLE;
                }
            } else {
                // Still held — check for long press
                if ((now - s_btn_ts) >= BTN_LONG_HOLD_MS) {
                    s_btn_state = BTN_LONG_PRESS;
                    ControlEvent evt = s_menu_mode ? EVT_SELECT : EVT_MENU;
                    emit(evt);
                }
            }
            break;

        // -----------------------------------------------------------------
        case BTN_WAIT_DOUBLE:
            if (pressed) {
                // Second press within window — could be double or triple
                s_btn_state = BTN_WAIT_SECOND_RELEASE;
                s_btn_ts    = now;
            } else if ((now - s_btn_ts) >= BTN_DOUBLE_WINDOW_MS) {
                // Window expired — emit single short press
                ControlEvent evt = s_menu_mode ? EVT_NEXT_ITEM : EVT_PLAY_PAUSE;
                emit(evt);
                s_btn_state = BTN_IDLE;
            }
            break;

        // -----------------------------------------------------------------
        case BTN_WAIT_SECOND_RELEASE:
            if (!pressed) {
                // Second press released — wait for possible third press
                s_btn_state = BTN_WAIT_TRIPLE;
                s_btn_ts    = now;
            }
            break;

        // -----------------------------------------------------------------
        case BTN_WAIT_TRIPLE:
            if (pressed) {
                // Third press within window → triple press → prev chapter
                emit(EVT_PREV_CHAPTER);
                s_btn_state = BTN_WAIT_RELEASE;
                s_btn_ts    = now;
            } else if ((now - s_btn_ts) >= BTN_TRIPLE_WINDOW_MS) {
                // Window expired — it was a double press → next chapter
                emit(EVT_NEXT_CHAPTER);
                s_btn_state = BTN_IDLE;
            }
            break;

        // -----------------------------------------------------------------
        case BTN_LONG_PRESS:
            // Event already emitted; wait for release
            if (!pressed) {
                s_btn_state = BTN_IDLE;
            }
            break;

        // -----------------------------------------------------------------
        case BTN_WAIT_RELEASE:
            if (!pressed) {
                s_btn_state = BTN_IDLE;
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// Public: imu_init
// ---------------------------------------------------------------------------
void imu_init() {
    pinMode(BOOT_BTN, INPUT_PULLUP);
}

// ---------------------------------------------------------------------------
// Public: imu_set_callback
// ---------------------------------------------------------------------------
void imu_set_callback(control_event_cb_t cb) {
    s_callback = cb;
}

// ---------------------------------------------------------------------------
// Public: imu_set_menu_mode
// ---------------------------------------------------------------------------
void imu_set_menu_mode(bool in_menu) {
    s_menu_mode = in_menu;
}

// ---------------------------------------------------------------------------
// Public: imu_tick — call from Arduino loop()
// ---------------------------------------------------------------------------
void imu_tick() {
    btn_tick();
}
