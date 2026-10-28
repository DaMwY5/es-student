#pragma once

#include "stdio-text-protocol/stdio-text-protocol.h"
#include "telemetry-task/telemetry-task.h"

void api_handle(const command_t *command);
void api_print_telemetry(const telemetry_record_t *record);
