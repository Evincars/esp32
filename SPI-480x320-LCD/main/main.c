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
  #include "freertos/queue.h"
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

  /* Static border/header frame; the scrolling log only occupies the inner text area. */
  #define BORDER_THICKNESS 4
  #define HEADER_HEIGHT 20
  #define TEXT_AREA_X0 BORDER_THICKNESS
  #define TEXT_AREA_Y0 (BORDER_THICKNESS + HEADER_HEIGHT)
  #define TEXT_AREA_MAX_WIDTH (LCD_WIDTH - 2 * BORDER_THICKNESS)
  #define TEXT_AREA_MAX_HEIGHT (LCD_HEIGHT - 2 * BORDER_THICKNESS - HEADER_HEIGHT)
  #define TERM_COLUMNS (TEXT_AREA_MAX_WIDTH / FONT_WIDTH)
  #define TERM_ROWS (TEXT_AREA_MAX_HEIGHT / FONT_HEIGHT)
  #define TEXT_AREA_WIDTH (TERM_COLUMNS * FONT_WIDTH)
  #define TEXT_AREA_HEIGHT (TERM_ROWS * FONT_HEIGHT)
  #define TEXT_AREA_X1 (TEXT_AREA_X0 + TEXT_AREA_WIDTH - 1)
  #define TEXT_AREA_Y1 (TEXT_AREA_Y0 + TEXT_AREA_HEIGHT - 1)

  #define COLOR_BACKGROUND 0x0000
  #define COLOR_FOREGROUND 0x07e0
  #define COLOR_ACCENT 0x0320
  #define COLOR_RED 0xf800
  #define COLOR_GREEN 0x07e0
  #define COLOR_BLUE 0x001f
  #define COLOR_GRAY 0x39c7

  #define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

  /* Distinct green-family shades, one per graph metric (no red/amber severity colors). */
  #define COLOR_CPU_CORE RGB565(60, 220, 90)
  #define COLOR_CPU_TEMP RGB565(40, 180, 140)
  #define COLOR_RAM RGB565(120, 200, 60)
  #define COLOR_VRAM RGB565(30, 170, 120)
  #define COLOR_GPU_USAGE RGB565(90, 230, 150)
  #define COLOR_GPU_TEMP RGB565(20, 150, 100)
  #define COLOR_NET_UP RGB565(150, 220, 80)
  #define COLOR_NET_DOWN RGB565(50, 190, 170)

  /* Layout for the "graphs" screen; drawn inside the same border/header chrome. */
  #define GRAPH_MARGIN 8
  #define GRAPH_X0 (BORDER_THICKNESS + GRAPH_MARGIN)
  #define GRAPH_X1 (LCD_WIDTH - BORDER_THICKNESS - GRAPH_MARGIN - 1)
  #define GRAPH_WIDTH (GRAPH_X1 - GRAPH_X0 + 1)
  #define GRAPH_Y0 (BORDER_THICKNESS + HEADER_HEIGHT + GRAPH_MARGIN)

  /* Left column: one row per CPU core (index, usage%, optional thread count, mini bar). */
  #define CORE_LIST_WIDTH 108
  #define CORE_ROW_HEIGHT 17
  #define CORE_BAR_HEIGHT 4
  #define RIGHT_PANEL_X0 (GRAPH_X0 + CORE_LIST_WIDTH + 10)

  /* Structured stats line protocol (see system_monitor.py), e.g.:
     #SYS#cpus=12.3;45.0|cputemp=61.5|ramused=8192|ramtotal=16384|
          vramused=2048|vramtotal=8192|gpuusage=33|gputemp=55|
          netup=120.5|netdown=980.2|procs=312|threads=1904|corethreads=3;2;4;1#END#
     An empty value (key=) means that metric is unavailable on the sender.
     Anything not matching this exact wrapper (e.g. plain log text, "ping")
     falls back to the scrolling terminal view unchanged. */
  #define STATS_LINE_PREFIX "#SYS#"
  #define STATS_LINE_SUFFIX "#END#"
  #define MAX_CPU_CORES 16

  static const char *TAG = "serial_tft";
  static spi_device_handle_t s_lcd;
  static StreamBufferHandle_t s_serial_stream;
  static QueueHandle_t s_stats_queue;

  typedef struct {
    int cpu_core_count;
    float cpu_usage[MAX_CPU_CORES];
    int core_thread_count[MAX_CPU_CORES];
    int core_thread_data_count;
    bool has_cpu_temp;
    float cpu_temp;
    bool has_ram_used;
    float ram_used_mb;
    bool has_ram_total;
    float ram_total_mb;
    bool has_vram_used;
    float vram_used_mb;
    bool has_vram_total;
    float vram_total_mb;
    bool has_gpu_usage;
    float gpu_usage;
    bool has_gpu_temp;
    float gpu_temp;
    bool has_net_up;
    float net_up_kbps;
    bool has_net_down;
    float net_down_kbps;
    bool has_procs;
    int procs;
    bool has_threads;
    int threads_total;
  } system_stats_t;


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

  static void lcd_fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
  {
    int width = x1 - x0 + 1;
    uint8_t *scanline = heap_caps_malloc(width * 3, MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(scanline == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    uint8_t pixel[3];
    rgb565_to_rgb666(color, pixel);
    for (int x = 0; x < width; ++x) {
      memcpy(&scanline[x * 3], pixel, sizeof(pixel));
    }

    lcd_set_window(x0, y0, x1, y1);
    for (int y = y0; y <= y1; ++y) {
      lcd_send(true, scanline, width * 3);
    }
    free(scanline);
  }

  static void lcd_fill_screen(uint16_t color)
  {
    lcd_fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, color);
  }

  /* Scales a base color's brightness by percent (0-100) so bars glow brighter under load,
     without ever changing hue away from the metric's assigned green shade. */
  static uint16_t metric_color(uint16_t base_color, float percent)
  {
    if (percent < 0.0f) {
      percent = 0.0f;
    }
    if (percent > 100.0f) {
      percent = 100.0f;
    }
    float factor = 0.35f + 0.65f * (percent / 100.0f);
    uint8_t r = (uint8_t)(((base_color >> 11) & 0x1f) * factor);
    uint8_t g = (uint8_t)(((base_color >> 5) & 0x3f) * factor);
    uint8_t b = (uint8_t)((base_color & 0x1f) * factor);
    return (uint16_t)((r << 11) | (g << 5) | b);
  }

  /* Maps a Celsius reading onto a 0-100 bar scale for temperature gauges. */
  static float temp_to_bar_percent(float celsius)
  {
    const float min_c = 20.0f;
    const float max_c = 95.0f;
    float percent = (celsius - min_c) / (max_c - min_c) * 100.0f;
    if (percent < 0.0f) {
      percent = 0.0f;
    }
    if (percent > 100.0f) {
      percent = 100.0f;
    }
    return percent;
  }

  /* Horizontal meter: background track plus a proportional filled portion. */
  static void lcd_draw_bar(int x, int y, int width, int height, float percent, uint16_t fill_color)
  {
    if (percent < 0.0f) {
      percent = 0.0f;
    }
    if (percent > 100.0f) {
      percent = 100.0f;
    }
    lcd_fill_rect(x, y, x + width - 1, y + height - 1, COLOR_GRAY);
    int fill_width = (int)((width - 2) * percent / 100.0f);
    if (fill_width > 0) {
      lcd_fill_rect(x + 1, y + 1, x + fill_width, y + height - 2, fill_color);
    }
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

  static void lcd_draw_char(int x, int y, char ch, uint16_t fg_color, uint16_t bg_color)
  {
    uint8_t fg[3];
    uint8_t bg[3];
    rgb565_to_rgb666(fg_color, fg);
    rgb565_to_rgb666(bg_color, bg);

    unsigned char glyph_index = (unsigned char)ch;
    if (glyph_index < 32 || glyph_index > 127) {
      glyph_index = '?';
    }

    uint8_t glyph[FONT_WIDTH * FONT_HEIGHT * 3];
    for (int row = 0; row < FONT_HEIGHT; ++row) {
      for (int col = 0; col < FONT_WIDTH; ++col) {
        bool pixel_set = col < 5 && row < 7 && (s_font[glyph_index - 32][col] & (1U << row));
        memcpy(&glyph[(row * FONT_WIDTH + col) * 3], pixel_set ? fg : bg, 3);
      }
    }

    lcd_set_window(x, y, x + FONT_WIDTH - 1, y + FONT_HEIGHT - 1);
    lcd_send(true, glyph, sizeof(glyph));
  }

  static void lcd_draw_text(int x, int y, const char *text, uint16_t fg_color, uint16_t bg_color)
  {
    for (int i = 0; text[i] != '\0'; ++i) {
      lcd_draw_char(x + i * FONT_WIDTH, y, text[i], fg_color, bg_color);
    }
  }

  /* Draws the static border/header once; scrolling log updates only repaint the text area. */
  static void lcd_draw_chrome_titled(const char *title)
  {
    lcd_fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, COLOR_ACCENT);
    lcd_fill_rect(BORDER_THICKNESS, BORDER_THICKNESS + HEADER_HEIGHT,
                  LCD_WIDTH - BORDER_THICKNESS - 1, LCD_HEIGHT - BORDER_THICKNESS - 1,
                  COLOR_BACKGROUND);

    int title_width = (int)strlen(title) * FONT_WIDTH;
    int title_x = BORDER_THICKNESS + ((LCD_WIDTH - 2 * BORDER_THICKNESS) - title_width) / 2;
    int title_y = BORDER_THICKNESS + (HEADER_HEIGHT - FONT_HEIGHT) / 2;
    lcd_draw_text(title_x, title_y, title, COLOR_BACKGROUND, COLOR_ACCENT);
  }

  static void lcd_draw_chrome(void)
  {
    lcd_draw_chrome_titled("LINUX SERIAL MONITOR");
  }

  /* Left column: one compact row per CPU core - "C<index> <usage%> <threads>t" plus a mini bar.
     Showing the core index alongside the percentage avoids the ambiguous bare numbers. */
  static void draw_cpu_core_rows(const system_stats_t *stats)
  {
    int count = stats->cpu_core_count;
    if (count > MAX_CPU_CORES) {
      count = MAX_CPU_CORES;
    }

    lcd_fill_rect(GRAPH_X0, GRAPH_Y0, GRAPH_X0 + CORE_LIST_WIDTH - 1,
                  GRAPH_Y0 + MAX_CPU_CORES * CORE_ROW_HEIGHT - 1, COLOR_BACKGROUND);

    for (int i = 0; i < count; ++i) {
      int row_y = GRAPH_Y0 + i * CORE_ROW_HEIGHT;
      float usage = stats->cpu_usage[i];

      char line[24];
      if (i < stats->core_thread_data_count) {
        snprintf(line, sizeof(line), "C%-2d%3.0f%%%3dt", i, usage, stats->core_thread_count[i]);
      } else {
        snprintf(line, sizeof(line), "C%-2d%3.0f%%", i, usage);
      }
      lcd_draw_text(GRAPH_X0, row_y, line, COLOR_FOREGROUND, COLOR_BACKGROUND);

      int bar_y = row_y + FONT_HEIGHT + 1;
      lcd_draw_bar(GRAPH_X0, bar_y, CORE_LIST_WIDTH - 4, CORE_BAR_HEIGHT, usage,
                   metric_color(COLOR_CPU_CORE, usage));
    }
  }

  /* One "label: value" text line followed by a proportional bar underneath, within [x0, x1]. */
  static void draw_metric_row(int x0, int x1, int y, const char *label, const char *value_text,
                               float percent, bool valid, uint16_t color)
  {
    char line[64];
    if (valid) {
      snprintf(line, sizeof(line), "%s: %s", label, value_text);
    } else {
      snprintf(line, sizeof(line), "%s: N/A", label);
    }
    lcd_fill_rect(x0, y, x1, y + FONT_HEIGHT - 1, COLOR_BACKGROUND);
    lcd_draw_text(x0, y, line, COLOR_FOREGROUND, COLOR_BACKGROUND);

    int bar_y = y + FONT_HEIGHT + 2;
    int bar_height = 10;
    float bar_percent = valid ? percent : 0.0f;
    uint16_t fill_color = valid ? metric_color(color, bar_percent) : COLOR_GRAY;
    lcd_draw_bar(x0, bar_y, x1 - x0 + 1, bar_height, bar_percent, fill_color);
  }

  /* Plain text line with no bar, used for the processes/threads summary. */
  static void draw_text_row(int x0, int x1, int y, const char *text)
  {
    lcd_fill_rect(x0, y, x1, y + FONT_HEIGHT - 1, COLOR_BACKGROUND);
    lcd_draw_text(x0, y, text, COLOR_FOREGROUND, COLOR_BACKGROUND);
  }

  /* Formats a KB/s rate, switching to MB/s once it gets large. */
  static void format_rate(float kbps, char *out, size_t out_size)
  {
    if (kbps >= 1024.0f) {
      snprintf(out, out_size, "%.2f MB/s", kbps / 1024.0f);
    } else {
      snprintf(out, out_size, "%.0f KB/s", kbps);
    }
  }

  /* Renders the full "graphs" screen for a system_stats_t sample: CPU core list on the
     left, every other metric stacked on the right. */
  static void render_stats_graphs(const system_stats_t *stats)
  {
    draw_cpu_core_rows(stats);

    const int x0 = RIGHT_PANEL_X0;
    const int x1 = GRAPH_X1;
    int y = GRAPH_Y0;
    const int row_height = 26;
    char value_text[48];

    snprintf(value_text, sizeof(value_text), "%.1f C", stats->cpu_temp);
    draw_metric_row(x0, x1, y, "CPU TEMP", value_text, temp_to_bar_percent(stats->cpu_temp),
                     stats->has_cpu_temp, COLOR_CPU_TEMP);
    y += row_height;

    bool ram_valid = stats->has_ram_used && stats->has_ram_total && stats->ram_total_mb > 0.0f;
    float ram_percent = ram_valid ? (stats->ram_used_mb / stats->ram_total_mb * 100.0f) : 0.0f;
    snprintf(value_text, sizeof(value_text), "%.0f/%.0f MB (%.0f%%)",
             stats->ram_used_mb, stats->ram_total_mb, ram_percent);
    draw_metric_row(x0, x1, y, "RAM", value_text, ram_percent, ram_valid, COLOR_RAM);
    y += row_height;

    bool vram_valid = stats->has_vram_used && stats->has_vram_total && stats->vram_total_mb > 0.0f;
    float vram_percent = vram_valid ? (stats->vram_used_mb / stats->vram_total_mb * 100.0f) : 0.0f;
    snprintf(value_text, sizeof(value_text), "%.0f/%.0f MB (%.0f%%)",
             stats->vram_used_mb, stats->vram_total_mb, vram_percent);
    draw_metric_row(x0, x1, y, "VRAM", value_text, vram_percent, vram_valid, COLOR_VRAM);
    y += row_height;

    snprintf(value_text, sizeof(value_text), "%.0f%%", stats->gpu_usage);
    draw_metric_row(x0, x1, y, "GPU USAGE", value_text, stats->gpu_usage, stats->has_gpu_usage,
                     COLOR_GPU_USAGE);
    y += row_height;

    snprintf(value_text, sizeof(value_text), "%.1f C", stats->gpu_temp);
    draw_metric_row(x0, x1, y, "GPU TEMP", value_text, temp_to_bar_percent(stats->gpu_temp),
                     stats->has_gpu_temp, COLOR_GPU_TEMP);
    y += row_height;

    /* Network rates have no natural 0-100 scale; bar-fill is relative to this reference speed. */
    const float net_bar_reference_kbps = 12500.0f; /* ~100 Mbit/s */

    char rate_text[24];
    format_rate(stats->net_up_kbps, rate_text, sizeof(rate_text));
    float net_up_percent = stats->net_up_kbps / net_bar_reference_kbps * 100.0f;
    draw_metric_row(x0, x1, y, "NET UP", rate_text, net_up_percent, stats->has_net_up, COLOR_NET_UP);
    y += row_height;

    format_rate(stats->net_down_kbps, rate_text, sizeof(rate_text));
    float net_down_percent = stats->net_down_kbps / net_bar_reference_kbps * 100.0f;
    draw_metric_row(x0, x1, y, "NET DOWN", rate_text, net_down_percent, stats->has_net_down, COLOR_NET_DOWN);
    y += row_height;

    char summary[48];
    if (stats->has_procs && stats->has_threads) {
      snprintf(summary, sizeof(summary), "Processes: %d  Threads: %d", stats->procs, stats->threads_total);
    } else if (stats->has_procs) {
      snprintf(summary, sizeof(summary), "Processes: %d", stats->procs);
    } else {
      snprintf(summary, sizeof(summary), "Processes: N/A");
    }
    draw_text_row(x0, x1, y, summary);
  }


  static void render_terminal(const char screen[TERM_ROWS][TERM_COLUMNS])
  {
    uint8_t *scanline = heap_caps_malloc(TEXT_AREA_WIDTH * 3, MALLOC_CAP_DMA);
    if (scanline == NULL) {
      ESP_LOGE(TAG, "Unable to allocate display scanline");
      return;
    }

    uint8_t foreground[3];
    uint8_t background[3];
    rgb565_to_rgb666(COLOR_FOREGROUND, foreground);
    rgb565_to_rgb666(COLOR_BACKGROUND, background);

    lcd_set_window(TEXT_AREA_X0, TEXT_AREA_Y0, TEXT_AREA_X1, TEXT_AREA_Y1);
    for (int y = 0; y < TEXT_AREA_HEIGHT; ++y) {
      int text_row = y / FONT_HEIGHT;
      int glyph_row = y % FONT_HEIGHT;
      for (int x = 0; x < TEXT_AREA_WIDTH; ++x) {
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
      lcd_send(true, scanline, TEXT_AREA_WIDTH * 3);
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
    bool graphs_mode = false;
    memset(screen, ' ', sizeof(screen));

    const char banner[] = "Serial console ready - UART2 115200 8N1";
    for (size_t i = 0; i < sizeof(banner) - 1; ++i) {
      terminal_put(screen, &row, &column, &escape_state, banner[i]);
    }
    terminal_put(screen, &row, &column, &escape_state, '\n');
    render_terminal(screen);

    while (true) {
      system_stats_t stats;
      if (xQueueReceive(s_stats_queue, &stats, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (!graphs_mode) {
          graphs_mode = true;
          lcd_draw_chrome_titled("SYSTEM MONITOR");
        }
        render_stats_graphs(&stats);
        continue;
      }

      size_t received = xStreamBufferReceive(s_serial_stream, input, sizeof(input), 0);
      if (received == 0) {
        continue;
      }
      if (graphs_mode) {
        /* A regular text line arrived (e.g. "ping"): fall back to the scrolling terminal view. */
        graphs_mode = false;
        lcd_draw_chrome();
        memset(screen, ' ', sizeof(screen));
        row = 0;
        column = 0;
        escape_state = 0;
      }
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

  /* Dumps raw bytes as hex so garbled/non-printable data is still visible. */
  static void log_rx_hex(const uint8_t *data, int length)
  {
    char hex[16 * 3 + 1];
    for (int offset = 0; offset < length; offset += 16) {
      int chunk = length - offset < 16 ? length - offset : 16;
      char *cursor = hex;
      for (int i = 0; i < chunk; ++i) {
        cursor += sprintf(cursor, "%02x ", data[offset + i]);
      }
      ESP_LOGI(TAG, "RX[%3d]: %s", offset, hex);
    }
  }

  static bool parse_float_token(const char *value, float *out)
  {
    if (value == NULL || value[0] == '\0') {
      return false;
    }
    char *end = NULL;
    float parsed = strtof(value, &end);
    if (end == value) {
      return false;
    }
    *out = parsed;
    return true;
  }

  static bool parse_int_token(const char *value, int *out)
  {
    if (value == NULL || value[0] == '\0') {
      return false;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) {
      return false;
    }
    *out = (int)parsed;
    return true;
  }

  /* Parses one "#SYS#...#END#" line into a system_stats_t. Returns false for anything else. */
  static bool parse_stats_line(const char *line, size_t length, system_stats_t *stats)
  {
    size_t prefix_len = strlen(STATS_LINE_PREFIX);
    size_t suffix_len = strlen(STATS_LINE_SUFFIX);
    if (length < prefix_len + suffix_len ||
        memcmp(line, STATS_LINE_PREFIX, prefix_len) != 0 ||
        memcmp(line + length - suffix_len, STATS_LINE_SUFFIX, suffix_len) != 0) {
      return false;
    }

    size_t body_len = length - prefix_len - suffix_len;
    char body[400];
    if (body_len >= sizeof(body)) {
      return false;
    }
    memcpy(body, line + prefix_len, body_len);
    body[body_len] = '\0';

    memset(stats, 0, sizeof(*stats));

    char *field_state = NULL;
    char *field = strtok_r(body, "|", &field_state);
    while (field != NULL) {
      char *equals = strchr(field, '=');
      if (equals != NULL) {
        *equals = '\0';
        const char *key = field;
        const char *value = equals + 1;

        if (strcmp(key, "cpus") == 0) {
          char *cpu_state = NULL;
          char *cpu_token = strtok_r((char *)value, ";", &cpu_state);
          while (cpu_token != NULL && stats->cpu_core_count < MAX_CPU_CORES) {
            float usage;
            if (parse_float_token(cpu_token, &usage)) {
              stats->cpu_usage[stats->cpu_core_count++] = usage;
            }
            cpu_token = strtok_r(NULL, ";", &cpu_state);
          }
        } else if (strcmp(key, "corethreads") == 0) {
          char *thread_state = NULL;
          char *thread_token = strtok_r((char *)value, ";", &thread_state);
          while (thread_token != NULL && stats->core_thread_data_count < MAX_CPU_CORES) {
            int threads;
            if (parse_int_token(thread_token, &threads)) {
              stats->core_thread_count[stats->core_thread_data_count++] = threads;
            }
            thread_token = strtok_r(NULL, ";", &thread_state);
          }
        } else if (strcmp(key, "cputemp") == 0) {
          stats->has_cpu_temp = parse_float_token(value, &stats->cpu_temp);
        } else if (strcmp(key, "ramused") == 0) {
          stats->has_ram_used = parse_float_token(value, &stats->ram_used_mb);
        } else if (strcmp(key, "ramtotal") == 0) {
          stats->has_ram_total = parse_float_token(value, &stats->ram_total_mb);
        } else if (strcmp(key, "vramused") == 0) {
          stats->has_vram_used = parse_float_token(value, &stats->vram_used_mb);
        } else if (strcmp(key, "vramtotal") == 0) {
          stats->has_vram_total = parse_float_token(value, &stats->vram_total_mb);
        } else if (strcmp(key, "gpuusage") == 0) {
          stats->has_gpu_usage = parse_float_token(value, &stats->gpu_usage);
        } else if (strcmp(key, "gputemp") == 0) {
          stats->has_gpu_temp = parse_float_token(value, &stats->gpu_temp);
        } else if (strcmp(key, "netup") == 0) {
          stats->has_net_up = parse_float_token(value, &stats->net_up_kbps);
        } else if (strcmp(key, "netdown") == 0) {
          stats->has_net_down = parse_float_token(value, &stats->net_down_kbps);
        } else if (strcmp(key, "procs") == 0) {
          stats->has_procs = parse_int_token(value, &stats->procs);
        } else if (strcmp(key, "threads") == 0) {
          stats->has_threads = parse_int_token(value, &stats->threads_total);
        }
      }
      field = strtok_r(NULL, "|", &field_state);
    }
    return true;
  }

  /* Routes one complete line (without the trailing \n) to either the stats queue or the terminal stream. */
  static void process_line(const uint8_t *line, size_t length)
  {
    size_t trimmed_length = length;
    if (trimmed_length > 0 && line[trimmed_length - 1] == '\r') {
      --trimmed_length;
    }

    system_stats_t stats;
    if (parse_stats_line((const char *)line, trimmed_length, &stats)) {
      if (xQueueSend(s_stats_queue, &stats, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Stats queue full, dropping update");
      }
      return;
    }

    if (length > 0) {
      size_t sent = xStreamBufferSend(s_serial_stream, line, length, portMAX_DELAY);
      if (sent != length) {
        ESP_LOGW(TAG, "Serial stream overflow");
      }
    }
    uint8_t newline = '\n';
    xStreamBufferSend(s_serial_stream, &newline, 1, portMAX_DELAY);
  }

  static void uart_receive_task(void *argument)
  {
    (void)argument;
    uint8_t input[256];
    static uint8_t line_buffer[512];
    size_t line_length = 0;
    TickType_t last_activity = xTaskGetTickCount();
    while (true) {
      int length = uart_read_bytes(SERIAL_PORT, input, sizeof(input), pdMS_TO_TICKS(2000));
      if (length > 0) {
        /* Visible on the flashing/console port (ttyACM0), never on ttyUSB0. */
        ESP_LOGI(TAG, "UART2 RX %d byte(s)", length);
        log_rx_hex(input, length);
        last_activity = xTaskGetTickCount();
        for (int i = 0; i < length; ++i) {
          uint8_t byte = input[i];
          if (byte == '\n') {
            process_line(line_buffer, line_length);
            line_length = 0;
            continue;
          }
          if (line_length < sizeof(line_buffer)) {
            line_buffer[line_length++] = byte;
          } else {
            /* Line too long for the buffer (and definitely not a stats line): flush as text. */
            process_line(line_buffer, line_length);
            line_length = 0;
          }
        }
      } else if (xTaskGetTickCount() - last_activity > pdMS_TO_TICKS(5000)) {
        ESP_LOGW(TAG, "UART2 idle: 0 bytes in the last 5s. Check GND common with the "
                      "USB-TTL adapter, TX/RX not swapped, matching 115200 baud on the "
                      "host side, and that this ESP32 module has no PSRAM on GPIO16/17.");
        last_activity = xTaskGetTickCount();
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
        lcd_draw_chrome();
    s_serial_stream = xStreamBufferCreate(4096, 1);
    if (s_serial_stream == NULL) {
      ESP_LOGE(TAG, "Unable to allocate serial stream buffer");
      return;
    }
    s_stats_queue = xQueueCreate(4, sizeof(system_stats_t));
    if (s_stats_queue == NULL) {
      ESP_LOGE(TAG, "Unable to allocate stats queue");
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
