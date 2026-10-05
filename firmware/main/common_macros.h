#pragma once

#include <stdlib.h>

// Runs `action` and aborts when `condition` is false.
#define ABORT_APP_ON_FAILURE(condition, action) \
    do                                          \
    {                                           \
        if (!(condition))                       \
        {                                       \
            action;                             \
            abort();                            \
        }                                       \
    } while (0)
