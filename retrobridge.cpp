// retrobridge.cpp - jembatan JNI: frontend libretro minimal untuk Android.
//
//  - memuat core libretro (.so) dengan dlopen
//  - video: software (ANativeWindow) atau hardware GLES (EGL + FBO, lalu blit ke layar)
//  - audio: AAudio (PCM 16-bit stereo)
//  - input: tombol digital + 2 analog (port 0)
//  - save state: serialize / unserialize
//
// Semua pemanggilan core dilakukan di SATU thread emulasi. Fungsi JNI hanya
// mengubah state atomik / antrian perintah, sehingga aman dipanggil dari thread UI.

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <aaudio/AAudio.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libretro.h"

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif

#define TAG "retrobridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

using clk = std::chrono::steady_clock;

struct Cmd {
    int type = 0;            // 1 = simpan state, 2 = muat state
    std::string path;
    bool pending = false;
    bool done = false;
    bool ok = false;
};

struct Bridge {
    // ---- siklus hidup ----
    std::mutex lifeMx;                 // melindungi pembuatan / join thread
    std::thread th;
    std::mutex mx;                     // melindungi field di bawah
    std::condition_variable cv;
    std::atomic<bool> quit{false};
    std::atomic<bool> paused{false};
    bool loadDone = false;
    bool loadOk = false;
    std::string lastError;
    std::string lastCoreLog;
    Cmd cmd;

    // ---- argumen ----
    std::string corePath, gamePath, systemDir, saveDir;

    // ---- window ----
    ANativeWindow* newWindow = nullptr;   // menunggu diterapkan (dijaga mx)
    bool windowDirty = false;
    ANativeWindow* window = nullptr;      // hanya dipakai thread emulasi
    int swW = 0, swH = 0;

    // ---- core ----
    void* lib = nullptr;
    bool coreInited = false;
    bool gameLoaded = false;
    void (*retro_set_environment)(retro_environment_t) = nullptr;
    void (*retro_set_video_refresh)(retro_video_refresh_t) = nullptr;
    void (*retro_set_audio_sample)(retro_audio_sample_t) = nullptr;
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
    void (*retro_set_input_poll)(retro_input_poll_t) = nullptr;
    void (*retro_set_input_state)(retro_input_state_t) = nullptr;
    void (*retro_init)() = nullptr;
    void (*retro_deinit)() = nullptr;
    unsigned (*retro_api_version)() = nullptr;
    void (*retro_get_system_info)(retro_system_info*) = nullptr;
    void (*retro_get_system_av_info)(retro_system_av_info*) = nullptr;
    void (*retro_set_controller_port_device)(unsigned, unsigned) = nullptr;
    void (*retro_reset)() = nullptr;
    void (*retro_run)() = nullptr;
    size_t (*retro_serialize_size)() = nullptr;
    bool (*retro_serialize)(void*, size_t) = nullptr;
    bool (*retro_unserialize)(const void*, size_t) = nullptr;
    bool (*retro_load_game)(const retro_game_info*) = nullptr;
    void (*retro_unload_game)() = nullptr;

    retro_system_av_info av{};
    unsigned pixFmt = RETRO_PIXEL_FORMAT_0RGB1555;

    // ---- opsi core ----
    std::map<std::string, std::vector<std::string>> optValues;
    std::map<std::string, std::string> optCur;
    std::map<std::string, std::string> userOpts;

    // ---- hardware render ----
    bool hwRequested = false;
    retro_hw_render_callback hw{};
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLConfig cfg = nullptr;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface pbuf = EGL_NO_SURFACE;
    EGLSurface surf = EGL_NO_SURFACE;
    bool es3 = false;
    GLuint fbo = 0, tex = 0, rbo = 0, prog = 0, vbo = 0, vao = 0;
    GLint locTex = -1;
    int fboW = 0, fboH = 0;
    bool ctxResetDone = false;

    // ---- audio ----
    AAudioStream* audio = nullptr;
    bool audioStarted = false;
    int audioRate = 0;
    std::vector<int16_t> sampleBuf;
    clk::time_point audioRetry{};

    // ---- timing ----
    bool needFullPath = true;
};

Bridge g;

// ---- input (ditulis thread UI, dibaca thread emulasi) ----
std::atomic<uint32_t> g_buttons{0};
std::atomic<int> g_axis[4];

void setErr(const std::string& s) {
    {
        std::lock_guard<std::mutex> l(g.mx);
        g.lastError = s;
    }
    LOGE("%s", s.c_str());
}

// =====================================================================
//  Log core
// =====================================================================
void logCb(enum retro_log_level level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int prio = ANDROID_LOG_DEBUG;
    switch (level) {
        case RETRO_LOG_INFO: prio = ANDROID_LOG_INFO; break;
        case RETRO_LOG_WARN: prio = ANDROID_LOG_WARN; break;
        case RETRO_LOG_ERROR: prio = ANDROID_LOG_ERROR; break;
        default: break;
    }
    __android_log_print(prio, "retrocore", "%s", buf);
    if (level >= RETRO_LOG_ERROR) {
        std::lock_guard<std::mutex> l(g.mx);
        g.lastCoreLog = buf;
        while (!g.lastCoreLog.empty() && (g.lastCoreLog.back() == '\n' || g.lastCoreLog.back() == '\r'))
            g.lastCoreLog.pop_back();
    }
}

// =====================================================================
//  Opsi core
// =====================================================================
std::vector<std::string> splitBar(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find('|', start);
        if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

bool hasValue(const std::string& key, const std::string& val) {
    auto it = g.optValues.find(key);
    if (it == g.optValues.end()) return false;
    for (auto& v : it->second) if (v == val) return true;
    return false;
}

// Baca "<systemDir>/core_options.cfg" (baris: kunci = nilai) untuk menimpa opsi core.
void loadUserOptions() {
    g.userOpts.clear();
    // Bawaan: paksa backend GLES untuk PPSSPP (jembatan ini belum mendukung Vulkan).
    g.userOpts["ppsspp_backend"] = "opengl";
    std::string path = g.systemDir + "/core_options.cfg";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string t) {
            size_t a = t.find_first_not_of(" \t\r\n");
            size_t b = t.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
        };
        std::string k = trim(s.substr(0, eq));
        std::string v = trim(s.substr(eq + 1));
        if (k.empty() || k[0] == '#') continue;
        g.userOpts[k] = v;
    }
    fclose(f);
}

void parseVariables(const retro_variable* v) {
    for (; v && v->key; ++v) {
        std::string key = v->key;
        std::string desc = v->value ? v->value : "";
        size_t p = desc.find("; ");
        if (p == std::string::npos) continue;
        std::vector<std::string> vals = splitBar(desc.substr(p + 2));
        g.optValues[key] = vals;
        if (!g.optCur.count(key)) g.optCur[key] = vals.empty() ? "" : vals[0];
    }
    for (auto& kv : g.userOpts) {
        if (hasValue(kv.first, kv.second)) g.optCur[kv.first] = kv.second;
    }
}

// =====================================================================
//  Hardware render (EGL / GLES)
// =====================================================================
uintptr_t getCurrentFramebuffer() { return (uintptr_t)g.fbo; }

retro_proc_address_t getProcAddress(const char* sym) {
    return (retro_proc_address_t)eglGetProcAddress(sym);
}

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("shader gagal: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool createBlitProgram() {
    const char* vs =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUv;\n"
        "varying vec2 vUv;\n"
        "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); vUv = aUv; }\n";
    const char* fs =
        "precision mediump float;\n"
        "varying vec2 vUv;\n"
        "uniform sampler2D uTex;\n"
        "void main(){ gl_FragColor = texture2D(uTex, vUv); }\n";
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return false;
    g.prog = glCreateProgram();
    glAttachShader(g.prog, v);
    glAttachShader(g.prog, f);
    glBindAttribLocation(g.prog, 0, "aPos");
    glBindAttribLocation(g.prog, 1, "aUv");
    glLinkProgram(g.prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(g.prog, GL_LINK_STATUS, &ok);
    if (!ok) { LOGE("link program gagal"); return false; }
    g.locTex = glGetUniformLocation(g.prog, "uTex");
    glGenBuffers(1, &g.vbo);
    if (g.es3) glGenVertexArrays(1, &g.vao);
    return true;
}

bool ensureFbo(int w, int h) {
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    if (g.fbo && w <= g.fboW && h <= g.fboH) return true;
    if (g.fbo) { glDeleteFramebuffers(1, &g.fbo); g.fbo = 0; }
    if (g.tex) { glDeleteTextures(1, &g.tex); g.tex = 0; }
    if (g.rbo) { glDeleteRenderbuffers(1, &g.rbo); g.rbo = 0; }

    glGenTextures(1, &g.tex);
    glBindTexture(GL_TEXTURE_2D, g.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, g.es3 ? GL_RGBA8 : GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &g.rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, g.rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, g.es3 ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT16, w, h);

    glGenFramebuffers(1, &g.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.tex, 0);
    if (g.es3) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g.rbo);
    } else {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g.rbo);
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("FBO tidak lengkap: 0x%x (%dx%d)", st, w, h);
        return false;
    }
    g.fboW = w;
    g.fboH = h;
    LOGI("FBO %dx%d dibuat", w, h);
    return true;
}

void recreateWindowSurface() {
    if (g.ctx == EGL_NO_CONTEXT) return;
    eglMakeCurrent(g.dpy, g.pbuf, g.pbuf, g.ctx);
    if (g.surf != EGL_NO_SURFACE) {
        eglDestroySurface(g.dpy, g.surf);
        g.surf = EGL_NO_SURFACE;
    }
    if (g.window) {
        EGLSurface s = eglCreateWindowSurface(g.dpy, g.cfg, g.window, nullptr);
        if (s != EGL_NO_SURFACE) {
            if (eglMakeCurrent(g.dpy, s, s, g.ctx)) {
                eglSwapInterval(g.dpy, 0);
                g.surf = s;
            } else {
                eglDestroySurface(g.dpy, s);
                LOGE("eglMakeCurrent(window) gagal: 0x%x", eglGetError());
            }
        } else {
            LOGE("eglCreateWindowSurface gagal: 0x%x", eglGetError());
        }
    }
}

bool initGL() {
    g.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g.dpy == EGL_NO_DISPLAY) { setErr("EGL: display tidak tersedia."); return false; }
    EGLint maj = 0, min = 0;
    if (!eglInitialize(g.dpy, &maj, &min)) { setErr("EGL: eglInitialize gagal."); return false; }

    for (int ver = 3; ver >= 2; --ver) {
        const EGLint cfgAttr[] = {
            EGL_RENDERABLE_TYPE, ver == 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE};
        EGLConfig c = nullptr;
        EGLint n = 0;
        if (!eglChooseConfig(g.dpy, cfgAttr, &c, 1, &n) || n < 1) continue;
        const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, ver, EGL_NONE};
        EGLContext cx = eglCreateContext(g.dpy, c, EGL_NO_CONTEXT, ctxAttr);
        if (cx == EGL_NO_CONTEXT) continue;
        g.cfg = c;
        g.ctx = cx;
        g.es3 = (ver == 3);
        break;
    }
    if (g.ctx == EGL_NO_CONTEXT) { setErr("EGL: tidak bisa membuat konteks GLES."); return false; }

    const EGLint pbAttr[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    g.pbuf = eglCreatePbufferSurface(g.dpy, g.cfg, pbAttr);
    if (g.pbuf == EGL_NO_SURFACE) { setErr("EGL: pbuffer gagal dibuat."); return false; }
    if (!eglMakeCurrent(g.dpy, g.pbuf, g.pbuf, g.ctx)) { setErr("EGL: eglMakeCurrent gagal."); return false; }

    LOGI("GLES: %s | %s (ES3=%d)", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION), g.es3 ? 1 : 0);

    if (!createBlitProgram()) { setErr("GL: shader blit gagal."); return false; }
    int mw = (int)g.av.geometry.max_width, mh = (int)g.av.geometry.max_height;
    if (mw <= 0) mw = (int)g.av.geometry.base_width;
    if (mh <= 0) mh = (int)g.av.geometry.base_height;
    if (!ensureFbo(mw, mh)) { setErr("GL: framebuffer gagal dibuat."); return false; }

    recreateWindowSurface();
    // recreateWindowSurface membuat konteks aktif di surface; pastikan FBO core tetap valid.
    if (g.hw.context_reset) g.hw.context_reset();
    g.ctxResetDone = true;
    return true;
}

void shutdownGL() {
    if (g.ctx == EGL_NO_CONTEXT) return;
    eglMakeCurrent(g.dpy, g.surf != EGL_NO_SURFACE ? g.surf : g.pbuf, g.surf != EGL_NO_SURFACE ? g.surf : g.pbuf, g.ctx);
    if (g.fbo) glDeleteFramebuffers(1, &g.fbo);
    if (g.tex) glDeleteTextures(1, &g.tex);
    if (g.rbo) glDeleteRenderbuffers(1, &g.rbo);
    if (g.vbo) glDeleteBuffers(1, &g.vbo);
    if (g.vao) glDeleteVertexArrays(1, &g.vao);
    if (g.prog) glDeleteProgram(g.prog);
    g.fbo = g.tex = g.rbo = g.vbo = g.vao = g.prog = 0;
    eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g.surf != EGL_NO_SURFACE) eglDestroySurface(g.dpy, g.surf);
    if (g.pbuf != EGL_NO_SURFACE) eglDestroySurface(g.dpy, g.pbuf);
    eglDestroyContext(g.dpy, g.ctx);
    eglTerminate(g.dpy);
    g.surf = g.pbuf = EGL_NO_SURFACE;
    g.ctx = EGL_NO_CONTEXT;
    g.dpy = EGL_NO_DISPLAY;
}

void presentGL(unsigned w, unsigned h) {
    if (g.surf == EGL_NO_SURFACE || !g.fbo || w == 0 || h == 0) return;
    EGLint sw = 0, sh = 0;
    eglQuerySurface(g.dpy, g.surf, EGL_WIDTH, &sw);
    eglQuerySurface(g.dpy, g.surf, EGL_HEIGHT, &sh);
    if (sw <= 0 || sh <= 0) return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, sw, sh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    float u = (float)w / (float)g.fboW;
    float v = (float)h / (float)g.fboH;
    if (u > 1.f) u = 1.f;
    if (v > 1.f) v = 1.f;
    // Origin kiri-bawah (konvensi GL) vs kiri-atas.
    float v0 = g.hw.bottom_left_origin ? 0.f : v;   // v untuk tepi bawah layar
    float v1 = g.hw.bottom_left_origin ? v : 0.f;   // v untuk tepi atas layar
    const float quad[16] = {
        -1.f, -1.f, 0.f, v0,
         1.f, -1.f, u,   v0,
        -1.f,  1.f, 0.f, v1,
         1.f,  1.f, u,   v1,
    };

    glUseProgram(g.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.tex);
    glUniform1i(g.locTex, 0);
    if (g.es3 && g.vao) glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (g.es3) glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    eglSwapBuffers(g.dpy, g.surf);
}

// =====================================================================
//  Video software
// =====================================================================
void presentSW(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!g.window || w == 0 || h == 0) return;
    if ((int)w != g.swW || (int)h != g.swH) {
        ANativeWindow_setBuffersGeometry(g.window, (int32_t)w, (int32_t)h, WINDOW_FORMAT_RGBA_8888);
        g.swW = (int)w;
        g.swH = (int)h;
    }
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(g.window, &buf, nullptr) != 0) return;
    uint32_t rows = h < (unsigned)buf.height ? h : (unsigned)buf.height;
    uint32_t cols = w < (unsigned)buf.width ? w : (unsigned)buf.width;
    for (uint32_t y = 0; y < rows; ++y) {
        uint32_t* dst = (uint32_t*)buf.bits + (size_t)y * buf.stride;
        const uint8_t* src = (const uint8_t*)data + (size_t)y * pitch;
        if (g.pixFmt == RETRO_PIXEL_FORMAT_XRGB8888) {
            const uint32_t* s = (const uint32_t*)src;
            for (uint32_t x = 0; x < cols; ++x) {
                uint32_t p = s[x];
                dst[x] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
            }
        } else if (g.pixFmt == RETRO_PIXEL_FORMAT_RGB565) {
            const uint16_t* s = (const uint16_t*)src;
            for (uint32_t x = 0; x < cols; ++x) {
                uint32_t p = s[x];
                uint32_t r = (p >> 11) & 31u, gg = (p >> 5) & 63u, b = p & 31u;
                r = (r << 3) | (r >> 2);
                gg = (gg << 2) | (gg >> 4);
                b = (b << 3) | (b >> 2);
                dst[x] = 0xFF000000u | (b << 16) | (gg << 8) | r;
            }
        } else {  // 0RGB1555
            const uint16_t* s = (const uint16_t*)src;
            for (uint32_t x = 0; x < cols; ++x) {
                uint32_t p = s[x];
                uint32_t r = (p >> 10) & 31u, gg = (p >> 5) & 31u, b = p & 31u;
                r = (r << 3) | (r >> 2);
                gg = (gg << 3) | (gg >> 2);
                b = (b << 3) | (b >> 2);
                dst[x] = 0xFF000000u | (b << 16) | (gg << 8) | r;
            }
        }
    }
    ANativeWindow_unlockAndPost(g.window);
}

void videoCb(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;  // frame duplikat
    if (g.hwRequested) {
        if (data == RETRO_HW_FRAME_BUFFER_VALID) presentGL(w, h);
    } else {
        presentSW(data, w, h, pitch);
    }
}

// =====================================================================
//  Audio (AAudio)
// =====================================================================
void closeAudio() {
    if (g.audio) {
        AAudioStream_requestStop(g.audio);
        AAudioStream_close(g.audio);
        g.audio = nullptr;
    }
    g.audioStarted = false;
}

bool openAudio(int rate) {
    closeAudio();
    if (rate < 8000) rate = 44100;
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g.audio);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !g.audio) {
        LOGE("AAudio openStream gagal: %s", AAudio_convertResultToText(r));
        g.audio = nullptr;
        return false;
    }
    int32_t cap = AAudioStream_getBufferCapacityInFrames(g.audio);
    int32_t want = rate / 12;
    if (cap > 0 && want > cap) want = cap;
    AAudioStream_setBufferSizeInFrames(g.audio, want);
    g.audioRate = rate;
    AAudioStream_requestStart(g.audio);
    g.audioStarted = true;
    LOGI("AAudio dibuka: %d Hz, buffer %d frame", rate, want);
    return true;
}

void pauseAudio(bool pause) {
    if (!g.audio) return;
    if (pause && g.audioStarted) {
        AAudioStream_requestPause(g.audio);
        g.audioStarted = false;
    } else if (!pause && !g.audioStarted) {
        AAudioStream_requestStart(g.audio);
        g.audioStarted = true;
    }
}

void writeAudio(const int16_t* d, size_t frames) {
    if (!g.audio) {
        // coba buka ulang tiap 1 detik bila stream sebelumnya mati
        if (g.audioRate > 0 && clk::now() > g.audioRetry) {
            g.audioRetry = clk::now() + std::chrono::seconds(1);
            openAudio(g.audioRate);
        }
        return;
    }
    size_t done = 0;
    while (done < frames) {
        aaudio_result_t r = AAudioStream_write(g.audio, d + done * 2, (int32_t)(frames - done), 40LL * 1000 * 1000);
        if (r < 0) {
            LOGW("AAudio write error: %s", AAudio_convertResultToText(r));
            AAudioStream_close(g.audio);
            g.audio = nullptr;
            g.audioStarted = false;
            return;
        }
        if (r == 0) return;  // buffer penuh / timeout: buang sisanya
        done += (size_t)r;
    }
}

void audioSampleCb(int16_t l, int16_t r) {
    g.sampleBuf.push_back(l);
    g.sampleBuf.push_back(r);
}

size_t audioBatchCb(const int16_t* d, size_t frames) {
    if (!g.paused.load()) writeAudio(d, frames);
    return frames;
}

void flushSamples() {
    if (!g.sampleBuf.empty()) {
        if (!g.paused.load()) writeAudio(g.sampleBuf.data(), g.sampleBuf.size() / 2);
        g.sampleBuf.clear();
    }
}

// =====================================================================
//  Input
// =====================================================================
void inputPollCb() {}

int16_t inputStateCb(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
    uint32_t b = g_buttons.load(std::memory_order_relaxed);
    switch (device & RETRO_DEVICE_MASK) {
        case RETRO_DEVICE_JOYPAD:
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)(b & 0xFFFFu);
            return id < 16 ? (int16_t)((b >> id) & 1u) : 0;
        case RETRO_DEVICE_ANALOG:
            if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT || index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
                if (id > RETRO_DEVICE_ID_ANALOG_Y) return 0;
                return (int16_t)g_axis[index * 2 + id].load(std::memory_order_relaxed);
            }
            if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON && id < 16)
                return ((b >> id) & 1u) ? (int16_t)0x7FFF : (int16_t)0;
            return 0;
        default:
            return 0;
    }
}

// =====================================================================
//  Environment
// =====================================================================
bool envCb(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool*)data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            *(const char**)data = g.systemDir.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char**)data = g.saveDir.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
            *(const char**)data = g.corePath.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((retro_log_callback*)data)->log = logCb;
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            unsigned f = (unsigned)(*(const int*)data);
            if (f == RETRO_PIXEL_FORMAT_0RGB1555 || f == RETRO_PIXEL_FORMAT_XRGB8888 || f == RETRO_PIXEL_FORMAT_RGB565) {
                g.pixFmt = f;
                return true;
            }
            return false;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES:
            parseVariables((const retro_variable*)data);
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            retro_variable* v = (retro_variable*)data;
            if (!v || !v->key) return false;
            auto it = g.optCur.find(v->key);
            if (it == g.optCur.end()) { v->value = nullptr; return false; }
            v->value = it->second.c_str();
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLE: {
            const retro_variable* v = (const retro_variable*)data;
            if (!v || !v->key) return false;
            if (v->value) g.optCur[v->key] = v->value;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool*)data = false;
            return true;
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            retro_hw_render_callback* cb = (retro_hw_render_callback*)data;
            switch (cb->context_type) {
                case RETRO_HW_CONTEXT_OPENGLES2:
                case RETRO_HW_CONTEXT_OPENGLES3:
                case RETRO_HW_CONTEXT_OPENGLES_VERSION:
                    break;
                default:
                    LOGW("SET_HW_RENDER ditolak: tipe konteks %d tidak didukung", (int)cb->context_type);
                    return false;
            }
            cb->get_current_framebuffer = getCurrentFramebuffer;
            cb->get_proc_address = getProcAddress;
            g.hw = *cb;
            g.hwRequested = true;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
            *(unsigned*)data = (unsigned)RETRO_HW_CONTEXT_OPENGLES3;
            return true;
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            const retro_game_geometry* gm = (const retro_game_geometry*)data;
            g.av.geometry = *gm;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
            const retro_system_av_info* av = (const retro_system_av_info*)data;
            g.av = *av;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
            *(unsigned*)data = 1;
            return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *(unsigned*)data = 0;  // RETRO_LANGUAGE_ENGLISH
            return true;
        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
            *(unsigned*)data = 0;
            return true;
        case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
            *(bool*)data = true;
            return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE: {
            const retro_message* m = (const retro_message*)data;
            if (m && m->msg) LOGI("core: %s", m->msg);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
            return true;
        default:
            // Tidak didukung: GET_CORE_OPTIONS_VERSION (core jatuh ke SET_VARIABLES),
            // GET_PERF_INTERFACE, rumble, disk control, Vulkan, dll.
            return false;
    }
}

// =====================================================================
//  Jendela / perintah
// =====================================================================
void applyWindow() {
    ANativeWindow* nw = nullptr;
    bool dirty = false;
    {
        std::lock_guard<std::mutex> l(g.mx);
        if (g.windowDirty) {
            nw = g.newWindow;
            g.newWindow = nullptr;
            g.windowDirty = false;
            dirty = true;
        }
    }
    if (!dirty) return;
    ANativeWindow* old = g.window;
    g.window = nw;
    g.swW = g.swH = 0;
    if (g.ctx != EGL_NO_CONTEXT) recreateWindowSurface();
    if (old) ANativeWindow_release(old);
}

void processCmd() {
    int type = 0;
    std::string path;
    {
        std::lock_guard<std::mutex> l(g.mx);
        if (!g.cmd.pending) return;
        type = g.cmd.type;
        path = g.cmd.path;
    }
    bool ok = false;
    if (type == 1) {
        size_t sz = g.retro_serialize_size ? g.retro_serialize_size() : 0;
        if (sz > 0) {
            std::vector<uint8_t> buf(sz);
            if (g.retro_serialize(buf.data(), sz)) {
                std::string tmp = path + ".tmp";
                FILE* f = fopen(tmp.c_str(), "wb");
                if (f) {
                    ok = fwrite(buf.data(), 1, sz, f) == sz;
                    fclose(f);
                    if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
                    if (!ok) remove(tmp.c_str());
                }
            }
        }
    } else if (type == 2) {
        FILE* f = fopen(path.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0) {
                std::vector<uint8_t> buf((size_t)n);
                if (fread(buf.data(), 1, (size_t)n, f) == (size_t)n) ok = g.retro_unserialize(buf.data(), (size_t)n);
            }
            fclose(f);
        }
    }
    {
        std::lock_guard<std::mutex> l(g.mx);
        g.cmd.ok = ok;
        g.cmd.done = true;
        g.cmd.pending = false;
    }
    g.cv.notify_all();
}

// =====================================================================
//  Thread emulasi
// =====================================================================
bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t r = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return r == (size_t)n;
}

#define LOAD_SYM(name)                                                      \
    g.name = (decltype(g.name))dlsym(g.lib, #name);                         \
    if (!g.name) { setErr("Core tidak punya fungsi " #name); return false; }

bool initSession() {
    g.hwRequested = false;
    g.ctxResetDone = false;
    g.pixFmt = RETRO_PIXEL_FORMAT_0RGB1555;
    g.optValues.clear();
    g.optCur.clear();
    g.sampleBuf.clear();
    g.audioRate = 0;
    {
        std::lock_guard<std::mutex> l(g.mx);
        g.lastCoreLog.clear();
    }
    loadUserOptions();

    g.lib = dlopen(g.corePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g.lib) {
        const char* e = dlerror();
        setErr(std::string("dlopen core gagal: ") + (e ? e : "?"));
        return false;
    }
    LOAD_SYM(retro_set_environment)
    LOAD_SYM(retro_set_video_refresh)
    LOAD_SYM(retro_set_audio_sample)
    LOAD_SYM(retro_set_audio_sample_batch)
    LOAD_SYM(retro_set_input_poll)
    LOAD_SYM(retro_set_input_state)
    LOAD_SYM(retro_init)
    LOAD_SYM(retro_deinit)
    LOAD_SYM(retro_api_version)
    LOAD_SYM(retro_get_system_info)
    LOAD_SYM(retro_get_system_av_info)
    LOAD_SYM(retro_set_controller_port_device)
    LOAD_SYM(retro_reset)
    LOAD_SYM(retro_run)
    LOAD_SYM(retro_serialize_size)
    LOAD_SYM(retro_serialize)
    LOAD_SYM(retro_unserialize)
    LOAD_SYM(retro_load_game)
    LOAD_SYM(retro_unload_game)

    if (g.retro_api_version() != RETRO_API_VERSION) {
        setErr("Versi API libretro core tidak cocok.");
        return false;
    }

    g.retro_set_environment(envCb);
    g.retro_init();
    g.coreInited = true;
    g.retro_set_video_refresh(videoCb);
    g.retro_set_audio_sample(audioSampleCb);
    g.retro_set_audio_sample_batch(audioBatchCb);
    g.retro_set_input_poll(inputPollCb);
    g.retro_set_input_state(inputStateCb);

    retro_system_info si;
    memset(&si, 0, sizeof(si));
    g.retro_get_system_info(&si);
    g.needFullPath = si.need_fullpath;
    LOGI("Core: %s %s (need_fullpath=%d)", si.library_name ? si.library_name : "?",
         si.library_version ? si.library_version : "?", g.needFullPath ? 1 : 0);

    retro_game_info gi;
    memset(&gi, 0, sizeof(gi));
    gi.path = g.gamePath.c_str();
    std::vector<uint8_t> blob;
    if (!g.needFullPath) {
        if (!readFile(g.gamePath, blob)) { setErr("Gagal membaca file game: " + g.gamePath); return false; }
        gi.data = blob.data();
        gi.size = blob.size();
    }

    if (!g.retro_load_game(&gi)) {
        std::string core;
        { std::lock_guard<std::mutex> l(g.mx); core = g.lastCoreLog; }
        setErr("Core menolak memuat game." + (core.empty() ? std::string() : " (" + core + ")"));
        return false;
    }
    g.gameLoaded = true;

    // Core PS1 (pcsx_rearmed dll.) memakai DualShock bila perangkat port diset ke ANALOG.
    if (g.corePath.find("/ps1_") != std::string::npos) g.retro_set_controller_port_device(0, RETRO_DEVICE_ANALOG);

    memset(&g.av, 0, sizeof(g.av));
    g.retro_get_system_av_info(&g.av);
    if (g.av.timing.fps < 1.0) g.av.timing.fps = 60.0;
    LOGI("AV: %ux%u (maks %ux%u) %.3f fps, %.0f Hz, hw=%d", g.av.geometry.base_width, g.av.geometry.base_height,
         g.av.geometry.max_width, g.av.geometry.max_height, g.av.timing.fps, g.av.timing.sample_rate, g.hwRequested ? 1 : 0);

    applyWindow();
    if (g.hwRequested && !initGL()) return false;
    openAudio((int)g.av.timing.sample_rate);
    return true;
}

void runLoop() {
    auto next = clk::now();
    while (!g.quit.load()) {
        applyWindow();
        processCmd();
        if (g.paused.load() || !g.window) {
            pauseAudio(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            next = clk::now();
            continue;
        }
        pauseAudio(false);
        if (g.hwRequested) {
            glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
        }
        g.retro_run();
        flushSamples();

        std::chrono::nanoseconds frame((int64_t)(1e9 / (g.av.timing.fps > 1.0 ? g.av.timing.fps : 60.0)));
        next += frame;
        auto now = clk::now();
        if (next > now) std::this_thread::sleep_until(next);
        else if (now - next > std::chrono::milliseconds(100)) next = now;
    }
}

void shutdownSession() {
    if (g.gameLoaded && g.retro_unload_game) g.retro_unload_game();
    g.gameLoaded = false;
    if (g.hwRequested && g.ctxResetDone && g.hw.context_destroy && g.ctx != EGL_NO_CONTEXT) g.hw.context_destroy();
    g.ctxResetDone = false;
    if (g.coreInited && g.retro_deinit) g.retro_deinit();
    g.coreInited = false;
    shutdownGL();
    closeAudio();
    if (g.lib) { dlclose(g.lib); g.lib = nullptr; }
    if (g.window) { ANativeWindow_release(g.window); g.window = nullptr; }
    g.hwRequested = false;
    memset(&g.hw, 0, sizeof(g.hw));
    {
        std::lock_guard<std::mutex> l(g.mx);
        g.cmd.pending = false;
        g.cmd.done = true;
        g.cmd.ok = false;
    }
    g.cv.notify_all();
}

void emuMain() {
    bool ok = initSession();
    {
        std::lock_guard<std::mutex> l(g.mx);
        g.loadOk = ok;
        g.loadDone = true;
    }
    g.cv.notify_all();
    if (ok) runLoop();
    shutdownSession();
}

void joinThread() {
    if (g.th.joinable()) {
        g.quit = true;
        g.cv.notify_all();
        g.th.join();
    }
}

std::string toStr(JNIEnv* env, jstring s) {
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

bool runCmd(int type, const std::string& path) {
    std::unique_lock<std::mutex> lk(g.mx);
    if (!g.th.joinable() || !g.loadDone || !g.loadOk) return false;
    g.cmd.type = type;
    g.cmd.path = path;
    g.cmd.pending = true;
    g.cmd.done = false;
    g.cmd.ok = false;
    bool got = g.cv.wait_for(lk, std::chrono::seconds(20), [] { return g.cmd.done || g.quit.load(); });
    return got && g.cmd.done && g.cmd.ok;
}

}  // namespace

// =====================================================================
//  JNI (object RetroBridge di Emulator.kt)
// =====================================================================
extern "C" {

JNIEXPORT jboolean JNICALL Java_com_playstation_allinone_RetroBridge_nativeLoad(
    JNIEnv* env, jclass, jstring core, jstring game, jstring system, jstring saves) {
    std::lock_guard<std::mutex> life(g.lifeMx);
    joinThread();
    g.quit = false;
    g.paused = false;
    g_buttons = 0;
    for (auto& a : g_axis) a = 0;
    {
        std::lock_guard<std::mutex> l(g.mx);
        g.loadDone = false;
        g.loadOk = false;
        g.lastError.clear();
        g.cmd = Cmd();
    }
    g.corePath = toStr(env, core);
    g.gamePath = toStr(env, game);
    g.systemDir = toStr(env, system);
    g.saveDir = toStr(env, saves);

    g.th = std::thread(emuMain);

    bool ok = false;
    bool finished = false;
    {
        std::unique_lock<std::mutex> lk(g.mx);
        finished = g.cv.wait_for(lk, std::chrono::seconds(180), [] { return g.loadDone || g.quit.load(); });
        ok = finished && g.loadDone && g.loadOk;
    }
    if (!ok) {
        {
            std::lock_guard<std::mutex> l(g.mx);
            if (g.lastError.empty()) g.lastError = finished ? "Pemuatan dibatalkan." : "Pemuatan core melewati batas waktu.";
        }
        g.quit = true;
        g.th.join();
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_playstation_allinone_RetroBridge_nativeSetSurface(JNIEnv* env, jclass, jobject surface) {
    ANativeWindow* w = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    std::lock_guard<std::mutex> l(g.mx);
    if (g.newWindow) ANativeWindow_release(g.newWindow);
    g.newWindow = w;
    g.windowDirty = true;
}

JNIEXPORT void JNICALL Java_com_playstation_allinone_RetroBridge_nativeSetPaused(JNIEnv*, jclass, jboolean p) {
    g.paused = (p == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_playstation_allinone_RetroBridge_nativeSetButton(JNIEnv*, jclass, jint id, jboolean down) {
    if (id < 0 || id > 15) return;
    uint32_t bit = 1u << id;
    if (down == JNI_TRUE) g_buttons.fetch_or(bit);
    else g_buttons.fetch_and(~bit);
}

JNIEXPORT void JNICALL Java_com_playstation_allinone_RetroBridge_nativeSetAxis(JNIEnv*, jclass, jint stick, jfloat x, jfloat y) {
    if (stick < 0 || stick > 1) return;
    auto conv = [](float v) {
        if (v > 1.f) v = 1.f;
        if (v < -1.f) v = -1.f;
        return (int)(v * 32767.f);
    };
    g_axis[stick * 2].store(conv(x));
    g_axis[stick * 2 + 1].store(conv(y));
}

JNIEXPORT jboolean JNICALL Java_com_playstation_allinone_RetroBridge_nativeSaveState(JNIEnv* env, jclass, jstring path) {
    return runCmd(1, toStr(env, path)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_playstation_allinone_RetroBridge_nativeLoadState(JNIEnv* env, jclass, jstring path) {
    return runCmd(2, toStr(env, path)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_playstation_allinone_RetroBridge_nativeStop(JNIEnv*, jclass) {
    {
        std::lock_guard<std::mutex> l(g.mx);  // set di dalam lock agar nativeLoad yang menunggu tidak kehilangan sinyal
        g.quit = true;
    }
    g.cv.notify_all();
    std::lock_guard<std::mutex> life(g.lifeMx);
    joinThread();
    std::lock_guard<std::mutex> l(g.mx);
    if (g.newWindow) { ANativeWindow_release(g.newWindow); g.newWindow = nullptr; }
    g.windowDirty = false;
}

JNIEXPORT jstring JNICALL Java_com_playstation_allinone_RetroBridge_nativeLastError(JNIEnv* env, jclass) {
    std::string s;
    {
        std::lock_guard<std::mutex> l(g.mx);
        s = g.lastError;
    }
    return env->NewStringUTF(s.c_str());
}

}  // extern "C"
