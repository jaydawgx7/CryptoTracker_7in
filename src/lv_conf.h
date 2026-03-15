#pragma once

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE "esp_heap_caps.h"
#define LV_MEM_CUSTOM_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define LV_MEM_CUSTOM_REALLOC(ptr, size) heap_caps_realloc((ptr), (size), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define LV_MEM_CUSTOM_FREE(ptr) heap_caps_free(ptr)
#define LV_MEM_SIZE (128 * 1024U)
#define LV_MEM_ADR 0

#define LV_USE_THEME_DEFAULT 1
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_GPU_NXP_PXP 0
#define LV_USE_GPU_NXP_VG_LITE 0

#define LV_DISP_DEF_REFR_PERIOD 10
#define LV_INDEV_DEF_READ_PERIOD 10

#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_12 1

#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_32 1
#if LV_USE_LOG
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#endif
#define LV_LOG_PRINTF 0

#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MEM 1
#define LV_USE_ASSERT_OBJ 0

#define LV_TICK_CUSTOM 0


#define LV_USE_CHART 1
#define LV_USE_KEYBOARD 1
#define LV_USE_COLORWHEEL 1
