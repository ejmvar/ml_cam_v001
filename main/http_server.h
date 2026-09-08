#pragma once

#include "esp_err.h"

/** Start the local-LAN HTTP checkpoint server after Wi-Fi has an IP address. */
esp_err_t ml_http_server_start(void);
