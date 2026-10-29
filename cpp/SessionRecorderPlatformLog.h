#pragma once

#if defined(__ANDROID__)
#include <android/log.h>
#define LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "SessionRecorderTAG", __VA_ARGS__)
#else
#include <cstdio>
#define LOG_ERROR(...)                         \
    do                                        \
    {                                         \
        std::fprintf(stderr, "[SessionRecorder] "); \
        std::fprintf(stderr, __VA_ARGS__);     \
        std::fputc('\n', stderr);              \
    } while (false)
#endif
