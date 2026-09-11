  #include <stdbool.h>
  #include <stdint.h>
  #include <stdio.h>
  #include <stdlib.h>
  #include <string.h>

  #include "driver/gpio.h"
  #include "driver/spi_master.h"
  #include "driver/uart.h"
  #include "esp_err.h"
  #include "esp_heap_caps.h"
  #include "esp_log.h"
  #include "freertos/FreeRTOS.h"
  #include "freertos/stream_buffer.h"
  #include "freertos/task.h"

  #define TFT_HOST SPI2_HOST
  #define TFT_PIN_CS GPIO_NUM_15
  #define TFT_PIN_RST GPIO_NUM_4
  #define TFT_PIN_DC GPIO_NUM_2
  #define TFT_PIN_MOSI GPIO_NUM_23
  #define TFT_PIN_SCLK GPIO_NUM_18

  #define SERIAL_PORT UART_NUM_2
  #define SERIAL_PIN_RX GPIO_NUM_16
  #define SERIAL_PIN_TX GPIO_NUM_17
  #define SERIAL_BAUD_RATE 115200

  #define LCD_WIDTH 480
  #define LCD_HEIGHT 320
  #define FONT_WIDTH 6
  #define FONT_HEIGHT 8
  #define TERM_COLUMNS (LCD_WIDTH / FONT_WIDTH)
  #define TERM_ROWS (LCD_HEIGHT / FONT_HEIGHT)

  #define COLOR_BACKGROUND 0x0000
  #define COLOR_FOREGROUND 0xffff
  #define COLOR_RED 0xf800
  #define COLOR_GREEN 0x07e0
  #define COLOR_BLUE 0x001f

  static const char *TAG = "serial_tft";
  static spi_device_handle_t s_lcd;
  static StreamBufferHandle_t s_serial_stream;

  /* 5x7 ASCII font, stored as five vertical columns per character. */
  static const uint8_t s_font[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5f,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1c,0x22,0x41,0x00},{0x00,0x41,0x22,0x1c,0x00},{0x14,0x08,0x3e,0x08,0x14},{0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3e},{0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},{0x3e,0x41,0x49,0x49,0x7a},
    {0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},
    {0x7f,0x40,0x40,0x40,0x40},{0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7f,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},{0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78},{0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x44,0x3d,0x00},{0x7f,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7f,0x40,0x00},{0x7c,0x04,0x18,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7c,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20},{0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7f,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08},{0x00,0x00,0x00,0x00,0x00}
  };

  static void lcd_send(bool data_mode, const void *data, size_t length)
  {
    gpio_set_level(TFT_PIN_DC, data_mode);
    spi_transaction_t transaction = {
      .length = length * 8,
      .tx_buffer = data,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_lcd, &transaction));
  }

  static void lcd_command(uint8_t command)
  {
    lcd_send(false, &command, 1);
  }

  static void lcd_command_data(uint8_t command, const uint8_t *data, size_t length)
  {
    lcd_command(command);
    if (length > 0) {
      lcd_send(true, data, length);
    }
  }

  static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
  {
    uint8_t column[] = {x0 >> 8, x0 & 0xff, x1 >> 8, x1 & 0xff};
    uint8_t row[] = {y0 >> 8, y0 & 0xff, y1 >> 8, y1 & 0xff};
    lcd_command_data(0x2a, column, sizeof(column));
    lcd_command_data(0x2b, row, sizeof(row));
    lcd_command(0x2c);
  }

  static void lcd_initialize(void)
  {
    gpio_config_t output_config = {
      .pin_bit_mask = (1ULL << TFT_PIN_DC) | (1ULL << TFT_PIN_RST),
      .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));

    gpio_set_level(TFT_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TFT_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus_config = {
      .mosi_io_num = TFT_PIN_MOSI,
      .miso_io_num = -1,
      .sclk_io_num = TFT_PIN_SCLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = LCD_WIDTH * 3,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TFT_HOST, &bus_config, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t device_config = {
      .clock_speed_hz = 20 * 1000 * 1000,
      .mode = 0,
      .spics_io_num = TFT_PIN_CS,
      .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(TFT_HOST, &device_config, &s_lcd));

    lcd_command(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));

    const uint8_t positive_gamma[] = {0x00,0x03,0x09,0x08,0x16,0x0a,0x3f,0x78,0x4c,0x09,0x0a,0x08,0x16,0x1a,0x0f};
    const uint8_t negative_gamma[] = {0x00,0x16,0x19,0x03,0x0f,0x05,0x32,0x45,0x46,0x04,0x0e,0x0d,0x35,0x37,0x0f};
    const uint8_t power_control_1[] = {0x17, 0x15};
    const uint8_t power_control_2[] = {0x41};
    const uint8_t vcom_control[] = {0x00, 0x12, 0x80};
    const uint8_t memory_access[] = {0xe8};
    const uint8_t pixel_format[] = {0x66};
    const uint8_t interface_mode[] = {0x00};
    const uint8_t frame_rate[] = {0xa0};
    const uint8_t inversion_control[] = {0x02};
    const uint8_t display_function[] = {0x02, 0x02};
    const uint8_t image_function[] = {0x00};
    const uint8_t adjust_control[] = {0xa9, 0x51, 0x2c, 0x82};

    lcd_command_data(0xe0, positive_gamma, sizeof(positive_gamma));
    lcd_command_data(0xe1, negative_gamma, sizeof(negative_gamma));
    lcd_command_data(0xc0, power_control_1, sizeof(power_control_1));
    lcd_command_data(0xc1, power_control_2, sizeof(power_control_2));
    lcd_command_data(0xc5, vcom_control, sizeof(vcom_control));
    lcd_command_data(0x36, memory_access, sizeof(memory_access));
    lcd_command_data(0x3a, pixel_format, sizeof(pixel_format));
    lcd_command_data(0xb0, interface_mode, sizeof(interface_mode));
    lcd_command_data(0xb1, frame_rate, sizeof(frame_rate));
    lcd_command_data(0xb4, inversion_control, sizeof(inversion_control));
    lcd_command_data(0xb6, display_function, sizeof(display_function));
    lcd_command_data(0xe9, image_function, sizeof(image_function));
    lcd_command_data(0xf7, adjust_control, sizeof(adjust_control));
    lcd_command(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_command(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  static void rgb565_to_rgb666(uint16_t color, uint8_t *destination)
  {
    destination[0] = (color >> 8) & 0xf8;
    destination[1] = (color >> 3) & 0xfc;
    destination[2] = (color << 3) & 0xf8;
  }

  static void lcd_fill_screen(uint16_t color)
  {
    uint8_t *scanline = heap_caps_malloc(LCD_WIDTH * 3, MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(scanline == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    uint8_t pixel[3];
    rgb565_to_rgb666(color, pixel);
    for (int x = 0; x < LCD_WIDTH; ++x) {
      memcpy(&scanline[x * 3], pixel, sizeof(pixel));
    }

    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    for (int y = 0; y < LCD_HEIGHT; ++y) {
      lcd_send(true, scanline, LCD_WIDTH * 3);
    }
    free(scanline);
  }

  static void lcd_self_test(void)
  {
    ESP_LOGI(TAG, "TFT self-test: red");
    lcd_fill_screen(COLOR_RED);
    vTaskDelay(pdMS_TO_TICKS(350));
    ESP_LOGI(TAG, "TFT self-test: green");
    lcd_fill_screen(COLOR_GREEN);
    vTaskDelay(pdMS_TO_TICKS(350));
    ESP_LOGI(TAG, "TFT self-test: blue");
    lcd_fill_screen(COLOR_BLUE);
    vTaskDelay(pdMS_TO_TICKS(350));
    lcd_fill_screen(COLOR_BACKGROUND);
  }

  static void render_terminal(const char screen[TERM_ROWS][TERM_COLUMNS])
  {
    uint8_t *scanline = heap_caps_malloc(LCD_WIDTH * 3, MALLOC_CAP_DMA);
    if (scanline == NULL) {
      ESP_LOGE(TAG, "Unable to allocate display scanline");
      return;
    }

    uint8_t foreground[3];
    uint8_t background[3];
    rgb565_to_rgb666(COLOR_FOREGROUND, foreground);
    rgb565_to_rgb666(COLOR_BACKGROUND, background);

    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    for (int y = 0; y < LCD_HEIGHT; ++y) {
      int text_row = y / FONT_HEIGHT;
      int glyph_row = y % FONT_HEIGHT;
      for (int x = 0; x < LCD_WIDTH; ++x) {
        int text_column = x / FONT_WIDTH;
        int glyph_column = x % FONT_WIDTH;
        unsigned char character = screen[text_row][text_column];
        if (character < 32 || character > 127) {
          character = '?';
        }
        bool pixel_set = glyph_column < 5 && glyph_row < 7 &&
                 (s_font[character - 32][glyph_column] & (1U << glyph_row));
        memcpy(&scanline[x * 3], pixel_set ? foreground : background, 3);
      }
      lcd_send(true, scanline, LCD_WIDTH * 3);
    }
    free(scanline);
  }

  static void terminal_newline(char screen[TERM_ROWS][TERM_COLUMNS], int *row, int *column)
  {
    *column = 0;
    if (*row < TERM_ROWS - 1) {
      ++*row;
      return;
    }
    memmove(screen[0], screen[1], (TERM_ROWS - 1) * TERM_COLUMNS);
    memset(screen[TERM_ROWS - 1], ' ', TERM_COLUMNS);
  }

  static void terminal_put(char screen[TERM_ROWS][TERM_COLUMNS], int *row, int *column,
               uint8_t *escape_state, uint8_t character)
  {
    if (*escape_state == 1) {
      *escape_state = character == '[' ? 2 : 0;
      return;
    }
    if (*escape_state == 2) {
      if (character >= 0x40 && character <= 0x7e) {
        *escape_state = 0;
      }
      return;
    }
    if (character == 0x1b) {
      *escape_state = 1;
    } else if (character == '\r') {
      *column = 0;
    } else if (character == '\n') {
      terminal_newline(screen, row, column);
    } else if (character == '\b' || character == 0x7f) {
      if (*column > 0) {
        --*column;
        screen[*row][*column] = ' ';
      }
    } else if (character == '\t') {
      int spaces = 4 - (*column % 4);
      while (spaces-- > 0) {
        terminal_put(screen, row, column, escape_state, ' ');
      }
    } else if (character >= 32 && character < 127) {
      screen[*row][*column] = (char)character;
      if (++*column == TERM_COLUMNS) {
        terminal_newline(screen, row, column);
      }
    } else if (character >= 0xc0) {
      terminal_put(screen, row, column, escape_state, '?');
    }
  }

  static void display_task(void *argument)
  {
    (void)argument;
    static char screen[TERM_ROWS][TERM_COLUMNS];
    uint8_t input[256];
    int row = 0;
    int column = 0;
    uint8_t escape_state = 0;
    memset(screen, ' ', sizeof(screen));

    const char banner[] = "Serial console ready - UART2 115200 8N1";
    for (size_t i = 0; i < sizeof(banner) - 1; ++i) {
      terminal_put(screen, &row, &column, &escape_state, banner[i]);
    }
    terminal_put(screen, &row, &column, &escape_state, '\n');
    render_terminal(screen);

    while (true) {
      size_t received = xStreamBufferReceive(s_serial_stream, input, sizeof(input), portMAX_DELAY);
      do {
        for (size_t i = 0; i < received; ++i) {
          terminal_put(screen, &row, &column, &escape_state, input[i]);
        }
        received = xStreamBufferReceive(s_serial_stream, input, sizeof(input), 0);
      } while (received > 0);
      render_terminal(screen);
      vTaskDelay(pdMS_TO_TICKS(40));
    }
  }

  static void uart_receive_task(void *argument)
  {
    (void)argument;
    uint8_t input[256];
    while (true) {
      int length = uart_read_bytes(SERIAL_PORT, input, sizeof(input), pdMS_TO_TICKS(100));
      if (length > 0) {
        /* Visible on the flashing/console port (ttyACM0), never on ttyUSB0. */
        ESP_LOGI(TAG, "UART2 RX %d bytes: %.*s", length, length, (const char *)input);
        size_t sent = xStreamBufferSend(s_serial_stream, input, (size_t)length, portMAX_DELAY);
        if (sent != (size_t)length) {
          ESP_LOGW(TAG, "Serial stream overflow");
        }
      }
    }
  }

  static void uart_self_test(void)
  {
    const char pattern[] = "UART2 loopback OK";
    uint8_t response[sizeof(pattern)] = {0};

    /* Internal loopback bypasses GPIO16/17 wiring entirely, isolating the
       driver/config from the external USB-TTL adapter and its baud rate. */
    uart_set_loop_back(SERIAL_PORT, true);
    uart_write_bytes(SERIAL_PORT, pattern, sizeof(pattern) - 1);
    int received = uart_read_bytes(SERIAL_PORT, response, sizeof(pattern) - 1, pdMS_TO_TICKS(200));
    uart_set_loop_back(SERIAL_PORT, false);

    if (received == (int)(sizeof(pattern) - 1) && memcmp(response, pattern, received) == 0) {
      ESP_LOGI(TAG, "UART2 loopback OK: driver/config is correct, check external wiring/baud next");
    } else {
      ESP_LOGW(TAG, "UART2 loopback FAILED (%d bytes received): driver/config problem", received);
    }
  }

  void app_main(void)
  {
    const uart_config_t uart_config = {
      .baud_rate = SERIAL_BAUD_RATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(SERIAL_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(SERIAL_PORT, SERIAL_PIN_TX, SERIAL_PIN_RX,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(SERIAL_PORT, 2048, 0, 0, NULL, 0));
    ESP_LOGI(TAG, "UART2 RX=GPIO%d TX=GPIO%d at %d baud",
      SERIAL_PIN_RX, SERIAL_PIN_TX, SERIAL_BAUD_RATE);
    uart_self_test();

    lcd_initialize();
        ESP_LOGI(TAG, "ILI9488 initialized at 20 MHz");
        lcd_self_test();
    s_serial_stream = xStreamBufferCreate(4096, 1);
    if (s_serial_stream == NULL) {
      ESP_LOGE(TAG, "Unable to allocate serial stream buffer");
      return;
    }

    BaseType_t uart_task_created =
      xTaskCreate(uart_receive_task, "uart_receive", 3072, NULL, 10, NULL);
    BaseType_t display_task_created =
      xTaskCreate(display_task, "tft_terminal", 4096, NULL, 8, NULL);
    ESP_ERROR_CHECK(uart_task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(display_task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "UART and TFT terminal tasks started");
  }
