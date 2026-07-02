// NukeScript — a gameplay plugin that adds a Lua-backed ScriptComponent.
//
// Registered into the engine's shared reflection registry at load, so it is serializable,
// create-by-name, and shows in the auto-inspector — yet only exists when this module loads.
//
// Script convention: the .lua file (path in the `script` field) returns a table:
//     local props = { speed = 2.0, height = 1.0 }   -- exported, editable in the inspector
//     return {
//       props  = props,                              -- inspector reads/writes this table
//       update = function(self, dt) ... use props ... end
//     }
// `props` is captured by the update closure AND exposed on the table, so inspector edits
// are seen live by the script (same table by reference). Edited values persist via the
// hidden, serialized `props` JSON field.

#include <interface/NUKEEInteface.h>   // NUKEModule + AppInstance
#include <interface/AssetCreators.h>   // register a "New Lua Script" browser command (data only)
#include <interface/iGUI.h>            // runtime GUI facade (scripts' gui() draws via this)
#include <service/iScript.h>           // the scripting SERVICE contract this plugin provides
#include <reflect/Reflect.h>
#include <reflect/ReflectBind.h>       // reflection->script layer: generic component access (0.8)
#include <API/Model/Atom.h>
#include <API/Model/Transform.h>
#include <API/Model/Time.h>
#include <API/Model/World.h>           // World::Settings (fixedUpdate dt) + game lock

#include <lua.hpp>
#include <LuaBridge/LuaBridge.h>
#include <nlohmann/json.hpp>          // persist edited prop values

#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace std;
using namespace nuke;
namespace lb = luabridge;
using json = nlohmann::json;

// ----------------------------------------------------------------------------
// One Lua VM, created lazily on the thread that first runs/inspects a script
// (the main/update thread), never on the loader thread.
// ----------------------------------------------------------------------------
static lua_State* gL = nullptr;

// ----------------------------------------------------------------------------
// Generic component proxy (reflection-driven, roadmap 0.8). NO per-class wrappers:
// a component is a tiny userdata handle {atomId, componentId}; __index/__newindex
// resolve the live component through the engine's ReflectBind layer and access its
// [[nuke::prop]] fields by name via Field::addr + FT. Handles are STALE-SAFE — after
// a world reload / component removal, reads yield nil and writes raise a Lua error
// instead of touching freed memory.
//
// Value mapping: Bool <-> boolean, Int/Float/Double <-> number, String <-> string,
// Vec3 <-> nuke.Vector3 (or a {x,y,z} table on write), Vec2/Vec4/Quat <-> {x,y,z,w}
// table, Color <-> {r,g,b,a} table. Built-in keys: valid, enabled, type, atom.
// ----------------------------------------------------------------------------
struct CompRef { unsigned long atomId; unsigned long compId; };
static const char* kCompMeta = "nuke.Component";

static Component* CompRefResolve(lua_State* L, int idx)
{
    CompRef* r = (CompRef*)luaL_checkudata(L, idx, kCompMeta);
    return Reflect_ResolveComponent(r->atomId, r->compId);
}

// Push a reflected value as the matching Lua value (see mapping above).
static int PushReflectValue(lua_State* L, const ReflectValue& v)
{
    switch (v.type)
    {
        case FT::Bool:   lua_pushboolean(L, v.b); break;
        case FT::Int:
        case FT::Float:
        case FT::Double: lua_pushnumber(L, v.num); break;
        case FT::String: lua_pushlstring(L, v.str.data(), v.str.size()); break;
        case FT::Vec3:
            if (!lb::push(L, Vector3(v.v[0], v.v[1], v.v[2])))
                lua_pushnil(L);
            break;
        case FT::Vec2:
            lua_createtable(L, 0, 2);
            lua_pushnumber(L, v.v[0]); lua_setfield(L, -2, "x");
            lua_pushnumber(L, v.v[1]); lua_setfield(L, -2, "y");
            break;
        case FT::Vec4:
        case FT::Quat:
            lua_createtable(L, 0, 4);
            lua_pushnumber(L, v.v[0]); lua_setfield(L, -2, "x");
            lua_pushnumber(L, v.v[1]); lua_setfield(L, -2, "y");
            lua_pushnumber(L, v.v[2]); lua_setfield(L, -2, "z");
            lua_pushnumber(L, v.v[3]); lua_setfield(L, -2, "w");
            break;
        case FT::Color:
            lua_createtable(L, 0, 4);
            lua_pushnumber(L, v.v[0]); lua_setfield(L, -2, "r");
            lua_pushnumber(L, v.v[1]); lua_setfield(L, -2, "g");
            lua_pushnumber(L, v.v[2]); lua_setfield(L, -2, "b");
            lua_pushnumber(L, v.v[3]); lua_setfield(L, -2, "a");
            break;
        case FT::AtomRef:
        {
            Atom* a = Reflect_AtomById(v.atom);   // stale-safe: dead id -> nil
            if (!a || !lb::push(L, a)) lua_pushnil(L);
            break;
        }
        default: lua_pushnil(L); break;
    }
    return 1;
}

// Read one numeric slot of a vector-ish table ("x"/"y"/... or "r"/"g"/...); keeps the
// previous value when the key is absent (so partial tables like {x=1} work).
static void ReadTableSlot(lua_State* L, int idx, const char* key, double& into)
{
    lua_getfield(L, idx, key);
    if (lua_isnumber(L, -1)) into = lua_tonumber(L, -1);
    lua_pop(L, 1);
}

// Convert the Lua value at `idx` into a ReflectValue of field type `ft`.
// `cur` = the field's current value (partial table writes keep unset slots).
static bool ReadReflectValue(lua_State* L, int idx, FT ft, const ReflectValue& cur, ReflectValue& out)
{
    out = cur;
    out.type = ft;
    switch (ft)
    {
        case FT::Bool:
            out.b = lua_toboolean(L, idx) != 0;
            return true;
        case FT::Int:
        case FT::Float:
        case FT::Double:
            if (!lua_isnumber(L, idx)) return false;
            out.num = lua_tonumber(L, idx);
            return true;
        case FT::String:
        {
            const char* s = lua_tostring(L, idx);
            if (!s) return false;
            out.str = s;
            return true;
        }
        case FT::Vec3:
            if (lua_isuserdata(L, idx))   // a bound nuke.Vector3
            {
                lb::LuaRef ref = lb::LuaRef::fromStack(L, idx);
                if (ref.isInstance<Vector3>())
                {
                    auto tv = ref.cast<Vector3>();
                    if (!tv) return false;
                    out.v[0] = tv->x; out.v[1] = tv->y; out.v[2] = tv->z;
                    return true;
                }
                return false;
            }
            [[fallthrough]];
        case FT::Vec2:
        case FT::Vec4:
        case FT::Quat:
            if (!lua_istable(L, idx)) return false;
            ReadTableSlot(L, idx, "x", out.v[0]);
            ReadTableSlot(L, idx, "y", out.v[1]);
            ReadTableSlot(L, idx, "z", out.v[2]);
            ReadTableSlot(L, idx, "w", out.v[3]);
            return true;
        case FT::Color:
            if (!lua_istable(L, idx)) return false;
            ReadTableSlot(L, idx, "r", out.v[0]);
            ReadTableSlot(L, idx, "g", out.v[1]);
            ReadTableSlot(L, idx, "b", out.v[2]);
            ReadTableSlot(L, idx, "a", out.v[3]);
            return true;
        case FT::AtomRef:
        {
            if (lua_isnil(L, idx)) { out.atom = 0; return true; }
            lb::LuaRef ref = lb::LuaRef::fromStack(L, idx);
            if (!ref.isInstance<Atom>()) return false;
            auto ta = ref.cast<Atom*>();
            if (!ta) return false;
            out.atom = Reflect_AtomId(*ta);
            return true;
        }
        default:
            return false;
    }
}

// Bound method call: `comp:Method(args...)`. The closure carries (atomId, compId,
// methodName) — ids + name, never pointers, so a stale call errors instead of crashing,
// and re-registration (plugin re-enable) can't leave a dangling Method*.
static int CompMethodCall(lua_State* L)
{
    unsigned long atomId = (unsigned long)lua_tointeger(L, lua_upvalueindex(1));
    unsigned long compId = (unsigned long)lua_tointeger(L, lua_upvalueindex(2));
    const char*   mname  = lua_tostring(L, lua_upvalueindex(3));
    Component* c = Reflect_ResolveComponent(atomId, compId);
    if (!c) return luaL_error(L, "nuke.Component: component no longer exists (stale handle)");
    const Method* m = Reflect_FindMethod(c->GetType(), mname);
    if (!m) return luaL_error(L, "nuke.Component: method '%s' vanished (plugin toggled?)", mname);

    // Colon call puts the component handle at slot 1 — args follow. Dot-call without the
    // handle also works: args then start at 1.
    const int base = luaL_testudata(L, 1, kCompMeta) ? 2 : 1;
    const size_t nargs = m->params.size();
    if ((size_t)(lua_gettop(L) - base + 1) < nargs)
        return luaL_error(L, "nuke.Component: %s expects %d argument(s)", mname, (int)nargs);

    std::vector<ReflectValue> args(nargs);
    for (size_t i = 0; i < nargs; ++i)
    {
        ReflectValue blank; blank.type = m->params[i];   // no "current value" for an argument
        if (!ReadReflectValue(L, base + (int)i, m->params[i], blank, args[i]))
            return luaL_error(L, "nuke.Component: bad argument #%d to %s", (int)i + 1, mname);
    }
    ReflectValue ret;
    if (!Reflect_Invoke(c, *m, args.data(), nargs, ret))
        return luaL_error(L, "nuke.Component: invoking %s failed", mname);
    if (ret.type == FT::Unknown) return 0;               // void
    return PushReflectValue(L, ret);
}

static int CompIndex(lua_State* L)
{
    Component* c = CompRefResolve(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (strcmp(key, "valid") == 0) { lua_pushboolean(L, c != nullptr); return 1; }
    if (!c) { lua_pushnil(L); return 1; }               // dead handle: reads yield nil
    if (strcmp(key, "enabled") == 0) { lua_pushboolean(L, c->enabled); return 1; }
    if (strcmp(key, "type") == 0)
    {
        lua_pushstring(L, c->GetType() ? c->GetType()->name.c_str() : c->name);
        return 1;
    }
    if (strcmp(key, "atom") == 0)
    {
        if (!c->atom || !lb::push(L, c->atom)) lua_pushnil(L);
        return 1;
    }
    const Field* f = Reflect_FindField(c->GetType(), key);
    if (f) return PushReflectValue(L, Reflect_GetField(c, *f));
    if (Reflect_FindMethod(c->GetType(), key))           // [[nuke::func]] method -> bound closure
    {
        CompRef* r = (CompRef*)lua_touserdata(L, 1);
        lua_pushinteger(L, (lua_Integer)r->atomId);
        lua_pushinteger(L, (lua_Integer)r->compId);
        lua_pushstring(L, key);
        lua_pushcclosure(L, CompMethodCall, 3);
        return 1;
    }
    lua_pushnil(L);                                      // unknown name: Lua-idiomatic nil
    return 1;
}

static int CompNewIndex(lua_State* L)
{
    Component* c = CompRefResolve(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (!c) return luaL_error(L, "nuke.Component: component no longer exists (stale handle)");
    if (strcmp(key, "enabled") == 0) { c->enabled = lua_toboolean(L, 3) != 0; return 0; }
    const Field* f = Reflect_FindField(c->GetType(), key);
    if (!f)
        return luaL_error(L, "nuke.Component: '%s' has no property '%s'",
                          c->GetType() ? c->GetType()->name.c_str() : c->name, key);
    ReflectValue v;
    if (!ReadReflectValue(L, 3, f->type, Reflect_GetField(c, *f), v))
        return luaL_error(L, "nuke.Component: bad value for '%s.%s'",
                          c->GetType() ? c->GetType()->name.c_str() : c->name, key);
    Reflect_SetField(c, *f, v);
    return 0;
}

static int CompToString(lua_State* L)
{
    Component* c = CompRefResolve(L, 1);
    if (!c) { lua_pushstring(L, "nuke.Component(<dead>)"); return 1; }
    lua_pushfstring(L, "nuke.Component(%s)", c->GetType() ? c->GetType()->name.c_str() : c->name);
    return 1;
}

static void RegisterComponentProxy(lua_State* L)
{
    luaL_newmetatable(L, kCompMeta);
    lua_pushcfunction(L, CompIndex);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, CompNewIndex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, CompToString); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}

// Wrap a live component into a stale-safe handle (ids, not pointers). `atom` is the
// owner we found/created it on — Component::atom may lag Init, the caller knows better.
static lb::LuaRef MakeComponentRef(lua_State* L, Atom* atom, Component* c)
{
    CompRef* r = (CompRef*)lua_newuserdata(L, sizeof(CompRef));
    r->atomId = atom->id.id;
    r->compId = c->id.id;
    luaL_setmetatable(L, kCompMeta);
    lb::LuaRef ref = lb::LuaRef::fromStack(L, -1);
    lua_pop(L, 1);
    return ref;
}

// Bound STATIC reflected function: nuke.<Type>.<Fn>(args...). Upvalues carry (typeName,
// fnName) strings — resolved through the registry per call, so plugin re-registration
// can never dangle. Fully reflection-driven: no per-facade wrappers anywhere.
static int StaticFnCall(lua_State* L)
{
    const char* tname = lua_tostring(L, lua_upvalueindex(1));
    const char* fname = lua_tostring(L, lua_upvalueindex(2));
    TypeInfo* ti = Registry_Find(tname);
    const Method* m = ti ? Reflect_FindMethod(ti, fname) : nullptr;
    if (!m || !m->isStatic) return luaL_error(L, "nuke.%s.%s vanished (plugin toggled?)", tname, fname);

    const size_t nargs = m->params.size();
    if ((size_t)lua_gettop(L) < nargs)
        return luaL_error(L, "nuke.%s.%s expects %d argument(s)", tname, fname, (int)nargs);
    std::vector<ReflectValue> args(nargs);
    for (size_t i = 0; i < nargs; ++i)
    {
        ReflectValue blank; blank.type = m->params[i];
        if (!ReadReflectValue(L, 1 + (int)i, m->params[i], blank, args[i]))
            return luaL_error(L, "nuke.%s.%s: bad argument #%d", tname, fname, (int)i + 1);
    }
    ReflectValue ret;
    if (!Reflect_Invoke(nullptr, *m, args.data(), nargs, ret))
        return luaL_error(L, "nuke.%s.%s: invoke failed", tname, fname);
    if (ret.type == FT::Unknown) return 0;   // void
    return PushReflectValue(L, ret);
}

// Walk the reflection registry and expose every [[nuke::func]] STATIC method as
// nuke.<Type>.<Fn> — facades (Physics, ...) become scriptable with zero hand-written glue.
static void BindReflectedStatics(lua_State* L)
{
    lua_getglobal(L, "nuke");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    for (TypeInfo* ti : Registry_All())
    {
        if (!ti) continue;
        bool any = false;
        for (const Method& m : ti->methods)
        {
            if (!m.isStatic) continue;
            if (!any)
            {
                lua_newtable(L);
                any = true;
            }
            lua_pushstring(L, m.name.c_str());        // key
            lua_pushstring(L, ti->name.c_str());      // upvalue 1: type name
            lua_pushstring(L, m.name.c_str());        // upvalue 2: fn name
            lua_pushcclosure(L, StaticFnCall, 2);     // value
            lua_rawset(L, -3);                        // subtable[fn] = closure
        }
        if (any)
        {
            // rawset: the LuaBridge namespace table is __newindex-protected (read-only).
            lua_pushstring(L, ti->name.c_str());
            lua_insert(L, -2);                        // key under the subtable
            lua_rawset(L, -3);                        // nuke[Type] = subtable
        }
    }
    lua_pop(L, 1);
}

// ---- generic reflected-OBJECT dispatch (non-component engine objects: Transform, ...) --
// Bound method call for an object reached through a live pointer (LuaBridge class
// userdata): upvalues = (obj lightuserdata, typeName, methodName). Same lifetime contract
// as the pointer binding itself — the closure is produced per-access and used immediately.
static int ObjMethodCall(lua_State* L)
{
    void* obj = lua_touserdata(L, lua_upvalueindex(1));
    const char* tname = lua_tostring(L, lua_upvalueindex(2));
    const char* mname = lua_tostring(L, lua_upvalueindex(3));
    TypeInfo* ti = Registry_Find(tname);
    const Method* m = ti ? Reflect_FindMethod(ti, mname) : nullptr;
    if (!m || !obj) return luaL_error(L, "%s.%s vanished", tname ? tname : "?", mname ? mname : "?");

    const int base = lua_isuserdata(L, 1) ? 2 : 1;   // colon call: self sits at slot 1
    const size_t nargs = m->params.size();
    if ((size_t)(lua_gettop(L) - base + 1) < nargs)
        return luaL_error(L, "%s:%s expects %d argument(s)", tname, mname, (int)nargs);
    std::vector<ReflectValue> args(nargs);
    for (size_t i = 0; i < nargs; ++i)
    {
        ReflectValue blank; blank.type = m->params[i];
        if (!ReadReflectValue(L, base + (int)i, m->params[i], blank, args[i]))
            return luaL_error(L, "%s:%s: bad argument #%d", tname, mname, (int)i + 1);
    }
    ReflectValue ret;
    if (!Reflect_Invoke(obj, *m, args.data(), nargs, ret))
        return luaL_error(L, "%s:%s: invoke failed", tname, mname);
    if (ret.type == FT::Unknown) return 0;
    return PushReflectValue(L, ret);
}

// __index over the reflection registry: fields by name (FT::Vec3 comes back as a LIVE
// Vector3* so nested writes — `t.position.y = 1` — mutate in place, exactly like the old
// hand-written binding), [[nuke::func]] methods as bound closures. Reusable for any
// reflected engine object exposed as a LuaBridge class.
static lb::LuaRef ReflectedIndex(void* obj, TypeInfo* ti, const lb::LuaRef& key, lua_State* L)
{
    if (!ti || !key.isString()) return lb::LuaRef(L);
    const std::string k = key.tostring();
    if (const Field* f = Reflect_FindField(ti, k))
    {
        if (f->type == FT::Vec3)
        {
            if (!lb::push(L, (Vector3*)f->addr(obj))) lua_pushnil(L);
        }
        else
            PushReflectValue(L, Reflect_GetField(obj, *f));
        lb::LuaRef ref = lb::LuaRef::fromStack(L, -1);
        lua_pop(L, 1);
        return ref;
    }
    if (Reflect_FindMethod(ti, k))
    {
        lua_pushlightuserdata(L, obj);
        lua_pushstring(L, ti->name.c_str());
        lua_pushstring(L, k.c_str());
        lua_pushcclosure(L, ObjMethodCall, 3);
        lb::LuaRef ref = lb::LuaRef::fromStack(L, -1);
        lua_pop(L, 1);
        return ref;
    }
    return lb::LuaRef(L);   // unknown name: nil
}

// __newindex over the registry: reflected field writes (t.position = v, t.eulerHint = {…}).
static lb::LuaRef ReflectedNewIndex(void* obj, TypeInfo* ti, const lb::LuaRef& key,
                                    const lb::LuaRef& value, lua_State* L)
{
    if (!ti || !key.isString()) return lb::LuaRef(L);
    const std::string k = key.tostring();
    const Field* f = Reflect_FindField(ti, k);
    if (!f) { luaL_error(L, "%s has no property '%s'", ti->name.c_str(), k.c_str()); return lb::LuaRef(L); }
    value.push();
    ReflectValue v;
    const bool ok = ReadReflectValue(L, -1, f->type, Reflect_GetField(obj, *f), v);
    lua_pop(L, 1);
    if (!ok) { luaL_error(L, "%s.%s: bad value", ti->name.c_str(), k.c_str()); return lb::LuaRef(L); }
    Reflect_SetField(obj, *f, v);
    return lb::LuaRef(L);
}

static void BindEngineAPI(lua_State* L)
{
    RegisterComponentProxy(L);
    lb::getGlobalNamespace(L)
        .beginNamespace("nuke")
            .beginClass<Vector3>("Vector3")
                .addConstructor<void(double, double, double)>()
                .addProperty("x",
                    [](const Vector3* v) { return v->x; },
                    [](Vector3* v, double d) { v->x = d; })
                .addProperty("y",
                    [](const Vector3* v) { return v->y; },
                    [](Vector3* v, double d) { v->y = d; })
                .addProperty("z",
                    [](const Vector3* v) { return v->z; },
                    [](Vector3* v, double d) { v->z = d; })
            .endClass()
            // Transform: NO hand-written members — __index/__newindex dispatch through the
            // reflection registry ([[nuke::prop]] fields incl. live Vector3*, [[nuke::func]]
            // methods incl. the legacy setEuler/euler aliases, which are reflected methods).
            .beginClass<Transform>("Transform")
                .addIndexMetaMethod(+[](Transform& t, const lb::LuaRef& key, lua_State* LL) {
                    return ReflectedIndex(&t, &TypeOf<Transform>(), key, LL);
                })
                .addNewIndexMetaMethod(+[](Transform& t, const lb::LuaRef& key, const lb::LuaRef& value, lua_State* LL) {
                    return ReflectedNewIndex(&t, &TypeOf<Transform>(), key, value, LL);
                })
            .endClass()
            .beginClass<Atom>("Atom")
                .addProperty("transform",
                    [](Atom* a) -> Transform* { return &a->GetTransform(); })
                // Reflection-driven component access (0.8): NO per-class bindings — any
                // reflected component (engine or plugin) works by its type name.
                //   local light = self:getComponent("Light")
                //   if light then light.intensity = 5 end
                .addFunction("getComponent",
                    [](Atom* a, const char* type, lua_State* L) -> lb::LuaRef {
                        Component* c = Reflect_FindComponent(a, type ? type : "");
                        return c ? MakeComponentRef(L, a, c) : lb::LuaRef(L);
                    })
                .addFunction("addComponent",
                    [](Atom* a, const char* type, lua_State* L) -> lb::LuaRef {
                        Component* c = Reflect_AddComponent(a, type ? type : "");
                        return c ? MakeComponentRef(L, a, c) : lb::LuaRef(L);
                    })
            .endClass()
            // LEGACY aliases (older scripts): the reflected surface is nuke.Time.Elapsed()/Delta().
            .addFunction("time",  [] { return Time::Elapsed(); })
            .addFunction("delta", [] { return Time::Delta(); })
            // Every component type a script can getComponent/addComponent (reflection registry).
            .addFunction("componentTypes",
                [](lua_State* L) -> lb::LuaRef {
                    lb::LuaRef t = lb::newTable(L);
                    int i = 1;
                    for (const std::string& n : Reflect_ComponentTypes()) t[i++] = n;
                    return t;
                })
        .endNamespace()
        // LEGACY alias namespace (older scripts + the template use gui.begin/done); the
        // reflected surface is nuke.Gui.* (auto-bound from [[nuke::func]] statics).
        .beginNamespace("gui")
            .addFunction("begin",     [](const char* n) { return nuke::GUI()->Begin(n); })
            .addFunction("done",      [] { nuke::GUI()->End(); })
            .addFunction("text",      [](const char* s) { nuke::GUI()->Text(s); })
            .addFunction("button",    [](const char* s) { return nuke::GUI()->Button(s); })
            .addFunction("sameLine",  [] { nuke::GUI()->SameLine(); })
            .addFunction("separator", [] { nuke::GUI()->Separator(); })
            .addFunction("checkbox",  [](const char* l, bool v)  { nuke::GUI()->Checkbox(l, &v); return v; })
            .addFunction("slider",    [](const char* l, float v, float lo, float hi) { nuke::GUI()->SliderFloat(l, &v, lo, hi); return v; })
        .endNamespace();

    BindReflectedStatics(L);   // nuke.<Type>.<Fn> for every [[nuke::func]] static (e.g. nuke.Physics.Raycast)
}

static void EnsureLua()
{
    if (gL) return;
    gL = luaL_newstate();
    luaL_openlibs(gL);
    BindEngineAPI(gL);
    cout << "[NukeScript]\tLua VM ready." << endl;
}

static std::string ReadFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ----------------------------------------------------------------------------
// ScriptComponent
// ----------------------------------------------------------------------------
class ScriptComponent : public Component
{
    NUKE_CLASS(ScriptComponent, Component)
public:
    [[nuke::prop]] std::string script;   // path to a .lua file (returns { props=, update= })
    [[nuke::prop]] std::string props;    // edited prop values as JSON (hidden; serialized)

    ScriptComponent() : Component("ScriptComponent"), script("scripts/bob.lua") {}

    void Init(Atom* parent) override
    {
        atom = parent;
        transform = &parent->GetTransform();
        parent->components.push_back(this);
    }

    void Update() override
    {
        if (!EnsureLoaded()) return;
        double dt = Time::getSingleton()->delta;
        lb::LuaRef upd = (*table)["update"];
        if (upd.isFunction())
        {
            try { upd(atom, dt); }
            catch (const lb::LuaException& e)
            {
                cerr << "[NukeScript]\tupdate error: " << e.what() << endl;
                Clear();
            }
        }
    }

    // Fixed-rate script hook: `fixedUpdate(self, dt)` at the world's fixedDt cadence.
    // Called by World's FIXED THREAD under the game lock — the same lock every other VM
    // entry (Update, OnGUI, contact hooks) holds, so the shared VM is cross-thread safe.
    void FixedUpdate() override
    {
        if (!EnsureLoaded()) return;
        lb::LuaRef fu = (*table)["fixedUpdate"];
        if (!fu.isFunction()) return;
        World* w = AppInstance::GetSingleton()->currentScene;
        double dt = (w && w->settings.fixedDt > 0.0001f) ? w->settings.fixedDt : 1.0 / 60.0;
        try { fu(atom, dt); }
        catch (const lb::LuaException& e)
        {
            cerr << "[NukeScript]\tfixedUpdate error: " << e.what() << endl;
            Clear();
        }
    }

    // ---- physics contact hooks -------------------------------------------------------
    // Dispatched by the FIXED THREAD under the game lock -> the VM is safe to enter
    // DIRECTLY (no queueing, no frame delay). Script hooks:
    // onCollisionEnter/onCollisionExit/onTriggerEnter/onTriggerExit(self, other).
    void OnCollisionEnter(Atom* other) override { CallContactHook("onCollisionEnter", other); }
    void OnCollisionExit(Atom* other) override  { CallContactHook("onCollisionExit", other); }
    void OnTriggerEnter(Atom* other) override   { CallContactHook("onTriggerEnter", other); }
    void OnTriggerExit(Atom* other) override    { CallContactHook("onTriggerExit", other); }

    // DATA only — expose the script's exported props (the editor renders + edits them).
    // Runs on the editor's render thread: take the game lock — the fixed thread may be
    // inside this same VM (fixedUpdate/contact hooks).
    std::vector<DynProp> DynamicProps() override
    {
        std::vector<DynProp> out;
        World* w = AppInstance::GetSingleton()->currentScene;
        struct Guard { World* w; ~Guard() { if (w) w->UnlockGame(); } } guard{ w };
        if (w) w->LockGame();
        if (!EnsureLoaded() || !propsTable || propsTable->isNil())
            return out;
        json defs = defaultsJson.empty() ? json::object() : json::parse(defaultsJson, nullptr, false);
        for (lb::Iterator it(*propsTable); !it.isNil(); ++it)
        {
            DynProp p;
            p.name  = it.key().tostring();
            p.value = ToVar(it.value());
            if (p.value.kind == NukeVar::Kind::None) continue;   // unsupported type
            p.def = (defs.is_object() && defs.contains(p.name)) ? JsonToVar(defs[p.name]) : p.value;
            out.push_back(p);
        }
        return out;
    }

    void SetDynamicProp(const std::string& name, const NukeVar& v) override
    {
        World* w = AppInstance::GetSingleton()->currentScene;
        struct Guard { World* w; ~Guard() { if (w) w->UnlockGame(); } } guard{ w };
        if (w) w->LockGame();
        if (!EnsureLoaded() || !propsTable) return;
        switch (v.kind)
        {
            case NukeVar::Kind::Number: (*propsTable)[name] = v.num; break;
            case NukeVar::Kind::Bool:   (*propsTable)[name] = v.b;   break;
            case NukeVar::Kind::String: (*propsTable)[name] = v.str; break;
            default: break;
        }
        EncodeProps();
    }

    void Destroy() override     { Clear(); }
    // Runtime UI: call the script's gui(self) each frame (NukeGUI dispatches this). Same VM/thread as
    // Update in the editor (single-threaded play), so no extra locking.
    void OnGUI() override
    {
        if (!EnsureLoaded()) return;
        lb::LuaRef g = (*table)["gui"];
        if (g.isFunction())
        {
            try { g(atom); }
            catch (const lb::LuaException& e) { cerr << "[NukeScript]\tgui error: " << e.what() << endl; Clear(); }
        }
    }

    void Pause() override       {}
    void Reset() override       { Clear(); }

private:
    lb::LuaRef* table = nullptr;        // chunk's returned table
    lb::LuaRef* propsTable = nullptr;   // table["props"] (the exported props)
    std::string loadedScript;           // path the current chunk came from (for reload-on-change)
    std::string defaultsJson;           // script's original prop defaults (for the reset button)

    // Direct contact-hook dispatch (fixed thread, game lock held by the caller).
    void CallContactHook(const char* fnName, Atom* other)
    {
        if (!EnsureLoaded()) return;
        lb::LuaRef fn = (*table)[fnName];
        if (!fn.isFunction()) return;
        try { fn(atom, other); }
        catch (const lb::LuaException& ex)
        {
            cerr << "[NukeScript]\t" << fnName << " error: " << ex.what() << endl;
        }
    }

    void Clear()
    {
        delete table;      table = nullptr;
        delete propsTable; propsTable = nullptr;
    }

    // Load (or reload) the chunk; returns true if a valid table is ready.
    bool EnsureLoaded()
    {
        EnsureLua();
        // Attempt a given path ONCE (success OR failure). Without this, a missing/broken script
        // re-reads and logs every frame. Changing the `script` field re-triggers a load.
        if (script == loadedScript)
            return table != nullptr;
        Clear();
        loadedScript = script;

        // Resolve relative to the project content root (not the exe root), with a cwd fallback.
        std::string resolved = AppInstance::GetSingleton()->ResolveContent(script);
        std::string src = ReadFile(resolved);
        if (src.empty())
        {
            cerr << "[NukeScript]\tcannot read script '" << script << "' (resolved: " << resolved << ")" << endl;
            return false;
        }
        if (luaL_dostring(gL, src.c_str()) != LUA_OK)
        {
            cerr << "[NukeScript]\tload error in '" << script << "': " << lua_tostring(gL, -1) << endl;
            lua_pop(gL, 1);
            return false;
        }
        table = new lb::LuaRef(lb::LuaRef::fromStack(gL, -1));
        lua_pop(gL, 1);

        lb::LuaRef p = (*table)["props"];
        if (p.isTable())
        {
            propsTable = new lb::LuaRef(p);
            defaultsJson = TableToJson(propsTable).dump();   // capture script defaults first
            ApplySavedProps();                                // then overlay saved edits
        }
        return true;
    }

    static json TableToJson(lb::LuaRef* t)
    {
        json j = json::object();
        if (!t) return j;
        for (lb::Iterator it(*t); !it.isNil(); ++it)
        {
            std::string key = it.key().tostring();
            lb::LuaRef val = it.value();
            switch (val.type())
            {
                case LUA_TNUMBER:  j[key] = *val.cast<double>(); break;
                case LUA_TBOOLEAN: j[key] = *val.cast<bool>(); break;
                case LUA_TSTRING:  j[key] = *val.cast<std::string>(); break;
                default: break;
            }
        }
        return j;
    }

    static NukeVar ToVar(lb::LuaRef val)
    {
        NukeVar nv;
        switch (val.type())
        {
            case LUA_TNUMBER:  nv.kind = NukeVar::Kind::Number; nv.num = *val.cast<double>(); break;
            case LUA_TBOOLEAN: nv.kind = NukeVar::Kind::Bool;   nv.b   = *val.cast<bool>(); break;
            case LUA_TSTRING:  nv.kind = NukeVar::Kind::String; nv.str = *val.cast<std::string>(); break;
            default: break;
        }
        return nv;
    }

    static NukeVar JsonToVar(const json& v)
    {
        NukeVar nv;
        if (v.is_number())       { nv.kind = NukeVar::Kind::Number; nv.num = v.get<double>(); }
        else if (v.is_boolean()) { nv.kind = NukeVar::Kind::Bool;   nv.b   = v.get<bool>(); }
        else if (v.is_string())  { nv.kind = NukeVar::Kind::String; nv.str = v.get<std::string>(); }
        return nv;
    }

    void SetProp(const std::string& key, const json& v)
    {
        if (!propsTable) return;
        if (v.is_number())       (*propsTable)[key] = v.get<double>();
        else if (v.is_boolean()) (*propsTable)[key] = v.get<bool>();
        else if (v.is_string())  (*propsTable)[key] = v.get<std::string>();
    }

    // propsTable -> `props` JSON (so reflection serializes the edited values).
    void EncodeProps() { if (propsTable) props = TableToJson(propsTable).dump(); }

    // `props` JSON -> propsTable (overlay onto the script's defaults).
    void ApplySavedProps()
    {
        if (!propsTable || props.empty()) return;
        json j = json::parse(props, nullptr, false);
        if (!j.is_object()) return;
        for (auto& kv : j.items()) SetProp(kv.key(), kv.value());
    }
};

// Register ScriptComponent at DLL-LOAD time. This static initializer runs synchronously
// when InitModules loads the module (before Run is spawned on a worker thread), so a world
// deserialized right after InitModules already knows the type — no registration race.
static bool RegisterScriptComponent()
{
    TypeInfo& t = TypeOf<ScriptComponent>();
    t.base = "Component";
    if (t.fields.empty())
    {
        t.fields.push_back(MakeField("script", &ScriptComponent::script, "script"));   // asset picker (.lua)
        Field pf = MakeField("props", &ScriptComponent::props);
        pf.hidden = true;   // serialized, but drawn by OnInspector instead of the raw JSON
        t.fields.push_back(pf);
    }
    t.create = []() -> void* { return new ScriptComponent(); };
    cout << "[NukeScript]\tScriptComponent registered." << endl;
    return true;
}

// ----------------------------------------------------------------------------
// Plugin
// ----------------------------------------------------------------------------
// The scripting service implementation (iScript, kServiceName "scripting"): snippets run
// in the SAME shared VM as ScriptComponents, so a console line can poke live script state.
struct LuaScriptService : public iScript
{
    const char* Language() override { return "lua"; }

    bool Run(const char* code, const char* chunkName) override
    {
        if (!code) return false;
        EnsureLua();
        if (luaL_loadbuffer(gL, code, strlen(code), chunkName ? chunkName : "snippet") != LUA_OK
            || lua_pcall(gL, 0, 0, 0) != LUA_OK)
        {
            cerr << "[NukeScript]\tRun('" << (chunkName ? chunkName : "snippet")
                 << "') error: " << lua_tostring(gL, -1) << endl;
            lua_pop(gL, 1);
            return false;
        }
        return true;
    }
};
static LuaScriptService gScriptService;

struct NukeScriptModule : public NUKEModule
{
    NukeScriptModule()
    {
        strcpy(title, "NukeScript");
        strcpy(author, "Luastris");
        strcpy(description, "Lua scripting: ScriptComponent with inspector-editable props.");
        strcpy(version, "1.0.0.0");
        strcpy(site, "https://luastris.com");
        tags = { "lua", "scripting", "gameplay" };
    }

    // Service metadata: the active scripting backend (one at a time; a future C#/Mono
    // plugin provides the same iScript service). The loader registers queryService()
    // under "scripting"; consumers use GetService<iScript>() / the Script facade.
    const char* provides() override { return "scripting"; }
    void*       queryService() override { return static_cast<iScript*>(&gScriptService); }

    // Activation hook (sync, before Run). Register ScriptComponent here so the type only
    // exists while the plugin is enabled — disabled, its components stay inert placeholders.
    void OnLoad() override
    {
        RegisterScriptComponent();
        // Full file-type descriptor for .lua (the editor does the actual file IO — no boost
        // here): New-menu entry under "Scripts", text-editable, "lua" syntax highlighting.
        nuke::AssetCreator luaType;
        luaType.label = "Lua Script";
        luaType.ext = ".lua";
        luaType.baseName = "New Script";
        luaType.category = "Scripts";
        luaType.textEditable = true;
        luaType.syntaxLanguage = "lua";
        luaType.content =
            "-- NukeEngine Lua script\n"
            "return {\n"
            "    props = {\n"
            "        -- speed = 1.0,\n"
            "    },\n"
            "    update = function(self, dt)\n"
            "        -- self is the atom; any reflected component works by type name:\n"
            "        -- local light = self:getComponent(\"Light\")\n"
            "        -- if light then light.intensity = 5 end\n"
            "    end,\n"
            "    -- Fixed-rate tick (physics cadence, frame-independent):\n"
            "    -- fixedUpdate = function(self, dt) end,\n"
            "    -- Physics hooks (need a Collider on this atom):\n"
            "    -- onCollisionEnter = function(self, other) end,\n"
            "    -- onCollisionExit  = function(self, other) end,\n"
            "    -- onTriggerEnter   = function(self, other) end,\n"
            "    -- onTriggerExit    = function(self, other) end,\n"
            "    -- Ray cast (reflected facade, nuke.Physics.*):\n"
            "    -- if nuke.Physics.Raycast(from, dir, 100) then\n"
            "    --     local a = nuke.Physics.HitAtom(); local d = nuke.Physics.HitDistance()\n"
            "    -- end\n"
            "    -- Runtime UI (drawn while playing); always pair begin/done.\n"
            "    gui = function(self)\n"
            "        gui.begin(\"Script UI\")\n"
            "        gui.text(\"hello from lua\")\n"
            "        gui.done()\n"
            "    end,\n"
            "}\n";
        nuke::RegisterAssetCreator(luaType);
    }

    void Run(AppInstance* inst) override
    {
        instance = inst;
        stopped = false;
    }

    bool HasSettings() override { return false; }
    void Settings() override {}
    void Shutdown() override { stopped = true; }
};

extern "C" BOOST_SYMBOL_EXPORT NukeScriptModule plugin;
NukeScriptModule plugin;
