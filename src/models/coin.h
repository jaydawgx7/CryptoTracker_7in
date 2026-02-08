#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char id[32];
    char symbol[8];
    char name[32];
    double price;
    double change_1h;
    double change_24h;
    double change_7d;
    double high_24h;
    double low_24h;
    double market_cap;
    double volume_24h;
    double holdings;
    double alert_low;
    double alert_high;
    bool pinned;
} coin_t;
