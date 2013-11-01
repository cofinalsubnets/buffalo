#include <xcb/xcb.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NUM_REGISTERS (1<<8)
#define VERSION "0"
#define LENGTH(x) (sizeof(x) / sizeof(*x))
#define XCB_EVENT_RESPONSE_TYPE_MASK   (0x7f) /* see xcb_event.h */
#define EV_TYPE(e)     (e->response_type & XCB_EVENT_RESPONSE_TYPE_MASK)
#define ERR(...) {fprintf(stderr, __VA_ARGS__); running = 0; status = EXIT_FAILURE; return;}
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define XCB_ERROR 0
#define DISPATCH(handler, ev, key) for(int i=0; i<LENGTH(handler); i++) if (handler[i].name == key) handler[i].handle(ev)

/* types */
enum Atom {
  COPY,
  PASTE,
  PRIMARY,
  TEXT,
  STRING,
  UTF8_STRING,
  BUFFALO_DAEMON,
  WINDOW,
  ATOM,
  TARGETS
};

struct Internable {
  int index;
  const char *name;
};

typedef struct {
  int name;
  int needs_x;
  void (*run)();
} Command;

typedef struct {
  int name;
  void (*handle)(xcb_generic_event_t*);
} EventHandler;

typedef struct {
  xcb_atom_t atom;
  char *data;
} Register;

/* end types */


/* function prototypes */

/* setup & teardown */
void init_x_protocol();
void init_window();
xcb_window_t daemon_window();
void init_buffalo_atoms();
void buffalo_daemon();

/* event dispatchers & handlers */
void handle_event(xcb_generic_event_t*);
void handle_message(xcb_generic_event_t*);
void handle_selection_notify(xcb_generic_event_t*);
void handle_selection_request(xcb_generic_event_t*);
void handle_selection_clear(xcb_generic_event_t*);
void handle_copy(xcb_generic_event_t*);
void handle_paste(xcb_generic_event_t*);
void handle_arg(int);
void cli_copy();
void cli_paste();
void cli_version();

/* utility functions */
Register *find_register(uint32_t);
void set_register(Register*, char*);
void send_message(xcb_atom_t, uint32_t);

/* end function prototypes */

/* globals */

Register registers[NUM_REGISTERS],
         *active_register = NULL;

int running = 1,
    status  = EXIT_SUCCESS;

#define I(x) {x, #x}
struct Internable internable_atoms[] = {
  I(COPY),
  I(PASTE),
  I(PRIMARY),
  I(TEXT),
  I(STRING),
  I(UTF8_STRING),
  I(BUFFALO_DAEMON),
  I(WINDOW),
  I(ATOM),
  I(TARGETS)
};
#undef I

xcb_connection_t *X = NULL;
xcb_window_t win;
xcb_atom_t atoms[LENGTH(internable_atoms)];

/* end globals */

void send_message(xcb_atom_t type, uint32_t d) {
  xcb_window_t dw = daemon_window();

  if (!dw) ERR("ERROR: Can't find buffalo daemon.\n");

  xcb_client_message_event_t msg = {
    .response_type = XCB_CLIENT_MESSAGE,
    .type = type,
    .format = 32,
    .window = win,
    .data = { .data32 = { [0] = d % NUM_REGISTERS } }
  };

  xcb_send_event(X, 0, dw, XCB_EVENT_MASK_NO_EVENT, (char*) &msg);
  xcb_flush(X);
}

void handle_event(xcb_generic_event_t *ev) {
  static EventHandler handler[] = {
    {XCB_CLIENT_MESSAGE,    handle_message},
    {XCB_SELECTION_NOTIFY,  handle_selection_notify},
    {XCB_SELECTION_REQUEST, handle_selection_request},
    {XCB_SELECTION_CLEAR,   handle_selection_clear}
  };

  DISPATCH(handler, ev, EV_TYPE(ev));
}

void handle_message(xcb_generic_event_t *ev) {
  static EventHandler handler[] = {
    {COPY,  handle_copy},
    {PASTE, handle_paste}
  };

  DISPATCH(handler, ev, ((xcb_client_message_event_t*)ev)->type);
}

void handle_copy(xcb_generic_event_t *gev) {
  xcb_client_message_event_t *ev = (xcb_client_message_event_t*) gev;
  xcb_convert_selection(X, win, atoms[PRIMARY], atoms[UTF8_STRING],
      registers[ev->data.data32[0]].atom, XCB_CURRENT_TIME);
}

void handle_paste(xcb_generic_event_t *gev) {
  uint32_t reg = ((xcb_client_message_event_t*) gev)->data.data32[0]; // really?
  if (registers[reg].data) {
    active_register = registers + reg;
    xcb_set_selection_owner(X, win, atoms[PRIMARY], XCB_CURRENT_TIME);
  }
}

void handle_selection_notify(xcb_generic_event_t *gev) {
  xcb_selection_notify_event_t *ev = (xcb_selection_notify_event_t*) gev;
  Register *reg;

  if (ev->property != XCB_NONE) {
    if ((reg = find_register(ev->property))) {

      xcb_get_property_reply_t *reply = xcb_get_property_reply(X,
          xcb_get_property(X, 0, win, ev->property,
            XCB_GET_PROPERTY_TYPE_ANY, 0, UINT_MAX),
          NULL);

      if (reply) {
        int l = xcb_get_property_value_length(reply);
        char *data = malloc(sizeof(char) * l + 1);
        memcpy(data, xcb_get_property_value(reply), l);
        set_register(reg, data);
        free(reply);
      }
    }

    xcb_delete_property(X, win, ev->property);
  }
}

void handle_selection_clear(xcb_generic_event_t *ev) {
  xcb_atom_t sel = ((xcb_selection_clear_event_t*) ev)->selection;
  if (sel == atoms[BUFFALO_DAEMON]) running = 0;
  else active_register = NULL;
}

void handle_selection_request(xcb_generic_event_t *gev) {
  xcb_selection_request_event_t *ev = (xcb_selection_request_event_t*) gev;

  /* the selection, target, time, and property fields should be the same as in
   * the request, except when the requested conversion can't be made (then the
   * response property should be None) or when the request property is None
   * (then the response property should be the response target). */
  xcb_selection_notify_event_t response = {
    .response_type = XCB_SELECTION_NOTIFY,
    .requestor = ev->requestor,
    .selection = ev->selection,
    .property  = ev->property == XCB_NONE ? ev->target : ev->property,
    .target    = ev->target,
    .time      = ev->time
  };

  if (active_register) {
    if (ev->target == atoms[TEXT] ||
        ev->target == atoms[STRING] ||
        ev->target == atoms[UTF8_STRING]) {

      xcb_change_property(X, XCB_PROP_MODE_REPLACE, response.requestor,
          response.property, response.target, 8,
          strlen(active_register->data), active_register->data);
    } else if (ev->target == atoms[TARGETS]) {

      xcb_atom_t ok_targs[] = { atoms[TEXT], atoms[STRING], atoms[UTF8_STRING] };
      xcb_change_property(X, XCB_PROP_MODE_REPLACE, ev->requestor,
          atoms[TARGETS], atoms[ATOM], 32, LENGTH(ok_targs), ok_targs);
    } else response.property = XCB_NONE;
  } else response.property = XCB_NONE;

  xcb_send_event(X, 0, response.requestor,XCB_EVENT_MASK_NO_EVENT, (char*) &response);
}

void set_register(Register *reg, char *data) {
  if (reg->data) free(reg->data);
  reg->data = data;
}

Register *find_register(uint32_t key) {
  for (int i=0; i<NUM_REGISTERS; i++)
    if (registers[i].atom == key) return registers + i;
  return NULL;
}

/* return the daemon's window if a daemon is running,
 * else return 0 */
xcb_window_t daemon_window() {
  xcb_window_t ret = 0;
  xcb_get_selection_owner_reply_t *reply = xcb_get_selection_owner_reply(
      X, xcb_get_selection_owner(X, atoms[BUFFALO_DAEMON]), NULL);

  if (reply) {
    ret = reply->owner;
    free(reply);
  }

  return ret;
}

/* various X-related setup necessary to do anything */
void init_x_protocol() {
  if (!(X = xcb_connect(NULL, NULL)))
    ERR("ERROR: Can't connect to X server.\n");
  init_window();
  init_buffalo_atoms();
}

/* create & map window */
void init_window() {
  xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(X)).data;

  win = xcb_generate_id(X);
  xcb_create_window(X, screen->root_depth, win, screen->root,
      0, 0, 1, 1, 0, XCB_COPY_FROM_PARENT, screen->root_visual,
      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
      (uint32_t[]) {screen->black_pixel, 1, XCB_EVENT_MASK_PROPERTY_CHANGE});
  xcb_map_window(X, win);
}

/* intern a bunch of atoms that we'll need later. */
void init_buffalo_atoms() {
  xcb_intern_atom_cookie_t atom_cookies[LENGTH(atoms)],
                           register_atom_cookies[NUM_REGISTERS];
  int i;
  xcb_intern_atom_reply_t *reply;

  for (i=0; i<LENGTH(atoms); i++)
    atom_cookies[i] = xcb_intern_atom(X, 0,
        strlen(internable_atoms[i].name), internable_atoms[i].name);

  for (i=0; i<NUM_REGISTERS; i++)
    register_atom_cookies[i] = xcb_intern_atom(X, 0, 4, (char*) &i);


  for (i=0; i<LENGTH(atoms); i++) {
    reply = xcb_intern_atom_reply(X, atom_cookies[i], NULL);
    atoms[internable_atoms[i].index] = reply->atom;
    free(reply);
  }

  for (i=0; i<NUM_REGISTERS; i++) {
    reply = xcb_intern_atom_reply(X, register_atom_cookies[i], NULL);
    registers[i].atom = reply->atom;
    free(reply);
  }
}

void take_daemon_selection() {
  xcb_set_selection_owner(X, win, atoms[BUFFALO_DAEMON], XCB_CURRENT_TIME);
  xcb_flush(X);
  while (daemon_window() != win);
}

void buffalo_daemon() {
  xcb_generic_event_t *ev;
  take_daemon_selection();
  daemon(1,1);

  /* main loop */
  while (running && (ev = xcb_wait_for_event(X))) {
    handle_event(ev);
    xcb_flush(X);
    free(ev);
  }
}

void cli_copy()    {send_message(COPY,  strtol(optarg, NULL, 10));}
void cli_paste()   {send_message(PASTE, strtol(optarg, NULL, 10));}
void cli_version() {puts(VERSION);}

void handle_arg(int arg) {
  static Command cmd[] = {
    {'c', 1, cli_copy},
    {'p', 1, cli_paste},
    {'d', 1, buffalo_daemon},
    {'x', 1, take_daemon_selection},
    {'v', 0, cli_version}
  };

  for (int i = 0; i<LENGTH(cmd); i++)
    if (arg == cmd[i].name) {
      if (!X && cmd[i].needs_x) init_x_protocol();
      cmd[i].run();
    }
}

int main(int argc, char *argv[]) {
  int arg;

  while(running && (arg = getopt(argc, argv, "c:p:dxv")) != -1)
    handle_arg(arg);

  if (X) xcb_disconnect(X);

  return status;
}

