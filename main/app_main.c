#include "audio_io.h"
#include "board.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ptt_app.h"

static const char *TAG = "main";
static board_t s_board;

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(board_init(&s_board));
    ESP_LOGI(TAG, "board %s %dx%d%s", s_board.name, s_board.width, s_board.height,
             s_board.touch ? " touch" : "");
    ESP_ERROR_CHECK(audio_io_init(&s_board));
    ESP_ERROR_CHECK(ptt_app_start(&s_board));
}
