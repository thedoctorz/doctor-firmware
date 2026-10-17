#pragma once

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnalyzerLog AnalyzerLog;

AnalyzerLog* analyzer_log_alloc(void);

void analyzer_log_free(AnalyzerLog* instance);

bool analyzer_log_open(AnalyzerLog* instance, uint32_t frequency);

void analyzer_log_printf(AnalyzerLog* instance, const char* format, ...)
    _ATTRIBUTE((__format__(__printf__, 2, 3)));

void analyzer_log_newline(AnalyzerLog* instance);

void analyzer_log_hex(AnalyzerLog* instance, const char* label, const uint8_t* data, size_t len);

void analyzer_log_flush(AnalyzerLog* instance);

void analyzer_log_close(AnalyzerLog* instance);

bool analyzer_log_is_open(const AnalyzerLog* instance);

const char* analyzer_log_path(const AnalyzerLog* instance);

#ifdef __cplusplus
}
#endif
