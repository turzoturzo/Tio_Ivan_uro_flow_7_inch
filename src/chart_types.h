#pragma once
#include <stdint.h>

// Shared between Session (producer) and Display (consumer).
// Kept in a standalone header to avoid circular include chains.

struct ChartSample {
    uint32_t t_ms;
    float    weight_g;
};

static const int CHART_BUF_SIZE = 240;
