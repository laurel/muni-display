#pragma once
#include <cstdio>
#define ESP_LOGW(tag, ...) (fprintf(stderr, "W %s: ", tag), fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n"))
#define ESP_LOGI(tag, ...) (fprintf(stderr, "I %s: ", tag), fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n"))
