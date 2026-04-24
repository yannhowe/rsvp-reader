// =============================================================================
// imu_controls.cpp
// BOOT button state machine + QMI8658 tilt gesture detection
// =============================================================================

#include "imu_controls.h"
#include "user_config.h"
#include <Wire.h>
#include <math.h>

// ---------------------------------------------------------------------------
// QMI8658 register map (subset)
// ---------------------------------------------------------------------------
#define QMI8658_REG_WHO_AM_I    0x00
#define QMI8658_REG_CTRL1       0x02
#define QMI8658_REG_CTRL2       0x03
#define QMI8658_REG_CTRL7       0x07
#define QMI8658_REG_AX_L        0x35    // Accel X low byte (6 bytes: AX_L/H, AY_L/H, AZ_L/H)

#define QMI8658_WHO_AM_I_VAL    0x05
#define QMI8658_CTRL2_ACCEL_CFG 0x65    // ±8 g, 125 Hz ODR
#define QMI8658_CTRL7_ACCEL_EN  0x01    // Enable accelerometer only

// ---------------------------------------------------------------------------
// Timing constants
// ---------------------------------------------------------------------------
static const uint32_t BTN_DEBOUNCE_MS       = 50;
static const uint32_t BTN_SHORT_MAX_MS      = 500;
static const uint32_t BTN_LONG_HOLD_MS      = 1000;
static const uint32_t BTN_DOUBLE_WINDOW_MS  = 400;
static const uint32_t IMU_POLL_INTERVAL_MS  = 50;   // ~20 Hz
static const uint32_t GESTURE_COOLDOWN_MS   = 500;

// ---------------------------------------------------------------------------
// Tilt thresholds (degrees)
// ---------------------------------------------------------------------------
static const float TILT_TRIGGER  = 30.0f;
static const float TILT_DEADZONE = 15.0f;

// ---------------------------------------------------------------------------
// Button state machine
// ---------------------------------------------------------------------------
enum BtnState {
    BTN_IDLE,
    BTN_PRESSED,
    BTN_WAIT_DOUBLE,
    BTN_LONG_PRESS,
    BTN_WAIT_RELEASE,
};

// ---------------------------------------------------------------------------
// Tilt gesture tracking — one slot per axis direction
// ---------------------------------------------------------------------------
enum TiltAxis {
    TILT_ROLL_POS = 0,  // roll > +threshold  → EVT_WPM_UP
    TILT_ROLL_NEG,      // roll < -threshold  → EVT_WPM_DOWN
    TILT_PITCH_POS,     // pitch > +threshold → EVT_NEXT_CHAPTER
    TILT_PITCH_NEG,     // pitch < -threshold → EVT_PREV_CHAPTER
    TILT_AXIS_COUNT
};

static struct {
    bool            active;         // Currently past trigger threshold
    bool            returned;       // Has returned through dead zone since last fire
    uint32_t        last_fired_ms;  // Timestamp of last emission
} s_tilt[TILT_AXIS_COUNT];

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static control_event_cb_t   s_callback      = nullptr;
static orientation_cb_t     s_orient_cb     = nullptr;
static bool                 s_gestures_en   = true;
static bool                 s_menu_mode     = false;
static bool                 s_imu_ok        = false;

// Orientation
static Orientation          s_orientation   = ORIENT_PORTRAIT;
static bool                 s_orient_locked = false;
static uint32_t             s_orient_stable_ms = 0;   // How long current reading has been stable
static Orientation          s_orient_candidate = ORIENT_PORTRAIT;
static const uint32_t       ORIENT_DEBOUNCE_MS = 500; // Must hold new orientation for 500ms

// Button
static BtnState             s_btn_state     = BTN_IDLE;
static uint32_t             s_btn_ts        = 0;    // Timestamp of last significant transition
static bool                 s_btn_prev_raw  = HIGH; // Previous debounced reading

// IMU polling
static uint32_t             s_imu_last_ms   = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void emit(ControlEvent evt) {
    if (s_callback && evt != EVT_NONE) {
        s_callback(evt);
    }
}

// ---------------------------------------------------------------------------
// QMI8658 I2C helpers
// ---------------------------------------------------------------------------
static bool qmi_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(QMI8658_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool qmi_read_bytes(uint8_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(QMI8658_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    uint8_t got = Wire.requestFrom((uint8_t)QMI8658_ADDR, len);
    if (got != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

// ---------------------------------------------------------------------------
// Public: imu_init
// ---------------------------------------------------------------------------
void imu_init() {
    // Configure BOOT button
    pinMode(BOOT_BTN, INPUT_PULLUP);

    // I2C bus
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);

    // Validate WHO_AM_I
    uint8_t who = 0;
    if (!qmi_read_bytes(QMI8658_REG_WHO_AM_I, &who, 1) || who != QMI8658_WHO_AM_I_VAL) {
        // IMU absent or unresponsive — tilt gestures will be silently skipped
        s_imu_ok = false;
        return;
    }

    // Configure accelerometer: ±8 g, 125 Hz
    if (!qmi_write(QMI8658_REG_CTRL2, QMI8658_CTRL2_ACCEL_CFG)) { s_imu_ok = false; return; }
    // Enable accelerometer only
    if (!qmi_write(QMI8658_REG_CTRL7, QMI8658_CTRL7_ACCEL_EN))  { s_imu_ok = false; return; }

    s_imu_ok = true;

    // Initialise tilt tracking — all axes start "returned" so first tilt fires immediately
    for (int i = 0; i < TILT_AXIS_COUNT; i++) {
        s_tilt[i].active       = false;
        s_tilt[i].returned     = true;
        s_tilt[i].last_fired_ms = 0;
    }
}

// ---------------------------------------------------------------------------
// Public: imu_set_callback
// ---------------------------------------------------------------------------
void imu_set_callback(control_event_cb_t cb) {
    s_callback = cb;
}

// ---------------------------------------------------------------------------
// Public: imu_gestures_enable
// ---------------------------------------------------------------------------
void imu_gestures_enable(bool enable) {
    s_gestures_en = enable;
}

// ---------------------------------------------------------------------------
// Public: imu_set_menu_mode
// ---------------------------------------------------------------------------
void imu_set_menu_mode(bool in_menu) {
    s_menu_mode = in_menu;
}

// ---------------------------------------------------------------------------
// Button state machine — called every imu_tick()
// ---------------------------------------------------------------------------
static void btn_tick() {
    uint32_t now      = millis();
    bool     raw      = digitalRead(BOOT_BTN);   // LOW = pressed
    bool     pressed  = (raw == LOW);

    // Debounce: ignore if raw changed but not yet settled
    // We track transitions via s_btn_prev_raw; the debounce check happens
    // inside each state's transition logic using s_btn_ts.
    bool edge = (raw != s_btn_prev_raw);
    if (edge) {
        // Only honour the edge if enough time has passed since the last transition
        if ((now - s_btn_ts) < BTN_DEBOUNCE_MS) {
            return; // Bounce — discard
        }
        s_btn_prev_raw = raw;
        s_btn_ts = now;  // record transition time for this new edge
    }

    switch (s_btn_state) {
        // -----------------------------------------------------------------
        case BTN_IDLE:
            if (pressed) {
                s_btn_state = BTN_PRESSED;
                // s_btn_ts already updated by edge detection above
            }
            break;

        // -----------------------------------------------------------------
        case BTN_PRESSED:
            if (!pressed) {
                // Released
                uint32_t held = now - s_btn_ts;
                if (held < BTN_SHORT_MAX_MS) {
                    // Short press — could be start of a double press
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
                    // Stay in LONG_PRESS until release
                }
            }
            break;

        // -----------------------------------------------------------------
        case BTN_WAIT_DOUBLE:
            if (pressed) {
                // Second press within window → double press
                emit(EVT_NEXT_CHAPTER);
                s_btn_state = BTN_WAIT_RELEASE;
                s_btn_ts    = now;
            } else if ((now - s_btn_ts) >= BTN_DOUBLE_WINDOW_MS) {
                // Window expired — emit single short press
                ControlEvent evt = s_menu_mode ? EVT_NEXT_ITEM : EVT_PLAY_PAUSE;
                emit(evt);
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
// Evaluate one tilt axis and emit if conditions met
// ---------------------------------------------------------------------------
static void eval_tilt_axis(TiltAxis axis, float angle, ControlEvent evt) {
    uint32_t now = millis();

    // Determine sign convention: positive axes use positive threshold
    bool over_trigger   = (angle >  TILT_TRIGGER);
    bool over_neg       = (angle < -TILT_TRIGGER);
    bool is_neg_axis    = (axis == TILT_ROLL_NEG || axis == TILT_PITCH_NEG);
    bool triggered      = is_neg_axis ? over_neg : over_trigger;
    bool in_deadzone    = (fabsf(angle) < TILT_DEADZONE);

    // Track return to dead zone for re-trigger guard
    if (in_deadzone) {
        s_tilt[axis].returned = true;
        s_tilt[axis].active   = false;
    }

    if (triggered) {
        s_tilt[axis].active = true;
        if (s_tilt[axis].returned &&
            (now - s_tilt[axis].last_fired_ms) >= GESTURE_COOLDOWN_MS) {
            emit(evt);
            s_tilt[axis].last_fired_ms = now;
            s_tilt[axis].returned      = false; // Must pass through dead zone again
        }
    }
}

// ---------------------------------------------------------------------------
// IMU read + gesture dispatch — called every IMU_POLL_INTERVAL_MS
// ---------------------------------------------------------------------------
static void imu_tick_gesture() {
    if (!s_imu_ok || !s_gestures_en) return;

    uint8_t raw[6];
    if (!qmi_read_bytes(QMI8658_REG_AX_L, raw, 6)) return;

    int16_t ax = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t ay = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t az = (int16_t)((raw[5] << 8) | raw[4]);

    float roll  = atan2f((float)ay, (float)az) * (180.0f / (float)M_PI);
    float pitch = atan2f(-(float)ax, sqrtf((float)ay * ay + (float)az * az))
                  * (180.0f / (float)M_PI);

    eval_tilt_axis(TILT_ROLL_POS,  roll,  EVT_WPM_UP);
    eval_tilt_axis(TILT_ROLL_NEG,  roll,  EVT_WPM_DOWN);
    eval_tilt_axis(TILT_PITCH_POS, pitch, EVT_NEXT_CHAPTER);
    eval_tilt_axis(TILT_PITCH_NEG, pitch, EVT_PREV_CHAPTER);
}

// ---------------------------------------------------------------------------
// Orientation detection — uses gravity vector to determine portrait/landscape
// ---------------------------------------------------------------------------
static void orient_tick() {
    if (!s_imu_ok || s_orient_locked) return;

    uint8_t raw[6];
    if (!qmi_read_bytes(QMI8658_REG_AX_L, raw, 6)) return;

    int16_t ax = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t ay = (int16_t)((raw[3] << 8) | raw[2]);

    // Determine which axis has more gravity → that's "down"
    // Portrait: Y-axis dominant (device held upright, tall)
    // Landscape: X-axis dominant (device on its side, wide)
    float abs_ax = fabsf((float)ax);
    float abs_ay = fabsf((float)ay);

    // Need a clear winner (>30% more) to avoid jitter at 45°
    Orientation detected;
    if (abs_ay > abs_ax * 1.3f) {
        detected = ORIENT_PORTRAIT;
    } else if (abs_ax > abs_ay * 1.3f) {
        detected = ORIENT_LANDSCAPE;
    } else {
        // Ambiguous — keep current orientation
        s_orient_stable_ms = 0;
        return;
    }

    // Debounce: require stable reading for ORIENT_DEBOUNCE_MS
    if (detected != s_orient_candidate) {
        s_orient_candidate = detected;
        s_orient_stable_ms = millis();
        return;
    }

    if (detected != s_orientation && (millis() - s_orient_stable_ms) >= ORIENT_DEBOUNCE_MS) {
        s_orientation = detected;
        if (s_orient_cb) s_orient_cb(s_orientation);
    }
}

// ---------------------------------------------------------------------------
// Public: imu_tick — call from Arduino loop()
// ---------------------------------------------------------------------------
void imu_tick() {
    btn_tick();

    uint32_t now = millis();
    if ((now - s_imu_last_ms) >= IMU_POLL_INTERVAL_MS) {
        s_imu_last_ms = now;
        imu_tick_gesture();
        orient_tick();
    }
}

// ---------------------------------------------------------------------------
// Public: orientation API
// ---------------------------------------------------------------------------
void imu_set_orientation_callback(orientation_cb_t cb) {
    s_orient_cb = cb;
}

Orientation imu_get_orientation() {
    return s_orientation;
}

void imu_lock_orientation(Orientation orient) {
    s_orient_locked = true;
    if (orient != s_orientation) {
        s_orientation = orient;
        if (s_orient_cb) s_orient_cb(s_orientation);
    }
}

void imu_unlock_orientation() {
    s_orient_locked = false;
}
