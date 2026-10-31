#ifndef HYPRLODR_LOADER_H
#define HYPRLODR_LOADER_H

#include "hyprlodr/plugin.h"

typedef struct hyprlodr_loader hyprlodr_loader;

hyprlodr_loader *hyprlodr_loader_create(void);
int hyprlodr_loader_run(hyprlodr_loader *loader, const char *plugin_directory, const char *event_log_path);
void hyprlodr_loader_shutdown(hyprlodr_loader *loader);
void hyprlodr_loader_destroy(hyprlodr_loader *loader);
int hyprlodr_loader_had_error(const hyprlodr_loader *loader);

#endif
