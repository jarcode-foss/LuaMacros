
#ifndef GMACROS_H
#define GMACROS_H

#ifdef __GNUC__
#define GM_API __attribute__((visibility("default")))
#else
#error no visibility macros for the target compiler have been defined
#endif

typedef void* gm_handle; /* opaque library handle type */
typedef void* gm_latch; /* opaque latch type        */

typedef struct {
    void* arg;                   /* user argument    */
    void (*f)(int value, void*); /* handler function (value: 0 release, 1 press, 2 repeat) */
    
    const char* key;  /*
                        single, null terminated character in the system
                        encoding, or a special character (DEL, INS, etc)
                      */
                     
} gm_macro;

typedef struct {
    long sched_intval; /* Maximum scheduler interval (ms) in which the scheduler must check for
                          pending events. Does not effect gm_sleep call accuracy or initial macro
                          response time (due to calculated thread sleep times and broadcasts). */
} gm_settings;

extern const gm_settings gm_default_settings; /* default settings */


/*
  Initialize the library with the provided device from /dev/input. An example of
  valid input (for a specific system) is shown below:
  
  h = gm_init("/dev/input/by-path/pci-0000:00:1d.0-usb-0:1.6.3:1.0-event-kbd", NULL);
*/
GM_API gm_handle gm_init  (const char* devpath, const gm_settings* settings);

GM_API void      gm_close (gm_handle h); /* close handle */

GM_API void      gm_start (gm_handle h); /* start listening for any registered macros */
GM_API void      gm_stop  (gm_handle h); /* stop listening for any registered macros  */

/*
  the gm_macro struct passed to the register function must be filled accordingly:
  
  gm_macro m = {
      .arg = NULL,                                      -- any argument passed to the handler
                                                        --
      .f   = (void (*) (void*)) &my_handler_function,   -- handler function, where all calls to
                                                        -- the gmh_XXX fuctions should be placed
                                                        --
      .key = "a"                                        -- key that should trigger the handler
  };
*/
GM_API int gm_register       (gm_handle h, gm_macro* macro); /* register macro (returns non-zero for error) */
GM_API int gm_unregister     (gm_handle h, gm_macro* macro); /* unregister an existing macro                */
GM_API int gm_unregister_all (gm_handle h);                  /* unregister all macros for this handle       */

/* safely execute handler commands (through the scheduler) without a key binding */
GM_API void gm_sched (gm_handle h, void (*f)(void* d), void* d);

/* below functions to be executed in the handler */

GM_API void gmh_key      (gm_handle h, int press, const char* key);         /* simulate key            */
GM_API void gmh_mouse    (gm_handle h, int press, unsigned int button);     /* simulate mouse button   */
GM_API void gmh_move     (gm_handle h, int x, int y);                       /* simulate mouse move     */
GM_API void gmh_getmouse (gm_handle h, int* x, int* y);                     /* store mouse position    */

GM_API void gmh_sleep    (gm_handle h, int ms);                             /* sleep for milliseconds  */
GM_API void gmh_wait     (gm_handle h, gm_latch l);                         /* wait until open         */

GM_API void gmh_flush    (gm_handle h, int toggle);                         /* toggle flushing, performs
                                                                               a flush when toggled on  */

/*
  latch functions -- latches can be created and destroyed from anywhere, but should
  only be opened and reset from handler (scheduled) code
*/
GM_API gm_latch gm_latch_new     (void);
GM_API void     gm_latch_destroy (gm_latch l);

GM_API void     gmh_latch_open    (gm_handle h, gm_latch l);
GM_API void     gmh_latch_reset   (gm_handle h, gm_latch l);

#endif
