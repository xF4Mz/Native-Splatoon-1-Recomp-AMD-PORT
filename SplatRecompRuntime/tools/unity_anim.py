"""unity_anim.py -- read the float curves out of a Unity AnimationClip asset (binary, with its type tree).

    python unity_anim.py <clip asset> [--list]

Unity editor assets (.anim inside a .unitypackage: <guid>/asset) are "SerializedFile"s: a header,
the types of the objects in it described node by node (the type tree), then the objects. The
objects are read generically by walking the type tree -- no table of Unity's classes needed.
A humanoid clip keeps its motion as float curves named after Unity's "muscles" ("Spine
Front-Back", "Left Arm Down-Up", ...) plus RootT/RootQ (the body's position / rotation).
Used by emote_convert.py.
"""
import struct, sys

class R:
    def __init__(self, d, le=True, p=0):
        self.d, self.p, self.e = d, p, '<' if le else '>'
    def u8(self): v = self.d[self.p]; self.p += 1; return v
    def u16(self): v = struct.unpack_from(self.e + 'H', self.d, self.p)[0]; self.p += 2; return v
    def i16(self): v = struct.unpack_from(self.e + 'h', self.d, self.p)[0]; self.p += 2; return v
    def u32(self): v = struct.unpack_from(self.e + 'I', self.d, self.p)[0]; self.p += 4; return v
    def i32(self): v = struct.unpack_from(self.e + 'i', self.d, self.p)[0]; self.p += 4; return v
    def i64(self): v = struct.unpack_from(self.e + 'q', self.d, self.p)[0]; self.p += 8; return v
    def f32(self): v = struct.unpack_from(self.e + 'f', self.d, self.p)[0]; self.p += 4; return v
    def cstr(self):
        e = self.d.index(b'\0', self.p); s = self.d[self.p:e].decode('utf-8', 'replace'); self.p = e + 1; return s
    def align(self, n=4): self.p = (self.p + n - 1) & ~(n - 1)

# Unity's built-in strings (type and field names the type trees point at with the top bit set), in order
COMMON = ('AABB AnimationClip AnimationCurve AnimationState Array Base BitField bitset bool char ColorRGBA Component data deque double '
          'dynamic_array FastPropertyName first float Font GameObject Generic_Mono GradientNEW GUID GUIStyle int list long_long map '
          'Matrix4x4f MdFour MonoBehaviour MonoScript m_ByteSize m_Curve m_EditorClassIdentifier m_EditorHideFlags m_Enabled '
          'm_ExtensionPtr m_GameObject m_Index m_IsArray m_IsStatic m_MetaFlag m_Name m_ObjectHideFlags m_PrefabInternal '
          'm_PrefabParentObject m_Script m_StaticEditorFlags m_Type m_Version Object pair PPtr<Component> PPtr<GameObject> '
          'PPtr<Material> PPtr<MonoBehaviour> PPtr<MonoScript> PPtr<Object> PPtr<Prefab> PPtr<Sprite> PPtr<TextAsset> PPtr<Texture> '
          'PPtr<Texture2D> PPtr<Transform> Prefab Quaternionf Rectf RectInt RectOffset second set short size SInt16 SInt32 SInt64 SInt8 '
          'staticvector string TextAsset TextMesh Texture Texture2D Transform TypelessData UInt16 UInt32 UInt64 UInt8 unsigned_int '
          'unsigned_long_long unsigned_short vector Vector2f Vector3f Vector4f m_ScriptingClassIdentifier Gradient Type* int2_storage '
          'int3_storage BoundsInt m_CorrespondingSourceObject m_PrefabInstance m_PrefabAsset FileSize Hash128').split()
COMMON_AT = {}
_o = 0
for _s in COMMON:
    _s = _s.replace('_', ' ') if _s in ('Generic_Mono', 'long_long', 'unsigned_int', 'unsigned_long_long', 'unsigned_short') else _s
    COMMON_AT[_o] = _s
    _o += len(_s) + 1

class Node:
    __slots__ = ('type', 'name', 'size', 'flags', 'meta', 'level', 'kids')

def read_tree(r):
    n, strsize = r.u32(), r.u32()
    raw = []
    for _ in range(n):
        ver, level, flags = r.u16(), r.u8(), r.u8()
        to, no, size, idx, meta = r.u32(), r.u32(), r.i32(), r.i32(), r.u32()
        raw.append((level, flags, to, no, size, meta))
    strs = r.d[r.p:r.p + strsize]; r.p += strsize
    def s(o):
        if o & 0x80000000: return COMMON_AT.get(o & 0x7FFFFFFF, '#%d' % (o & 0x7FFFFFFF))   # (Unity's built-in strings)
        e = strs.index(b'\0', o); return strs[o:e].decode()
    nodes = []
    for level, flags, to, no, size, meta in raw:
        x = Node(); x.type, x.name, x.size, x.flags, x.meta, x.level, x.kids = s(to), s(no), size, flags, meta, level, []
        nodes.append(x)
    root, stack = nodes[0], [nodes[0]]
    for x in nodes[1:]:
        while stack[-1].level >= x.level: stack.pop()
        stack[-1].kids.append(x); stack.append(x)
    return root

def read_value(r, n):
    if n.flags & 1:   # an array: its size, then that many of its element
        cnt = read_value(r, n.kids[0])
        el = n.kids[1]
        if not el.kids and el.size == 1: v = r.d[r.p:r.p + cnt]; r.p += cnt   # (bytes: chars of a string, raw data)
        else: v = [read_value(r, el) for _ in range(cnt)]
    elif n.kids:
        if len(n.kids) == 1 and n.kids[0].flags & 1:   # (a string or a vector: just its array)
            v = read_value(r, n.kids[0])
            if n.type == 'string' and isinstance(v, (bytes, bytearray)): v = v.decode('utf-8', 'replace')
        else:
            v = {}
            for k in n.kids: v[k.name] = read_value(r, k)
    else:
        sz = n.size
        if sz == 4: v = r.f32() if n.type == 'float' else r.i32()
        elif sz == 1: v = r.u8()
        elif sz == 2: v = r.i16()
        elif sz == 8: v = r.i64()
        else: v = r.d[r.p:r.p + max(sz, 0)]; r.p += max(sz, 0)
    if n.meta & 0x4000: r.align()
    return v

def load(path):
    d = open(path, 'rb').read()
    h = R(d, le=False)
    meta_size, file_size, version, data_off = h.u32(), h.u32(), h.u32(), h.u32()
    endian = h.u8(); h.p += 3
    r = R(d, le=(endian == 0), p=h.p)
    unity = r.cstr(); platform = r.u32(); typetree = r.u8()
    ntypes = r.i32(); types = []
    for _ in range(ntypes):
        cid = r.i32(); stripped = r.u8(); script_idx = r.i16()
        if cid == 114: r.p += 16
        r.p += 16
        types.append((cid, read_tree(r) if typetree else None))
    nobj = r.i32(); objs = []
    for _ in range(nobj):
        r.align(); pid = r.i64(); start, size, tidx = r.u32(), r.u32(), r.i32()
        objs.append((pid, start, size, tidx))
    out = []
    for pid, start, size, tidx in objs:
        cid, tree = types[tidx]
        if tree is None: continue
        rr = R(d, le=(endian == 0), p=data_off + start)
        out.append((cid, tree.type, read_value(rr, tree)))
    return unity, out

def float_curves(clip):
    """{attribute: [(time, value, in_slope, out_slope), ...]} from a clip's float curves (editor curves win)."""
    res = {}
    for key in ('m_FloatCurves', 'm_EditorCurves'):
        for fc in clip.get(key, []) or []:
            name = fc.get('attribute')
            keys = fc.get('curve', {}).get('m_Curve', [])
            kl = []
            for k in keys:
                vals = list(k.values())   # time, value, inSlope, outSlope, (weighted mode, in / out weight)
                kl.append((vals[0], vals[1], vals[2], vals[3]))
            if name and kl: res[name] = kl
    return res

def sample(keys, t):
    """Unity's Hermite curve at time t."""
    if t <= keys[0][0]: return keys[0][1]
    if t >= keys[-1][0]: return keys[-1][1]
    lo, hi = 0, len(keys) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if keys[mid][0] <= t: lo = mid
        else: hi = mid
    t0, v0, _, o0 = keys[lo]; t1, v1, i1, _ = keys[hi]
    dt = t1 - t0
    if dt <= 0: return v1
    if abs(o0) == float('inf') or abs(i1) == float('inf'): return v0   # (stepped)
    s = (t - t0) / dt
    h00 = 2 * s ** 3 - 3 * s ** 2 + 1; h10 = s ** 3 - 2 * s ** 2 + s; h01 = -2 * s ** 3 + 3 * s ** 2; h11 = s ** 3 - s ** 2
    return h00 * v0 + h10 * dt * o0 + h01 * v1 + h11 * dt * i1

if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    unity, objs = load(sys.argv[1])
    for cid, tname, v in objs:
        if cid != 74: continue   # AnimationClip
        print('clip "%s" (Unity %s): %s' % (v.get('m_Name'), unity, ', '.join('%s=%s' % (k, (len(x) if isinstance(x, list) else x)) for k, x in v.items()
                                                                              if not isinstance(x, (dict, bytes)))[:600]))
        fc = float_curves(v)
        end = max(k[-1][0] for k in fc.values())
        print('%d float curves, %.2f s' % (len(fc), end))
        if '--list' in sys.argv:
            for name, k in sorted(fc.items()): print('  %-40s %4d keys  %7.3f .. %7.3f' % (name, len(k), min(x[1] for x in k), max(x[1] for x in k)))
