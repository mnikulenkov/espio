idf_component_register(
    SRCS
        "src/espio_version.c"
        "src/common/err.c"
        "src/audio/audio_duplex.c"
        "src/audio/audio_i2s_hal.c"
        "src/audio/amp.c"
        "src/audio/amp_tx.c"
        "src/audio/audio_ctrl_gpio.c"
        "src/audio/drivers/amp_max98357a.c"
        "src/audio/mic.c"
        "src/audio/mic_rx.c"
        "src/audio/drivers/mic_adc_generic.c"
        "src/audio/drivers/mic_inmp441.c"
        "src/audio/drivers/mic_pdm_generic.c"
        "src/audio/drivers/mic_tdm_generic.c"
        "src/button/button.c"
        "src/button/drivers/button_gpio.c"
        "src/button/drivers/button_mcp23017.c"
        "src/display/display.c"
        "src/display/drivers/ili9341.c"
        "src/sd/sd.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "internal"
    REQUIRES driver esp_timer esp_lcd esp_adc esp_driver_i2c fatfs espressif__esp_lcd_ili9341
    PRIV_REQUIRES heap
)
