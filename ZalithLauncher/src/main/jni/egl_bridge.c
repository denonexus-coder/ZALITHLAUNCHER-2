#include <jni.h>
#include <assert.h>
#include <dlfcn.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <pthread.h>
#include <time.h>
#include <stddef.h>

#include <EGL/egl.h>
#include <GL/osmesa.h>
#include "ctxbridges/egl_loader.h"
#include "ctxbridges/osmesa_loader.h"
#include "ctxbridges/renderer_config.h"
#include "ctxbridges/virgl_bridge.h"
#include "driver_helper/nsbypass.h"

#ifdef GLES_TEST
#include <GLES2/gl2.h>
#endif

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/rect.h>
#include <string.h>
#include <environ/environ.h>
#include <android/dlext.h>

#include "utils.h"
#include "ctxbridges/bridge_tbl.h"
#include "ctxbridges/osm_bridge.h"

#define GLFW_CLIENT_API 0x22001
/* Consider GLFW_NO_API as Vulkan API */
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001

// This means that the function is an external API and that it will be used
#define EXTERNAL_API __attribute__((used))
// This means that you are forced to have this function/variable for ABI compatibility
#define ABI_COMPAT __attribute__((unused))

EGLConfig config;
struct PotatoBridge potatoBridge;

// MC 最近一次请求的交换间隔；-1 表示尚未请求
static int lastSwapInterval = -1;

void* loadTurnipVulkan(const char* driver_path, const char* native_dir, const char* cache_dir);
void calculateFPS(void);

extern void updateMonitorSize(int width, int height);
extern jboolean ensureGlfwNativeBridgeInitialized(JNIEnv *env);


/*
 * ============================================================
 * Frame statistics
 * ============================================================
 *
 * Statistics are calculated from the real time between
 * successive pojavSwapBuffers() calls.
 *
 * History window:
 *     2 seconds
 *
 * Returned JNI array:
 *
 *     [0] current FPS
 *     [1] minimum FPS
 *     [2] average FPS
 *     [3] maximum FPS
 *     [4] frame time in microseconds
 *
 * CLOCK_MONOTONIC is used instead of time(NULL), because
 * time(NULL) only has one-second resolution.
 */

#define FRAME_STATS_WINDOW_NS   2000000000ULL
#define FRAME_STATS_HISTORY_SIZE 1024

typedef struct {
    uint64_t timestampNs;
    int fps;
} FrameStatsSample;

static pthread_mutex_t frameStatsMutex =
        PTHREAD_MUTEX_INITIALIZER;

static FrameStatsSample frameStatsHistory[
        FRAME_STATS_HISTORY_SIZE
];

static size_t frameStatsHistoryStart = 0;
static size_t frameStatsHistoryCount = 0;

static uint64_t lastFrameTimestampNs = 0;

static int fps = 0;
static int minFps = 0;
static int averageFps = 0;
static int maxFps = 0;

static double frameTimeMs = 0.0;


/*
 * Get monotonic time in nanoseconds.
 */
static uint64_t getMonotonicTimeNs(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return
        (uint64_t) ts.tv_sec * 1000000000ULL +
        (uint64_t) ts.tv_nsec;
}


/*
 * Reset all frame statistics.
 *
 * Caller must hold frameStatsMutex.
 */
static void resetFrameStatsLocked(void) {
    frameStatsHistoryStart = 0;
    frameStatsHistoryCount = 0;

    lastFrameTimestampNs = 0;

    fps = 0;
    minFps = 0;
    averageFps = 0;
    maxFps = 0;

    frameTimeMs = 0.0;
}


/*
 * Public reset function.
 */
static void resetFrameStatsInternal(void) {
    pthread_mutex_lock(&frameStatsMutex);

    resetFrameStatsLocked();

    pthread_mutex_unlock(&frameStatsMutex);
}


/*
 * Remove samples older than the rolling window.
 *
 * Caller must hold frameStatsMutex.
 */
static void pruneFrameStatsLocked(uint64_t nowNs) {
    while (frameStatsHistoryCount > 0) {
        FrameStatsSample *oldest =
                &frameStatsHistory[frameStatsHistoryStart];

        /*
         * Protect against an invalid clock difference.
         */
        if (nowNs >= oldest->timestampNs &&
            nowNs - oldest->timestampNs <= FRAME_STATS_WINDOW_NS) {
            break;
        }

        frameStatsHistoryStart =
                (frameStatsHistoryStart + 1)
                % FRAME_STATS_HISTORY_SIZE;

        frameStatsHistoryCount--;
    }
}


/*
 * Add a new instantaneous FPS sample.
 *
 * Caller must hold frameStatsMutex.
 */
static void addFrameStatsSampleLocked(
        uint64_t timestampNs,
        int sampleFps
) {
    size_t index;

    if (frameStatsHistoryCount < FRAME_STATS_HISTORY_SIZE) {
        index =
                (frameStatsHistoryStart +
                 frameStatsHistoryCount)
                % FRAME_STATS_HISTORY_SIZE;

        frameStatsHistoryCount++;
    } else {
        /*
         * Ring buffer full.
         * Replace the oldest sample.
         */
        index = frameStatsHistoryStart;

        frameStatsHistoryStart =
                (frameStatsHistoryStart + 1)
                % FRAME_STATS_HISTORY_SIZE;
    }

    frameStatsHistory[index].timestampNs = timestampNs;
    frameStatsHistory[index].fps = sampleFps;
}


/*
 * Recalculate min/average/max FPS.
 *
 * Caller must hold frameStatsMutex.
 */
static void recomputeFrameStatsLocked(void) {
    if (frameStatsHistoryCount == 0) {
        minFps = 0;
        averageFps = 0;
        maxFps = 0;
        return;
    }

    int minValue =
            frameStatsHistory[
                    frameStatsHistoryStart
            ].fps;

    int maxValue = minValue;

    uint64_t sum = 0;

    for (size_t i = 0;
         i < frameStatsHistoryCount;
         ++i) {

        size_t index =
                (frameStatsHistoryStart + i)
                % FRAME_STATS_HISTORY_SIZE;

        int value =
                frameStatsHistory[index].fps;

        if (value < minValue) {
            minValue = value;
        }

        if (value > maxValue) {
            maxValue = value;
        }

        sum += (uint64_t) value;
    }

    minFps = minValue;
    maxFps = maxValue;

    averageFps =
            (int) (
                    (sum + frameStatsHistoryCount / 2)
                    / frameStatsHistoryCount
            );
}


/*
 * ============================================================
 * GLFW / EGL bridge
 * ============================================================
 */

EXTERNAL_API void pojavTerminate() {
    printf("EGLBridge: Terminating\n");

    /*
     * Stop reporting stale FPS data after Minecraft terminates.
     */
    resetFrameStatsInternal();

    switch (pojav_environ->config_renderer) {
        case RENDERER_GL4ES: {
            eglMakeCurrent_p(
                    potatoBridge.eglDisplay,
                    EGL_NO_SURFACE,
                    EGL_NO_SURFACE,
                    EGL_NO_CONTEXT
            );

            eglDestroySurface_p(
                    potatoBridge.eglDisplay,
                    potatoBridge.eglSurface
            );

            eglDestroyContext_p(
                    potatoBridge.eglDisplay,
                    potatoBridge.eglContext
            );

            eglTerminate_p(
                    potatoBridge.eglDisplay
            );

            eglReleaseThread_p();

            potatoBridge.eglContext = EGL_NO_CONTEXT;
            potatoBridge.eglDisplay = EGL_NO_DISPLAY;
            potatoBridge.eglSurface = EGL_NO_SURFACE;
        } break;

        //case RENDERER_VIRGL:
        case RENDERER_VK_ZINK: {
            // Nothing to do here
        } break;
    }
}


JNIEXPORT void JNICALL
Java_com_movtery_zalithlauncher_bridge_ZLBridge_setupBridgeWindow(
        JNIEnv* env,
        ABI_COMPAT jclass clazz,
        jobject surface
) {
    /*
     * 首个窗口由 pojavInit 应用交换间隔；
     * 此处处理窗口重建（旋转、分屏等）。
     *
     * 生产者状态会随新窗口重置，若不重新应用，
     * MC 不会再次发起交换间隔调用，帧率会退回锁定在屏幕刷新率。
     */
    bool windowRecreated =
            pojav_environ->pojavWindow != NULL;

    pojav_environ->pojavWindow =
            ANativeWindow_fromSurface(env, surface);

    if (windowRecreated &&
        pojav_environ->config_renderer != RENDERER_VULKAN) {

        if (lastSwapInterval >= 0) {
            setNativeWindowSwapInterval(
                    pojav_environ->pojavWindow,
                    lastSwapInterval
            );
        } else if (!getenv("POJAV_VSYNC_IN_ZINK")) {
            setNativeWindowSwapInterval(
                    pojav_environ->pojavWindow,
                    0
            );
        }
    }

    if (br_setup_window) {
        br_setup_window();
    }
}


JNIEXPORT void JNICALL
Java_com_movtery_zalithlauncher_bridge_ZLBridge_releaseBridgeWindow(
        ABI_COMPAT JNIEnv *env,
        ABI_COMPAT jclass clazz
) {
    ANativeWindow_release(
            pojav_environ->pojavWindow
    );
}


EXTERNAL_API void* pojavGetCurrentContext() {
    if (pojav_environ->config_renderer ==
        RENDERER_VIRGL) {

        return virglGetCurrentContext();
    }

    return br_get_current();
}


static void set_vulkan_ptr(void* ptr) {
    char envval[64];

    sprintf(
            envval,
            "%" PRIxPTR,
            (uintptr_t) ptr
    );

    setenv(
            "VULKAN_PTR",
            envval,
            1
    );
}


void load_vulkan() {
    const char* zinkPreferSystemDriver =
            getenv("POJAV_ZINK_PREFER_SYSTEM_DRIVER");

    int deviceApiLevel =
            android_get_device_api_level();

    if (zinkPreferSystemDriver == NULL &&
        deviceApiLevel >= 28) {

#ifdef ADRENO_POSSIBLE
        const char* native_dir =
                getenv("DRIVER_PATH");

        const char* cache_dir =
                getenv("TMPDIR");

        void* result =
                loadTurnipVulkan(
                        NULL,
                        native_dir,
                        cache_dir
                );

        if (result != NULL) {
            printf(
                    "AdrenoSupp: Loaded Turnip, loader address: %p\n",
                    result
            );

            set_vulkan_ptr(result);
            return;
        }
#endif
    }

    printf(
            "OSMDroid: Loading Vulkan regularly...\n"
    );

    void* vulkanPtr =
            dlopen(
                    "libvulkan.so",
                    RTLD_LAZY | RTLD_LOCAL
            );

    printf(
            "OSMDroid: Loaded Vulkan, ptr=%p\n",
            vulkanPtr
    );

    set_vulkan_ptr(vulkanPtr);
}


int pojavInitOpenGL() {
    const char *renderer =
            getenv("POJAV_RENDERER");

    if (!strncmp("opengles", renderer, 8)) {
        pojav_environ->config_renderer =
                RENDERER_GL4ES;

        if (!strcmp(
                renderer,
                "opengles3_desktopgl_zink_kopper"
        )) {
            load_vulkan();

            setenv(
                    "GALLIUM_DRIVER",
                    "zink",
                    1
            );

            setenv(
                    "MESA_ANDROID_NO_KMS_SWRAST",
                    "1",
                    1
            );
        }

        set_gl_bridge_tbl();
    }

    if (!strcmp(renderer, "custom_gallium")) {
        pojav_environ->config_renderer =
                RENDERER_VK_ZINK;

        load_vulkan();
        set_osm_bridge_tbl();
    }

    if (!strcmp(renderer, "vulkan_zink")) {
        pojav_environ->config_renderer =
                RENDERER_VK_ZINK;

        load_vulkan();

        setenv(
                "GALLIUM_DRIVER",
                "zink",
                1
        );

        set_osm_bridge_tbl();
    }

    if (!strcmp(renderer, "gallium_freedreno")) {
        pojav_environ->config_renderer =
                RENDERER_VK_ZINK;

        load_vulkan();

        setenv(
                "MESA_LOADER_DRIVER_OVERRIDE",
                "kgsl",
                1
        );

        setenv(
                "GALLIUM_DRIVER",
                "freedreno",
                1
        );

        set_osm_bridge_tbl();
    }

    if (!strcmp(renderer, "gallium_panfrost")) {
        pojav_environ->config_renderer =
                RENDERER_VK_ZINK;

        setenv(
                "GALLIUM_DRIVER",
                "panfrost",
                1
        );

        setenv(
                "MESA_DISK_CACHE_SINGLE_FILE",
                "1",
                1
        );

        set_osm_bridge_tbl();
    }

    if (!strcmp(renderer, "gallium_virgl")) {
        pojav_environ->config_renderer =
                RENDERER_VIRGL;

        setenv(
                "GALLIUM_DRIVER",
                "virpipe",
                1
        );

        setenv(
                "OSMESA_NO_FLUSH_FRONTBUFFER",
                "1",
                false
        );

        setenv(
                "MESA_GL_VERSION_OVERRIDE",
                "4.3",
                1
        );

        setenv(
                "MESA_GLSL_VERSION_OVERRIDE",
                "430",
                1
        );

        if (!strcmp(
                getenv("OSMESA_NO_FLUSH_FRONTBUFFER"),
                "1"
        )) {
            printf(
                    "VirGL: OSMesa buffer flush is DISABLED!\n"
            );
        }

        loadSymbolsVirGL();
        virglInit();

        return 0;
    }

    if (br_init()) {
        br_setup_window();
    }

    return 0;
}


/*
 * 获取当前线程的 JNIEnv。
 *
 * 未附着则 Attach。
 * 不 Detach，保留渲染线程的附着状态。
 */
static JNIEnv *get_attached_env_for_renderer(
        JavaVM *jvm
) {
    JNIEnv *jvm_env = NULL;

    jint env_result =
            (*jvm)->GetEnv(
                    jvm,
                    (void **) &jvm_env,
                    JNI_VERSION_1_4
            );

    if (env_result == JNI_EDETACHED) {
        env_result =
                (*jvm)->AttachCurrentThread(
                        jvm,
                        &jvm_env,
                        NULL
                );
    }

    if (env_result != JNI_OK) {
        printf(
                "get_attached_env failed: %i\n",
                env_result
        );

        return NULL;
    }

    return jvm_env;
}


EXTERNAL_API int pojavInit() {
    pojav_environ->glfwThreadVmEnv =
            get_attached_env_for_renderer(
                    pojav_environ->runtimeJavaVMPtr
            );

    if (pojav_environ->glfwThreadVmEnv == NULL) {
        printf(
                "Failed to attach Java-side JNIEnv to GLFW thread\n"
        );

        return 0;
    }

    /*
     * 桥初始化可能在其它线程上先行失败，
     * 此处于渲染线程兜底重试。
     */
    if (!ensureGlfwNativeBridgeInitialized(
            pojav_environ->glfwThreadVmEnv
    )) {
        printf(
                "pojavInit: GLFW bridge is not initialized\n"
        );

        return 0;
    }

    ANativeWindow_acquire(
            pojav_environ->pojavWindow
    );

    pojav_environ->savedWidth =
            ANativeWindow_getWidth(
                    pojav_environ->pojavWindow
            );

    pojav_environ->savedHeight =
            ANativeWindow_getHeight(
                    pojav_environ->pojavWindow
            );

    ANativeWindow_setBuffersGeometry(
            pojav_environ->pojavWindow,
            pojav_environ->savedWidth,
            pojav_environ->savedHeight,
            AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM
    );

    updateMonitorSize(
            pojav_environ->savedWidth,
            pojav_environ->savedHeight
    );

    /*
     * Start with a clean frame-statistics window.
     */
    resetFrameStatsInternal();

    pojavInitOpenGL();

    /*
     * 垂直同步开关关闭时主动切入异步模式，
     * 解除帧率对屏幕刷新率的锁定；
     * 开启时交由 MC 的交换间隔调用决定。
     */
    if (pojav_environ->config_renderer != RENDERER_VULKAN &&
        !getenv("POJAV_VSYNC_IN_ZINK")) {

        setNativeWindowSwapInterval(
                pojav_environ->pojavWindow,
                0
        );
    }

    return 1;
}


EXTERNAL_API void pojavSetWindowHint(
        int hint,
        int value
) {
    if (hint != GLFW_CLIENT_API) {
        return;
    }

    switch (value) {
        case GLFW_NO_API:
            pojav_environ->config_renderer =
                    RENDERER_VULKAN;

            /*
             * Nothing to do:
             * initialization is handled in Java-side
             */
            break;

        case GLFW_OPENGL_API: {
            const char *renderer =
                    getenv("POJAV_RENDERER");

            if (!strncmp("opengles", renderer, 8)) {
                pojav_environ->config_renderer =
                        RENDERER_GL4ES;

            } else if (!strcmp(
                    renderer,
                    "vulkan_zink"
            )) {
                pojav_environ->config_renderer =
                        RENDERER_VK_ZINK;
            }

            /*
             * Nothing to do:
             * initialization is called in pojavCreateContext
             */
            break;
        }

        default:
            printf(
                    "GLFW: Unimplemented API 0x%x\n",
                    value
            );

            abort();
    }
}


/*
 * ============================================================
 * Frame submission
 * ============================================================
 *
 * calculateFPS() is called immediately before the native
 * renderer performs the actual swap.
 *
 * This makes frame time represent the interval between
 * successive Minecraft swap requests.
 */
EXTERNAL_API void pojavSwapBuffers() {
    calculateFPS();

    if (pojav_environ->config_renderer ==
            RENDERER_VK_ZINK ||
        pojav_environ->config_renderer ==
            RENDERER_GL4ES) {

        br_swap_buffers();
    }

    if (pojav_environ->config_renderer ==
            RENDERER_VIRGL) {

        virglSwapBuffers();
    }
}


EXTERNAL_API void pojavMakeCurrent(
        void* window
) {
    if (pojav_environ->config_renderer ==
            RENDERER_VK_ZINK ||
        pojav_environ->config_renderer ==
            RENDERER_GL4ES) {

        br_make_current(
                (basic_render_window_t*) window
        );
    }

    if (pojav_environ->config_renderer ==
            RENDERER_VIRGL) {

        virglMakeCurrent(window);
    }
}


EXTERNAL_API void* pojavCreateContext(
        void* contextSrc
) {
    if (pojav_environ->config_renderer ==
            RENDERER_VULKAN) {

        return (void *)
                pojav_environ->pojavWindow;
    }

    if (pojav_environ->config_renderer ==
            RENDERER_VIRGL) {

        return virglCreateContext(
                contextSrc
        );
    }

    return br_init_context(
            (basic_render_window_t*) contextSrc
    );
}


void* maybe_load_vulkan() {
    /*
     * We use the env var because
     * 1. it's easier to do that
     * 2. it won't break if something will try to load
     *    vulkan and osmesa simultaneously
     */
    if (getenv("VULKAN_PTR") == NULL) {
        load_vulkan();
    }

    return (void*)
            strtoul(
                    getenv("VULKAN_PTR"),
                    NULL,
                    0x10
            );
}


/*
 * ============================================================
 * FPS calculation
 * ============================================================
 */

void calculateFPS() {
    const uint64_t nowNs =
            getMonotonicTimeNs();

    if (nowNs != 0) {
        pthread_mutex_lock(
                &frameStatsMutex
        );

        /*
         * First frame only establishes the timestamp.
         */
        if (lastFrameTimestampNs == 0) {
            lastFrameTimestampNs = nowNs;
        } else {
            uint64_t deltaNs =
                    nowNs - lastFrameTimestampNs;

            lastFrameTimestampNs = nowNs;

            /*
             * A pause longer than the statistics window
             * means the old frame interval is not meaningful.
             *
             * This prevents opening a paused game from
             * producing a fake 1 FPS result.
             */
            if (deltaNs > 0 &&
                deltaNs <= FRAME_STATS_WINDOW_NS) {

                frameTimeMs =
                        (double) deltaNs /
                        1000000.0;

                int currentFps =
                        (int) (
                                (
                                        1000000000.0 /
                                        (double) deltaNs
                                ) + 0.5
                        );

                if (currentFps < 1) {
                    currentFps = 1;
                }

                fps = currentFps;

                pruneFrameStatsLocked(
                        nowNs
                );

                addFrameStatsSampleLocked(
                        nowNs,
                        currentFps
                );

                recomputeFrameStatsLocked();

            } else if (
                    deltaNs > FRAME_STATS_WINDOW_NS
            ) {
                /*
                 * Long pause / background / surface pause.
                 */
                frameStatsHistoryStart = 0;
                frameStatsHistoryCount = 0;

                fps = 0;
                minFps = 0;
                averageFps = 0;
                maxFps = 0;

                frameTimeMs = 0.0;
            }
        }

        pthread_mutex_unlock(
                &frameStatsMutex
        );
    }

    /*
     * Existing graphic-output callback logic.
     */
    if (!pojav_environ->hasGraphicOutput &&
        pojav_environ->dalvikJavaVMPtr &&
        pojav_environ->bridgeClazz &&
        pojav_environ->method_onGraphicOutput) {

        pojav_environ->hasGraphicOutput = true;

        JNIEnv *dalvikEnv = NULL;

        jboolean detachedBefore =
                (*pojav_environ->dalvikJavaVMPtr)
                ->GetEnv(
                        pojav_environ->dalvikJavaVMPtr,
                        (void **) &dalvikEnv,
                        JNI_VERSION_1_4
                ) == JNI_EDETACHED;

        if (detachedBefore) {
            (*pojav_environ->dalvikJavaVMPtr)
            ->AttachCurrentThread(
                    pojav_environ->dalvikJavaVMPtr,
                    &dalvikEnv,
                    NULL
            );
        }

        if (dalvikEnv != NULL) {
            (*dalvikEnv)
            ->CallStaticVoidMethod(
                    dalvikEnv,
                    pojav_environ->bridgeClazz,
                    pojav_environ->method_onGraphicOutput
            );

            if (detachedBefore) {
                (*pojav_environ->dalvikJavaVMPtr)
                ->DetachCurrentThread(
                        pojav_environ->dalvikJavaVMPtr
                );
            }
        }
    }
}


/*
 * LWJGL Vulkan FPS callback.
 *
 * Keep this for render paths where the Java-side Vulkan
 * implementation explicitly calls VK.updateFps().
 */
EXTERNAL_API JNIEXPORT void JNICALL
Java_org_lwjgl_vulkan_VK_updateFps(
        ABI_COMPAT JNIEnv *env,
        ABI_COMPAT jclass thiz
) {
    calculateFPS();
}


/*
 * ============================================================
 * CallbackBridge JNI
 * ============================================================
 */

/*
 * Current instantaneous FPS.
 */
EXTERNAL_API JNIEXPORT jint JNICALL
Java_org_lwjgl_glfw_CallbackBridge_getCurrentFps(
        JNIEnv *env,
        jclass clazz
) {
    jint result;

    pthread_mutex_lock(
            &frameStatsMutex
    );

    result = (jint) fps;

    pthread_mutex_unlock(
            &frameStatsMutex
    );

    return result;
}


/*
 * Complete performance statistics.
 *
 * Java:
 *
 *     long[] values = CallbackBridge.getFrameStats();
 *
 * values:
 *
 *     [0] current FPS
 *     [1] minimum FPS
 *     [2] average FPS
 *     [3] maximum FPS
 *     [4] frame time in microseconds
 */
EXTERNAL_API JNIEXPORT jlongArray JNICALL
Java_org_lwjgl_glfw_CallbackBridge_getFrameStats(
        JNIEnv *env,
        jclass clazz
) {
    jlong values[5];

    pthread_mutex_lock(
            &frameStatsMutex
    );

    values[0] = (jlong) fps;
    values[1] = (jlong) minFps;
    values[2] = (jlong) averageFps;
    values[3] = (jlong) maxFps;

    /*
     * Java expects microseconds here.
     *
     * frameTimeMs -> microseconds
     */
    values[4] =
            (jlong) (
                    frameTimeMs * 1000.0 +
                    0.5
            );

    pthread_mutex_unlock(
            &frameStatsMutex
    );

    jlongArray result =
            (*env)->NewLongArray(
                    env,
                    5
            );

    if (result == NULL) {
        return NULL;
    }

    (*env)->SetLongArrayRegion(
            env,
            result,
            0,
            5,
            values
    );

    return result;
}


/*
 * Reset statistics from Java.
 */
EXTERNAL_API JNIEXPORT void JNICALL
Java_org_lwjgl_glfw_CallbackBridge_resetFrameStats(
        JNIEnv *env,
        jclass clazz
) {
    resetFrameStatsInternal();
}


/*
 * Vulkan driver handle.
 */
EXTERNAL_API JNIEXPORT jlong JNICALL
Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(
        ABI_COMPAT JNIEnv *env,
        ABI_COMPAT jclass thiz
) {
    printf(
            "EGLBridge: LWJGL-side Vulkan loader requested the Vulkan handle\n"
    );

    return (jlong)
            maybe_load_vulkan();
}


/*
 * ============================================================
 * Swap interval
 * ============================================================
 */

EXTERNAL_API void pojavSwapInterval(
        int interval
) {
    lastSwapInterval = interval;

    if (pojav_environ->config_renderer ==
            RENDERER_VK_ZINK ||
        pojav_environ->config_renderer ==
            RENDERER_GL4ES) {

        br_swap_interval(interval);
    }

    if (pojav_environ->config_renderer ==
            RENDERER_VIRGL) {

        virglSwapInterval(interval);
    }
}
