// vyrn-focus: tells Vyrn which window is active, so the AI only presses keys in the game window.
//
// Speaks the wlr-foreign-toplevel-management protocol (labwc, sway, Wayfire, ...). Prints one line per
// change on stdout (see docs/dev/protocol.md section 6):
//
//   ready                                     the initial state has been printed
//   active<TAB><id><TAB><app_id><TAB><title>  this window is active (or its title/app_id changed)
//   none                                      no window is active
//   closed<TAB><id>                           this window closed
//   error<TAB><message>                       cannot work here; exits with status 2
//
// It exits when stdin reaches end-of-file (the Vyrn worker keeps a pipe open), on SIGTERM,
// or after the first state with --once.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

struct toplevel {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  unsigned id;
  char *title, *app_id;          // committed on "done"
  char *p_title, *p_app_id;      // pending
  int activated, p_activated, p_state_set;
  struct toplevel *next;
};

static struct zwlr_foreign_toplevel_manager_v1 *manager;
static struct toplevel *toplevels;
static unsigned next_id = 1;

// What we last printed, so we only print changes.
static int reported;             // 0 = nothing printed yet
static unsigned rep_id;          // 0 = "none"
static char *rep_title, *rep_app_id;

static void out(const char *line) {
  if (fputs(line, stdout) < 0 || fflush(stdout) != 0) exit(0);  // Vyrn went away
}

// Tabs and newlines would break the line format: turn them into spaces.
static char *clean(const char *s) {
  char *c = strdup(s ? s : "");
  if (!c) exit(1);
  for (char *p = c; *p; ++p)
    if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
  return c;
}

static int same(const char *a, const char *b) { return strcmp(a ? a : "", b ? b : "") == 0; }

static void report_if_changed(int force) {
  struct toplevel *active = NULL;
  for (struct toplevel *t = toplevels; t; t = t->next)
    if (t->activated) { active = t; break; }  // newest first; normally only one is active

  if (!active) {
    if (!force && reported && rep_id == 0) return;
    out("none\n");
    rep_id = 0;
  } else {
    if (!force && reported && rep_id == active->id && same(rep_title, active->title) &&
        same(rep_app_id, active->app_id))
      return;
    char *app = clean(active->app_id), *title = clean(active->title);
    size_t n = strlen(app) + strlen(title) + 64;
    char *line = malloc(n);
    if (!line) exit(1);
    snprintf(line, n, "active\t%u\t%s\t%s\n", active->id, app, title);
    out(line);
    free(line); free(app); free(title);
    rep_id = active->id;
    free(rep_title); free(rep_app_id);
    rep_title = strdup(active->title ? active->title : "");
    rep_app_id = strdup(active->app_id ? active->app_id : "");
  }
  reported = 1;
}

// ---- toplevel handle events ----------------------------------------------------------------------
static void h_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title) {
  struct toplevel *t = data;
  free(t->p_title);
  t->p_title = strdup(title ? title : "");
}

static void h_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *app_id) {
  struct toplevel *t = data;
  free(t->p_app_id);
  t->p_app_id = strdup(app_id ? app_id : "");
}

static void h_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {}
static void h_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {}

static void h_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *state) {
  struct toplevel *t = data;
  int activated = 0;
  uint32_t *s;
  wl_array_for_each(s, state) {
    if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) activated = 1;
  }
  t->p_activated = activated;
  t->p_state_set = 1;
}

static void h_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
  struct toplevel *t = data;
  if (t->p_title) { free(t->title); t->title = t->p_title; t->p_title = NULL; }
  if (t->p_app_id) { free(t->app_id); t->app_id = t->p_app_id; t->p_app_id = NULL; }
  if (t->p_state_set) { t->activated = t->p_activated; t->p_state_set = 0; }
}

static void h_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
  struct toplevel *t = data;
  char line[64];
  snprintf(line, sizeof line, "closed\t%u\n", t->id);
  out(line);
  for (struct toplevel **pp = &toplevels; *pp; pp = &(*pp)->next) {
    if (*pp == t) { *pp = t->next; break; }
  }
  zwlr_foreign_toplevel_handle_v1_destroy(t->handle);
  free(t->title); free(t->app_id); free(t->p_title); free(t->p_app_id);
  free(t);
}

static void h_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                     struct zwlr_foreign_toplevel_handle_v1 *parent) {}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title = h_title, .app_id = h_app_id, .output_enter = h_output_enter, .output_leave = h_output_leave,
    .state = h_state, .done = h_done, .closed = h_closed, .parent = h_parent,
};

// ---- manager events -------------------------------------------------------------------------------
static void m_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *m,
                       struct zwlr_foreign_toplevel_handle_v1 *handle) {
  struct toplevel *t = calloc(1, sizeof *t);
  if (!t) exit(1);
  t->handle = handle;
  t->id = next_id++;
  t->next = toplevels;
  toplevels = t;
  zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, t);
}

static void m_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *m) {
  out("error\tthe desktop stopped sharing window focus\n");
  exit(2);
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = m_toplevel, .finished = m_finished,
};

// ---- registry --------------------------------------------------------------------------------------
static void r_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
  if (strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0 && !manager) {
    uint32_t v = version < 3 ? version : 3;
    manager = wl_registry_bind(reg, name, &zwlr_foreign_toplevel_manager_v1_interface, v);
    zwlr_foreign_toplevel_manager_v1_add_listener(manager, &manager_listener, NULL);
  }
}
static void r_global_remove(void *data, struct wl_registry *reg, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {.global = r_global, .global_remove = r_global_remove};

int main(int argc, char **argv) {
  int once = argc > 1 && strcmp(argv[1], "--once") == 0;
  signal(SIGPIPE, SIG_IGN);

  struct wl_display *display = wl_display_connect(NULL);
  if (!display) {
    out("error\tcannot connect to the Wayland desktop (is WAYLAND_DISPLAY set?)\n");
    return 2;
  }
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  if (!manager) {
    out("error\tthis desktop does not share which window is active (wlr-foreign-toplevel missing)\n");
    return 2;
  }
  wl_display_roundtrip(display);  // the existing windows
  wl_display_roundtrip(display);  // their titles and states
  report_if_changed(1);
  out("ready\n");
  if (once) return 0;

  int wl_fd = wl_display_get_fd(display);
  for (;;) {
    while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
    wl_display_flush(display);
    struct pollfd fds[2] = {{.fd = wl_fd, .events = POLLIN}, {.fd = STDIN_FILENO, .events = POLLIN}};
    if (poll(fds, 2, -1) < 0) {
      wl_display_cancel_read(display);
      if (errno == EINTR) continue;
      return 1;
    }
    if (fds[0].revents & POLLIN) {
      if (wl_display_read_events(display) < 0) {
        out("error\tlost the connection to the desktop\n");
        return 2;
      }
    } else {
      wl_display_cancel_read(display);
    }
    if (fds[0].revents & (POLLERR | POLLHUP)) {
      out("error\tlost the connection to the desktop\n");
      return 2;
    }
    if (wl_display_dispatch_pending(display) < 0) {
      out("error\tlost the connection to the desktop\n");
      return 2;
    }
    report_if_changed(0);  // after the whole batch, so a focus switch never shows a fake "none" in between
    if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      char buf[256];
      ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
      if (n <= 0) return 0;  // Vyrn closed our stdin: time to go
    }
  }
}
