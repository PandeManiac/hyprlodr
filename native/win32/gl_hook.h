/* In-process OpenGL capture used to reverse-engineer the title renderer.
 * Research instrument: it observes, it does not change what the game draws. */
#ifndef HYPRLODR_GL_HOOK_H
#define HYPRLODR_GL_HOOK_H

/* Patch the executable's own slots from DllMain, before GLFW creates a context. */
int hyprlodr_gl_hook_install_early(const char *log_path);
/* Patch every loaded module; idempotent, called from the bootstrap thread. */
int hyprlodr_gl_hook_install(const char *log_path);
/* Directory for the frame dump (hyprlodr-frame.bmp); optional. */
void hyprlodr_gl_hook_set_game_dir(const char *game_dir);

const char *hyprlodr_gl_hook_status(void);
const char *hyprlodr_gl_hook_detail(void);

#endif /* HYPRLODR_GL_HOOK_H */
