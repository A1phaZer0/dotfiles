/* see license for copyright and license */

#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/Xproto.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xinerama.h>

#define LENGTH(x)       (sizeof(x)/sizeof(*x))
#define CLEANMASK(mask) (mask & ~(numlockmask | LockMask))
#define BUTTONMASK      ButtonPressMask|ButtonReleaseMask
#define ISFFT(c)        (c->isfull || c->isfloat || c->istrans)
#define ROOTMASK        SubstructureRedirectMask|ButtonPressMask|SubstructureNotifyMask|PropertyChangeMask

enum { RESIZE, MOVE };
enum { TILE, MONOCLE, BSTACK, GRID, FLOAT, MODES };
enum { WM_PROTOCOLS, WM_DELETE_WINDOW, WM_COUNT };
enum { NET_SUPPORTED, NET_FULLSCREEN, NET_WM_STATE, NET_ACTIVE, NET_COUNT };

/**
 * argument structure to be passed to function by config.h
 * com - function pointer ~ the command to run
 * i   - an integer to indicate different states
 * v   - any type argument
 */
typedef union {
    const char** com;
    const int i;
    const void *v;
} Arg;

/**
 * a key struct represents a combination of
 * mod    - a modifier mask
 * keysym - and the key pressed
 * func   - the function to be triggered because of the above combo
 * arg    - the argument to the function
 */
typedef struct {
    unsigned int mod;
    KeySym keysym;
    void (*func)(const Arg *);
    const Arg arg;
} Key;

/**
 * a button struct represents a combination of
 * mask   - a modifier mask
 * button - and the mouse button pressed
 * func   - the function to be triggered because of the above combo
 * arg    - the argument to the function
 */
typedef struct {
    unsigned int mask, button;
    void (*func)(const Arg *);
    const Arg arg;
} Button;

/**
 * define behavior of certain applications
 * configured in config.h
 *
 * class   - the class or name of the instance
 * desktop - what desktop it should be spawned at
 * follow  - whether to change desktop focus to the specified desktop
 */
typedef struct {
    const char *class;
    const int monitor;
    const int desktop;
    const Bool follow, floating;
} AppRule;

/* exposed function prototypes sorted alphabetically */
static void change_desktop(const Arg *arg);
static void change_monitor(const Arg *arg);
static void centerwindow();
static void client_to_desktop(const Arg *arg);
static void client_to_monitor(const Arg *arg);
static void focusurgent();
static void killclient();
static void last_desktop();
static void move_down();
static void move_up();
static void moveresize(const Arg *arg);
static void mousemotion(const Arg *arg);
static void next_win();
static void prev_win();
static void quit(const Arg *arg);
static void resize_master(const Arg *arg);
static void resize_stack(const Arg *arg);
static void rotate(const Arg *arg);
static void rotate_filled(const Arg *arg);
static void spawn(const Arg *arg);
static void swap_master();
static void switch_mode(const Arg *arg);
static void togglepanel();

#include "config.h"

/**
 * a client is a wrapper to a window that additionally
 * holds some properties for that window
 *
 * next    - the client after this one, or NULL if the current is the last client
 * isurgn  - set when the window received an urgent hint
 * isfull  - set when the window is fullscreen
 * isfloat - set when the window is floating
 * istrans - set when the window is transient
 * win     - the window this client is representing
 *
 * istrans is separate from isfloat as floating windows can be reset to
 * their tiling positions, while the transients will always be floating
 */
typedef struct Client {
    struct Client *next;
    Bool isurgn, isfull, isfloat, istrans;
    Window win;
} Client;

/**
 * properties of each desktop
 *
 * masz - the size of the master area
 * sasz - additional size of the first stack window area
 * mode - the desktop's tiling layout mode
 * head - the start of the client list
 * curr - the currently highlighted window
 * prev - the client that previously had focus
 * sbar - the visibility status of the panel/statusbar
 */
typedef struct {
    int mode, masz, sasz;
    Client *head, *curr, *prev;
    Bool sbar;
} Desktop;

/**
 * properties of each monitor
 *
 * wx, wy      - the starting position of the monitor area
 * wh, ww      - the width and height of the monitor
 * currdeskidx - the current desktop
 * desktops    - the desktops handled by the monitor
 */
typedef struct Monitor {
    int x, y, h, w, currdeskidx, prevdeskidx;
    Desktop desktops[DESKTOPS];
} Monitor;

/* hidden function prototypes sorted alphabetically */
static Client* addwindow(Window w, Desktop *d);
static void buttonpress(XEvent *e);
static void canclefullscreen(const Desktop *d, Monitor *m); 
static void cleanup(void);
static void clientmessage(XEvent *e);
static void configurerequest(XEvent *e);
static void deletewindow(Window w);
static void desktopinfo(void);
static void destroynotify(XEvent *e);
static void enternotify(XEvent *e);
static void focus(Client *c, Desktop *d, Monitor *m);
static void focusin(XEvent *e);
static unsigned long getcolor(const char* color, const int screen);
static void grabbuttons(Client *c);
static void grabkeys(void);
static void grid(int x, int y, int w, int h, const Desktop *d);
static void keypress(XEvent *e);
static void maprequest(XEvent *e);
static void monocle(int x, int y, int w, int h, const Desktop *d);
static Client* prevclient(Client *c, Desktop *d);
static void propertynotify(XEvent *e);
static void removeclient(Client *c, Desktop *d, Monitor *m);
static void run(void);
static void setfullscreen(Client *c, Desktop *d, Monitor *m, Bool fullscrn);
static void setup(void);
static void sigchld(int sig);
static void stack(int x, int y, int w, int h, const Desktop *d);
static void tile(Desktop *d, Monitor *m);
static void unmapnotify(XEvent *e);
static Bool wintoclient(Window w, Client **c, Desktop **d, Monitor **m);
static int xerror(Display *dis, XErrorEvent *ee);
static int xerrorstart(Display *dis, XErrorEvent *ee);

/**
 * global variables
 *
 * running      - whether the wm is accepting and processing more events
 * wh           - screen height
 * ww           - screen width
 * dis          - the display aka dpy
 * root         - the root window
 * wmatoms      - array holding atoms for ICCCM support
 * netatoms     - array holding atoms for EWMH support
 * desktops     - array of managed desktops
 * currdeskidx  - which desktop is currently active
 */
static Bool running = True;
static int nmonitors, currmonidx, retval;
static unsigned int numlockmask, win_focus, win_unfocus, win_infocus;
static Display *dis;
static Window root;
static Atom wmatoms[WM_COUNT], netatoms[NET_COUNT];
static Monitor *monitors;

/**
 * array of event handlers
 *
 * when a new event is received,
 * call the appropriate handler function
 */
static void (*events[LASTEvent])(XEvent *e) = {
    [KeyPress]         = keypress,     [EnterNotify]    = enternotify,
    [MapRequest]       = maprequest,   [ClientMessage]  = clientmessage,
    [ButtonPress]      = buttonpress,  [DestroyNotify]  = destroynotify,
    [UnmapNotify]      = unmapnotify,  [PropertyNotify] = propertynotify,
    [ConfigureRequest] = configurerequest,    [FocusIn] = focusin,
};

/**
 * array of layout handlers
 *
 * x - the start position in the x axis to place clients
 * y - the start position in the y axis to place clients
 * w - available width  that windows have to expand
 * h - available height that windows have to expand
 * d - the desktop to tile its clients
 */
static void (*layout[MODES])(int x, int y, int w, int h, const Desktop *d) = {
    [TILE] = stack, [BSTACK] = stack, [GRID] = grid, [MONOCLE] = monocle,
};

/**
 * add the given window to the given desktop
 *
 * create a new client to hold the new window
 *
 * if there is no head at the given desktop
 * add the window as the head
 * otherwise if ATTACH_ASIDE is not set,
 * add the window as the last client
 * otherwise add the window as head
 */
Client* addwindow(Window w, Desktop *d) {
    Client *c = NULL, *t = prevclient(d->head, d);
    if (!(c = (Client *)calloc(1, sizeof(Client)))) err(EXIT_FAILURE, "cannot allocate client");
    if (!d->head) d->head = c;
    else if (!ATTACH_ASIDE) { c->next = d->head; d->head = c; }
    else if (t) t->next = c; else d->head->next = c;

    XSelectInput(dis, (c->win = w), PropertyChangeMask|FocusChangeMask|(FOLLOW_MOUSE?EnterWindowMask:0));
    return c;
}

/**
 * on the press of a key binding (see grabkeys)
 * call the appropriate handler
 */
void buttonpress(XEvent *e) {
    Monitor *m = NULL; Desktop *d = NULL; Client *c = NULL;
    Bool w = wintoclient(e->xbutton.window, &c, &d, &m);

    int cm = 0; while (m != &monitors[cm] && cm < nmonitors) ++cm;

    if (w && CLICK_TO_FOCUS && e->xbutton.button == FOCUS_BUTTON && (c != d->curr || cm != currmonidx)) {
        if (cm != currmonidx) change_monitor(&(Arg){.i = cm});
        focus(c, d, m);
    }

    for (unsigned int i = 0; i < LENGTH(buttons); i++)
        if (CLEANMASK(buttons[i].mask) == CLEANMASK(e->xbutton.state) &&
            buttons[i].func && buttons[i].button == e->xbutton.button) {
            if (w && cm != currmonidx) change_monitor(&(Arg){.i = cm});
            if (w && c != d->curr) focus(c, d, m);
            buttons[i].func(&(buttons[i].arg));
        }
}

/**
 * focus another desktop
 *
 * to avoid flickering (esp. monocle mode):
 * first map the new windows
 * first the current window and then all other
 * then unmap the old windows
 * first all others then the current
 */
void change_desktop(const Arg *arg) {
    Monitor *m = &monitors[currmonidx];
    if (arg->i == m->currdeskidx || arg->i < 0 || arg->i >= DESKTOPS) return;
    Desktop *d = &m->desktops[(m->prevdeskidx = m->currdeskidx)], *n = &m->desktops[(m->currdeskidx = arg->i)];
    if (n->curr) XMapWindow(dis, n->curr->win);
    for (Client *c = n->head; c; c = c->next) XMapWindow(dis, c->win);
    XChangeWindowAttributes(dis, root, CWEventMask, &(XSetWindowAttributes){.do_not_propagate_mask = SubstructureNotifyMask});
    for (Client *c = d->head; c; c = c->next) if (c != d->curr) XUnmapWindow(dis, c->win);
    if (d->curr) XUnmapWindow(dis, d->curr->win);
    XChangeWindowAttributes(dis, root, CWEventMask, &(XSetWindowAttributes){.event_mask = ROOTMASK});
    if (n->head) { tile(n, m); focus(n->curr, n, m); }
    desktopinfo();
}

/**
 * focus another monitor
 */
void change_monitor(const Arg *arg) {
    if (arg->i == currmonidx || arg->i < 0 || arg->i >= nmonitors) return;
    Monitor *m = &monitors[currmonidx], *n = &monitors[(currmonidx = arg->i)];
    focus(m->desktops[m->currdeskidx].curr, &m->desktops[m->currdeskidx], m);
    focus(n->desktops[n->currdeskidx].curr, &n->desktops[n->currdeskidx], n);
    desktopinfo();
}

/**
 * place the current window in the center of the screen floating
 */
void centerwindow(void) {
    XWindowAttributes wa;
    Monitor *m = &monitors[currmonidx];
    Desktop *d = &m->desktops[m->currdeskidx];
    int border_width;
    if (!d->curr || !XGetWindowAttributes(dis, d->curr->win, &wa)) return;
    if (!d->curr->isfloat && !d->curr->istrans) { d->curr->isfloat = True; tile(d, m); }
    XRaiseWindow(dis, d->curr->win);
    if (d->mode == MONOCLE)
        border_width = MONOCLE_BORDER?BORDER_WIDTH:0;
    else
        border_width = BORDER_WIDTH;
    XMoveWindow(dis, d->curr->win, m->x + (m->w - wa.width)/2 - border_width,
            m->y + (m->h - wa.height - (TOP_PANEL && d->sbar ? PANEL_HEIGHT : 0))/2 - border_width 
            + (TOP_PANEL && d->sbar ? PANEL_HEIGHT : 0));
}

/**
 * remove all windows in all desktops by sending a delete window message
 */
void cleanup(void) {
    Window root_return, parent_return, *children;
    unsigned int nchildren;

    XUngrabKey(dis, AnyKey, AnyModifier, root);
    XQueryTree(dis, root, &root_return, &parent_return, &children, &nchildren);
    for (unsigned int i = 0; i < nchildren; i++) deletewindow(children[i]);
    if (children) XFree(children);
    XSync(dis, False);
    free(monitors);
}

/**
 * move the current focused client to another desktop
 *
 * add the current client as the last on the new desktop
 * then remove it from the current desktop
 */
void client_to_desktop(const Arg *arg) {
    Monitor *m = &monitors[currmonidx]; Desktop *d = &m->desktops[m->currdeskidx], *n = NULL;
    if (arg->i == m->currdeskidx || arg->i < 0 || arg->i >= DESKTOPS || !d->curr) return;

    Client *c = d->curr, *p = prevclient(d->curr, d),
           *l = prevclient(m->desktops[arg->i].head, (n = &m->desktops[arg->i]));

    /* unlink current client from current desktop */
    if (d->head == c || !p) d->head = c->next; else p->next = c->next;
    c->next = NULL;
    XChangeWindowAttributes(dis, root, CWEventMask, &(XSetWindowAttributes){.do_not_propagate_mask = SubstructureNotifyMask});
    if (XUnmapWindow(dis, c->win)) focus(d->prev, d, m);
    XChangeWindowAttributes(dis, root, CWEventMask, &(XSetWindowAttributes){.event_mask = ROOTMASK});
    if (!(c->isfloat || c->istrans) || (d->head && !d->head->next)) tile(d, m);

    /* link client to new desktop and make it the current */
    focus(l ? (l->next = c):n->head ? (n->head->next = c):(n->head = c), n, m);

    if (FOLLOW_WINDOW) change_desktop(arg); else desktopinfo();
}

/**
 * move the current focused client to another monitor
 *
 * add the current client as the last on the new monitor's current desktop
 * then remove it from the current monitor's current desktop
 *
 * removing the client means unlinking it and unmapping it.
 * add the client means linking it as the last client, and
 * mapping it. mapping must happen after the client has been
 * unmapped from the current monitor's current desktop.
 */
void client_to_monitor(const Arg *arg) {
    Monitor *cm = &monitors[currmonidx], *nm = NULL;
    Desktop *cd = &cm->desktops[cm->currdeskidx], *nd = NULL;
    if (arg->i == currmonidx || arg->i < 0 || arg->i >= nmonitors || !cd->curr) return;

    nd = &monitors[arg->i].desktops[(nm = &monitors[arg->i])->currdeskidx];
    Client *c = cd->curr, *p = prevclient(c, cd), *l = prevclient(nd->head, nd);

    /* unlink current client from current monitor's current desktop */
    if (cd->head == c || !p) cd->head = c->next; else p->next = c->next;
    c->next = NULL;
    focus(cd->prev, cd, cm);
    if (!(c->isfloat || c->istrans) || (cd->head && !cd->head->next)) tile(cd, cm);

    /* reset floating and fullscreen state */
    if (ISFFT(c)) c->isfloat = c->isfull = False;

    /* link to new monitor's current desktop */
    focus(l ? (l->next = c):nd->head ? (nd->head->next = c):(nd->head = c), nd, nm);
    tile(nd, nm);

    if (FOLLOW_MONITOR) change_monitor(arg); else desktopinfo();
}

/**
 * receive and process client messages
 *
 * check if window wants to change its state to fullscreen,
 * or if the window want to become active/focused
 *
 * to change the state of a mapped window, a client MUST
 * send a _NET_WM_STATE client message to the root window
 * message_type must be _NET_WM_STATE
 *   data.l[0] is the action to be taken
 *   data.l[1] is the property to alter three actions:
 *   - remove/unset _NET_WM_STATE_REMOVE=0
 *   - add/set _NET_WM_STATE_ADD=1,
 *   - toggle _NET_WM_STATE_TOGGLE=2
 *
 * to request to become active, a client should send a
 * message of _NET_ACTIVE_WINDOW type. when such a message
 * is received and a client holding that window exists,
 * the window becomes the current active focused window
 * on its desktop.
 */
void clientmessage(XEvent *e) {
    Monitor *m = NULL; Desktop *d = NULL; Client *c = NULL;
    if (!wintoclient(e->xclient.window, &c, &d, &m)) return;

    if (e->xclient.message_type        == netatoms[NET_WM_STATE] && (
        (unsigned)e->xclient.data.l[1] == netatoms[NET_FULLSCREEN]
     || (unsigned)e->xclient.data.l[2] == netatoms[NET_FULLSCREEN])) {
        setfullscreen(c, d, m, (e->xclient.data.l[0] == 1 || (e->xclient.data.l[0] == 2 && !c->isfull)));
        if (!(c->isfloat || c->istrans) || !d->head->next) tile(d, m);
    } else if (e->xclient.message_type == netatoms[NET_ACTIVE]) focus(c, d, m);
}

/**
 * configure a window's size, position, border width, and stacking order.
 *
 * windows usually have a prefered size (width, height) and position (x, y),
 * and sometimes borer with (border_width) and stacking order (above, detail).
 * a configure request attempts to reconfigure those properties for a window.
 *
 * we don't really care about those values, because a tiling wm will impose
 * its own values for those properties.
 * however the requested values must be set initially for some windows,
 * otherwise the window will misbehave or even crash (see gedit, geany, gvim).
 *
 * some windows depend on the number of columns and rows to set their
 * size, and not on pixels (terminals, consoles, some editors etc).
 * normally those clients when tiled and respecting the prefered size
 * will create gaps around them (window_hints).
 * however, clients are tiled to match the wm's prefered size,
 * not respecting those prefered values.
 *
 * some windows implement window manager functions themselves.
 * that is windows explicitly steal focus, or manage subwindows,
 * or move windows around w/o the window manager's help, etc..
 * to disallow this behavior, we 'tile()' the desktop to which
 * the window that sent the configure request belongs.
 */
void configurerequest(XEvent *e) {
    XConfigureRequestEvent *ev = &e->xconfigurerequest;
    XWindowChanges wc = { ev->x, ev->y,  ev->width, ev->height, ev->border_width, ev->above, ev->detail };
    if (XConfigureWindow(dis, ev->window, ev->value_mask, &wc)) XSync(dis, False);
    Monitor *m = NULL; Desktop *d = NULL; Client *c = NULL;
    if (wintoclient(ev->window, &c, &d, &m)) tile(d, m);
}

/**
 * clients receiving a WM_DELETE_WINDOW message should behave as if
 * the user selected "delete window" from a hypothetical menu and
 * also perform any confirmation dialog with the user.
 */
void deletewindow(Window w) {
    XEvent ev = { .type = ClientMessage };
    ev.xclient.window = w;
    ev.xclient.format = 32;
    ev.xclient.message_type = wmatoms[WM_PROTOCOLS];
    ev.xclient.data.l[0]    = wmatoms[WM_DELETE_WINDOW];
    ev.xclient.data.l[1]    = CurrentTime;
    XSendEvent(dis, w, False, NoEventMask, &ev);
}

/**
 * output info about the desktops on standard output stream
 *
 * the information is formatted as a space separated line
 * where each token contains information about a desktop.
 * each token is a formatted as ':' separated string of values.
 * the values are:
 *   - the desktop number/id
 *   - the desktop's client count
 *   - the desktop's tiling layout mode/id
 *   - whether the desktop is the current focused (1) or not (0)
 *   - whether any client in that desktop has received an urgent hint
 *
 * once the info is collected, immediately flush the stream
 */
void desktopinfo(void) {
    Monitor *m = NULL;
    Client *c = NULL;
    Bool urgent = False;

    for (int cm = 0; cm < nmonitors; cm++)
        for (int cd = 0, w = 0; cd < DESKTOPS; cd++, w = 0, urgent = False) {
            for (m = &monitors[cm], c = m->desktops[cd].head; c; urgent |= c->isurgn, ++w, c = c->next);
            printf("%d:%d:%d:%d:%d:%d:%d ", cm, cm == currmonidx, cd, w, m->desktops[cd].mode, cd == m->currdeskidx, urgent);
        }

    printf("\n");
    fflush(stdout);
}

/**
 * generated whenever a client application destroys a window
 *
 * a destroy notification is received when a window is being closed
 * on receival, remove the client that held that window
 */
void destroynotify(XEvent *e) {
    Monitor *m = NULL; Desktop *d = NULL; Client *c = NULL;
    if (wintoclient(e->xdestroywindow.window, &c, &d, &m)) removeclient(c, d, m);
}

/**
 * when the mouse enters a window's borders, that window,
 * if has set notifications of such events (EnterWindowMask)
 * will notify that the pointer entered its region
 * and will get focus if FOLLOW_MOUSE is set in the config.
 */
void enternotify(XEvent *e) {
    Monitor *m = NULL; Desktop *d = NULL; Client *c = NULL, *p = NULL;

    if (!FOLLOW_MOUSE || (e->xcrossing.mode != NotifyNormal && e->xcrossing.detail == NotifyInferior)
        || !wintoclient(e->xcrossing.window, &c, &d, &m) || e->xcrossing.window == d->curr->win) return;

    if (m != &monitors[currmonidx]) for (int cm = 0; cm < nmonitors; cm++)
        if (m == &monitors[cm]) change_monitor(&(Arg){.i = cm});

    if ((p = d->prev))
        XChangeWindowAttributes(dis, p->win, CWEventMask, &(XSetWindowAttributes){.do_not_propagate_mask = EnterWindowMask});
    focus(c, d, m);
    if (p) XChangeWindowAttributes(dis, p->win, CWEventMask, &(XSetWindowAttributes){.event_mask = EnterWindowMask});
}

/**
 * 1. set current/active/focused and previously focused client
 *    in other words, manage curr and prev references
 * 2. restack clients
 * 3. highlight borders and set active window property
 * 4. give input focus to the current/active/focused client
 */
void focus(Client *c, Desktop *d, Monitor *m) {
    /* update references to prev and curr,
     * previously focused and currently focused clients.
     *
     * if there are no clients (!head) or the new client
     * is NULL, then delete the _NET_ACTIVE_WINDOW property
     *
     * if the new client is the prev client then
     *  - either the current client was removed
     *    and thus focus(prev) was called
     *  - or the previous from current is prev
     *    ie, two consecutive clients were focused
     *    and then prev_win() was called, to focus
     *    the previous from current client, which
     *    happens to be prev (curr == c->next).
     * (below: h:head p:prev c:curr)
     *
     * [h]->[p]->[c]->NULL   ===>   [h|p]->[c]->NULL
     *            ^ remove current
     *
     * [h]->[p]->[c]->NULL   ===>   [h]->[c]->[p]->NULL
     *       ^ prev_win swaps prev and curr
     *
     * in the first case we need to update prev reference,
     * choice here is to set it to the previous from the
     * new current client.
     * the second case is handled as any other case, the
     * current client is now the previously focused (prev = curr)
     * and the new current client is now curr (curr = c)
     *
     * references should only change when the current
     * client is different from the one given to focus.
     *
     * the new client should never be NULL, except if,
     * there is no other client on the workspace (!head).
     * prev and curr always point to different clients.
     *
     * NOTICE: remove client can remove any client,
     * not just the current (curr). Thus, if prev is
     * removed, its reference needs to be updated.
     * That is handled by removeclient() function.
     * All other reference changes for curr and prev
     * should and are handled here.
     */
    if (!d->head || !c) { /* no clients - no active window - nothing to do */
        XDeleteProperty(dis, root, netatoms[NET_ACTIVE]);
        d->curr = d->prev = NULL;
        return;
    } else if (d->prev == c && d->curr != c->next) { d->prev = prevclient((d->curr = c), d);
    } else if (d->curr != c) { d->prev = d->curr; d->curr = c; }

    /* restack clients
     *
     * stack order is based on client properties.
     * from top to bottom:
     *  - current when floating or transient
     *  - floating or trancient windows
     *  - current when tiled
     *  - current when fullscreen
     *  - fullscreen windows
     *  - tiled windows
     *
     * num of n:all fl:fullscreen ft:floating/transient windows
     */
    int n = 0, fl = 0, ft = 0;
    for (c = d->head; c; c = c->next, ++n) if (ISFFT(c)) { fl++; if (!c->isfull) ft++; }
    Window w[n];
    w[(d->curr->isfloat || d->curr->istrans) ? 0:ft] = d->curr->win;
    for (fl += !ISFFT(d->curr) ? 1:0, c = d->head; c; c = c->next) {
        XSetWindowBorder(dis, c->win, (c != d->curr) ? win_unfocus:(m == &monitors[currmonidx]) ? win_focus:win_infocus);
        /* windows always get borders, except if fullscreen */
        XSetWindowBorderWidth(dis, c->win, c->isfull ? 0:BORDER_WIDTH);
        if (c != d->curr) w[c->isfull ? --fl:ISFFT(c) ? --ft:--n] = c->win;
        if (CLICK_TO_FOCUS || c == d->curr) grabbuttons(c);
    }
    XRestackWindows(dis, w, LENGTH(w));

    XSetInputFocus(dis, d->curr->win, RevertToPointerRoot, CurrentTime);
    XChangeProperty(dis, root, netatoms[NET_ACTIVE], XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&d->curr->win, 1);

    XSync(dis, False);
}

/**
 * dont give focus to any client except current.
 * some apps explicitly call XSetInputFocus (see
 * tabbed, chromium), resulting in loss of input
 * focuse (mouse/kbd) from the current focused
 * client.
 *
 * this gives focus back to the current selected
 * client, by the user, through the wm.
 */
void focusin(XEvent *e) {
    Monitor *m = &monitors[currmonidx]; Desktop *d = &m->desktops[m->currdeskidx];
    if (d->curr && d->curr->win != e->xfocus.window) focus(d->curr, d, m);
}

/**
 * find and focus the first client that received an urgent hint
 * first look in the current desktop then on other desktops
 */
void focusurgent(void) {
    Monitor *m = &monitors[currmonidx];
    Client *c = NULL;
    int d = -1;
    for (c = m->desktops[m->currdeskidx].head; c && !c->isurgn; c = c->next);
    while (!c && d < DESKTOPS-1) for (c = m->desktops[++d].head; c && !c->isurgn; c = c->next);
    if (c) { if (d != -1) change_desktop(&(Arg){.i = d}); focus(c, &m->desktops[m->currdeskidx], m); }
}

/**
 * get a pixel with the requested color to
 * fill some window area (such as borders)
 */
unsigned long getcolor(const char* color, const int screen) {
    XColor c; Colormap map = DefaultColormap(dis, screen);
    if (!XAllocNamedColor(dis, map, color, &c, &c)) err(EXIT_FAILURE, "cannot allocate color");
    return c.pixel;
}

/**
 * register button bindings to be notified of
 * when they occur.
 * the wm listens to those button bindings and
 * calls an appropriate handler when a binding
 * occurs (see buttonpress).
 */
void grabbuttons(Client *c) {
    Monitor *cm = &monitors[currmonidx];
    unsigned int b, m, modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };

    for (m = 0; CLICK_TO_FOCUS && m < LENGTH(modifiers); m++)
        if (c != cm->desktops[cm->currdeskidx].curr) XGrabButton(dis, FOCUS_BUTTON, modifiers[m],
                c->win, False, BUTTONMASK, GrabModeAsync, GrabModeAsync, None, None);
        else XUngrabButton(dis, FOCUS_BUTTON, modifiers[m], c->win);

    for (b = 0, m = 0; b < LENGTH(buttons); b++, m = 0) while (m < LENGTH(modifiers))
        XGrabButton(dis, buttons[b].button, buttons[b].mask|modifiers[m++], c->win,
                      False, BUTTONMASK, GrabModeAsync, GrabModeAsync, None, None);
}

/**
 * register key bindings to be notified of
 * when they occur.
 * the wm listens to those key bindings and
 * calls an appropriate handler when a binding
 * occurs (see keypressed).
 */
void grabkeys(void) {
    KeyCode code;
    XUngrabKey(dis, AnyKey, AnyModifier, root);
    unsigned int k, m, modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };

    for (k = 0, m = 0; k < LENGTH(keys); k++, m = 0)
        while ((code = XKeysymToKeycode(dis, keys[k].keysym)) && m < LENGTH(modifiers))
            XGrabKey(dis, code, keys[k].mod|modifiers[m++], root, True, GrabModeAsync, GrabModeAsync);
}

/**
 * grid mode / grid layout
 * arrange windows in a grid aka fair
 */
void grid(int x, int y, int w, int h, const Desktop *d) {
    int n = 0, cols = 0, cn = 0, rn = 0, i = -1;
    canclefullscreen(d, &monitors[currmonidx]);
    for (Client *c = d->head; c; c = c->next) if (!ISFFT(c)) ++n;
    for (cols = 0; cols <= n/2; cols++) if (cols*cols >= n) break; /* emulate square root */
    if (n == 0) return; else if (n == 5) cols = 2;

    int rows = n/cols, ch = h - USELESSGAP, cw = (w - USELESSGAP)/(cols ? cols:1);
    for (Client *c = d->head; c; c = c->next) {
        if (ISFFT(c)) continue; else ++i;
        if (i/rows + 1 > cols - n%cols) rows = n/cols + 1;
        XMoveResizeWindow(dis, c->win, x + cn*cw + USELESSGAP, y + rn*ch/rows + USELESSGAP,
                   cw - 2*BORDER_WIDTH - USELESSGAP, ch/rows - 2*BORDER_WIDTH - USELESSGAP);
        if (++rn >= rows) { rn = 0; cn++; }
    }
}

/**
 * on the press of a key binding (see grabkeys)
 * call the appropriate handler
 */
void keypress(XEvent *e) {
    KeySym keysym = XkbKeycodeToKeysym(dis, e->xkey.keycode, 0, 0);
    for (unsigned int i = 0; i < LENGTH(keys); i++)
        if (keysym == keys[i].keysym && CLEANMASK(keys[i].mod) == CLEANMASK(e->xkey.state))
            if (keys[i].func) keys[i].func(&keys[i].arg);
}

/**
 * explicitly kill the current client - close the highlighted window
 * if the client accepts WM_DELETE_WINDOW requests send a delete message
 * otherwise forcefully kill and remove the client
 */
void killclient(void) {
    Monitor *m = &monitors[currmonidx];
    Desktop *d = &m->desktops[m->currdeskidx];
    if (!d->curr) return;

    Atom *prot = NULL; int n = -1;
    if (XGetWMProtocols(dis, d->curr->win, &prot, &n))
        while(--n >= 0 && prot[n] != wmatoms[WM_DELETE_WINDOW]);
    if (n < 0) { XKillClient(dis, d->curr->win); removeclient(d->curr, d, m); }
    else deletewindow(d->curr->win);
    if (prot) XFree(prot);
}

/**
 * focus the previously focused desktop
 */
void last_desktop(void) {
    change_desktop(&(Arg){.i = monitors[currmonidx].prevdeskidx});
}

/**
 * a map request is received when a window wants to display itself.
 * if the window has override_redirect flag set,
 * then it should not be handled by the wm.
 * if the window already has a client then there is nothing to do.
 *
 * match window class and/or install name against an app rule.
 * create a new client for the window and add it to the appropriate desktop.
 * set the floating, transient and fullscreen state of the client.
 * if the desktop in which the window is to be spawned is the current desktop
 * then display/map the window, else, if follow is set, focus the new desktop.
 */
void maprequest(XEvent *e) {
    Monitor *m = NULL; Desktop *d = NULL; Client *c = NULL;
    Window w = e->xmaprequest.window;
    XWindowAttributes wa = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (wintoclient(w, &c, &d, &m) || (XGetWindowAttributes(dis, w, &wa) && wa.override_redirect)) return;

    XClassHint ch = {0, 0};
    Bool follow = False, floating = False;
    int newmon = currmonidx, newdsk = monitors[currmonidx].currdeskidx;

    if (XGetClassHint(dis, w, &ch)) for (unsigned int i = 0; i < LENGTH(rules); i++)
        if (strstr(ch.res_class, rules[i].class) || strstr(ch.res_name, rules[i].class)) {
            if (rules[i].monitor >= 0 && rules[i].monitor < nmonitors) newmon = rules[i].monitor;
            if (rules[i].desktop >= 0 && rules[i].desktop < DESKTOPS) newdsk = rules[i].desktop;
            follow = rules[i].follow, floating = rules[i].floating;
            break;
        }
    if (ch.res_class) XFree(ch.res_class);
    if (ch.res_name) XFree(ch.res_name);

    c = addwindow(w, (d = &(m = &monitors[newmon])->desktops[newdsk])); /* from now on, use c->win */
    c->istrans = XGetTransientForHint(dis, c->win, &w);
    if ((c->isfloat = (floating || d->mode == FLOAT)) && !c->istrans)
        //XMoveWindow(dis, c->win, m->x + (m->w - wa.width)/2, m->y + (m->h - wa.height)/2);
          XMoveWindow(dis, c->win, m->x + (m->w - wa.width)/2 - BORDER_WIDTH,
                      m->y + (m->h - wa.height - (TOP_PANEL && d->sbar ? PANEL_HEIGHT : 0))/2 - BORDER_WIDTH 
                      + (TOP_PANEL && d->sbar ? PANEL_HEIGHT : 0));

    int i; unsigned long l; unsigned char *state = NULL; Atom a;
    if (XGetWindowProperty(dis, c->win, netatoms[NET_WM_STATE], 0L, sizeof a,
                False, XA_ATOM, &a, &i, &l, &l, &state) == Success && state)
        setfullscreen(c, d, m, (*(Atom *)state == netatoms[NET_FULLSCREEN]));
    if (state) XFree(state);

    if (m->currdeskidx == newdsk) { if (!ISFFT(c)) tile(d, m); XMapWindow(dis, c->win); }
    if (follow) { change_monitor(&(Arg){.i = newmon}); change_desktop(&(Arg){.i = newdsk}); }
    focus(c, d, m);

    if (!follow) desktopinfo();
}

/**
 * handle resize and positioning of a window with the pointer.
 *
 * grab the pointer and get it's current position.
 * now, all pointer movement events will be reported until it is ungrabbed.
 *
 * while the mouse is pressed, grab interesting events (see button press,
 * button release, pointer motion).
 * on on pointer movement resize or move the window under the curson.
 * also handle map requests and configure requests.
 *
 * finally, on ButtonRelease, ungrab the poitner.
 * event handling is passed back to run() function.
 *
 * once a window has been moved or resized, it's marked as floating.
 */
void mousemotion(const Arg *arg) {
    Monitor *m = &monitors[currmonidx]; Desktop *d = &m->desktops[m->currdeskidx];
    XWindowAttributes wa;
    XEvent ev;

    if (!d->curr || !XGetWindowAttributes(dis, d->curr->win, &wa)) return;

    if (arg->i == RESIZE) XWarpPointer(dis, d->curr->win, d->curr->win, 0, 0, 0, 0, --wa.width, --wa.height);
    int rx, ry, c, xw, yh; unsigned int v; Window w;
    if (!XQueryPointer(dis, root, &w, &w, &rx, &ry, &c, &c, &v) || w != d->curr->win) return;

    if (XGrabPointer(dis, root, False, BUTTONMASK|PointerMotionMask, GrabModeAsync,
                     GrabModeAsync, None, None, CurrentTime) != GrabSuccess) return;

    if (!d->curr->isfloat && !d->curr->istrans) { d->curr->isfloat = True; tile(d, m); focus(d->curr, d, m); }
    XRaiseWindow(dis, d->curr->win);

    do {
        XMaskEvent(dis, BUTTONMASK|PointerMotionMask|SubstructureRedirectMask, &ev);
        if (ev.type == MotionNotify) {
            xw = (arg->i == MOVE ? wa.x:wa.width)  + ev.xmotion.x - rx;
            yh = (arg->i == MOVE ? wa.y:wa.height) + ev.xmotion.y - ry;
            if (arg->i == RESIZE) XResizeWindow(dis, d->curr->win,
                    xw > MINWSZ ? xw:wa.width, yh > MINWSZ ? yh:wa.height);
            else if (arg->i == MOVE) XMoveWindow(dis, d->curr->win, xw, yh);
        } else if (ev.type == ConfigureRequest || ev.type == MapRequest) events[ev.type](&ev);
    } while (ev.type != ButtonRelease);

    XUngrabPointer(dis, CurrentTime);
}

/**
 * monocle aka max aka fullscreen mode/layout
 * each window should cover all the available screen space
 */
void monocle(int x, int y, int w, int h, const Desktop *d) {
    int gap_width = MONOCLE_GAP?USELESSGAP:0;
    int border_width = MONOCLE_BORDER?BORDER_WIDTH:0;
    if (d->mode != MONOCLE) {
        canclefullscreen(d, &monitors[currmonidx]);
        for (Client *c = d->head; c; c = c->next) {
            if (!(c->isfloat || c->istrans))
                XMoveResizeWindow(dis, c->win, x + USELESSGAP, y + USELESSGAP, w - 2*USELESSGAP - 2*BORDER_WIDTH, h - 2*USELESSGAP - 2*BORDER_WIDTH);
        }
    } else {
        for (Client *c = d->head; c; c = c->next) 
            if (!c->istrans) {
                XChangeProperty(dis, c->win, netatoms[NET_WM_STATE], XA_ATOM, 32, PropModeReplace, (unsigned char*)
                        ((c->isfull = !(MONOCLE_GAP || MONOCLE_BORDER)) ? &netatoms[NET_FULLSCREEN]:0), !(MONOCLE_GAP || MONOCLE_BORDER));
                XMoveResizeWindow(dis, c->win, x + gap_width, y - (COVER_PANEL?(TOP_PANEL && d->sbar ? PANEL_HEIGHT:0):0) + gap_width,
                        w - 2*gap_width - 2*border_width, h + (COVER_PANEL?(TOP_PANEL && d->sbar ? PANEL_HEIGHT:0):0) - 2*gap_width - 2*border_width);
                XSetWindowBorderWidth(dis, c->win,  (MONOCLE_BORDER?BORDER_WIDTH:0));
            }
    }
}

/**
 * swap positions of current and next from current clients
 */
void move_down(void) {
    Desktop *d = &monitors[currmonidx].desktops[monitors[currmonidx].currdeskidx];
    if (!d->curr || !d->head->next) return;
    /* p is previous, c is current, n is next, if current is head n is last */
    Client *p = prevclient(d->curr, d), *n = (d->curr->next) ? d->curr->next:d->head;
    /*
     * if c is head, swapping with n should update head to n
     * [c]->[n]->..  ==>  [n]->[c]->..
     *  ^head              ^head
     *
     * else there is a previous client and p->next should be what's after c
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     */
    if (d->curr == d->head) d->head = n; else p->next = d->curr->next;
    /*
     * if c is the last client, c will be the current head
     * [n]->..->[p]->[c]->NULL  ==>  [c]->[n]->..->[p]->NULL
     *  ^head                         ^head
     * else c will take the place of n, so c-next will be n->next
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     */
    d->curr->next = (d->curr->next) ? n->next:n;
    /*
     * if c was swapped with n then they now point to the same ->next. n->next should be c
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     *                                        [c]-^
     *
     * else c is the last client and n is head,
     * so c will be move to be head, no need to update n->next
     * [n]->..->[p]->[c]->NULL  ==>  [c]->[n]->..->[p]->NULL
     *  ^head                         ^head
     */
    if (d->curr->next == n->next) n->next = d->curr; else d->head = d->curr;
    if (!d->curr->isfloat && !d->curr->istrans) tile(d, &monitors[currmonidx]);
}

/**
 * swap positions of current and previous from current clients
 */
void move_up(void) {
    Desktop *d = &monitors[currmonidx].desktops[monitors[currmonidx].currdeskidx];
    if (!d->curr || !d->head->next) return;
    /* p is previous from current or last if current is head */
    Client *pp = NULL, *p = prevclient(d->curr, d);
    /* pp is previous from p, or null if current is head and thus p is last */
    if (p->next) for (pp = d->head; pp && pp->next != p; pp = pp->next);
    /*
     * if p has a previous client then the next client should be current (current is c)
     * ..->[pp]->[p]->[c]->..  ==>  ..->[pp]->[c]->[p]->..
     *
     * if p doesn't have a previous client, then p might be head, so head must change to c
     * [p]->[c]->..  ==>  [c]->[p]->..
     *  ^head              ^head
     * if p is not head, then c is head (and p is last), so the new head is next of c
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    if (pp) pp->next = d->curr; else d->head = (d->curr == d->head) ? d->curr->next:d->curr;
    /*
     * next of p should be next of c
     * ..->[pp]->[p]->[c]->[n]->..  ==>  ..->[pp]->[c]->[p]->[n]->..
     * except if c was head (now c->next is head), so next of p should be c
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    p->next = (d->curr->next == d->head) ? d->curr:d->curr->next;
    /*
     * next of c should be p
     * ..->[pp]->[p]->[c]->[n]->..  ==>  ..->[pp]->[c]->[p]->[n]->..
     * except if c was head (now c->next is head), so c is must be last
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    d->curr->next = (d->curr->next == d->head) ? NULL:p;
    if (!d->curr->isfloat && !d->curr->istrans) tile(d, &monitors[currmonidx]);
}

/**
 * move and resize a window with the keyboard
 */
void moveresize(const Arg *arg) {
    Monitor *m = &monitors[currmonidx]; Desktop *d = &m->desktops[m->currdeskidx];
    XWindowAttributes wa;
    if (!d->curr || !XGetWindowAttributes(dis, d->curr->win, &wa)) return;
    if (!d->curr->isfloat && !d->curr->istrans) { d->curr->isfloat = True; tile(d, m); focus(d->curr, d, m); }
    XRaiseWindow(dis, d->curr->win);
    XMoveResizeWindow(dis, d->curr->win, wa.x + ((int *)arg->v)[0], wa.y + ((int *)arg->v)[1],
                                wa.width + ((int *)arg->v)[2], wa.height + ((int *)arg->v)[3]);
}

/**
 * cyclic focus the next window
 * if the window is the last on stack, focus head
 */
void next_win(void) {
    Desktop *d = &monitors[currmonidx].desktops[monitors[currmonidx].currdeskidx];
    if (d->curr && d->head->next) focus(d->curr->next ? d->curr->next:d->head, d, &monitors[currmonidx]);
}

/**
 * get the previous client from the given
 * if no such client, return NULL
 */
Client* prevclient(Client *c, Desktop *d) {
    Client *p = NULL;
    if (c && d->head && d->head->next) for (p = d->head; p->next && p->next != c; p = p->next);
    return p;
}

/**
 * cyclic focus the previous window
 * if the window is head, focus the last stack window
 */
void prev_win(void) {
    Desktop *d = &monitors[currmonidx].desktops[monitors[currmonidx].currdeskidx];
    if (d->curr && d->head->next) focus(prevclient(d->curr, d), d, &monitors[currmonidx]);
}

/**
 * set unrgent hint for a window
 */
void propertynotify(XEvent *e) {
    Monitor *m = NULL; Desktop *d = NULL; Client *c = NULL;
    if (e->xproperty.atom != XA_WM_HINTS || !wintoclient(e->xproperty.window, &c, &d, &m)) return;

    XWMHints *wmh = XGetWMHints(dis, c->win);
    Desktop *cd = &monitors[currmonidx].desktops[monitors[currmonidx].currdeskidx];
    c->isurgn = (c != cd->curr && wmh && (wmh->flags & XUrgencyHint));

    if (wmh) XFree(wmh);
    desktopinfo();
}

/**
 * to quit just stop receiving events
 * run is stopped and control is back to main
 */
void quit(const Arg *arg) {
    retval = arg->i;
    running = False;
}

/**
 * remove the specified client from the given desktop
 *
 * if c was the previous client, previous must be updated.
 * if c was the current client, current must be updated.
 */
void removeclient(Client *c, Desktop *d, Monitor *m) {
    Client **p = NULL;
    for (p = &d->head; *p && (*p != c); p = &(*p)->next);
    if (!*p) return; else *p = c->next;
    if (c == d->prev && !(d->prev = prevclient(d->curr, d))) d->prev = d->head;
    if (c == d->curr || (d->head && !d->head->next)) focus(d->prev, d, m);
    if (!(c->isfloat || c->istrans) || (d->head && !d->head->next)) tile(d, m);
    free(c);
    desktopinfo();
}

/**
 * resize the master size
 * we should check for window size limits for both master and
 * stack clients. the size of a window can't be less than MINWSZ
 */
void resize_master(const Arg *arg) {
    Monitor *m = &monitors[currmonidx];
    Desktop *d = &m->desktops[m->currdeskidx];
    int msz = (d->mode == BSTACK ? m->h:m->w) * MASTER_SIZE + (d->masz += arg->i);
    if (msz >= MINWSZ && (d->mode == BSTACK ? m->h:m->w) - msz >= MINWSZ) tile(d, m);
    else d->masz -= arg->i; /* reset master area size */
}

/**
 * resize the first stack window
 */
void resize_stack(const Arg *arg) {
    monitors[currmonidx].desktops[monitors[currmonidx].currdeskidx].sasz += arg->i;
    tile(&monitors[currmonidx].desktops[monitors[currmonidx].currdeskidx], &monitors[currmonidx]);
}

/**
 * jump and focus the next or previous desktop
 */
void rotate(const Arg *arg) {
    change_desktop(&(Arg){.i = (DESKTOPS + monitors[currmonidx].currdeskidx + arg->i) % DESKTOPS});
}

/**
 * jump and focus the next non-empty desktop
 */
void rotate_filled(const Arg *arg) {
    Monitor *m = &monitors[currmonidx];
    int n = arg->i;
    while (n < DESKTOPS && !m->desktops[(DESKTOPS + m->currdeskidx + n) % DESKTOPS].head) (n += arg->i);
    change_desktop(&(Arg){.i = (DESKTOPS + m->currdeskidx + n) % DESKTOPS});
}

/**
 * main event loop
 * on receival of an event call the appropriate handler
 */
void run(void) {
    XEvent ev;
    while(running && !XNextEvent(dis, &ev)) if (events[ev.type]) events[ev.type](&ev);
}

/**
 * set the fullscreen state of a client
 *
 * if a client gets fullscreen resize it
 * to cover all screen space.
 * the border should be zero (0).
 *
 * if a client is reset from fullscreen,
 * the border should be BORDER_WIDTH.
 */
void setfullscreen(Client *c, Desktop *d, Monitor *m, Bool fullscrn) {
    if (fullscrn != c->isfull) XChangeProperty(dis, c->win,
            netatoms[NET_WM_STATE], XA_ATOM, 32, PropModeReplace, (unsigned char*)
            ((c->isfull = fullscrn) ? &netatoms[NET_FULLSCREEN]:0), fullscrn);
    if (fullscrn) XMoveResizeWindow(dis, c->win, m->x, m->y, m->w, m->h);
    XSetWindowBorderWidth(dis, c->win, (c->isfull || !d->head->next ? 0:BORDER_WIDTH));
}

/**
 * cancle fullscreen state
 * if layout is float, resize window, reset window border,
 * otherwise just reset window border waiting corresponding
 * layout function to resize window.
 */

void canclefullscreen(const Desktop *d, Monitor *m) 
{
    for (Client *c = d->head; c; c = c->next) 
        if ((d->mode == FLOAT || c->isfloat) && c->isfull) {
            XChangeProperty(dis, c->win, netatoms[NET_WM_STATE], XA_ATOM, 32, PropModeReplace, (unsigned char*)
                    ((c->isfull = False) ? &netatoms[NET_FULLSCREEN]:0), False);
            XMoveResizeWindow(dis, c->win, m->x + USELESSGAP, m->y + USELESSGAP + (TOP_PANEL && d->sbar ? PANEL_HEIGHT:0), 
                    m->w - 2*USELESSGAP - 2*BORDER_WIDTH, m->h - 2*USELESSGAP - 2*BORDER_WIDTH - (TOP_PANEL && d->sbar ? PANEL_HEIGHT:0));
            XSetWindowBorderWidth(dis, c->win,  BORDER_WIDTH);
        } else {
            XChangeProperty(dis, c->win, netatoms[NET_WM_STATE], XA_ATOM, 32, PropModeReplace, (unsigned char*)
                    ((c->isfull = False) ? &netatoms[NET_FULLSCREEN]:0), False);
            XSetWindowBorderWidth(dis, c->win,  BORDER_WIDTH);
        }
}


/**
 * set initial values
 */
void setup(void) {
    sigchld(0);

    /* screen and root window */
    const int screen = DefaultScreen(dis);
    root = RootWindow(dis, screen);

    /* initialize monitors and desktops */
    XineramaScreenInfo *info = XineramaQueryScreens(dis, &nmonitors);

    if (!nmonitors || !info)
        errx(EXIT_FAILURE, "Xinerama is not active");
    if (!(monitors = calloc(nmonitors, sizeof(Monitor))))
        err(EXIT_FAILURE, "cannot allocate monitors");

    for (int m = 0; m < nmonitors; m++) {
        monitors[m] = (Monitor){ .x = info[m].x_org, .y = info[m].y_org,
                                 .w = info[m].width, .h = info[m].height };
        for (unsigned int d = 0; d < DESKTOPS; d++)
            monitors[m].desktops[d] = (Desktop){ .mode = DEFAULT_MODE, .sbar = SHOW_PANEL };
    }
    XFree(info);

    /* init values for each monitor and desktop */
    for (unsigned int i = 0, m = init[0].m, d = init[0].d; i < LENGTH(init); i++, m = init[i].m, d = init[i].d) {
        monitors[m].desktops[d].mode = init[i].dl.mode;
        monitors[m].desktops[d].sbar = init[i].dl.sbar;
        monitors[m].desktops[d].masz = init[i].dl.masz;
    }

    /* get color for focused and unfocused client borders */
    win_focus = getcolor(FOCUS, screen);
    win_unfocus = getcolor(UNFOCUS, screen);
    win_infocus = getcolor(INFOCUS, screen);

    /* set numlockmask */
    XModifierKeymap *modmap = XGetModifierMapping(dis);
    for (int k = 0; k < 8; k++) for (int j = 0; j < modmap->max_keypermod; j++)
        if (modmap->modifiermap[modmap->max_keypermod*k + j] == XKeysymToKeycode(dis, XK_Num_Lock))
            numlockmask = (1 << k);
    XFreeModifiermap(modmap);

    /* set up atoms for dialog/notification windows */
    wmatoms[WM_PROTOCOLS]     = XInternAtom(dis, "WM_PROTOCOLS",     False);
    wmatoms[WM_DELETE_WINDOW] = XInternAtom(dis, "WM_DELETE_WINDOW", False);
    netatoms[NET_SUPPORTED]   = XInternAtom(dis, "_NET_SUPPORTED",   False);
    netatoms[NET_WM_STATE]    = XInternAtom(dis, "_NET_WM_STATE",    False);
    netatoms[NET_ACTIVE]      = XInternAtom(dis, "_NET_ACTIVE_WINDOW",       False);
    netatoms[NET_FULLSCREEN]  = XInternAtom(dis, "_NET_WM_STATE_FULLSCREEN", False);

    /* propagate EWMH support */
    XChangeProperty(dis, root, netatoms[NET_SUPPORTED], XA_ATOM, 32,
              PropModeReplace, (unsigned char *)netatoms, NET_COUNT);

    /* set the appropriate error handler
     * try an action that will cause an error if another wm is active
     * wait until events are processed to process the error from the above action
     * if all is good set the generic error handler */
    XSetErrorHandler(xerrorstart);
    /* set masks for reporting events handled by the wm */
    XSelectInput(dis, root, ROOTMASK);
    XSync(dis, False);
    XSetErrorHandler(xerror);
    XSync(dis, False);

    grabkeys();
    if (DEFAULT_DESKTOP >= 0 && DEFAULT_DESKTOP < DESKTOPS) change_desktop(&(Arg){.i = DEFAULT_DESKTOP});
    if (DEFAULT_MONITOR >= 0 && DEFAULT_MONITOR < nmonitors) change_monitor(&(Arg){.i = DEFAULT_MONITOR});
}

void sigchld(__attribute__((unused)) int sig) {
    if (signal(SIGCHLD, sigchld) != SIG_ERR) while(0 < waitpid(-1, NULL, WNOHANG));
    else err(EXIT_FAILURE, "cannot install SIGCHLD handler");
}

/**
 * execute a command
 */
void spawn(const Arg *arg) {
    if (fork()) return;
    if (dis) close(ConnectionNumber(dis));
    setsid();
    execvp((char*)arg->com[0], (char**)arg->com);
    err(EXIT_SUCCESS, "execvp %s", (char *)arg->com[0]);
}

/**
 * tile or common tiling aka v-stack mode/layout
 * bstack or bottom stack aka h-stack mode/layout
 */
void stack(int x, int y, int w, int h, const Desktop *d) {
    Client *c = NULL, *t = NULL; Bool b = (d->mode == BSTACK);
    int n = 0, p = 0, z = (b ? w:h), ma = (b ? h:w) * MASTER_SIZE + d->masz;

    canclefullscreen(d, &monitors[currmonidx]);
    /* count stack windows and grab first non-floating, non-fullscreen window */
    for (t = d->head; t; t = t->next) if (!ISFFT(t)) { if (c) ++n; else c = t; }

    /* if there is only one window (c && !n), it should cover the available screen space
     * if there is only one stack window, then we don't care about growth
     * if more than one stack windows (n > 1) adjustments may be needed.
     *
     *   - p is the num of pixels than remain when spliting the
     *       available width/height to the number of windows
     *   - z is each client's height/width
     *
     *      ----------  --.    ----------------------.
     *      |   |----| }--|--> sasz                  }--> first client will have
     *      |   | 1s |    |                          |    z+p+sasz height/width.
     *      | M |----|-.  }--> screen height (h)  ---'
     *      |   | 2s | }--|--> client height (z)    two stack clients on tile mode
     *      -----------' -'                         ::: ascii art by c00kiemon5ter
     *
     * what we do is, remove the sasz from the screen height/width and then
     * divide that space with the windows on the stack so all windows have
     * equal height/width: z = (z - sasz)/n
     *
     * sasz was left out (subtrackted), to later be added to the first client
     * height/width. before we do that, there will be cases when the num of
     * windows cannot be perfectly divided with the available screen height/width.
     * for example: 100px scr. height, and 3 stack windows: 100/3 = 33,3333..
     * so we get that remaining space and merge it to sasz: p = (z - sasz) % n + sasz
     *
     * in the end, we know each client's height/width (z), and how many pixels
     * should be added to the first stack client (p) so that it satisfies sasz,
     * and also, does not result in gaps created on the bottom of the screen.
     */
    if (c && !n) XMoveResizeWindow(dis, c->win, x + USELESSGAP, y + USELESSGAP,
           w - 2*(BORDER_WIDTH + USELESSGAP), h - 2*(BORDER_WIDTH + USELESSGAP));
    if (!c || !n) return; else if (n > 1) { p = (z - d->sasz)%n + d->sasz; z = (z - d->sasz)/n; }

    /* tile the first non-floating, non-fullscreen window to cover the master area */
    if (b) XMoveResizeWindow(dis, c->win, x + USELESSGAP, y + USELESSGAP,
    w - 2*(BORDER_WIDTH + USELESSGAP), ma - 2*(BORDER_WIDTH + USELESSGAP));
    else   XMoveResizeWindow(dis, c->win, x + USELESSGAP, y + USELESSGAP,
    ma - 2*(BORDER_WIDTH + USELESSGAP), h - 2*(BORDER_WIDTH + USELESSGAP));

    /* tile the next non-floating, non-fullscreen (and first) stack window adding p */
    for (c = c->next; c && ISFFT(c); c = c->next);
    int ch = z - 2*BORDER_WIDTH - USELESSGAP, cw = (b ? h:w) - 2*BORDER_WIDTH - ma - USELESSGAP;
    if (b) XMoveResizeWindow(dis, c->win, x += USELESSGAP, y += ma, ch - USELESSGAP + p, cw);
    else   XMoveResizeWindow(dis, c->win, x += ma, y += USELESSGAP, cw, ch - USELESSGAP + p);

    /* tile the rest of the non-floating, non-fullscreen stack windows */
    for (b ? (x += z+p-USELESSGAP):(y += z+p-USELESSGAP), c = c->next; c; c = c->next) {
        if (ISFFT(c)) continue;
        if (b) { XMoveResizeWindow(dis, c->win, x, y, ch, cw); x += z; }
        else   { XMoveResizeWindow(dis, c->win, x, y, cw, ch); y += z; }
    }
}

/**
 * swap master window with current.
 * if current is head swap with next
 * if current is not head, then head
 * is behind us, so move_up until we
 * are the head
 */
void swap_master(void) {
    Desktop *d = &monitors[currmonidx].desktops[monitors[currmonidx].currdeskidx];
    if (!d->curr || !d->head->next) return;
    if (d->curr == d->head) move_down();
    else while (d->curr != d->head) move_up();
    focus(d->head, d, &monitors[currmonidx]);
}

/**
 * switch tiling mode/layout
 *
 * if mode is reselected reset all floating clients
 * if mode is FLOAT set all clients floating
 */
void switch_mode(const Arg *arg) {
    Desktop *d = &monitors[currmonidx].desktops[monitors[currmonidx].currdeskidx];
    if (d->mode != arg->i) d->mode = arg->i;
    else if (d->mode != FLOAT) for (Client *c = d->head; c; c = c->next) c->isfloat = False;
    if (d->head) { tile(d, &monitors[currmonidx]); focus(d->curr, d, &monitors[currmonidx]); }
    desktopinfo();
}

/**
 * tile clients of the given desktop with the desktop's mode/layout
 * call the tiling handler fucntion taking account the panel height
 */
void tile(Desktop *d, Monitor *m) {
    if (!d->head || d->mode == FLOAT) {
        canclefullscreen(d, &monitors[currmonidx]);
        return; /* nothing to arange */
    }
    layout[d->head->next ? d->mode:MONOCLE](m->x, m->y + (TOP_PANEL && d->sbar ? PANEL_HEIGHT:0),
                                            m->w, m->h - (d->sbar ? PANEL_HEIGHT:0), d);
}

/**
 * toggle visibility state of the panel/bar
 */
void togglepanel(void) {
    Monitor *m = &monitors[currmonidx];
    m->desktops[m->currdeskidx].sbar = !m->desktops[m->currdeskidx].sbar;
    tile(&m->desktops[m->currdeskidx], m);
}

/**
 * windows that request to unmap should lose their client
 * so invisible windows do not exist on screen
 */
void unmapnotify(XEvent *e) {
    Monitor *m = NULL; Desktop *d = NULL; Client *c = NULL;
    if (wintoclient(e->xunmap.window, &c, &d, &m)) removeclient(c, d, m);
}

/**
 * find to which client and desktop the given window belongs to
 */
Bool wintoclient(Window w, Client **c, Desktop **d, Monitor **m) {
    for (int cm = 0; cm < nmonitors && !*c; cm++)
        for (int cd = 0; cd < DESKTOPS && !*c; cd++)
            for (*m = &monitors[cm], *d = &(*m)->desktops[cd], *c = (*d)->head; *c && (*c)->win != w; *c = (*c)->next);
    return (*c != NULL);
}

/**
 * There's no way to check accesses to destroyed windows,
 * thus those cases are ignored (especially on UnmapNotify's).
 */
int xerror(__attribute__((unused)) Display *dis, XErrorEvent *ee) {
    if ((ee->error_code == BadAccess   && (ee->request_code == X_GrabKey
                                       ||  ee->request_code == X_GrabButton))
    || (ee->error_code  == BadMatch    && (ee->request_code == X_SetInputFocus
                                       ||  ee->request_code == X_ConfigureWindow))
    || (ee->error_code  == BadDrawable && (ee->request_code == X_PolyFillRectangle
    || ee->request_code == X_CopyArea  ||  ee->request_code == X_PolySegment
                                       ||  ee->request_code == X_PolyText8))
    || ee->error_code   == BadWindow) return 0;
    err(EXIT_FAILURE, "xerror: request: %d code: %d", ee->request_code, ee->error_code);
}

/**
 * error handler function to display an appropriate error message
 * when the window manager initializes (see setup - XSetErrorHandler)
 */
int xerrorstart(__attribute__((unused)) Display *dis, __attribute__((unused)) XErrorEvent *ee) {
    errx(EXIT_FAILURE, "xerror: another window manager is already running");
}

int main(int argc, char *argv[]) {
    if (argc == 2 && !strncmp(argv[1], "-v", 3))
        errx(EXIT_SUCCESS, "version: %s - by c00kiemon5ter >:3 omnomnomnom", VERSION);
    else if (argc != 1) errx(EXIT_FAILURE, "usage: man monsterwm");
    if (!(dis = XOpenDisplay(NULL))) errx(EXIT_FAILURE, "cannot open display");
    setup();
    desktopinfo(); /* zero out every desktop on (re)start */
    run();
    cleanup();
    XCloseDisplay(dis);
    return retval;
}

/* vim: set expandtab ts=4 sts=4 sw=4 : */