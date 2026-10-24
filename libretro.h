/*
 * Subset minimal dari libretro.h (API libretro, lisensi MIT).
 * Hanya berisi definisi yang dipakai oleh retrobridge.cpp.
 * Header lengkap: https://github.com/libretro/libretro-common/blob/master/include/libretro.h
 */
#ifndef LIBRETRO_SUBSET_H
#define LIBRETRO_SUBSET_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RETRO_API_VERSION 1

/* ---- Device / input ---- */
#define RETRO_DEVICE_MASK     0xFF
#define RETRO_DEVICE_NONE     0
#define RETRO_DEVICE_JOYPAD   1
#define RETRO_DEVICE_MOUSE    2
#define RETRO_DEVICE_KEYBOARD 3
#define RETRO_DEVICE_LIGHTGUN 4
#define RETRO_DEVICE_ANALOG   5
#define RETRO_DEVICE_POINTER  6

#define RETRO_DEVICE_ID_JOYPAD_B      0
#define RETRO_DEVICE_ID_JOYPAD_Y      1
#define RETRO_DEVICE_ID_JOYPAD_SELECT 2
#define RETRO_DEVICE_ID_JOYPAD_START  3
#define RETRO_DEVICE_ID_JOYPAD_UP     4
#define RETRO_DEVICE_ID_JOYPAD_DOWN   5
#define RETRO_DEVICE_ID_JOYPAD_LEFT   6
#define RETRO_DEVICE_ID_JOYPAD_RIGHT  7
#define RETRO_DEVICE_ID_JOYPAD_A      8
#define RETRO_DEVICE_ID_JOYPAD_X      9
#define RETRO_DEVICE_ID_JOYPAD_L      10
#define RETRO_DEVICE_ID_JOYPAD_R      11
#define RETRO_DEVICE_ID_JOYPAD_L2     12
#define RETRO_DEVICE_ID_JOYPAD_R2     13
#define RETRO_DEVICE_ID_JOYPAD_L3     14
#define RETRO_DEVICE_ID_JOYPAD_R3     15
#define RETRO_DEVICE_ID_JOYPAD_MASK   256

#define RETRO_DEVICE_INDEX_ANALOG_LEFT   0
#define RETRO_DEVICE_INDEX_ANALOG_RIGHT  1
#define RETRO_DEVICE_INDEX_ANALOG_BUTTON 2
#define RETRO_DEVICE_ID_ANALOG_X 0
#define RETRO_DEVICE_ID_ANALOG_Y 1

#define RETRO_MEMORY_SAVE_RAM 0

/* ---- Pixel format ---- */
enum retro_pixel_format {
   RETRO_PIXEL_FORMAT_0RGB1555 = 0,
   RETRO_PIXEL_FORMAT_XRGB8888 = 1,
   RETRO_PIXEL_FORMAT_RGB565   = 2,
   RETRO_PIXEL_FORMAT_UNKNOWN  = INT_MAX
};

/* ---- Log ---- */
enum retro_log_level {
   RETRO_LOG_DEBUG = 0,
   RETRO_LOG_INFO  = 1,
   RETRO_LOG_WARN  = 2,
   RETRO_LOG_ERROR = 3,
   RETRO_LOG_DUMMY = INT_MAX
};
typedef void (*retro_log_printf_t)(enum retro_log_level level, const char *fmt, ...);
struct retro_log_callback { retro_log_printf_t log; };

/* ---- Hardware render ---- */
enum retro_hw_context_type {
   RETRO_HW_CONTEXT_NONE             = 0,
   RETRO_HW_CONTEXT_OPENGL           = 1,
   RETRO_HW_CONTEXT_OPENGLES2        = 2,
   RETRO_HW_CONTEXT_OPENGL_CORE      = 3,
   RETRO_HW_CONTEXT_OPENGLES3        = 4,
   RETRO_HW_CONTEXT_OPENGLES_VERSION = 5,
   RETRO_HW_CONTEXT_VULKAN           = 6,
   RETRO_HW_CONTEXT_D3D11            = 7,
   RETRO_HW_CONTEXT_D3D10            = 8,
   RETRO_HW_CONTEXT_D3D12            = 9,
   RETRO_HW_CONTEXT_D3D9             = 10,
   RETRO_HW_CONTEXT_DUMMY            = INT_MAX
};

typedef void (*retro_hw_context_reset_t)(void);
typedef uintptr_t (*retro_hw_get_current_framebuffer_t)(void);
typedef void (*retro_proc_address_t)(void);
typedef retro_proc_address_t (*retro_hw_get_proc_address_t)(const char *sym);

struct retro_hw_render_callback {
   enum retro_hw_context_type context_type;
   retro_hw_context_reset_t context_reset;
   retro_hw_get_current_framebuffer_t get_current_framebuffer;
   retro_hw_get_proc_address_t get_proc_address;
   bool depth;
   bool stencil;
   bool bottom_left_origin;
   unsigned version_major;
   unsigned version_minor;
   bool cache_context;
   retro_hw_context_reset_t context_destroy;
   bool debug_context;
};

#define RETRO_HW_FRAME_BUFFER_VALID ((void*)-1)

/* ---- Structs ---- */
struct retro_system_info {
   const char *library_name;
   const char *library_version;
   const char *valid_extensions;
   bool need_fullpath;
   bool block_extract;
};

struct retro_game_geometry {
   unsigned base_width;
   unsigned base_height;
   unsigned max_width;
   unsigned max_height;
   float    aspect_ratio;
};

struct retro_system_timing {
   double fps;
   double sample_rate;
};

struct retro_system_av_info {
   struct retro_game_geometry geometry;
   struct retro_system_timing timing;
};

struct retro_game_info {
   const char *path;
   const void *data;
   size_t      size;
   const char *meta;
};

struct retro_variable {
   const char *key;
   const char *value;
};

struct retro_message {
   const char *msg;
   unsigned    frames;
};

/* ---- Environment ---- */
#define RETRO_ENVIRONMENT_EXPERIMENTAL 0x10000

#define RETRO_ENVIRONMENT_SET_ROTATION                 1
#define RETRO_ENVIRONMENT_GET_OVERSCAN                 2
#define RETRO_ENVIRONMENT_GET_CAN_DUPE                 3
#define RETRO_ENVIRONMENT_SET_MESSAGE                  6
#define RETRO_ENVIRONMENT_SHUTDOWN                     7
#define RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL        8
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY         9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT             10
#define RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS        11
#define RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK        12
#define RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE   13
#define RETRO_ENVIRONMENT_SET_HW_RENDER                14
#define RETRO_ENVIRONMENT_GET_VARIABLE                 15
#define RETRO_ENVIRONMENT_SET_VARIABLES                16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE          17
#define RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME          18
#define RETRO_ENVIRONMENT_GET_LIBRETRO_PATH            19
#define RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK      21
#define RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK           22
#define RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE         23
#define RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES 24
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE            27
#define RETRO_ENVIRONMENT_GET_PERF_INTERFACE           28
#define RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY    30
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY           31
#define RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO           32
#define RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO           34
#define RETRO_ENVIRONMENT_SET_CONTROLLER_INFO          35
#define RETRO_ENVIRONMENT_SET_MEMORY_MAPS              (36 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_GEOMETRY                 37
#define RETRO_ENVIRONMENT_GET_USERNAME                 38
#define RETRO_ENVIRONMENT_GET_LANGUAGE                 39
#define RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER (40 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE      (41 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS     (42 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS     44
#define RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE       (47 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_INPUT_BITMASKS           (51 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION     52
#define RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER      56
#define RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION 59
#define RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY    63
#define RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS          61
#define RETRO_ENVIRONMENT_SET_VARIABLE                 70
#define RETRO_ENVIRONMENT_GET_JIT_CAPABLE              74

/* ---- Callback types ---- */
typedef bool    (*retro_environment_t)(unsigned cmd, void *data);
typedef void     (*retro_video_refresh_t)(const void *data, unsigned width, unsigned height, size_t pitch);
typedef void     (*retro_audio_sample_t)(int16_t left, int16_t right);
typedef size_t   (*retro_audio_sample_batch_t)(const int16_t *data, size_t frames);
typedef void     (*retro_input_poll_t)(void);
typedef int16_t  (*retro_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);

#ifdef __cplusplus
}
#endif
#endif
