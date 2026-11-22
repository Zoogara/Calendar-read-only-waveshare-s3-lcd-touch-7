#pragma once
/* Just what ui_radar.c's loader uses - the renderer never runs the loader (see host_radar.c). */
#include <stddef.h>
typedef struct cJSON { char *valuestring; } cJSON;
cJSON *cJSON_ParseWithLength(const char *value, size_t length);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string);
int cJSON_IsString(const cJSON *item);
void cJSON_Delete(cJSON *item);
