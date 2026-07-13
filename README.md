# NukeScript

Lua scripting backend for [NukeEngine](https://github.com/Luastris/NukeEngine-Eco)
(scripting is a SHARED service — Lua loads beside C#). Lua is vendored statically
(no lua.dll), bound through LuaBridge3. All bindings are **reflection-driven**: any
`[[nuke::prop]]` field and `[[nuke::func]]` method of any registered class — engine or
plugin — is scriptable with zero hand-written glue.

## The component

Add a `ScriptComponent` to an atom and point its `script` field at a `.lua` file
(content-relative). The file returns a table:

```lua
local props = { speed = 2.0, height = 1.0 }   -- shown & editable in the inspector

return {
    props = props,

    update = function(self, dt)               -- every frame; self = the atom
        self.transform.position.y = props.height * math.sin(nuke.Time.Elapsed() * props.speed)
    end,

    fixedUpdate = function(self, dt) end,     -- physics-rate tick (optional)

    -- physics hooks (need a Collider on this atom):
    onCollisionEnter = function(self, other) end,
    onCollisionExit  = function(self, other) end,
    onTriggerEnter   = function(self, other) end,
    onTriggerExit    = function(self, other) end,

    animEvent = function(self, name) end,     -- events from a sibling Animator clip

    gui = function(self)                      -- runtime UI (drawn while playing)
        gui.begin("Script UI")
        gui.text("hello from lua")
        gui.done()
    end,
}
```

Inspector edits of `props` persist with the world and are visible to the script live.
Scripts hot-reload when the `.lua` file changes.

## API

### Atoms & components (reflection-driven — any registered type works)

```lua
local light = self:getComponent("Light")      -- nil if absent; stale-safe handle
if light then
    light.intensity = 5                       -- [[nuke::prop]] fields by name
    light.enabled   = true                    -- builtins: enabled, valid, type, atom
end
local rb = self:addComponent("Rigidbody")
rb:AddForce(nuke.Vector3(0, 10, 0))           -- [[nuke::func]] methods by name
```

`self.transform` — the live Transform: `position` / `scale` (in-place `Vector3`),
`rotation` (quaternion), `eulerHint`, plus its reflected methods (`setEuler`, ...).

### Reflected static facades — `nuke.<Type>.<Fn>`

Every `[[nuke::func]] static` of every registered class is bound automatically:

```lua
if nuke.Physics.Raycast(from, dir, 100) then
    local hit = nuke.Physics.HitAtom()
end
nuke.Audio.Play("music/theme.ogg", 0.8, true, 0)   -- clip, volume, loop, bus
nuke.DebugDraw.Line(a, b, color)
local t = nuke.Time.Elapsed()
```

### Game & World — load worlds, spawn and destroy atoms

```lua
local w = nuke.Game.GetWorld()                -- the CURRENT world as an object
print(w.name)

local a = w:CreateAtom("Enemy")               -- new atom at the world root
a:SetTag("hostile")
a.transform.position = nuke.Vector3(0, 1, 0)
a:addComponent("MeshRenderer")

local player = w:Get("Player")                -- whole-tree lookup by name
a:SetParent(player)                           -- parenting (nil = back to root)
a:Destroy()                                   -- DEFERRED: removed at end of frame (safe on self)

local boss = nuke.Prefabs.Instantiate("Prefabs/Boss.nuprefab")   -- spawn a saved subtree

nuke.Game.LoadWorld("Worlds/level2.nuworld")  -- switch worlds (applied at the frame boundary)
if nuke.Game.IsPaused() then nuke.Game.SetPaused(false) end
nuke.Game.Quit()                              -- Player: close; editor: ignored (stop PIE yourself)
```

Atoms carry the reflected API too: `GetName/SetName`, `GetTag/SetTag`, `GetParent/SetParent`,
`AddChild`, `Destroy`, plus the `name`/`tag` properties. World adds `GetById`, `Pick(origin, dir)`,
`Reparent`, `QueueDestroy`, `SaveToString`/`LoadFromString`, `Clear`.

### Window / video settings

Each setter updates the window config, **persists it** (so the next launch uses it), and
applies live (in the Player; the editor skips the live change but still saves for the game):

```lua
nuke.Game.SetResolution(1600, 900)
nuke.Game.SetWindowMode(0)      -- 0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen
nuke.Game.SetBorderless(true)   -- windowed decoration off
nuke.Game.SetOpacity(0.9)       -- whole-window 0..1 (live)
nuke.Game.SetTransparent(true)  -- per-pixel alpha (takes effect on next launch)
local w, m = nuke.Game.WindowWidth(), nuke.Game.WindowMode()   -- + WindowHeight/IsBorderless/Opacity
```

### Utilities

```lua
nuke.Log.Info("mygame", "reached checkpoint")   -- editor Console (also .Warn / .Error)
local c = nuke.Clock.Create()                   -- pausable monotonic stopwatch
c:Pause(); c:Resume(); print(c:Elapsed())
```

### The object model — every Model class is first-class

```lua
-- assets by NAME (never guids), created or found:
local mat = self:getComponent("MeshRenderer").material   -- the live material instance
mat.shader  = nuke.Shader.Find("world")                  -- assign OBJECTS to objects
mat.metallic = 0.4

local tex = nuke.Texture.Create()                        -- registers into ResDB
tex:setPixels(64, 64, rgbaString)                        -- raw RGBA8 bytes (#s == w*h*4)
mat.diffuse = tex

local mesh = nuke.Mesh.Create()                          -- procedural geometry:
mesh:setGeometry({ -1,0,-1, -1,0,1, 1,0,1 })             -- flat triangle list (normals/uvs optional)
self:getComponent("MeshRenderer").mesh = mesh

local bytes = nuke.Packages.read("Worlds/Main.nuworld")  -- content through pak layers
nuke.Audio.PlayData(bytes, 0.5)                          -- play encoded audio from memory
local any = nuke.Assets.find("bricks")                   -- any-type lookup by name
```

Object handles expose reflected fields (`obj.field`), methods (`obj:Method(...)`),
builtins `guid` / `type` / `valid`; per-type factories are `nuke.<Type>.Create()`,
`nuke.<Type>.Find(name)`, `nuke.<Type>.FromGuid(guid)`.

## Building

Part of the [NukeEngine-Eco](https://github.com/Luastris/NukeEngine-Eco) superbuild, or
standalone: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` +
`cmake --build build --config Debug` (needs `VCPKG_ROOT`; the engine must be built
first). The post-build deploys `NukeScript.dll` into the run dir's `modules/`.
