#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <time.h>

#include <fcntl.h>

#include <linux/input.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <unistd.h>
#include <ucontext.h> /* we need to do some low-level context switching for the gmh_sleep implementation */

#include <pthread.h>

#include <mapped-codes.h> /* generated mappings from buildtool, using input-event-codes.h */

#include <gmacros.h>

#include <libgmacros.h>

@ {
    typedef struct gm_macro_node {
        struct gm_macro_node* next;
        gm_macro* macro;
        unsigned int keycode;
        struct {
            ucontext_t context;
            uint8_t stack[1024 * 1024];
            int req_sleep_time;
            volatile bool running;     /* used by wrapper         */
            bool returned;             /* used by gmi_sleep, wait */
            bool waiting;              /* used by wait            */
            struct wrapper_data* data; /* used by wait            */
        } routine;
    } gm_macro_node;

    typedef struct gmi_latch {
        gm_macro_node** links;
        size_t linksz;
        size_t idx;
        bool state; /* true for open, false for closed */
    } gmi_latch;
    
    typedef struct lnode {
        struct lnode* next;
        void (*f) (void*);
        long target; /* millis, 0 for immediate */
        void* arg;
    } lnode;

    /* internal handle data */
    typedef struct {
        const char* dev;
        gm_macro* active;
    
        pthread_cond_t chain_cond;
        pthread_mutex_t chain_lock;
    
        lnode* chain;
    
        Display* display;
    
        pthread_t thread;
        pthread_t lthread;
        volatile bool lthread_control;

        bool flush;
    
        int fd; /* device fd */

        gm_macro_node* macro_chain;

        volatile bool listening;

        ucontext_t context;

        gm_macro_node* active_handler;

        struct sigaction sa;

        void* lstate;

        const gm_settings* settings;
    } gmi_handle;
}

const gm_settings gm_default_settings = {
    .sched_intval = 50
};

#ifndef DEBUG_MODE
#define DEBUG_MODE 0
#endif

#define SCHED(h, d, ...)                                                 \
    chain_register_eventd(h, ({ void _fn(void* _ignored) __VA_ARGS__; _fn; }), d, NULL);

#define SCHED_A(h, d, a, n, ...)                                         \
    chain_register_eventd(h, ({ void _fn(void* n) __VA_ARGS__; _fn; }), d, a);

#define X11_KEYSYM(D, S) ((unsigned int) XKeysymToKeycode(D, XStringToKeysym(S)))

static void chain_register_eventd(gmi_handle* h, void (*f) (void*), long delay, void* arg);
/* static void chain_debug(gmi_handle* h); */

int gm_register(gm_handle _h, gm_macro* macro) {
    gmi_handle* h = (gmi_handle*) _h;

    if (h->listening) return 2;

    /* match keycode with string */
    unsigned int code;
    bool match = false;
    size_t t;
    for (t = 0; t < sizeof(gm_mapped) / sizeof(*gm_mapped); ++t) {
        if (!strcmp(gm_mapped[t].name, macro->key)) {
            code = gm_mapped[t].code;
            match = true;
            break;
        }
    }
    if (!match) return 1;
    
    #if DEBUG_MODE
    printf("gm_register(): matched macro->key (%s) to code %d\n", macro->key, (int) code);
    #endif
    
    gm_macro_node* end = h->macro_chain;
    while (end != NULL && end->next != NULL)
        end = end->next;
    
    gm_macro_node** new = end == NULL ? &h->macro_chain : &(end->next); /* handle NULL chain */
    
    *new = malloc(sizeof(struct gm_macro_node));
    (*new)->keycode = code;
    (*new)->macro = macro;
    (*new)->next = NULL;
    (*new)->routine.running = false;
    
    return 0;
}

int gm_unregister(gm_handle _h, gm_macro* macro) {
    gmi_handle* h = (gmi_handle*) _h;

    if (h->listening) return 2;
    
    gm_macro_node* c, * prev = NULL;
    for (c = h->macro_chain; c != NULL; c = c->next) {
        if (c->macro == macro) {
            if (prev == NULL) {
                h->macro_chain = c->next ? c->next : NULL;
            } else {
                prev->next = c->next;
            }
            free(c);
            return 0;
        }
        prev = c;
    }
    return 1;
}

int gm_unregister_all(gm_handle _h) {
    gmi_handle* h = (gmi_handle*) _h;

    if (h->listening) return 2;
    
    gm_macro_node* c;
    for (c = h->macro_chain; c != NULL;) {
        gm_macro_node* tmp = c;
        c = c->next;
        free(tmp);
    }
    h->macro_chain = NULL;
    
    return 0;
}

static void gm_routine(int value, gm_macro_node* c) {
    c->macro->f(value, c->macro->arg);
    c->routine.running = false;
}

/* data that is passed around with this handler */
struct wrapper_data {
    gm_macro_node* c;
    gmi_handle* h;
};

static void gm_wrapper(void* w) {
    gm_macro_node* c = ((struct wrapper_data*) w)->c;
    gmi_handle* h = ((struct wrapper_data*) w)->h;
                            
    h->active_handler = c;
                        
    c->routine.req_sleep_time = 0;
    c->routine.waiting = false;
                            
    getcontext(&h->context); /* save current (return) context */

    if (c->routine.running) {
        if (c->routine.req_sleep_time == 0) {

            if (c->routine.waiting) {
                /*
                  a wait was triggered, so just store the wrapper (argument) data
                  so we can wake up this macro later
                 */
                c->routine.data = (struct wrapper_data*) w;
            } else {
                /*
                  if no sleep or wait was requested, this is probably the first
                  invocation or a resume, so just execute in the current
                  scheduled context.
                */
                setcontext(&c->routine.context);
            }
        } else {
            /*
              if we got here, a sleep was requested (jumped back to main context
              with c->routine->req_sleep_time set), so we need to schedule again
              to continue this context later.
            */
            chain_register_eventd(h, &gm_wrapper, c->routine.req_sleep_time, w);
        }
    } else {
        #if DEBUG_MODE
        printf("end of event, freeing struct wrapper_data (%p)\n", w);
        #endif
        free(w);
    }
}

static void gm_routine_entry(gmi_handle* h, gm_macro_node* c, int value) {
                    
    if (c->routine.running) return; /* ignore if the macro is already executing */
    c->routine.running = true;
                    
    #if DEBUG_MODE
    printf("executing macro (%p) for keycode %d\n", c->macro, (int) c->keycode);
    #endif
                    
    /* setup new stack and context for this handler */
    c->routine.req_sleep_time = 0;
                    
    getcontext(&c->routine.context);
                    
    c->routine.context.uc_link = &h->context;
    c->routine.context.uc_stack = (stack_t) {
        .ss_sp = c->routine.stack,
        .ss_size = sizeof(c->routine.stack)
    };

    /*
      TODO: fix this so it works on other versions of libc, currently only works
      on glibc due to passing non int-sized value to context.
    */
    makecontext(&c->routine.context, (void (*)()) gm_routine, 2, value, c);

    struct wrapper_data* w = malloc(sizeof(struct wrapper_data));
    *w = (struct wrapper_data) { .c = c, .h = h };
                        
    /* wrapper function for executing user code in scheduler (recursive) */
    chain_register_eventd(h, &gm_wrapper, 0, w);
}

static void* listen(void* _h) {
    gmi_handle* h = (gmi_handle*) _h;
    
    struct input_event ev;
    ssize_t n;

    if ((h->fd = open(h->dev, O_RDONLY)) == -1) {
        fprintf(stderr, "open(): %s\n", strerror(errno));
        return NULL;
    }

    
    while (h->lthread_control) {
        n = read(h->fd, &ev, sizeof(ev));
        if (!h->lthread_control) break;
        if (n == (ssize_t) -1) { 
            if (errno == EINTR) continue;
            else break;
        } else if (n != sizeof(ev)) {
            errno = EIO;
            break;
        }
        /* ev.value: 0 release, 1 press, 2 repeat */
        if (h->listening && ev.type == EV_KEY) {
            /*
              We have some lightweight coroutining going on here, we provide
              time for other events in the event chain to execute while
              gmh_sleep is called.
            */

            /* cycle though macro chain */
            gm_macro_node* c;
            for (c = h->macro_chain; c != NULL; c = c->next) {
                /* if we kind a matching keycode, execute */
                if (ev.code == c->keycode) {
                    gm_routine_entry(h, c, ev.value);
                }
            }
        }
    }
    #if DEBUG_MODE
    printf("exited listen()\n");
    #endif
    return NULL;
}

    
struct gm_pass_data {
    void (*f)(void* udata);
    void* udata;
    gm_macro_node* node;
    gmi_handle* h;
};

static void gm_sched_wrapper(int ignored, void* _pd) {
    struct gm_pass_data* d = (struct gm_pass_data*) _pd;
                    
    d->f(d->udata);
                    
    /*
      cleanup everything after this is finished executing. We have
      to re-schedule this so it happens in the main context, so we're
      not wiping our own stack.
    */
    void _fn(void* _pd) {
        struct gm_pass_data* d = (struct gm_pass_data*) _pd;
        free(d->node->macro);
        free(d->node);
        free(d);
    }
                    
    chain_register_eventd(d->h, &_fn, 0, d);
}

/* registers a dummy macro and immediately executes it */
void gm_sched(gm_handle _h, void (*f)(void* udata), void* udata) {
    gmi_handle* h = (gmi_handle*) _h;
    
    struct gm_macro_node* new = malloc(sizeof(struct gm_macro_node));
    
    new->keycode = 0;
    new->macro = malloc(sizeof(gm_macro));
    
    struct gm_pass_data* pd = malloc(sizeof(struct gm_pass_data));
    *pd = (struct gm_pass_data) {
        .f = f, .udata = udata, .node = new, .h = h
    };

    /* dummy macro routine */
    *(new->macro) = (gm_macro) {
        .arg = pd,
        .f = gm_sched_wrapper,
        .key = ""
    };

    /* this isn't part of any macro chain */
    new->next = NULL;
    
    new->routine.running = false;
    
    /* immediately start execution */
    gm_routine_entry(h, new, 0);
}

static void chain_register_event(lnode** chain_p, void (*f) (void*), long target, void* arg);

/* register event with delay, locking on main chain */
static void chain_register_eventd(gmi_handle* h, void (*f) (void*), long delay, void* arg) {
    #if DEBUG_MODE
    printf("reg: %d\n", (int) delay);
    #endif
    
    pthread_mutex_lock(&h->chain_lock);
    if (delay) {
        struct timespec tm;
        clock_gettime(CLOCK_REALTIME, &tm);
        long tml = ((long) (tm.tv_sec * 1000L)) + (tm.tv_nsec / (1000L * 1000L));
        chain_register_event(&h->chain, f, tml + delay, arg);
    } else {
        chain_register_event(&h->chain, f, 0, arg);
    }
    pthread_mutex_unlock(&h->chain_lock);
    pthread_cond_signal(&h->chain_cond);
}

/*
static void chain_debug(gmi_handle* h) {
    printf("dumping chain...\n");
    pthread_mutex_lock(&h->chain_lock);
    int i = 0;
    lnode* c;
    for (c = h->chain; c != NULL; c = c->next) {
        printf("%d: [ f: %p, target: %ld]\n", i, c->f, c->target);
        ++i;
    }
    printf("sz: %d\n", i);
    pthread_mutex_unlock(&h->chain_lock);
}
*/

/* register event */
static void chain_register_event(lnode** chain_p, void (*f) (void*), long target, void* arg) {
    /* find end */
    lnode* end = *chain_p;
    while (end != NULL && end->next != NULL)
        end = end->next;

    #if DEBUG_MODE
    if (end == NULL)
        printf("registering at head\n");
    else 
        printf("appending to chain\n");
    #endif
    
    lnode** new = end == NULL ? chain_p : &(end->next); /* handle NULL chain */
    
    *new = malloc(sizeof(struct lnode));
    (*new)->f = f;

    (*new)->target = target;
    (*new)->arg = arg;
    (*new)->next = NULL;
}

/* cycle through chain, returning true if there is an event needing execution */
static bool chain_contains_events(lnode* chain, long* wait) {
    lnode* c;
    *wait = -1;
    for (c = chain; c != NULL; c = c->next) {
        
        if (c->target == 0)
            return true;
        struct timespec tm;
        clock_gettime(CLOCK_REALTIME, &tm);
        long tml = ((long) (tm.tv_sec * 1000L)) + (tm.tv_nsec / (1000L * 1000L));
        if (c->target <= tml) {
            return true;
        } else {
            /* pass the amount of time we should wait until the next event */
            if (*wait < c->target - tml)
                *wait = c->target - tml;
        }
    }
    return false;
}

/* copy events that aren't ready to be executed to a new list */
static void chain_copy_waiting(lnode** chain_dest, lnode* source) {
    lnode* c;
    for (c = source; c != NULL; c = c->next) {

        #if DEBUG_MODE
        printf("copy iter: %p\n", c);
        #endif
        
        if (c->target == 0)
            continue;
        struct timespec tm;
        clock_gettime(CLOCK_REALTIME, &tm);
        long tml = ((long) (tm.tv_sec * 1000L)) + (tm.tv_nsec / (1000L * 1000L));
        if (c->target > tml) {
            #if DEBUG_MODE
            printf("copying pending event: %p\n", c);
            #endif
            chain_register_event(chain_dest, c->f, c->target, c->arg);
        }
    }
}

/* cycle through chain, executing pending events and purging them from the chain */
/* sweep - free the entire chain instead of purging individual nodes */
static bool chain_cycle_events(lnode** chain_p, bool sweep) {
    void* tofree = NULL;
    lnode* prev = NULL;
    lnode* c;
    for (c = *chain_p; c != NULL; c = c->next) {

        #if DEBUG_MODE
        printf("iter: %p\n", c);
        #endif
        
        /* if we needed to free a node on the last iteration, do it now */
        if (tofree) {
            free(tofree);
            tofree = NULL;
        }
        
        struct timespec tm;
        long tml;
        
        if (c->target == 0) {
            
            #if DEBUG_MODE
            printf("exec (inst): %p\n", c->f);
            #endif
            
            c->f(c->arg);
            goto purge;
        }
        clock_gettime(CLOCK_REALTIME, &tm);
        tml = ((long) (tm.tv_sec * 1000L)) + (tm.tv_nsec / (1000L * 1000L));
        if (c->target <= tml) {
            
            #if DEBUG_MODE
            printf("exec (target: %ld, now: %ld): %p\n", c->target, tml, c->f);
            #endif
            
            c->f(c->arg);
            
            /*
              purge the node. We free on the next iteration,
              since we still need to read from the node
            */
        purge:
            if (sweep) continue;
            if (prev != NULL) {
                prev->next = c->next; /* remove the node and link the previous to the next */
                tofree = c;
            } else {
                *chain_p = c->next; /* reassign the start of the list */
                tofree = c;
            }
        }
        prev = c; /* store previous node */
    }
    
    if (tofree) {
        free(tofree);
        tofree = NULL;
    }
    
    if (sweep) {
        for (c = *chain_p; c != NULL;) {
            lnode* tmp = c;
            c = c->next;
            free(tmp);
        }
        *chain_p = NULL;
    }
    return false;
}

static void* gm_sched_entry(void* arg) {
    gmi_handle* h = (gmi_handle*) arg;
    while (h->lthread_control) {
        /* we're operating on the main chain, we need to lock it */
        pthread_mutex_lock(&h->chain_lock);

        long wait;
        /* wait for wakeup if the chain is empty */
        while (!chain_contains_events(h->chain, &wait)) {
            struct timeval tv;
            struct timespec ts;
            
            gettimeofday(&tv, NULL);

            /* wait until the next event, or just the default interval if there are no events */
            long delay = wait > 0 ? wait : h->settings->sched_intval;
            
            ts.tv_sec = time(NULL) + delay / 1000;
            ts.tv_nsec = tv.tv_usec * 1000 + 1000 * 1000 * (delay % 1000);
            ts.tv_sec += ts.tv_nsec / (1000 * 1000 * 1000);
            ts.tv_nsec %= (1000 * 1000 * 1000);
            
            pthread_cond_timedwait(&h->chain_cond, &h->chain_lock, &ts);
            if (!h->lthread_control) {
                pthread_mutex_unlock(&h->chain_lock);
                return NULL;
            }
            #undef DELAY
        }
        lnode* localchain = h->chain;
        h->chain = NULL;
        chain_copy_waiting(&h->chain, localchain); /* copy over events that won't be executed this cycle */

        /* we will be operating on a local chain now, so we can unlock */
        pthread_mutex_unlock(&h->chain_lock);
        
        chain_cycle_events(&localchain, true); /* cycle events and free the entire chain */
    }
    return NULL;
}

static void gm_emptyhandler(int ignored) {}

gm_handle gm_init(const char* devpath, const gm_settings* settings) {

    #if DEBUG_MODE
    printf("initializing libgmacros for device: %s\n", devpath);
    #endif

    gmi_handle* h = malloc(sizeof(gmi_handle));
    *h = (gmi_handle) {
        .dev         = devpath,
        .active      = NULL,
        .chain_cond  = PTHREAD_COND_INITIALIZER,
        .chain_lock  = PTHREAD_MUTEX_INITIALIZER,
        .chain       = NULL,
        .macro_chain = NULL,
        .display     = NULL,
        .listening   = false,
        .flush       = true,
        .sa = { .sa_handler = &gm_emptyhandler },
        .settings = settings ? settings : &gm_default_settings
    };

    sigemptyset(&h->sa.sa_mask);
    
    sigaction(SIGUSR1, &h->sa, NULL);
    
    if (!(h->display = XOpenDisplay(NULL))) {
        fprintf(stderr, "failed to find display (NULL)\n");
        exit(EXIT_FAILURE);
    }

    int ret = pthread_create(&h->thread, NULL, &listen, h);
    if (ret) {
        fprintf(stderr, "pthread_create(): %d\n", ret);
        exit(EXIT_FAILURE);
    }

    
    h->lthread_control = true;
    
    pthread_create(&h->lthread, NULL, &gm_sched_entry, h);

    return h;
}

void gmh_sleep(gm_handle _h, int ms) {
    #if DEBUG_MODE
    printf("sleep called! (%d)\n", ms);
    #endif
    gmi_handle* h = (gmi_handle*) _h;
    h->active_handler->routine.req_sleep_time = ms;
    h->active_handler->routine.returned = false;
    getcontext(&h->active_handler->routine.context); /* save current context to restore into */
    if (!h->active_handler->routine.returned) { /* flag to check if we already returned from here */
        h->active_handler->routine.returned = true;
        setcontext(&h->context); /* return to the point in which we last set h->context (wrapper func) */
    }
}

void gmh_wait(gm_handle _h, gm_latch _l) {
    #if DEBUG_MODE
    printf("wait called! (%p)\n", _l);
    #endif
    gmi_handle* h = (gmi_handle*) _h;
    gmi_latch* l = (gmi_latch*) _l;

    if (l->state == true) return;
    
    if (l->idx >= l->linksz) {
        l->linksz *= 2;
        l->links = realloc(l->links, l->linksz);
    }
    
    l->links[l->idx] = h->active_handler;
    ++l->idx;
    
    h->active_handler->routine.waiting = true;
    h->active_handler->routine.returned = false;
    getcontext(&h->active_handler->routine.context); /* save context */
    if (!h->active_handler->routine.returned) { /* check return */
        h->active_handler->routine.returned = true;
        setcontext(&h->context);
    }
}

gm_latch gm_latch_new(void) {
    gmi_latch* l = malloc(sizeof(struct gmi_latch));
    l->links = malloc((l->linksz = 4));
    l->state = false;
    l->idx = 0;
    return l;
}

void gm_latch_destroy(gm_latch _l) {
    gmi_latch* l = (gmi_latch*) _l;
    free(l->links);
    free(l);
}

void gmh_latch_open(gm_handle h, gm_latch _l) {
    gmi_latch* l = (gmi_latch*) _l;
    l->state = true;
    size_t t;
    for (t = 0; t < l->idx; ++t) {
        chain_register_eventd((gmi_handle*) h, &gm_wrapper, 0, l->links[t]->routine.data);
    }
    l->idx = 0;
}

void gmh_latch_reset(gm_handle ignored, gm_latch _l) {
    ((gmi_latch*) _l)->state = false;
}

void gm_start(gm_handle _h) {
    gmi_handle* h = (gmi_handle*) _h;
    h->listening = true;
}

void gm_stop(gm_handle _h) {
    gmi_handle* h = (gmi_handle*) _h;
    h->listening = false;
}

void gm_close(gm_handle _h) {
    gmi_handle* h = (gmi_handle*) _h;
    h->lthread_control = false;
    pthread_cond_signal(&h->chain_cond); /* wakeup */
    pthread_kill(h->thread, SIGUSR1);    /* send dummy signal to break out of read() call */
    pthread_join(h->lthread, NULL);
    pthread_join(h->thread, NULL);
    XCloseDisplay(h->display);
}

void gmh_key(gm_handle _h, int press, const char* key) {
    gmi_handle* h = (gmi_handle*) _h;
    XTestFakeKeyEvent(h->display, X11_KEYSYM(h->display, key), press, 0);
    if (h->flush) XFlush(h->display);
}

void gmh_mouse(gm_handle _h, int press, unsigned int button) {
    if (button == 0) return; /* for some reason X freaks out if we ask for button 0 */
    gmi_handle* h = (gmi_handle*) _h;
    XTestFakeButtonEvent(h->display, button, press == 1 ? true : false, CurrentTime);
    if (h->flush) XFlush(h->display);
}

void gmh_move(gm_handle _h, int x, int y) {
    gmi_handle* h = (gmi_handle*) _h;
    XTestFakeMotionEvent(h->display, DefaultScreen(h->display), x, y, 0);
    if (h->flush) XFlush(h->display);
}

void gmh_getmouse(gm_handle _h, int* x, int* y) {
    gmi_handle* h = (gmi_handle*) _h;
    
    XEvent e;
    XQueryPointer(h->display, RootWindow(h->display, DefaultScreen(h->display)),
                  &e.xbutton.root, &e.xbutton.window,
                  &e.xbutton.x_root, &e.xbutton.y_root,
                  &e.xbutton.x, &e.xbutton.y,
                  &e.xbutton.state);

    *x = e.xbutton.x;
    *y = e.xbutton.y;
}

void gmh_flush(gm_handle _h, int toggle) {
    gmi_handle* h = (gmi_handle*) _h;
    h->flush = toggle ? true : false;
    if (toggle)
        XFlush(h->display);
}
