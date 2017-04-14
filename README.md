# LuaMacros

A simple Lua library written in C that features its own coroutining, simulated X input (for systems that contain the XTest extension), and input events. Input is currently handled through /dev/input, so root is required for the application to have permission to capture global input.

Macros can be written in pure C (see examples/simple.c), or lua (see examples/dota.lua). The library is loaded directly through the shared object via `package.loadlib("libgmacros.so", "gm_lua")`, and needs to be initialized with a path to the input device block.

There are bugs and this is by no means stable -- it is only an easy way to write macros that simulate input (that I personally use for Dota 2, among other games) on Linux. Windows and OSX are not supported and will not be supported.