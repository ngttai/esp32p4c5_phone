/* Phone-style on-screen keyboard on top of lv_keyboard (see phone_keyboard.h) */
#include "esp_timer.h"
#include "phone_fonts.h"
#include "phone_keyboard.h"

extern "C" {
extern const lv_image_dsc_t phone_kb_shift_off;
extern const lv_image_dsc_t phone_kb_shift_on;
extern const lv_image_dsc_t phone_kb_shift_lock;
}

namespace {

constexpr uint32_t KB_BG = 0x1A1F25;
constexpr uint32_t KEY_BG = 0x3A4048;          // letters, digits, symbols, space
constexpr uint32_t KEY_BG_PRESSED = 0x5A626C;
constexpr uint32_t FN_BG = 0x262C33;           // Shift, Backspace, ?123, hide
constexpr uint32_t FN_TEXT = 0xD0D6DC;
constexpr uint32_t KEY_TEXT = 0xFFFFFF;
constexpr uint32_t ACCENT = 0x2F80ED;          // OK, caps lock
constexpr uint32_t ACCENT_PRESSED = 0x1F5FB8;
constexpr uint32_t SHIFT_ON_BG = 0xE8ECF0;     // one-shot Shift: light key, dark arrow
constexpr uint32_t SHIFT_ON_TEXT = 0x1A1F25;
constexpr int64_t DOUBLE_TAP_US = 400 * 1000;  // Shift twice within this = caps lock

constexpr const char *SHIFT = LV_SYMBOL_UP;
constexpr const char *BKSP = LV_SYMBOL_BACKSPACE;
constexpr const char *HIDE = LV_SYMBOL_KEYBOARD;
constexpr const char *SYM = "?123";
constexpr const char *SYM2 = "=\\<";
constexpr const char *ABC = "ABC";
constexpr const char *OK = "OK";
constexpr const char *GAP = " ";               // hidden spacer that centers the middle row (never selectable)

constexpr lv_buttonmatrix_ctrl_t ctl(int v)
{
    return static_cast<lv_buttonmatrix_ctrl_t>(v);
}

/* Key kinds: `custom_1` paints the accent (OK, caps lock Shift), `custom_2` the light one-shot Shift */
constexpr lv_buttonmatrix_ctrl_t LETTER = ctl(LV_BUTTONMATRIX_CTRL_POPOVER | LV_BUTTONMATRIX_CTRL_NO_REPEAT | 2);
constexpr lv_buttonmatrix_ctrl_t SPACER = ctl(LV_BUTTONMATRIX_CTRL_HIDDEN | 1);
constexpr lv_buttonmatrix_ctrl_t FN3 = ctl(LV_BUTTONMATRIX_CTRL_CHECKED | LV_BUTTONMATRIX_CTRL_NO_REPEAT |
                                       LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 3);
constexpr lv_buttonmatrix_ctrl_t BKSP3 = ctl(LV_BUTTONMATRIX_CTRL_CHECKED | 3);       // repeats while held
/* Bottom row in its own units (26): ?123 4 + hide 3 = OK 7, so the space bar (12) sits in the middle */
constexpr lv_buttonmatrix_ctrl_t FN4 = ctl(LV_BUTTONMATRIX_CTRL_CHECKED | LV_BUTTONMATRIX_CTRL_NO_REPEAT |
                                       LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 4);
constexpr lv_buttonmatrix_ctrl_t SPACE = ctl(LV_BUTTONMATRIX_CTRL_NO_REPEAT | 12);
constexpr lv_buttonmatrix_ctrl_t OK7 = ctl(LV_BUTTONMATRIX_CTRL_CUSTOM_1 | LV_BUTTONMATRIX_CTRL_NO_REPEAT |
                                       LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 7);
constexpr lv_buttonmatrix_ctrl_t SHIFT_ON = ctl(LV_BUTTONMATRIX_CTRL_CUSTOM_2 | LV_BUTTONMATRIX_CTRL_NO_REPEAT |
                                            LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 3);
constexpr lv_buttonmatrix_ctrl_t SHIFT_LOCK = ctl(LV_BUTTONMATRIX_CTRL_CUSTOM_1 | LV_BUTTONMATRIX_CTRL_NO_REPEAT |
                                              LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 3);

/* Every letter row adds up to 20 units, so keys line up between rows and pages */
const char *const MAP_LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    GAP, "a", "s", "d", "f", "g", "h", "j", "k", "l", GAP, "\n",
    SHIFT, "z", "x", "c", "v", "b", "n", "m", BKSP, "\n",
    SYM, HIDE, " ", OK, ""
};
const char *const MAP_UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    GAP, "A", "S", "D", "F", "G", "H", "J", "K", "L", GAP, "\n",
    SHIFT, "Z", "X", "C", "V", "B", "N", "M", BKSP, "\n",
    SYM, HIDE, " ", OK, ""
};
const char *const MAP_SYM[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "@", "#", "$", "_", "&", "-", "+", "(", ")", "/", "\n",
    SYM2, "*", "\"", "'", ":", ";", "!", "?", BKSP, "\n",
    ABC, HIDE, " ", OK, ""
};
const char *const MAP_SYM2[] = {
    "~", "`", "|", "^", "{", "}", "[", "]", "<", ">", "\n",
    GAP, "%", "\\", "=", ",", ".", "\xC2\xA3", "\xC2\xA5", "\xC2\xB0", "\xC2\xB1", GAP, "\n",
    SYM, "\xC2\xA9", "\xC2\xAE", "\xC2\xAC", "\xC3\x97", "\xC3\xB7", "\xC2\xBF", "\xC2\xA1", BKSP, "\n",
    ABC, HIDE, " ", OK, ""
};

#define LETTER_ROWS(shift)                                                                              \
    LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER,                     \
    SPACER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, SPACER,             \
    shift, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, BKSP3,                               \
    FN4, FN3, SPACE, OK7

const lv_buttonmatrix_ctrl_t CTRL_LOWER[] = {LETTER_ROWS(FN3)};
const lv_buttonmatrix_ctrl_t CTRL_UPPER[] = {LETTER_ROWS(SHIFT_ON)};
const lv_buttonmatrix_ctrl_t CTRL_CAPS[] = {LETTER_ROWS(SHIFT_LOCK)};
const lv_buttonmatrix_ctrl_t CTRL_SYM[] = {
    LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER,
    LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER,
    FN3, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, BKSP3,
    FN4, FN3, SPACE, OK7
};
const lv_buttonmatrix_ctrl_t CTRL_SYM2[] = {
    LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER,
    SPACER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, SPACER,
    FN3, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, LETTER, BKSP3,
    FN4, FN3, SPACE, OK7
};

/* lv_keyboard modes used as pages */
constexpr lv_keyboard_mode_t PAGE_LOWER = LV_KEYBOARD_MODE_TEXT_LOWER;
constexpr lv_keyboard_mode_t PAGE_UPPER = LV_KEYBOARD_MODE_TEXT_UPPER;   // one letter, then back to lower
constexpr lv_keyboard_mode_t PAGE_CAPS = LV_KEYBOARD_MODE_USER_1;
constexpr lv_keyboard_mode_t PAGE_SYM = LV_KEYBOARD_MODE_SPECIAL;
constexpr lv_keyboard_mode_t PAGE_SYM2 = LV_KEYBOARD_MODE_USER_2;

int64_t s_last_shift_us;

bool is(const char *a, const char *b)
{
    return lv_strcmp(a, b) == 0;
}

void send(lv_obj_t *kb, lv_event_code_t code)
{
    if (lv_obj_send_event(kb, code, nullptr) != LV_RESULT_OK) {
        return;
    }
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    if (ta) {
        lv_obj_send_event(ta, code, nullptr);
    }
}

void on_key(lv_event_t *e)
{
    lv_obj_t *kb = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    const uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }
    const char *txt = lv_buttonmatrix_get_button_text(kb, id);
    if (!txt) {
        return;
    }
    const lv_keyboard_mode_t mode = lv_keyboard_get_mode(kb);
    if (is(txt, SHIFT)) {
        const int64_t now = esp_timer_get_time();
        if (mode == PAGE_LOWER) {
            lv_keyboard_set_mode(kb, PAGE_UPPER);
        } else if (mode == PAGE_UPPER && now - s_last_shift_us < DOUBLE_TAP_US) {
            lv_keyboard_set_mode(kb, PAGE_CAPS);
        } else {
            lv_keyboard_set_mode(kb, PAGE_LOWER);
        }
        s_last_shift_us = now;
        return;
    }
    if (is(txt, SYM)) {
        lv_keyboard_set_mode(kb, PAGE_SYM);
        return;
    }
    if (is(txt, SYM2)) {
        lv_keyboard_set_mode(kb, PAGE_SYM2);
        return;
    }
    if (is(txt, ABC)) {
        lv_keyboard_set_mode(kb, PAGE_LOWER);
        return;
    }
    if (is(txt, HIDE)) {
        send(kb, LV_EVENT_CANCEL);
        return;
    }
    if (is(txt, OK)) {
        send(kb, LV_EVENT_READY);
        return;
    }
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    if (!ta) {
        return;
    }
    if (is(txt, BKSP)) {
        lv_textarea_delete_char(ta);
        return;
    }
    lv_textarea_add_text(ta, txt);
    if (mode == PAGE_UPPER) {
        lv_keyboard_set_mode(kb, PAGE_LOWER);
    }
}

/* OK and the Shift states get their own colors (lv_buttonmatrix styles only tell plain from CHECKED keys); the Shift
 * key shows a drawn arrow instead of its text (the fonts have no shift arrow) */
void on_draw_task(lv_event_t *e)
{
    lv_obj_t *kb = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    const lv_draw_dsc_base_t *base = static_cast<const lv_draw_dsc_base_t *>(lv_draw_task_get_draw_dsc(t));
    if (!base || base->part != LV_PART_ITEMS) {
        return;
    }
    const uint32_t id = base->id1;
    const bool accent = lv_buttonmatrix_has_button_ctrl(kb, id, LV_BUTTONMATRIX_CTRL_CUSTOM_1);
    const bool light = lv_buttonmatrix_has_button_ctrl(kb, id, LV_BUTTONMATRIX_CTRL_CUSTOM_2);
    const char *txt = lv_buttonmatrix_get_button_text(kb, id);
    const bool shift = txt && is(txt, SHIFT);
    const bool pressed = lv_buttonmatrix_get_selected_button(kb) == id && lv_obj_has_state(kb, LV_STATE_PRESSED);
    const lv_draw_task_type_t type = lv_draw_task_get_type(t);
    if (type == LV_DRAW_TASK_TYPE_FILL) {
        if (accent || light) {
            lv_draw_task_get_fill_dsc(t)->color = lv_color_hex(accent ? (pressed ? ACCENT_PRESSED : ACCENT) : SHIFT_ON_BG);
        }
        if (shift) {
            const lv_image_dsc_t *img = accent ? &phone_kb_shift_lock : light ? &phone_kb_shift_on : &phone_kb_shift_off;
            lv_area_t key;
            lv_draw_task_get_area(t, &key);
            lv_area_t a;
            a.x1 = key.x1 + (lv_area_get_width(&key) - img->header.w) / 2;
            a.y1 = key.y1 + (lv_area_get_height(&key) - img->header.h) / 2;
            a.x2 = a.x1 + img->header.w - 1;
            a.y2 = a.y1 + img->header.h - 1;
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            d.src = img;
            d.recolor = lv_color_hex(accent ? KEY_TEXT : light ? SHIFT_ON_TEXT : FN_TEXT);
            d.recolor_opa = LV_OPA_COVER;
            lv_draw_image(base->layer, &d, &a);
        }
    } else if (type == LV_DRAW_TASK_TYPE_LABEL) {
        lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(t);
        if (shift) {
            label->opa = LV_OPA_TRANSP;
        } else if (accent || light) {
            label->color = lv_color_hex(accent ? KEY_TEXT : SHIFT_ON_TEXT);
        }
    }
}

} // namespace

extern "C" lv_obj_t *phone_keyboard_create(lv_obj_t *parent, lv_obj_t *textarea)
{
    lv_obj_t *kb = lv_keyboard_create(parent);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, on_key, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(kb, on_draw_task, LV_EVENT_DRAW_TASK_ADDED, nullptr);

    lv_keyboard_set_map(kb, PAGE_LOWER, MAP_LOWER, CTRL_LOWER);
    lv_keyboard_set_map(kb, PAGE_UPPER, MAP_UPPER, CTRL_UPPER);
    lv_keyboard_set_map(kb, PAGE_CAPS, MAP_UPPER, CTRL_CAPS);
    lv_keyboard_set_map(kb, PAGE_SYM, MAP_SYM, CTRL_SYM);
    lv_keyboard_set_map(kb, PAGE_SYM2, MAP_SYM2, CTRL_SYM2);
    lv_keyboard_set_mode(kb, PAGE_LOWER);
    lv_keyboard_set_popovers(kb, true);
    lv_keyboard_set_textarea(kb, textarea);

    lv_obj_set_style_bg_color(kb, lv_color_hex(KB_BG), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 8, 0);
    lv_obj_set_style_pad_row(kb, 12, 0);
    lv_obj_set_style_pad_column(kb, 6, 0);

    const lv_style_selector_t items = LV_PART_ITEMS;
    lv_obj_set_style_bg_color(kb, lv_color_hex(KEY_BG), items);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, items);
    lv_obj_set_style_text_color(kb, lv_color_hex(KEY_TEXT), items);
    lv_obj_set_style_text_font(kb, &phone_font_24, items);
    lv_obj_set_style_radius(kb, 10, items);
    lv_obj_set_style_border_width(kb, 0, items);
    lv_obj_set_style_shadow_width(kb, 0, items);
    lv_obj_set_style_outline_width(kb, 0, items | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(kb, lv_color_hex(KEY_BG_PRESSED), items | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(FN_BG), items | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(FN_TEXT), items | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(KEY_BG), items | LV_STATE_CHECKED | LV_STATE_PRESSED);
    return kb;
}

extern "C" void phone_keyboard_reset(lv_obj_t *kb)
{
    if (kb) {
        lv_keyboard_set_mode(kb, PAGE_LOWER);
    }
}
