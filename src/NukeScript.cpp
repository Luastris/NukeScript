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
#include <API/Model/Audio.h>           // sound content: PlayData blob channel

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

// Reflected OBJECT handles (task #67, defined below the component proxy): every reflected
// Model class is first-class in Lua too — create/find/edit/assign, never a guid in user
// code. Forward pieces the component proxy uses to expose asset-ref fields as objects.
struct ObjRef { unsigned long id; };
static const char* kObjMeta = "nuke.Object";
static int PushObjHandle(lua_State* L, unsigned long id);
static const Field* AssetAliasField(const TypeInfo* ti, const std::string& key);

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
        case FT::ObjectRef:                       // reflected instance -> nuke.Object handle
            PushObjHandle(L, v.obj);              // 0 -> nil
            break;
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
            // An object handle where a string is expected = an asset reference: its guid.
            if (ObjRef* o = (ObjRef*)luaL_testudata(L, idx, kObjMeta))
            {
                out.str = Reflect_ObjectGuid(o->id);
                return true;
            }
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
        case FT::ObjectRef:
        {
            if (lua_isnil(L, idx)) { out.obj = 0; return true; }   // null object
            ObjRef* r = (ObjRef*)luaL_testudata(L, idx, kObjMeta);
            if (!r) return false;
            out.obj = r->id;
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
    // OBJECT view of an asset-reference field: `mr.mesh` (field meshGuid) yields a live
    // object handle; the raw `mr.meshGuid` string stays available for tooling.
    if (const Field* af = AssetAliasField(c->GetType(), key))
        return PushObjHandle(L, Reflect_ObjectFromGuid(Reflect_GetField(c, *af).str));
    // A component-OWNED object: `mr.material` -> its live material instance.
    {
        CompRef* r = (CompRef*)lua_touserdata(L, 1);
        if (unsigned long oid = Reflect_ComponentObject(r->atomId, r->compId, key))
            return PushObjHandle(L, oid);
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
    if (!f) f = AssetAliasField(c->GetType(), key);      // `mr.mesh = obj` writes meshGuid
    if (!f)
        return luaL_error(L, "nuke.Component: '%s' has no property '%s'",
                          c->GetType() ? c->GetType()->name.c_str() : c->name, key);
    ReflectValue v;
    if (!f->asset.empty() && lua_isnil(L, 3))            // nil clears an asset reference
    {
        v.type = FT::String;
        v.str.clear();
    }
    else if (!ReadReflectValue(L, 3, f->type, Reflect_GetField(c, *f), v))
        return luaL_error(L, "nuke.Component: bad value for '%s.%s'",
                          c->GetType() ? c->GetType()->name.c_str() : c->name, key);
    Reflect_SetField(c, *f, v);
    Reflect_ComponentFieldChanged(c, *f);                // asset writes take effect this frame
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

// ----------------------------------------------------------------------------
// Reflected OBJECT handles (task #67). A nuke.Object userdata carries only an engine
// handle id (ReflectBind's ObjTable) — the same table C# rides on, so both languages see
// the same objects. __index/__newindex dispatch fields + [[nuke::func]] methods through
// the registry; asset-reference fields read/write as OBJECTS (never guids in user code).
// Builtins: valid, guid, type; Texture adds setPixels(w, h, rgbaString).
// ----------------------------------------------------------------------------
static int PushObjHandle(lua_State* L, unsigned long id)
{
    if (!id) { lua_pushnil(L); return 1; }
    ObjRef* r = (ObjRef*)lua_newuserdata(L, sizeof(ObjRef));
    r->id = id;
    luaL_setmetatable(L, kObjMeta);
    return 1;
}

// The OBJECT view of an asset-reference field: for a missing key `mesh`, matches the
// reflected `meshGuid` string field carrying asset metadata. Direct names always win.
static const Field* AssetAliasField(const TypeInfo* ti, const std::string& key)
{
    if (!ti || key.empty() || Reflect_FindField(ti, key)) return nullptr;
    const Field* f = Reflect_FindField(ti, key + "Guid");
    return (f && f->type == FT::String && !f->asset.empty()) ? f : nullptr;
}

static int ObjMethodInvoke(lua_State* L)   // upvalues: (objId, methodName)
{
    unsigned long id  = (unsigned long)lua_tointeger(L, lua_upvalueindex(1));
    const char*   mname = lua_tostring(L, lua_upvalueindex(2));
    TypeInfo* ti = Registry_Find(Reflect_ObjectType(id));
    const Method* m = ti ? Reflect_FindMethod(ti, mname) : nullptr;
    if (!m) return luaL_error(L, "nuke.Object: method '%s' on a dead handle", mname);

    const int base = luaL_testudata(L, 1, kObjMeta) ? 2 : 1;   // colon call: self at slot 1
    const size_t nargs = m->params.size();
    if ((size_t)(lua_gettop(L) - base + 1) < nargs)
        return luaL_error(L, "nuke.Object: %s expects %d argument(s)", mname, (int)nargs);
    std::vector<ReflectValue> args(nargs);
    for (size_t i = 0; i < nargs; ++i)
    {
        ReflectValue blank; blank.type = m->params[i];
        if (!ReadReflectValue(L, base + (int)i, m->params[i], blank, args[i]))
            return luaL_error(L, "nuke.Object: bad argument #%d to %s", (int)i + 1, mname);
    }
    ReflectValue ret;
    if (!Reflect_ObjectInvoke(id, mname, args.data(), nargs, ret))
        return luaL_error(L, "nuke.Object: invoking %s failed", mname);
    if (ret.type == FT::Unknown) return 0;
    return PushReflectValue(L, ret);
}

// Texture CONTENT: tex:setPixels(w, h, rgba) — rgba is a Lua string (the idiomatic byte
// buffer), tightly packed RGBA8, #rgba == w*h*4. Uploads live via the engine blob channel.
static int ObjSetPixels(lua_State* L)
{
    ObjRef* r = (ObjRef*)luaL_checkudata(L, 1, kObjMeta);
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);
    size_t len = 0;
    const char* p = luaL_checklstring(L, 4, &len);
    lua_pushboolean(L, Reflect_SetTexturePixels(r->id, w, h, p, len));
    return 1;
}

// Mesh CONTENT: mesh:setGeometry(verts [, normals [, uvs]]) — flat number arrays, an
// unindexed TRIANGLE LIST (verts = 9*T numbers; normals same length, optional -> flat
// per-triangle computed; uvs = 2 per vertex, optional -> zeros).
static bool ReadFloatArray(lua_State* L, int idx, std::vector<float>& out)
{
    if (!lua_istable(L, idx)) return false;
    size_t n = lua_rawlen(L, idx);
    out.resize(n);
    for (size_t i = 1; i <= n; ++i)
    {
        lua_rawgeti(L, idx, (lua_Integer)i);
        if (!lua_isnumber(L, -1)) { lua_pop(L, 1); return false; }
        out[i - 1] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    return true;
}

static int ObjSetGeometry(lua_State* L)
{
    ObjRef* r = (ObjRef*)luaL_checkudata(L, 1, kObjMeta);
    std::vector<float> v, n, u;
    if (!ReadFloatArray(L, 2, v) || v.empty() || v.size() % 9 != 0)
        return luaL_error(L, "setGeometry: vertices must be a flat array of 9*T numbers (triangle list)");
    const bool hasN = lua_istable(L, 3);
    const bool hasU = lua_istable(L, 4);
    if (hasN && (!ReadFloatArray(L, 3, n) || n.size() != v.size()))
        return luaL_error(L, "setGeometry: normals must match vertices (3 numbers per vertex)");
    if (hasU && (!ReadFloatArray(L, 4, u) || u.size() != v.size() / 3 * 2))
        return luaL_error(L, "setGeometry: uvs must be 2 numbers per vertex");
    lua_pushboolean(L, Reflect_SetMeshGeometry(r->id, (int)(v.size() / 3), v.data(),
                                               hasN ? n.data() : nullptr,
                                               hasU ? u.data() : nullptr));
    return 1;
}

static int ObjIndex(lua_State* L)
{
    ObjRef* r = (ObjRef*)luaL_checkudata(L, 1, kObjMeta);
    const char* key = luaL_checkstring(L, 2);
    TypeInfo* ti = Registry_Find(Reflect_ObjectType(r->id));
    if (strcmp(key, "valid") == 0) { lua_pushboolean(L, ti != nullptr); return 1; }
    if (!ti) { lua_pushnil(L); return 1; }               // dead handle: reads yield nil
    if (strcmp(key, "guid") == 0)
    {
        std::string g = Reflect_ObjectGuid(r->id);
        lua_pushlstring(L, g.data(), g.size());
        return 1;
    }
    if (strcmp(key, "type") == 0) { lua_pushstring(L, ti->name.c_str()); return 1; }
    if (strcmp(key, "setPixels") == 0 && ti->name == "Texture")
    {
        lua_pushcfunction(L, ObjSetPixels);
        return 1;
    }
    if (strcmp(key, "setGeometry") == 0 && ti->name == "Mesh")
    {
        lua_pushcfunction(L, ObjSetGeometry);
        return 1;
    }
    if (Reflect_FindField(ti, key))
        return PushReflectValue(L, Reflect_ObjectGet(r->id, key));
    if (Reflect_FindMethod(ti, key))
    {
        lua_pushinteger(L, (lua_Integer)r->id);
        lua_pushstring(L, key);
        lua_pushcclosure(L, ObjMethodInvoke, 2);
        return 1;
    }
    if (const Field* af = AssetAliasField(ti, key))      // mat.shader / mat.diffuse -> objects
        return PushObjHandle(L, Reflect_ObjectFromGuid(Reflect_ObjectGet(r->id, af->name).str));
    lua_pushnil(L);
    return 1;
}

static int ObjNewIndex(lua_State* L)
{
    ObjRef* r = (ObjRef*)luaL_checkudata(L, 1, kObjMeta);
    const char* key = luaL_checkstring(L, 2);
    TypeInfo* ti = Registry_Find(Reflect_ObjectType(r->id));
    if (!ti) return luaL_error(L, "nuke.Object: dead handle");
    const Field* f = Reflect_FindField(ti, key);
    if (!f) f = AssetAliasField(ti, key);                // mat.shader = obj writes shaderGuid
    if (!f) return luaL_error(L, "%s has no property '%s'", ti->name.c_str(), key);
    ReflectValue v;
    if (!f->asset.empty() && lua_isnil(L, 3))            // nil clears an asset reference
    {
        v.type = FT::String;
        v.str.clear();
    }
    else if (!ReadReflectValue(L, 3, f->type, Reflect_ObjectGet(r->id, f->name), v))
        return luaL_error(L, "%s.%s: bad value", ti->name.c_str(), key);
    if (!Reflect_ObjectSet(r->id, f->name, v))           // engine re-resolves asset refs
        return luaL_error(L, "%s.%s: write failed", ti->name.c_str(), key);
    return 0;
}

static int ObjToString(lua_State* L)
{
    ObjRef* r = (ObjRef*)luaL_checkudata(L, 1, kObjMeta);
    const char* t = Reflect_ObjectType(r->id);
    if (!*t) { lua_pushstring(L, "nuke.Object(<dead>)"); return 1; }
    lua_pushfstring(L, "nuke.Object(%s)", t);
    return 1;
}

static int ObjEq(lua_State* L)   // same engine handle = same object
{
    ObjRef* a = (ObjRef*)luaL_testudata(L, 1, kObjMeta);
    ObjRef* b = (ObjRef*)luaL_testudata(L, 2, kObjMeta);
    lua_pushboolean(L, a && b && a->id == b->id);
    return 1;
}

static void RegisterObjectProxy(lua_State* L)
{
    luaL_newmetatable(L, kObjMeta);
    lua_pushcfunction(L, ObjIndex);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ObjNewIndex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, ObjToString); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, ObjEq);       lua_setfield(L, -2, "__eq");
    lua_pop(L, 1);
}

// Per-type factories: nuke.<Type>.Create()/Find(name)/FromGuid(guid). Upvalue = type name.
static int TypeCreate(lua_State* L)
{
    return PushObjHandle(L, Reflect_CreateObject(lua_tostring(L, lua_upvalueindex(1))));
}
static int TypeFind(lua_State* L)
{
    return PushObjHandle(L, Reflect_FindAsset(lua_tostring(L, lua_upvalueindex(1)),
                                              luaL_checkstring(L, 1)));
}
static int TypeFromGuid(lua_State* L)
{
    const char* tname = lua_tostring(L, lua_upvalueindex(1));
    unsigned long id = Reflect_ObjectFromGuid(luaL_checkstring(L, 1));
    if (id && strcmp(Reflect_ObjectType(id), tname) != 0) id = 0;   // wrong class -> nil
    return PushObjHandle(L, id);
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
// Every creatable NON-component type additionally gets the object factories
// Create()/Find(name)/FromGuid(guid) (components atom through atom:addComponent instead).
static void BindReflectedStatics(lua_State* L)
{
    lua_getglobal(L, "nuke");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    for (TypeInfo* ti : Registry_All())
    {
        if (!ti) continue;
        // Create() for any creatable non-component; Find/FromGuid ONLY for ResDB assets
        // (looking a facade/singleton up by name/guid is meaningless — the same rule the
        // C# generator uses, so both languages expose the same factories).
        const bool creatable  = ti->create && ti->base != "Component";
        const bool assetLookup = Reflect_IsAssetType(ti->name);
        bool any = false;
        auto ensureTable = [&] { if (!any) { lua_newtable(L); any = true; } };
        for (const Method& m : ti->methods)
        {
            if (!m.isStatic) continue;
            ensureTable();
            lua_pushstring(L, m.name.c_str());        // key
            lua_pushstring(L, ti->name.c_str());      // upvalue 1: type name
            lua_pushstring(L, m.name.c_str());        // upvalue 2: fn name
            lua_pushcclosure(L, StaticFnCall, 2);     // value
            lua_rawset(L, -3);                        // subtable[fn] = closure
        }
        if (creatable || assetLookup)
        {
            ensureTable();
            struct { const char* name; lua_CFunction fn; bool on; } fns[] =
                { { "Create", TypeCreate, creatable },
                  { "Find", TypeFind, assetLookup },
                  { "FromGuid", TypeFromGuid, assetLookup } };
            for (auto& e : fns)
            {
                if (!e.on) continue;
                lua_pushstring(L, e.name);
                lua_pushstring(L, ti->name.c_str());  // upvalue: type name
                lua_pushcclosure(L, e.fn, 1);
                lua_rawset(L, -3);
            }
        }
        // Sound CONTENT: nuke.Audio.PlayData(bytes [, volume [, loop [, bus]]]) — encoded
        // audio (ogg/wav/mp3/flac) as a Lua string; a blob, so hand-bound like setPixels.
        if (ti->name == "Audio")
        {
            ensureTable();
            lua_pushstring(L, "PlayData");
            lua_pushcfunction(L, +[](lua_State* LL) -> int {
                size_t len = 0;
                const char* p = luaL_checklstring(LL, 1, &len);
                double vol  = luaL_optnumber(LL, 2, 1.0);
                bool   loop = lua_toboolean(LL, 3) != 0;
                double bus  = luaL_optnumber(LL, 4, 1.0);
                lua_pushnumber(LL, Audio::PlayData(p, (uint64_t)len, vol, loop, bus));
                return 1;
            });
            lua_rawset(L, -3);
        }
        if (any)
        {
            // rawset: the LuaBridge namespace table is __newindex-protected (read-only).
            lua_pushstring(L, ti->name.c_str());
            lua_insert(L, -2);                        // key under the subtable
            lua_rawset(L, -3);                        // nuke[Type] = subtable
        }
    }
    // nuke.Assets.find(name [, type]) — any-class asset lookup by name; .fromGuid(g).
    lua_newtable(L);
    lua_pushcfunction(L, +[](lua_State* LL) -> int {
        const char* type = lua_isstring(LL, 2) ? lua_tostring(LL, 2) : "";
        return PushObjHandle(LL, Reflect_FindAsset(type, luaL_checkstring(LL, 1)));
    });
    lua_setfield(L, -2, "find");
    lua_pushcfunction(L, +[](lua_State* LL) -> int {
        return PushObjHandle(LL, Reflect_ObjectFromGuid(luaL_checkstring(LL, 1)));
    });
    lua_setfield(L, -2, "fromGuid");
    lua_pushstring(L, "Assets");
    lua_insert(L, -2);
    lua_rawset(L, -3);
    // nuke.Packages.read(rel) — content bytes through the engine's layered resolution
    // (raw project or mounted pak), as a Lua string; nil when absent.
    lua_newtable(L);
    lua_pushcfunction(L, +[](lua_State* LL) -> int {
        std::string data;
        if (!AppInstance::GetSingleton()->ReadContent(luaL_checkstring(LL, 1), data))
        {
            lua_pushnil(LL);
            return 1;
        }
        lua_pushlstring(LL, data.data(), data.size());
        return 1;
    });
    lua_setfield(L, -2, "read");
    lua_pushstring(L, "Packages");
    lua_insert(L, -2);
    lua_rawset(L, -3);
    // Reflected ENUM tables: nuke.<EnumName>.<Label> = value (e.g. nuke.WindowMode.ExclusiveFullscreen).
    // A [[nuke::func]] enum parameter takes the plain int, so scripts pass nuke.WindowMode.X.
    for (const std::string& en : Reflect_AllEnumNames())
        if (const std::vector<std::string>* labels = Reflect_EnumLabels(en))
        {
            lua_newtable(L);
            for (size_t i = 0; i < labels->size(); ++i)
            {
                lua_pushinteger(L, (lua_Integer)i);
                lua_setfield(L, -2, (*labels)[i].c_str());
            }
            lua_pushstring(L, en.c_str());
            lua_insert(L, -2);
            lua_rawset(L, -3);   // nuke[EnumName] = { Label = value, ... }
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
    RegisterObjectProxy(L);
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
                .addProperty("name",
                    [](const Atom* a) { return a->name; },
                    [](Atom* a, const std::string& n) { a->name = n; })
                .addProperty("tag",
                    [](const Atom* a) { return a->tag; },
                    [](Atom* a, const std::string& t) { a->tag = t; })
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
                // Everything else dispatches through the reflection registry: the
                // [[nuke::func]] Atom API (GetName/SetName, GetParent/SetParent, AddChild,
                // Destroy, ...) — one fallback, zero per-method glue (same as Transform).
                .addIndexMetaMethod(+[](Atom& a, const lb::LuaRef& key, lua_State* LL) {
                    return ReflectedIndex(&a, &TypeOf<Atom>(), key, LL);
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
        World* w = AppInstance::GetSingleton()->currentWorld;
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
        World* w = AppInstance::GetSingleton()->currentWorld;
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
        World* w = AppInstance::GetSingleton()->currentWorld;
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

    // Animation event from the sibling Animator (3.1). Game thread with the game lock
    // held (same contract as OnGUI) — enter the VM directly: animEvent(self, name).
    void OnAnimEvent(const char* name) override
    {
        if (!EnsureLoaded()) return;
        lb::LuaRef h = (*table)["animEvent"];
        if (h.isFunction())
        {
            try { h(atom, std::string(name ? name : "")); }
            catch (const lb::LuaException& e) { cerr << "[NukeScript]\tanimEvent error: " << e.what() << endl; Clear(); }
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

        // Read through the engine's content layers: the raw project/overlay from disk,
        // mounted paks from MEMORY (a packed game never lays scripts out on disk).
        std::string resolved = script;
        std::string src;
        AppInstance::GetSingleton()->ReadContent(script, src);
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

    // Service metadata: scripting is a SHARED service — several backends (this Lua one,
    // C#/Mono, native plugins) may be live at once, each with its own component types and
    // file formats. The loader registers queryService() under "scripting"; consumers use
    // GetService<iScript>() (first) / GetServices<iScript>() (all) / the Script facade.
    const char* provides() override { return "scripting"; }
    void*       queryService() override { return static_cast<iScript*>(&gScriptService); }
    bool        sharedService() override { return true; }

    // Shipping cooker (3.2): .lua sources are THIS module's domain — the editor packs them
    // only because we claim them, and we report what they use: every quoted literal is a
    // potential asset reference (Game.LoadWorld / Audio.Play / getComponent props / ...);
    // the editor resolves each against ResDB guids + content paths and walks recursively.
    // Dynamically composed strings are invisible statically — projects force-include those
    // via "packInclude" in the .nuproj.
    bool cookContent(const char* contentRel, const char* bytes, uint64_t size,
                     std::vector<std::string>& outUses) override
    {
        std::string rel = contentRel ? contentRel : "";
        for (char& c : rel) c = (char)tolower((unsigned char)c);
        if (rel.size() < 4 || rel.compare(rel.size() - 4, 4, ".lua") != 0) return false;
        const char* src = bytes;
        for (uint64_t i = 0; src && i < size; ++i)
        {
            char q = src[i];
            if (q != '"' && q != '\'') continue;
            uint64_t e = i + 1;
            while (e < size && src[e] != q && src[e] != '\n') ++e;
            if (e < size && src[e] == q)
            {
                if (e - i > 1 && e - i < 512) outUses.emplace_back(src + i + 1, (size_t)(e - i - 1));
                i = e;
            }
        }
        return true;   // .lua is ours — it ships (with the scripting module present)
    }

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
            "        -- Assets are OBJECTS (create/find/assign by name — never a guid):\n"
            "        -- local mr = self:getComponent(\"MeshRenderer\")\n"
            "        -- mr.mesh = nuke.Mesh.Find(\"builtin:sphere\")\n"
            "        -- mr.material.shader = nuke.Shader.Find(\"world\")\n"
            "        -- local tex = nuke.Texture.Create()\n"
            "        -- tex:setPixels(w, h, rgbaString)   -- #rgbaString == w*h*4\n"
            "        -- mr.material.diffuse = tex\n"
            "        -- Content bytes (raw project or pak): nuke.Packages.read(\"path\")\n"
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
