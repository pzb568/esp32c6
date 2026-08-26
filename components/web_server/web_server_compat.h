#pragma once

/* ESP-IDF 5.3 compatibility declarations used by the existing web server. */
#include <stdint.h>
#include "esp_timer.h"
#include "ir_control.h"

/* web_server.c defines this helper later in the translation unit. */
static void ws_add_client(int fd);
