#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <lua.h>
#include <lauxlib.h>

#include <unistd.h>
#include <ucontext.h>
#include <X11/Xlib.h>

#include <gmacros.h>
#include <libgmacros.h>

#define STATE(H) ((lua_State*) (((gmi_handle*) H)->lstate))

#define LHANDLER(L)                                         \
    ({                                                      \
        lua_getglobal(L, "__gm_handler");                   \
        gm_handle h = (gm_handle) lua_touserdata(L, -1);    \
        lua_pop(L, 1);                                      \
        h;                                                  \
    })

#define PUSHFUNC(L, N, F)                       \
    do {                                        \
        lua_pushstring(L, N);                   \
        lua_pushcfunction(L, F);                \
        lua_rawset(L, -3);                      \
    } while (0)

struct gml_settings_accessor {
    const char* key;
    void (*set)(gm_settings* s);
};

#define ST_F(K, ...) { .key = #K, .set = ({ void _f(gm_settings* s) __VA_ARGS__; _f; }) }
#define ST_INT(K) ST_F(K, { s->K = lua_tointeger(L, -1); })

#define ST_SETTINGS_KEYS { ST_INT(sched_intval) }

static int gml_flush(lua_State* L) {
    gm_handle h = LHANDLER(L);
    if (lua_isboolean(L, -1)) {
        gmh_flush(h, lua_toboolean(L, -1));
    } else luaL_error(L, "gml_flush(): expected (boolean)");
    return 0;
}

static int gml_key(lua_State* L) {
    gm_handle h = LHANDLER(L);
    if (lua_isboolean(L, -2) && lua_isstring(L, -1)) {
        const char* key = lua_tostring(L, -1);
        int press = lua_toboolean(L, -2);
        gmh_key(h, press, key);
    } else luaL_error(L, "gml_key(): expected (boolean, string)");
    return 0;
}

static int gml_mouse(lua_State* L) {
    gm_handle h = LHANDLER(L);
    if (lua_isboolean(L, -2) && lua_isinteger(L, -1)) {
        unsigned int button = (unsigned int) lua_tointeger(L, -1);
        int press = lua_toboolean(L, -2);
        gmh_mouse(h, press, button);
    } else luaL_error(L, "gml_mouse(): expected (boolean, integer)");
    return 0;
}

static int gml_move(lua_State* L) {
    gm_handle h = LHANDLER(L);
    if (lua_isinteger(L, -2) && lua_isinteger(L, -1)) {
        int x = lua_tointeger(L, -2);
        int y = lua_tointeger(L, -1);
        gmh_move(h, x, y);
    } else luaL_error(L, "gml_move(): expected (integer, integer)");
    return 0;
}

static int gml_getmouse(lua_State* L) {
    gm_handle h = LHANDLER(L);
    int x, y;
    gmh_getmouse(h, &x, &y);
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    return 2;
}

static int gml_sleep(lua_State* L) {
    gm_handle h = LHANDLER(L);
    if (lua_isinteger(L, -1)) {
        int ms = lua_tointeger(L, -1);
        if (ms > 0) {
            gmh_sleep(h, ms);
        } else luaL_error(L, "gml_sleep(): expected first argument larger than 0");
    } else luaL_error(L, "gml_sleep(): expected (integer)");
    return 0;
}

struct wrapper_data {
    lua_State* L;
    int f_idx;
    gm_macro m;
};

static void gml_wrapper(int value, void* arg) {
    lua_State* M = ((struct wrapper_data*) arg)->L;
    int f_idx = ((struct wrapper_data*) arg)->f_idx;
    
    lua_State* L = lua_newthread(M); /* create new stack */
    int pos = lua_gettop(M);         /* position of this thread in the main stack */

    lua_getglobal(L, "__gm_reg");
    if (!lua_istable(L, -1)) {
        printf("gml_wrapper(): __gm_reg is not a table");
        lua_pop(L, 1);
        return;
    }
    lua_rawgeti(L, -1, f_idx);

    if (!lua_isfunction(L, -1)) {
        printf("gml_wrapper(): tried to execute function at invalid index: %d\n", f_idx);
        lua_pop(L, 2);
        return;
    }

    lua_pushinteger(L, value);
    
    switch (lua_pcall(L, 1, 0, 0)) {
    case LUA_ERRRUN:
        printf("gml_wrapper(): runtime error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        break;
    case LUA_ERRMEM:
        printf("gml_wrapper(): allocation error\n");
        break;
    case LUA_ERRERR:
        printf("gml_wrapper(): error while handling error\n");
        break;
    }

    lua_pop(L, 1); /* pop table */
    
    /*
      Since we can break execution in the middle of this wrapper and have
      new threads pushed onto the main stack, we kept track of the stack
      position of our thread object. We need to replace it with nil (so
      the stack doesn't overflow and it can be collected), and then
      cleanup the stack without shifting the positions of other threads
    */
    
    lua_pushnil(M);
    lua_replace(M, pos); /* remove thread from main stack */
    
    /* walk through stack and remove all nils before an active thread */
    int t, a = 0;
    for (t = lua_gettop(M); t >= 1; --t) {
        if (lua_isnil(M, t)) ++a;
        else break;
    }
    if (a) lua_pop(M, a);
}

static int gml_register(lua_State* L) {
    gm_handle h = LHANDLER(L);
    if (lua_isstring(L, -2) && lua_isfunction(L, -1)) {
        const char* lkey = lua_tostring(L, -2);
        
        lua_getglobal(L, "__gm_idx");
        if (!lua_isnumber(L, -1)) {
             luaL_error(L, "gml_register(): __gm_idx is not a number");
        }
        int idx = lua_tonumber(L, -1);
        lua_getglobal(L, "__gm_reg");
        if (!lua_istable(L, -1)) {
             luaL_error(L, "gml_register(): __gm_reg is not a table");
        }

        /* f, idx, table */
        
        lua_pushvalue(L, -3); /* copy function to top (f, idx, table, f)*/
        lua_rawseti(L, -2, idx);
        
        lua_pushinteger(L, idx + 1);
        lua_setglobal(L, "__gm_idx");
        
        size_t sz = strlen(lkey);
        struct wrapper_data* d = lua_newuserdata(L, sizeof(struct wrapper_data) + sz + 1);

        lua_rawseti(L, -2, -idx); /* push userdata to negative index */
        
        char* key = (char*) (d + 1);
        memcpy(key, lkey, sz + 1);
        
        size_t t;
        for (t = 0; t < sz; ++t) {
            if (key[t] >= 0x61 && key[t] <= 0x7A)
                key[t] -= 0x20;
        }
        
        *d = (struct wrapper_data) {
            .L = L, .f_idx = idx, .m = {
                .arg = d, .f = &gml_wrapper, .key = key
            }
        };

        if (gm_register(h, &d->m)) {
            luaL_error(L, "gml_register(): invalid key string \"%s\"", key);
        }
        
    } else luaL_error(L, "gml_register(): expected (string, function)");
    
    return 0;
}

static int gml_reset(lua_State* L) {
    
    gm_handle h = LHANDLER(L);
    gm_unregister_all(h);
    
    lua_newtable(L);
    lua_setglobal(L, "__gm_reg");
    lua_pushinteger(L, 0);
    lua_setglobal(L, "__gm_idx");
    return 0;
}

static int gml_latch_new(lua_State* L) {
    lua_pushlightuserdata(L, gm_latch_new());
    return 1;
}

static int gml_latch_destroy(lua_State* L) {
    if (lua_islightuserdata(L, 1)) {
        gm_latch_destroy(lua_touserdata(L, 1));
    } else luaL_error(L, "gml_latch_destroy(): expected (latch)");
    return 0;
}

static int gml_latch_open(lua_State* L) {
    gm_handle h = LHANDLER(L);
    if (lua_islightuserdata(L, 1)) {
        gmh_latch_open(h, lua_touserdata(L, 1));
    } else luaL_error(L, "gml_latch_open(): expected (latch)");
    return 0;
}

static int gml_latch_reset(lua_State* L) {
    gm_handle h = LHANDLER(L);
    if (lua_islightuserdata(L, 1)) {
        gmh_latch_reset(h, lua_touserdata(L, 1));
    } else luaL_error(L, "gml_latch_reset(): expected (latch)");
    return 0;
}

static int gml_wait(lua_State* L) {
    gm_handle h = LHANDLER(L);
    if (lua_islightuserdata(L, 1)) {
        gmh_wait(h, lua_touserdata(L, 1));
    } else luaL_error(L, "gml_wait(): expected (latch)");
    return 0;
}

static int gml_listen(lua_State* L) {

    /*
      Since the listener threads are already running, all we have to do is
      signal the library to start handling macros. We need to block during this
      entire period, since having more than one thread actively using Lua will
      cause problems
    */
    gm_handle h = LHANDLER(L);
    gm_start(h);

    pause();

    gm_stop(h);
    
    return 0;
}

static int gml_init(lua_State* L) {
    if (!lua_isstring(L, 1))
        luaL_error(L, "gml_init(): expected (string, [optional] table)");
    
    const gm_settings* settings = &gm_default_settings; 
    
    if (lua_istable(L, 2)) {
        struct gml_settings_accessor settings_keys[] = ST_SETTINGS_KEYS;
        
        gm_settings* c = malloc(sizeof(gm_settings));
        size_t t;
        for (t = 0; t < sizeof(settings_keys) / sizeof(struct gml_settings_accessor); ++t) {
            struct gml_settings_accessor* a = &settings_keys[t];
            lua_pushstring(L, a->key);
            lua_rawget(L, 2);
            a->set(c);
            #if DEBUG_MODE
            printf("lua set settings->%s = \"%s\"\n", a->key, lua_tostring(L, -1));
            #endif
            lua_pop(L, 1);
        }
        
        settings = c;
    }

    gm_handle* h = gm_init(lua_tostring(L, 1), settings);
    if (h == NULL) {
        luaL_error(L, "gml_init(): failed to initialize library (check device permissions?)");
    }
    
    ((gmi_handle*) h)->lstate = L; /* store in handler */
    
    lua_pushlightuserdata(L, h);
    lua_setglobal(L, "__gm_handler");
    
    return 0;
}

__attribute__((visibility("default"))) int gm_lua(lua_State* L) {
    
    lua_newtable(L);
    lua_setglobal(L, "__gm_reg");

    lua_pushinteger(L, 1);
    lua_setglobal(L, "__gm_idx");
    
    lua_newtable(L);
    PUSHFUNC(L, "key", &gml_key);
    PUSHFUNC(L, "mouse", &gml_mouse);
    PUSHFUNC(L, "move", &gml_move);
    PUSHFUNC(L, "getmouse", &gml_getmouse);
    PUSHFUNC(L, "sleep", &gml_sleep);
    PUSHFUNC(L, "wait", &gml_wait);
    PUSHFUNC(L, "flush", &gml_flush);
    
    PUSHFUNC(L, "register", &gml_register);
    PUSHFUNC(L, "reset", &gml_reset);
    PUSHFUNC(L, "init", &gml_init);
    PUSHFUNC(L, "listen", &gml_listen);
    
    PUSHFUNC(L, "latch_new", &gml_latch_new);
    PUSHFUNC(L, "latch_destroy", &gml_latch_destroy);
    PUSHFUNC(L, "latch_open", &gml_latch_open);
    PUSHFUNC(L, "latch_reset", &gml_latch_reset);
    
    lua_setglobal(L, "gm");
    
    return 0;
}
