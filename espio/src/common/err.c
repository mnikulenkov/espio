/**
 * @file err.c
 * @brief Implementation of unified error handling utilities.
 */

#include "espio/err.h"
#include <string.h>

#ifdef __has_include
    #if __has_include("pthread.h")
        #include "pthread.h"
        #define USE_PTHREAD_TLS 1
    #endif
#endif

#ifndef USE_PTHREAD_TLS
    #define USE_PTHREAD_TLS 0
#endif

/*============================================================================
 * Thread-Local Storage Implementation
 *============================================================================*/

#if USE_PTHREAD_TLS
// Use pthread thread-local storage
static pthread_key_t s_err_tls_key;
static pthread_once_t s_err_tls_once = PTHREAD_ONCE_INIT;

static void espio_err_tls_init(void) {
    pthread_key_create(&s_err_tls_key, NULL);
}

static espio_err_context_t* espio_err_get_tls(void) {
    pthread_once(&s_err_tls_once, espio_err_tls_init);
    espio_err_context_t* ctx = (espio_err_context_t*)pthread_getspecific(s_err_tls_key);
    if (!ctx) {
        // Allocate on first use (leaked intentionally - per-thread lifetime)
        ctx = (espio_err_context_t*)malloc(sizeof(espio_err_context_t));
        if (ctx) {
            memset(ctx, 0, sizeof(espio_err_context_t));
            ctx->code = ESPIO_OK;
            ctx->component = ESPIO_COMPONENT_NONE;
            pthread_setspecific(s_err_tls_key, ctx);
        }
    }
    return ctx;
}

#else
// Use simple static storage for single-threaded or FreeRTOS scenarios
// For FreeRTOS, each task could have its own storage using pvTaskGetThreadLocalStoragePointer
// This is a simplified implementation using static storage

// ESP-IDF/FreeRTOS alternative using task local storage pointers
#ifdef __has_include
    #if __has_include("freertos/FreeRTOS.h") && __has_include("freertos/task.h")
        #include "freertos/FreeRTOS.h"
        #include "freertos/task.h"
        #define USE_FREERTOS_TLS 1
    #endif
#endif

#ifndef USE_FREERTOS_TLS
    #define USE_FREERTOS_TLS 0
#endif

#if USE_FREERTOS_TLS
// Use FreeRTOS thread-local storage pointers
// Index 0 is used for error context
#define ERR_TLS_INDEX 0

static espio_err_context_t* espio_err_get_tls(void) {
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    espio_err_context_t* ctx = (espio_err_context_t*)pvTaskGetThreadLocalStoragePointer(task, ERR_TLS_INDEX);
    if (!ctx) {
        // Allocate on first use
        ctx = (espio_err_context_t*)pvPortMalloc(sizeof(espio_err_context_t));
        if (ctx) {
            memset(ctx, 0, sizeof(espio_err_context_t));
            ctx->code = ESPIO_OK;
            ctx->component = ESPIO_COMPONENT_NONE;
            vTaskSetThreadLocalStoragePointer(task, ERR_TLS_INDEX, ctx);
        }
    }
    return ctx;
}

#else
// Fallback: single static context (not thread-safe)
// This is suitable for single-threaded applications or when TLS is unavailable
static espio_err_context_t s_err_context = {
    .code = ESPIO_OK,
    .component = ESPIO_COMPONENT_NONE,
    .native_code = 0,
    .message = NULL,
    .file = NULL,
    .line = 0
};

static espio_err_context_t* espio_err_get_tls(void) {
    return &s_err_context;
}
#endif // USE_FREERTOS_TLS
#endif // USE_PTHREAD_TLS

/*============================================================================
 * Error Name/Description Lookup
 *============================================================================*/

/**
 * @brief Entry for error name/description lookup table.
 */
typedef struct {
    int32_t code;
    const char* name;
    const char* desc;
} espio_err_lookup_entry_t;

/**
 * @brief Lookup table for common error codes.
 */
static const espio_err_lookup_entry_t s_common_errors[] = {
    { ESPIO_OK,               "ESPIO_OK",               "Success" },
    { ESPIO_INVALID_ARG,      "ESPIO_INVALID_ARG",      "Invalid argument" },
    { ESPIO_NO_MEM,           "ESPIO_NO_MEM",           "Out of memory" },
    { ESPIO_TIMEOUT,          "ESPIO_TIMEOUT",          "Timeout" },
    { ESPIO_BUSY,             "ESPIO_BUSY",             "Resource busy" },
    { ESPIO_NOT_FOUND,        "ESPIO_NOT_FOUND",        "Not found" },
    { ESPIO_IO,               "ESPIO_IO",               "I/O error" },
    { ESPIO_UNSUPPORTED,      "ESPIO_UNSUPPORTED",      "Not supported" },
    { ESPIO_INTERNAL,         "ESPIO_INTERNAL",         "Internal error" },
    { ESPIO_NOT_INITIALIZED,  "ESPIO_NOT_INITIALIZED",  "Not initialized" },
    { ESPIO_ALREADY_INITIALIZED, "ESPIO_ALREADY_INITIALIZED", "Already initialized" },
    { ESPIO_PERMISSION,       "ESPIO_PERMISSION",       "Permission denied" },
    { ESPIO_OVERFLOW,         "ESPIO_OVERFLOW",         "Buffer overflow" },
    { ESPIO_UNDERFLOW,        "ESPIO_UNDERFLOW",        "Buffer underflow" },
    { ESPIO_CANCELLED,        "ESPIO_CANCELLED",        "Operation cancelled" },
    { ESPIO_NOT_READY,        "ESPIO_NOT_READY",        "Not ready" },
};

#define COMMON_ERROR_COUNT (sizeof(s_common_errors) / sizeof(s_common_errors[0]))

/**
 * @brief Component name lookup table.
 */
static const struct {
    espio_component_t component;
    const char* name;
} s_component_names[] = {
    { ESPIO_COMPONENT_NONE,    "none" },
    { ESPIO_COMPONENT_COMMON,  "common" },
    { ESPIO_COMPONENT_AUDIO,   "audio" },
    { ESPIO_COMPONENT_BUTTON,  "button" },
    { ESPIO_COMPONENT_DISPLAY, "display" },
    { ESPIO_COMPONENT_SD,      "sd" },
};

#define COMPONENT_NAME_COUNT (sizeof(s_component_names) / sizeof(s_component_names[0]))

/*============================================================================
 * Public API Implementation
 *============================================================================*/

const char* espio_err_common_name(int32_t code) {
    for (size_t i = 0; i < COMMON_ERROR_COUNT; i++) {
        if (s_common_errors[i].code == code) {
            return s_common_errors[i].name;
        }
    }
    return "UNKNOWN";
}

const char* espio_err_common_desc(int32_t code) {
    for (size_t i = 0; i < COMMON_ERROR_COUNT; i++) {
        if (s_common_errors[i].code == code) {
            return s_common_errors[i].desc;
        }
    }
    return NULL;
}

const char* espio_err_component_name(espio_component_t component) {
    for (size_t i = 0; i < COMPONENT_NAME_COUNT; i++) {
        if (s_component_names[i].component == component) {
            return s_component_names[i].name;
        }
    }
    return "unknown";
}

/*============================================================================
 * Thread-Local Storage API Implementation
 *============================================================================*/

void espio_err_set_last(const espio_err_context_t* ctx) {
    if (!ctx) {
        return;
    }
    espio_err_context_t* tls = espio_err_get_tls();
    if (tls) {
        *tls = *ctx;
    }
}

const espio_err_context_t* espio_err_get_last(void) {
    espio_err_context_t* tls = espio_err_get_tls();
    if (tls) {
        return tls;
    }
    // Return a static OK context if TLS is unavailable
    static const espio_err_context_t ok_ctx = {
        .code = ESPIO_OK,
        .component = ESPIO_COMPONENT_NONE,
        .native_code = 0,
        .message = NULL,
        .file = NULL,
        .line = 0
    };
    return &ok_ctx;
}

void espio_err_clear_last(void) {
    espio_err_context_t* tls = espio_err_get_tls();
    if (tls) {
        tls->code = ESPIO_OK;
        tls->component = ESPIO_COMPONENT_NONE;
        tls->native_code = 0;
        tls->message = NULL;
        tls->file = NULL;
        tls->line = 0;
    }
}

espio_err_context_t espio_err_take_last(void) {
    espio_err_context_t* tls = espio_err_get_tls();
    if (tls) {
        espio_err_context_t result = *tls;
        espio_err_clear_last();
        return result;
    }
    // Return OK context if TLS is unavailable
    espio_err_context_t ok_ctx = {
        .code = ESPIO_OK,
        .component = ESPIO_COMPONENT_NONE,
        .native_code = 0,
        .message = NULL,
        .file = NULL,
        .line = 0
    };
    return ok_ctx;
}
