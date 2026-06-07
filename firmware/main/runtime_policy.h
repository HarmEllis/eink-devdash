#pragma once

#include <stdbool.h>

bool clock_should_apply(const char *iso, bool stale);
bool api_url_is_relay(const char *url);
