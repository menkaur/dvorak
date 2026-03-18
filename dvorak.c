/*
 * Copyright 2018 Thomas Bocek
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/*
 * Why is this tool useful?
 * ========================
 *
 * Since I type with the "Dvorak" keyboard layout, the shortcuts such
 * as ctrl-c, ctrl-x, or ctrl-v are not comfortable anymore and one of them
 * require two hands to press.
 *
 * Furthermore, applications such as Intellij and Eclipse have their
 * shortcuts, which I'm used to. So for these shortcuts I prefer "Querty".
 * Since there is no way to configure this, I had to intercept the
 * keys and remap the keys from "Dvorak" to "Querty" once CTRL, ALT,
 * WIN or any of those combinations are pressed.
 *
 * With X.org I was relying on the wonderful tool from Kenton Varda,
 * which I modified a bit, to make it work when Numlock is active. Other
 * than that, it worked as expected.
 *
 * And then came Wayland. XGrabKey() works partially with some application
 * but not with others (e.g., gedit is not working). Since XGrabKey() is
 * an X.org function with some support in Wayland, I was looking for a more
 * stable solution. After a quick look to the repo
 * https://github.com/kentonv/dvorak-qwerty I saw that Kenton added a systemtap
 * script to implement the mapping. This scared me a bit to follow that path, so
 * I implemented an other solution based on /dev/uinput. The idea is to read
 * /dev/input, grab keys with EVIOCGRAB, create a virtual device that can emit
 * the keys and pass the keys from /dev/input to /dev/uinput. If CTRL/ALT/WIN is
 * pressed it will map the keys back to "Qwerty".
 *
 * Installation
 * ===========
 *
 * make dvorak
 * //make sure your user belongs to the group "input" -> ls -la /dev/input
 * //this also applies for /dev/uinput ->
 * https://github.com/tuomasjjrasanen/python-uinput/blob/master/udev-rules/40-uinput.rules
 * //start it in startup applications
 *
 * Signal-based mode switching
 * ===========================
 *
 * SIGUSR1 -> "on"  -> Dvorak mapping enabled (original behavior)
 * SIGUSR2 -> "off" -> Passthrough mode (no remapping at all)
 *
 * Once a signal has set the mode, only another signal can change it
 * (the Left-Alt toggle is suppressed while under signal control).
 *
 * Use -p <pidfile> to write a PID file for easy signaling of daemon processes.
 *
 * Example:
 *   kill -SIGUSR2 <pid>   # disable mapping (passthrough)
 *   kill -SIGUSR1 <pid>   # re-enable mapping
 *   pkill -SIGUSR2 dvorak # disable all instances
 *
 * If a signal arrives while a shortcut is in progress (modifier keys held),
 * the mode change is deferred until all modifier and shortcut keys are
 * released.
 *
 * Exit codes:
 *   0 — not a keyboard / skipped (wrapper should try next candidate)
 *   1 — configuration / setup error
 *   2 — device disappeared at runtime (should restart)
 *
 * Related Links
 * =============
 * I used the following sites for inspiration:
 * https://www.kernel.org/doc/html/v4.12/input/uinput.html
 * https://www.linuxquestions.org/questions/programming-9/uinput-any-complete-example-4175524044/
 * https://stackoverflow.com/questions/20943322/accessing-keys-from-linux-input-device
 * https://gist.github.com/toinsson/7e9fdd3c908b3c3d3cd635321d19d44d
 *
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/uinput.h>
#include <poll.h> // CHANGE: needed for poll() — bounded wait when deadline active
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h> // CHANGE: needed for clock_gettime / struct timespec
#include <unistd.h>

#define MAX_LENGTH 8
#define KEY_COUNT (KEY_MAX + 1)

// Exit codes
#define EXIT_DEVICE_GONE 2

// Pending mode change values
#define MODE_NO_CHANGE 0
#define MODE_ON 1
#define MODE_OFF 2

// CHANGE: Maximum seconds to wait for modifiers/keys to release before
// force-applying a deferred mode change.  Prevents indefinite deferral
// when mod_state or array_qwerty_counter gets stuck non-zero (e.g. from
// duplicate key-press events or a dropped release event).
#define PENDING_MODE_TIMEOUT_SEC 1

static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t pending_mode = MODE_NO_CHANGE;

static char *pidfile_path = NULL;

// Bitmap tracking every key currently held on the virtual output device.
static unsigned char keys_pressed[KEY_COUNT / 8 + 1];

/* ── Single global flag controlling all output ───────────────────── */
static bool quiet_mode = false;

/* All output (except usage()) goes through this function.
 * Set quiet_mode = true (--daemon / -D) to suppress everything,
 * eliminating any I/O that could stall the event loop.             */
__attribute__((format(printf, 2, 3))) static void
log_msg(FILE *stream, const char *fmt, ...) {
  if (quiet_mode)
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stream, fmt, ap);
  va_end(ap);
}
/* ─────────────────────────────────────────────────────────────────── */

static void cleanup_pidfile(void) {
  if (pidfile_path != NULL) {
    unlink(pidfile_path);
  }
}

static void sig_handler(int sig) {
  (void)sig;
  keep_running = 0;
}

static void sigusr1_handler(int sig) {
  (void)sig;
  pending_mode = MODE_ON;
}

static void sigusr2_handler(int sig) {
  (void)sig;
  pending_mode = MODE_OFF;
}

static int modifier_bit(int key) {
  switch (key) {
  case KEY_LEFTCTRL:
    return 1;
  case KEY_RIGHTCTRL:
    return 2;
  case KEY_LEFTALT:
    return 4;
  case KEY_LEFTMETA:
    return 8;
  case KEY_CAPSLOCK:
    return 16;
  default:
    return 0;
  }
}

static int qwerty2dvorak(int key) {
  switch (key) {
  case KEY_MINUS:
    return KEY_APOSTROPHE;
  case KEY_EQUAL:
    return KEY_RIGHTBRACE;
  case KEY_Q:
    return KEY_X;
  case KEY_W:
    return KEY_COMMA;
  case KEY_E:
    return KEY_D;
  case KEY_R:
    return KEY_O;
  case KEY_T:
    return KEY_K;
  case KEY_Y:
    return KEY_T;
  case KEY_U:
    return KEY_F;
  case KEY_I:
    return KEY_G;
  case KEY_O:
    return KEY_S;
  case KEY_P:
    return KEY_R;
  case KEY_LEFTBRACE:
    return KEY_MINUS;
  case KEY_RIGHTBRACE:
    return KEY_EQUAL;
  case KEY_A:
    return KEY_A;
  case KEY_S:
    return KEY_SEMICOLON;
  case KEY_D:
    return KEY_H;
  case KEY_F:
    return KEY_Y;
  case KEY_G:
    return KEY_U;
  case KEY_H:
    return KEY_J;
  case KEY_J:
    return KEY_C;
  case KEY_K:
    return KEY_V;
  case KEY_L:
    return KEY_P;
  case KEY_SEMICOLON:
    return KEY_Z;
  case KEY_APOSTROPHE:
    return KEY_Q;
  case KEY_Z:
    return KEY_SLASH;
  case KEY_X:
    return KEY_B;
  case KEY_C:
    return KEY_I;
  case KEY_V:
    return KEY_DOT;
  case KEY_B:
    return KEY_N;
  case KEY_N:
    return KEY_L;
  case KEY_M:
    return KEY_M;
  case KEY_COMMA:
    return KEY_W;
  case KEY_DOT:
    return KEY_E;
  case KEY_SLASH:
    return KEY_LEFTBRACE;
  default:
    return key;
  }
}

static void keys_pressed_set(int code) {
  if (code >= 0 && code < KEY_COUNT) {
    keys_pressed[code / 8] |= (1U << (code % 8));
  }
}

static void keys_pressed_clear(int code) {
  if (code >= 0 && code < KEY_COUNT) {
    keys_pressed[code / 8] &= ~(1U << (code % 8));
  }
}

static bool keys_pressed_test(int code) {
  if (code >= 0 && code < KEY_COUNT) {
    return (keys_pressed[code / 8] & (1U << (code % 8))) != 0;
  }
  return false;
}

// FIX: EINTR retry loop around write().  Without SA_RESTART on
// SIGUSR1/SIGUSR2, a signal can interrupt write() mid-call.
// Without the retry, the key event is silently dropped, which
// can cause stuck keys.
static bool emit(int fd, int type, int code, int value, struct timeval time) {
  struct input_event ev = {0};
  ev.type = type;
  ev.code = code;
  ev.value = value;
  ev.time = time;

  if (type == EV_KEY) {
    if (value == 1 || value == 2) {
      keys_pressed_set(code);
    } else if (value == 0) {
      keys_pressed_clear(code);
    }
  }

  ssize_t n;
  do {
    n = write(fd, &ev, sizeof(ev));
  } while (n < 0 && errno == EINTR);

  if (n == (ssize_t)sizeof(ev))
    return true;
  if (n < 0) {
    log_msg(stderr, "emit write failed: %s\n", strerror(errno));
  } else {
    log_msg(stderr, "emit short write: %zd/%zu\n", n, sizeof(ev));
  }
  return false;
}

static void release_all_keys(int fdo) {
  struct timeval now = {0};
  gettimeofday(&now, NULL);

  for (int code = 0; code < KEY_COUNT; code++) {
    if (keys_pressed_test(code)) {
      struct input_event ev = {0};
      ev.type = EV_KEY;
      ev.code = code;
      ev.value = 0;
      ev.time = now;
      (void)write(fdo, &ev, sizeof(ev));
    }
  }
  struct input_event syn = {0};
  syn.type = EV_SYN;
  syn.code = SYN_REPORT;
  syn.time = now;
  (void)write(fdo, &syn, sizeof(syn));

  memset(keys_pressed, 0, sizeof(keys_pressed));
}

// CHANGE: Rebuild mod_state from actual hardware key state.
// Called after every mode transition so that mod_state is accurate
// even if modifiers were pressed/released while in passthrough mode
// (where mod_state is not tracked by the event-processing logic).
// Uses EVIOCGKEY to read the physical device's key bitmap directly.
static void sync_mod_state_from_hardware(int fdi, int *mod_state_out,
                                         bool noCapsLockAsModifier) {
  unsigned char hw_keys[KEY_COUNT / 8 + 1];
  memset(hw_keys, 0, sizeof(hw_keys));
  if (ioctl(fdi, EVIOCGKEY(sizeof(hw_keys)), hw_keys) < 0)
    return; // best-effort: leave mod_state as-is on failure

  int new_mod = 0;
  static const int mod_keys[] = {KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT,
                                 KEY_LEFTMETA, KEY_CAPSLOCK};
  for (size_t i = 0; i < sizeof(mod_keys) / sizeof(mod_keys[0]); i++) {
    int k = mod_keys[i];
    if (hw_keys[k / 8] & (1U << (k % 8))) {
      int bit = modifier_bit(k);
      if (noCapsLockAsModifier && bit == modifier_bit(KEY_CAPSLOCK))
        continue;
      new_mod |= bit;
    }
  }
  *mod_state_out = new_mod;
}

static bool has_event_type(const unsigned int array_bit_ev[], int event_type) {
  return (array_bit_ev[event_type / 32] & (1U << (event_type % 32))) != 0;
}

static bool setup_event_type(int fdi, int fdo, unsigned long event_type,
                             int max_val, const unsigned int array_bit[]) {
  for (int i = 0; i < max_val; i++) {
    if (!(array_bit[i / 32] & (1U << (i % 32)))) {
      continue;
    }

    switch (event_type) {
    case UI_SET_EVBIT:
      if (ioctl(fdo, UI_SET_EVBIT, i) < 0) {
        log_msg(stderr, "Cannot set EV bit %d: %s\n", i, strerror(errno));
        return false;
      }
      break;
    case UI_SET_KEYBIT:
      if (ioctl(fdo, UI_SET_KEYBIT, i) < 0) {
        log_msg(stderr, "Cannot set KEY bit %d: %s\n", i, strerror(errno));
        return false;
      }
      break;
    case UI_SET_RELBIT:
      if (ioctl(fdo, UI_SET_RELBIT, i) < 0) {
        log_msg(stderr, "Cannot set REL bit %d: %s\n", i, strerror(errno));
        return false;
      }
      break;
    case UI_SET_ABSBIT: {
      struct uinput_abs_setup abs_setup = {0};
      abs_setup.code = i;
      if (ioctl(fdi, EVIOCGABS(i), &abs_setup.absinfo) < 0) {
        log_msg(stderr, "Failed to get ABS info for axis %d: %s\n", i,
                strerror(errno));
        continue;
      }
      if (ioctl(fdo, UI_ABS_SETUP, &abs_setup) < 0) {
        log_msg(stderr, "Failed to setup ABS axis %d: %s\n", i,
                strerror(errno));
        continue;
      }
      if (ioctl(fdo, UI_SET_ABSBIT, i) < 0) {
        log_msg(stderr, "Cannot set ABS bit %d: %s\n", i, strerror(errno));
        return false;
      }
      break;
    }
    case UI_SET_MSCBIT:
      if (ioctl(fdo, UI_SET_MSCBIT, i) < 0) {
        log_msg(stderr, "Cannot set MSC bit %d: %s\n", i, strerror(errno));
        return false;
      }
      break;
    }
  }
  return true;
}

// usage() always prints directly — it runs on bad args, before
// daemon mode matters, and the user needs to see the help text.
static void usage(const char *path) {
  const char *basename = strrchr(path, '/');
  basename = basename ? basename + 1 : path;

  fprintf(stderr, "usage: %s [OPTION]\n", basename);
  fprintf(stderr, "  -d /dev/input/by-id/...\t"
                  "Specifies which device should be captured.\n");
  fprintf(stderr,
          "  -m STRING\t\t"
          "Match only the STRING with the USB device name. \n"
          "\t\t\tSTRING can contain multiple words, separated by space.\n");
  fprintf(stderr,
          "  -t\t\t\t"
          "Disable layout toggle feature (press Left-Alt 3 times to switch "
          "layout).\n");
  fprintf(stderr, "  -c\t\t\t"
                  "Disable caps lock as a modifier.\n");
  fprintf(stderr, "  -p FILE\t\t"
                  "Write PID to FILE (useful for daemon mode).\n");
  fprintf(stderr, "  -D, --daemon\t\t"
                  "Suppress all output (for daemon/service use).\n\n");
  fprintf(stderr, "Signals:\n");
  fprintf(stderr, "  SIGUSR1\t\t"
                  "Enable Dvorak mapping (on).\n");
  fprintf(stderr, "  SIGUSR2\t\t"
                  "Disable mapping / passthrough (off).\n\n");
  fprintf(stderr,
          "example: %s -d "
          "/dev/input/by-id/usb-Logitech_USB_Receiver-if02-event-kbd -m "
          "\"k750 k350\"\n",
          basename);
}

static bool write_pidfile(const char *path) {
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    log_msg(stderr, "Error: Cannot write PID file [%s]: %s\n", path,
            strerror(errno));
    return false;
  }
  fprintf(f, "%d\n", getpid()); // writing to PID file, not logging
  fclose(f);
  return true;
}

static void shutdown_virtual_device(int fdo) {
  if (fdo < 0)
    return;
  release_all_keys(fdo);
  ioctl(fdo, UI_DEV_DESTROY);
  close(fdo);
}

int main(int argc, char *argv[]) {
  struct sigaction sa_term = {0};
  sa_term.sa_handler = sig_handler;
  sigemptyset(&sa_term.sa_mask);
  sa_term.sa_flags = 0;
  sigaction(SIGTERM, &sa_term, NULL);
  sigaction(SIGINT, &sa_term, NULL);

  // FIX: No SA_RESTART — read() returns EINTR on signal so the event
  // loop's "continue" brings us to the pending_mode check immediately.
  // Cross-mask USR1 ↔ USR2 so they cannot interrupt each other's handler.
  struct sigaction sa_usr1 = {0};
  sa_usr1.sa_handler = sigusr1_handler;
  sigemptyset(&sa_usr1.sa_mask);
  sigaddset(&sa_usr1.sa_mask, SIGUSR2);
  sa_usr1.sa_flags = 0;
  sigaction(SIGUSR1, &sa_usr1, NULL);

  struct sigaction sa_usr2 = {0};
  sa_usr2.sa_handler = sigusr2_handler;
  sigemptyset(&sa_usr2.sa_mask);
  sigaddset(&sa_usr2.sa_mask, SIGUSR1);
  sa_usr2.sa_flags = 0;
  sigaction(SIGUSR2, &sa_usr2, NULL);

  signal(SIGPIPE, SIG_IGN);

  static const struct option long_options[] = {
      {"daemon", no_argument, NULL, 'D'}, {NULL, 0, NULL, 0}};

  int opt;
  char *device = NULL, *match = NULL;
  bool noToggle = false, noCapsLockAsModifier = false;
  while ((opt = getopt_long(argc, argv, "d:m:p:tcD", long_options, NULL)) !=
         -1) {
    switch (opt) {
    case 'd':
      device = optarg;
      break;
    case 'm':
      match = optarg;
      break;
    case 'p':
      pidfile_path = optarg;
      break;
    case 't':
      noToggle = true;
      break;
    case 'c':
      noCapsLockAsModifier = true;
      break;
    case 'D':
      quiet_mode = true;
      break;
    default:
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (device == NULL) {
    usage(argv[0]);
    fprintf(stderr, "Error: Input device not specified.\n");
    return EXIT_FAILURE;
  }

  // Write PID file if requested
  if (pidfile_path != NULL) {
    if (!write_pidfile(pidfile_path)) {
      return EXIT_FAILURE;
    }
    atexit(cleanup_pidfile);
  }

  int fdi = open(device, O_RDONLY);
  if (fdi < 0) {
    log_msg(stderr, "Error: Failed to open device [%s]: %s.\n", device,
            strerror(errno));
    return (errno == ENOENT || errno == ENODEV) ? EXIT_DEVICE_GONE
                                                : EXIT_FAILURE;
  }

  char keyboard_name[UINPUT_MAX_NAME_SIZE] = "Unknown";
  int ret_val =
      ioctl(fdi, EVIOCGNAME(sizeof(keyboard_name) - 1), keyboard_name);
  if (ret_val < 0) {
    log_msg(stderr, "Error: Unable to retrieve device name for [%s]: %s.\n",
            device, strerror(errno));
    close(fdi);
    return EXIT_FAILURE;
  }

  static const char virtual_name[] = "Virtual Dvorak Keyboard";
  struct uinput_setup usetup = {
      .id = {.bustype = BUS_USB, .vendor = 0x1111, .product = 0x2222}};
  _Static_assert(sizeof(virtual_name) <= sizeof(usetup.name),
                 "virtual device name too long");
  memcpy(usetup.name, virtual_name, sizeof(virtual_name));

  if (strcmp(keyboard_name, virtual_name) == 0) {
    log_msg(stdout,
            "Info: Skipping mapping for the device we just created: %s.\n",
            keyboard_name);
    close(fdi);
    return EXIT_SUCCESS;
  }

  if (match != NULL) {
    char *match_copy = strdup(match);
    if (match_copy == NULL) {
      log_msg(stderr, "Error: strdup failed\n");
      close(fdi);
      return EXIT_FAILURE;
    }
    bool found = false;
    char *token = strtok(match_copy, " ");
    while (token != NULL) {
      if (strcasestr(keyboard_name, token) != NULL) {
        log_msg(stdout, "Info: Found matching input: [%s] for device [%s].\n",
                keyboard_name, device);
        found = true;
        break;
      }
      token = strtok(NULL, " ");
    }
    free(match_copy);
    if (!found) {
      log_msg(stderr,
              "Error: Device [%s] does not match any specified "
              "keywords.\n",
              keyboard_name);
      close(fdi);
      return EXIT_FAILURE;
    }
  }

  // Read capabilities
  unsigned int array_bit_ev[EV_MAX / 32 + 1] = {0},
                                          array_bit_key[KEY_MAX / 32 + 1] = {0},
                                          array_bit_rel[REL_MAX / 32 + 1] = {0},
                                          array_bit_abs[ABS_MAX / 32 + 1] = {0},
                                          array_bit_msc[MSC_MAX / 32 + 1] = {0};

  ret_val = ioctl(fdi, EVIOCGBIT(0, sizeof(array_bit_ev)), &array_bit_ev);
  if (ret_val < 0) {
    log_msg(stderr,
            "Error: Failed to retrieve event capabilities for [%s]: %s.\n",
            device, strerror(errno));
    close(fdi);
    return EXIT_FAILURE;
  }

  if (has_event_type(array_bit_ev, EV_KEY)) {
    ret_val =
        ioctl(fdi, EVIOCGBIT(EV_KEY, sizeof(array_bit_key)), &array_bit_key);
    if (ret_val < 0) {
      log_msg(stderr, "Error: Failed to retrieve EV_KEY capabilities: %s.\n",
              strerror(errno));
      close(fdi);
      return EXIT_FAILURE;
    }
  }

  if (has_event_type(array_bit_ev, EV_REL)) {
    ret_val =
        ioctl(fdi, EVIOCGBIT(EV_REL, sizeof(array_bit_rel)), &array_bit_rel);
    if (ret_val < 0) {
      log_msg(stderr, "Error: Failed to retrieve EV_REL capabilities: %s.\n",
              strerror(errno));
      close(fdi);
      return EXIT_FAILURE;
    }
  }

  if (has_event_type(array_bit_ev, EV_ABS)) {
    ret_val =
        ioctl(fdi, EVIOCGBIT(EV_ABS, sizeof(array_bit_abs)), &array_bit_abs);
    if (ret_val < 0) {
      log_msg(stderr, "Error: Failed to retrieve EV_ABS capabilities: %s.\n",
              strerror(errno));
      close(fdi);
      return EXIT_FAILURE;
    }
  }

  if (has_event_type(array_bit_ev, EV_MSC)) {
    ret_val =
        ioctl(fdi, EVIOCGBIT(EV_MSC, sizeof(array_bit_msc)), &array_bit_msc);
    if (ret_val < 0) {
      log_msg(stderr, "Error: Failed to retrieve EV_MSC capabilities: %s.\n",
              strerror(errno));
      close(fdi);
      return EXIT_FAILURE;
    }
  }

  // FIX: Clear EV_REP from the capabilities we copy to the virtual device.
  // The physical keyboard has EV_REP, which tells the kernel to run its
  // own auto-repeat timer generating value=2 events.  Since we already
  // *forward* value=2 events from the physical keyboard, enabling EV_REP
  // on the virtual device creates a second, independent stream of repeats
  // — doubling every held key.  Clearing the bit prevents this.
  array_bit_ev[EV_REP / 32] &= ~(1U << (EV_REP % 32));

  // Check we are a keyboard — exit 0 (not a keyboard) so wrapper tries next
  if (!(array_bit_key[KEY_X / 32] & (1U << (KEY_X % 32))) ||
      !(array_bit_key[KEY_C / 32] & (1U << (KEY_C % 32))) ||
      !(array_bit_key[KEY_V / 32] & (1U << (KEY_V % 32)))) {
    log_msg(stdout, "Info: Device [%s] is not recognized as a keyboard.\n",
            device);
    close(fdi);
    return EXIT_SUCCESS;
  }

  int fdo = open("/dev/uinput", O_WRONLY);
  if (fdo < 0) {
    log_msg(stderr, "Error: Failed to open /dev/uinput: %s.\n",
            strerror(errno));
    close(fdi);
    return EXIT_FAILURE;
  }

  if (ioctl(fdo, UI_DEV_SETUP, &usetup) < 0) {
    log_msg(stderr, "Error: Failed to configure virtual device: %s.\n",
            strerror(errno));
    close(fdo);
    close(fdi);
    return EXIT_FAILURE;
  }

  if (!setup_event_type(fdi, fdo, UI_SET_EVBIT, EV_MAX + 1, array_bit_ev) ||
      !setup_event_type(fdi, fdo, UI_SET_KEYBIT, KEY_COUNT, array_bit_key) ||
      !setup_event_type(fdi, fdo, UI_SET_RELBIT, REL_MAX + 1, array_bit_rel) ||
      !setup_event_type(fdi, fdo, UI_SET_ABSBIT, ABS_MAX + 1, array_bit_abs) ||
      !setup_event_type(fdi, fdo, UI_SET_MSCBIT, MSC_MAX + 1, array_bit_msc)) {
    log_msg(stderr, "Cannot setup event types for device [%s]: %s.\n", device,
            strerror(errno));
    close(fdo);
    close(fdi);
    return EXIT_FAILURE;
  }

  if (ioctl(fdo, UI_DEV_CREATE) < 0) {
    log_msg(stderr, "Cannot create device: %s.\n", strerror(errno));
    close(fdo);
    close(fdi);
    return EXIT_FAILURE;
  }

  // Wait for virtual device to settle
  usleep(200000);

  // Wait until all physical keys are released before grabbing
  {
    unsigned char key_state[KEY_COUNT / 8 + 1];
    for (int attempt = 0; attempt < 50; attempt++) {
      memset(key_state, 0, sizeof(key_state));
      if (ioctl(fdi, EVIOCGKEY(sizeof(key_state)), key_state) < 0) {
        log_msg(stderr, "Device gone during key-wait: %s\n", strerror(errno));
        ioctl(fdo, UI_DEV_DESTROY);
        close(fdo);
        close(fdi);
        return EXIT_DEVICE_GONE;
      }
      bool all_released = true;
      for (size_t i = 0; i < sizeof(key_state); i++) {
        if (key_state[i] != 0) {
          all_released = false;
          break;
        }
      }
      if (all_released)
        break;
      usleep(50000);
    }
  }

  if (ioctl(fdi, EVIOCGRAB, 1) < 0) {
    log_msg(stderr, "Cannot grab device: %s.\n", strerror(errno));
    ioctl(fdo, UI_DEV_DESTROY);
    close(fdo);
    close(fdi);
    return (errno == ENODEV) ? EXIT_DEVICE_GONE : EXIT_FAILURE;
  }

  struct input_event ev = {0};
  int l_alt = 0, mod_state = 0, array_qwerty_counter = 0;
  bool disable_mapping = false;
  bool signal_controlled = false;
  unsigned int array_qwerty[MAX_LENGTH] = {0};
  int exit_code = EXIT_SUCCESS;

  // CHANGE: Deadline tracking for deferred mode changes.
  bool pending_mode_has_deadline = false;
  struct timespec pending_mode_deadline = {0};

  log_msg(stderr, "Starting event loop with keyboard: [%s] for device [%s].\n",
          keyboard_name, device);
  log_msg(stderr,
          "PID: %d (send SIGUSR1 to enable mapping, SIGUSR2 to passthrough)\n",
          getpid());

  while (keep_running) {
    // ── CHANGE: Process pending mode change BEFORE read() ────────────
    // Signal → read()/poll() returns EINTR → continue → here.
    //
    // Three paths:
    //   1. No keys/modifiers held       → apply immediately.
    //   2. Keys held, no deadline yet    → set deadline, keep deferring.
    //   3. Keys held, deadline expired   → force release + apply.
    //
    // After every mode application we sync mod_state from hardware
    // (EVIOCGKEY) so it is accurate even if modifiers were pressed
    // or released while in passthrough mode.
    if (pending_mode != MODE_NO_CHANGE) {
      bool force_apply = false;

      if (mod_state == 0 && array_qwerty_counter == 0) {
        // Path 1: safe to apply now.
        force_apply = true;
      } else if (!pending_mode_has_deadline) {
        // Path 2: first deferral — start the clock.
        clock_gettime(CLOCK_MONOTONIC, &pending_mode_deadline);
        pending_mode_deadline.tv_sec += PENDING_MODE_TIMEOUT_SEC;
        pending_mode_has_deadline = true;
        log_msg(stderr,
                "Signal: mode change deferred (mod_state=0x%x, "
                "qwerty_counter=%d), deadline in %ds\n",
                mod_state, array_qwerty_counter, PENDING_MODE_TIMEOUT_SEC);
      } else {
        // Path 3: deadline already set — check expiry.
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > pending_mode_deadline.tv_sec ||
            (now.tv_sec == pending_mode_deadline.tv_sec &&
             now.tv_nsec >= pending_mode_deadline.tv_nsec)) {
          log_msg(stderr,
                  "Signal: deadline expired, force-applying "
                  "(mod_state=0x%x, qwerty_counter=%d)\n",
                  mod_state, array_qwerty_counter);
          release_all_keys(fdo);
          mod_state = 0;
          array_qwerty_counter = 0;
          memset(array_qwerty, 0, sizeof(array_qwerty));
          force_apply = true;
        }
      }

      if (force_apply) {
        if (pending_mode == MODE_ON) {
          disable_mapping = false;
          signal_controlled = true;
          l_alt = 0;
          log_msg(stderr, "Signal: Dvorak mapping enabled (on)\n");
        } else if (pending_mode == MODE_OFF) {
          disable_mapping = true;
          signal_controlled = true;
          l_alt = 0;
          log_msg(stderr, "Signal: passthrough mode (off)\n");
        }
        pending_mode = MODE_NO_CHANGE;
        pending_mode_has_deadline = false;

        // CHANGE: Sync mod_state from physical key state so it is
        // correct regardless of what happened during passthrough.
        sync_mod_state_from_hardware(fdi, &mod_state, noCapsLockAsModifier);
      }
    }
    // ── End pending mode change processing ───────────────────────────

    // ── CHANGE: Bounded wait when a deadline is active ───────────────
    // Without this, read() blocks indefinitely and the deadline check
    // at the top of the loop never fires if no key events arrive (e.g.
    // the user walked away from the keyboard while mod_state is stuck).
    // poll() returns after at most ms_remaining milliseconds, at which
    // point we loop back to the deadline check.
    if (pending_mode != MODE_NO_CHANGE && pending_mode_has_deadline) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long ms_remaining =
          (pending_mode_deadline.tv_sec - now.tv_sec) * 1000 +
          (pending_mode_deadline.tv_nsec - now.tv_nsec) / 1000000;
      if (ms_remaining < 0)
        ms_remaining = 0;

      struct pollfd pfd = {.fd = fdi, .events = POLLIN};
      int pret = poll(&pfd, 1, (int)ms_remaining);
      if (pret == 0) {
        // Timeout — no events before deadline.  Loop back so
        // Path 3 above will force-apply.
        continue;
      }
      if (pret < 0) {
        if (errno == EINTR)
          continue; // signal arrived — check pending_mode
        log_msg(stderr, "poll error on [%s]: %s\n", device, strerror(errno));
        exit_code = EXIT_DEVICE_GONE;
        break;
      }
      // pret > 0: data ready, fall through to read()
      if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        log_msg(stderr, "poll: device error on [%s] (revents=0x%x)\n", device,
                (unsigned)pfd.revents);
        exit_code = EXIT_DEVICE_GONE;
        break;
      }
    }
    // ── End bounded wait ─────────────────────────────────────────────

    ssize_t n = read(fdi, &ev, sizeof ev);
    if (n == (ssize_t)-1) {
      if (errno == EINTR)
        continue;
      log_msg(stderr, "read error on [%s]: %s (errno=%d)\n", device,
              strerror(errno), errno);
      exit_code = EXIT_DEVICE_GONE;
      break;
    } else if (n == 0) {
      log_msg(stderr, "EOF on [%s] — device disconnected\n", device);
      exit_code = EXIT_DEVICE_GONE;
      break;
    } else if (n != sizeof ev) {
      log_msg(stderr, "short read on [%s]: %zd bytes\n", device, n);
      exit_code = EXIT_DEVICE_GONE;
      break;
    }

    // Left-alt toggle: suppressed when under signal control.
    if (!signal_controlled && !noToggle && ev.type == EV_KEY &&
        ev.code == KEY_LEFTALT) {
      if (ev.value == 1 && ++l_alt >= 3) {
        disable_mapping = !disable_mapping;
        l_alt = 0;
        log_msg(stdout, "mapping is set to [%s]\n",
                !disable_mapping ? "true" : "false");
      }
    } else if (ev.type == EV_KEY) {
      l_alt = 0;
    }

    if (!disable_mapping && ev.type == EV_KEY) {
      int mod_current = modifier_bit(ev.code);

      if (noCapsLockAsModifier && mod_current == modifier_bit(KEY_CAPSLOCK)) {
        mod_current = 0;
      }

      if (mod_current > 0) {
        if (ev.value != 0) {
          mod_state |= mod_current;
        } else {
          mod_state &= ~mod_current;
        }
      }

      int qwerty_code = qwerty2dvorak(ev.code);
      if (ev.code != qwerty_code) {
        if (ev.value == 1) {
          if (mod_state > 0) {
            if (array_qwerty_counter == MAX_LENGTH) {
              log_msg(stderr,
                      "warning, too many keys pressed: %d. 0x%04x "
                      "(%d), arr:%d\n",
                      MAX_LENGTH, (int)ev.code, (int)ev.code,
                      array_qwerty_counter);
            } else {
              array_qwerty[array_qwerty_counter++] = (unsigned int)qwerty_code;
              emit(fdo, ev.type, qwerty_code, ev.value, ev.time);
            }
          } else {
            emit(fdo, ev.type, ev.code, ev.value, ev.time);
          }
        } else if (ev.value == 2) {
          bool is_in_array = false;
          for (int i = 0; i < array_qwerty_counter; i++) {
            if (array_qwerty[i] == (unsigned int)qwerty_code) {
              is_in_array = true;
              break;
            }
          }
          if (is_in_array) {
            emit(fdo, ev.type, qwerty_code, ev.value, ev.time);
          } else {
            emit(fdo, ev.type, ev.code, ev.value, ev.time);
          }
        } else if (ev.value == 0) {
          bool need_emit = false;
          for (int i = 0; i < array_qwerty_counter; i++) {
            if (array_qwerty[i] == (unsigned int)qwerty_code) {
              array_qwerty[i] = 0;
              need_emit = true;
              break;
            }
          }
          if (need_emit) {
            int last_nonzero = -1;
            for (int i = 0; i < array_qwerty_counter; i++) {
              if (array_qwerty[i] != 0) {
                last_nonzero = i;
              }
            }
            array_qwerty_counter = last_nonzero + 1;
            emit(fdo, ev.type, qwerty_code, ev.value, ev.time);
          } else {
            emit(fdo, ev.type, ev.code, ev.value, ev.time);
          }
        } else {
          emit(fdo, ev.type, ev.code, ev.value, ev.time);
        }
      } else {
        emit(fdo, ev.type, ev.code, ev.value, ev.time);
      }
    } else {
      emit(fdo, ev.type, ev.code, ev.value, ev.time);
    }
  }

  // Clean shutdown
  log_msg(stderr, "Shutting down: releasing grab and cleaning up.\n");
  ioctl(fdi, EVIOCGRAB, 0);
  shutdown_virtual_device(fdo);
  close(fdi);

  return exit_code;
}
