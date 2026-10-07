#include "buttons.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POLL_MS 10
#define DEBOUNCE_POLLS 3
#define LONG_PRESS_MS 800
#define LONG_PRESS_PTT_MS 350 /* BOOT: tap = call, hold = talk; keep the hold short */
#define VERY_LONG_MS 8000

typedef struct {
    board_button_t btn;
    bool pressed;
    int stable; /* polls the raw level has disagreed with `pressed` */
    TickType_t since;
    bool long_sent;
    bool very_long_sent;
} button_state_t;

static button_state_t s_state[BOARD_MAX_BUTTONS];
static int s_count;
static button_cb_t s_cb;

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < s_count; i++) {
            button_state_t *b = &s_state[i];
            bool raw = gpio_get_level(b->btn.gpio) == 0;
            if (raw != b->pressed) {
                if (++b->stable >= DEBOUNCE_POLLS) {
                    b->stable = 0;
                    b->pressed = raw;
                    if (raw) {
                        b->since = now;
                        b->long_sent = false;
                        b->very_long_sent = false;
                        s_cb(b->btn.role, BUTTON_PRESS);
                    } else {
                        s_cb(b->btn.role, BUTTON_RELEASE);
                        if (!b->long_sent) {
                            s_cb(b->btn.role, BUTTON_CLICK);
                        }
                    }
                }
            } else {
                b->stable = 0;
                int long_ms = b->btn.role == BOARD_BTN_PTT ? LONG_PRESS_PTT_MS : LONG_PRESS_MS;
                if (b->pressed && !b->long_sent && now - b->since >= pdMS_TO_TICKS(long_ms)) {
                    b->long_sent = true;
                    s_cb(b->btn.role, BUTTON_LONG_PRESS);
                }
                if (b->pressed && !b->very_long_sent && now - b->since >= pdMS_TO_TICKS(VERY_LONG_MS)) {
                    b->very_long_sent = true;
                    s_cb(b->btn.role, BUTTON_VERY_LONG);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_start(const board_t *b, button_cb_t cb)
{
    s_cb = cb;
    s_count = b->button_count;
    for (int i = 0; i < s_count; i++) {
        s_state[i] = (button_state_t){.btn = b->buttons[i]};
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << b->buttons[i].gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&io);
    }
    if (s_count) {
        xTaskCreatePinnedToCore(task, "buttons", 3072, NULL, 6, NULL, 0);
    }
}
