#!/usr/bin/lua

-- dota_screen_shake 0;bind "1" "dota_select_all_others";bind "0" +cameragrip;bind "2" "";bind "3" "";bind "4" "";bind "space" "";bind "v" "";bind "l" "say /laugh";bind "F7" "say Good game, well played"

io.stdout:setvbuf("no")

package.loadlib("./libgmacros.so", "gm_lua")()

local settings = {
    sched_intval = 20
}
gm.init("/dev/input/by-path/pci-0000:00:1d.0-usb-0:1.6.3:1.0-event-kbd", settings)

enabled = false -- global macro toggle
select_clone = "5"

gm.sim = function(key)
    gm.flush(false)
    gm.key(true, key)
    gm.key(false, key)
    gm.flush(true)
end

gm.click = function(button)
    gm.flush(false)
    gm.mouse(true, button)
    gm.mouse(false, button)
    gm.flush(true)
end

gm.target = function(button, x, y)
    gm.flush(false)
    local xo, yo = gm.getmouse()
    gm.move(x, y)
    gm.mouse(true, button)
    gm.mouse(false, button)
    gm.move(xo, yo)
    gm.flush(true)
end

-- mutex wrapper around primitive latches
mutex = {}
mutex.__index = mutex

function mutex.new()
    local self = { latch = gm.latch_new(), executing = false }
    gm.latch_open(self.latch) -- latch should be open by default
    return setmetatable(self, mutex)
end

function mutex:lock()
    gm.wait(self.latch)
    if self.executing then     -- continue locking if another routine started executing
        self:lock()            -- the latch will be locked by the first routine to enter the mutex
    else
        gm.latch_reset(self.latch) -- close the latch
        self.executing = true      -- continue as nothing was blocking this mutex
    end
end

function mutex:unlock()
    self.executing = false
    gm.latch_open(self.latch) -- open the latch to signal to other routines waiting on this mutex
end

function mutex:destroy()
    gm.latch_destroy(self.latch)
end

function toggle(value)
    if (value == 1) then
        if enabled then enabled = false else enabled = true end
        print("enabled: " .. tostring(enabled))
    end
end

dcast_mutex = mutex.new()

function dcast_function(skill)
    return function(value)
        if (value == 1 and enabled) then
            dcast_mutex:lock()
            gm.sim(skill)
            gm.click(1)
            gm.sim(select_clone)
            gm.sleep(20)
            gm.sim(skill)
            gm.click(1)
            gm.sim("Tab")
            dcast_mutex:unlock()
        end
    end
end

function use_item_on_ult(value)
    if (value == 1 and enabled) then
        gm.sim("r")
        gm.sleep(280)
        gm.sim(select_clone)
        gm.sleep(20)
        gm.sim("n")
        gm.sim("Tab")
        gm.sleep(50)
        gm.target(1, 500, 900)
    end
end

function dual_manta(value)
    if (value == 1 and enabled) then
        gm.sim("r")
        gm.sleep(20)
        gm.sim("n")
        gm.sleep(400)
        gm.sim(select_clone)
        gm.sleep(50)
        gm.target(1, 680, 980)
        gm.sleep(50)
        gm.sim("n")
        gm.sim("Tab")
    end
end

function dual_blink(value)
    if (value == 1 and enabled) then
        gm.sim("x")
        gm.click(1)
        gm.sim(select_clone)
        gm.sleep(20)
        gm.sim("x")
        gm.click(1)
        gm.sim("Tab")
    end
end

function flames(value)
    if (value == 1 and enabled) then
        gm.sim("E")
        gm.click(1)
        gm.sleep(500)
        gm.sim("Q")
        gm.click(1)
        gm.sim("S")
    end
end

function triple_flames(value)
    if (value == 1 and enabled) then
        gm.sim("E")
        gm.click(1)
        gm.sleep(1300)
        gm.sim("E")
        gm.click(1)
        gm.sleep(1300)
        gm.sim("E")
        gm.click(1)
        gm.sleep(200)
        gm.sim("Q")
        gm.click(1)
        gm.sim("S")
    end
end

function tinker_combo(value)
    if (value == 1 and enabled) then
        gm.sim("w")
        gm.sleep(80)
        gm.sim("z")
        gm.click(1)
        gm.sleep(300)
        gm.sim("c")
        gm.click(1)
        gm.sleep(80)
        gm.sim("q")
        gm.click(1)
    end
end

function gcast_function(a1, a2, a3, use, self_cast)
    return function(value)
        if (value == 1 and enabled) then
            gm.sim(a1)
            gm.sleep(20)
            gm.sim(a2)
            gm.sleep(20)
            gm.sim(a3)
            gm.sleep(20)
            gm.sim("r")
            if (use ~= nil) then
                gm.sleep(150)
                gm.sim("d")
                if (self_cast ~= nil) then
                    gm.sleep(40)
                    gm.target(1, 500, 900)
                end
            end
        end
    end
end

function ember_triple_ult(value)
    if (value == 1 and enabled) then
        gm.sim("r")
        gm.click(1)
        gm.sleep(300)
        gm.sim("r")
        gm.click(1)
        gm.sleep(40)
        gm.sim("r")
        gm.click(1)
    end
end

function mapgrab(value)
    if (enabled) then
        if (value == 1) then
            gm.key(false, "0")
        elseif (value == 0) then
            gm.key(true, "0")
        end
    end
end

is_laughspamming = false

function laughspam(value)
    if (value == 1 and enabled) then
        if is_laughspamming == false then
            print("enabling laughspam")
            is_laughspamming = true
            
            while is_laughspamming and enabled do
                gm.sim("l")
                print("laughing")
                gm.sleep((1000 * 15) + 100)
            end
        end
    end
end

function laughspam_end(value)
    if (value == 1 and enabled) then
        if is_laughspamming then
            print("disabling laughspam")
            is_laughspamming = false
        end
    end
end

gm.register("F11", toggle)
gm.register("F10", function (value) gm.move(720, 980) end)

gm.register("SPACE", mapgrab)

gm.register("F2", laughspam)
gm.register("F3", laughspam_end)

ctrl_state = 0
gm.register("LEFTCTRL", function(value) if (value == 1 or value == 0) then ctrl_state = value end end)

if (arg[1] == "arc") then
    print("using arc warden keybinds")
    gm.register("2", dcast_function("Q"))
    gm.register("3", dcast_function("W"))
    gm.register("4", dcast_function("E"))
    gm.register("V", dcast_function("C"))
    gm.register("D", dual_manta)
    gm.register("F", dual_blink)
elseif (arg[1] == "oracle") then
    print("using oracle keybinds")
    gm.register("D", flames)
    -- gm.register("F", triple_flames)
elseif (arg[1] == "tinker") then
    print("using tinker keybinds")
    gm.register("D", tinker_combo)
elseif (arg[1] == "invoker") then
    print("using invoker keybinds")
    gm.register("2", gcast_function("q", "q", "q", true)) -- cold snap
    gm.register("3", gcast_function("e", "e", "q", true)) -- forge spirit
    gm.register("4", gcast_function("w", "w", "e", true, true)) -- alactricy
    gm.register("5", gcast_function("e", "e", "e", true)) -- sun strike
    gm.register("6", gcast_function("q", "w", "e", true)) -- deaf blast
    gm.register("7", gcast_function("e", "e", "w", true)) -- meatball
    gm.register("8", gcast_function("w", "w", "q", true)) -- tornado
    gm.register("9", gcast_function("w", "w", "w", true)) -- emp
    gm.register("0", gcast_function("q", "q", "e", true)) -- ice wall
    gm.register("G", gcast_function("q", "q", "w", true)) -- ghost walk
elseif (arg[1] == "ember") then
    print("using ember spirit keybinds")
    gm.register("F", ember_triple_ult) -- cold snap
end

gm.listen()
