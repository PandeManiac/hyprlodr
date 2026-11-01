/* Mod-gated title overlay: shows the loader's own title art above the game window
 * while at least one mod is loaded. See title_overlay.c. */
#ifndef HYPRLODR_TITLE_OVERLAY_H
#define HYPRLODR_TITLE_OVERLAY_H

typedef struct {
    char image_path[1024];
    char log_path[1024];
    unsigned duration_ms; /* 0 = until the player presses a key or clicks */
} title_overlay_args;

/* Start the overlay thread. Safe to call once; later calls do nothing. */
int hyprlodr_title_overlay_start(const char *image_path, const char *log_path, unsigned duration_ms);

/* "starting", "shown", "hidden", "image_invalid", "no_window", ... */
const char *hyprlodr_title_overlay_status(void);

#endif /* HYPRLODR_TITLE_OVERLAY_H */
