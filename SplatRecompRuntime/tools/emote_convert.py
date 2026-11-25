"""emote_convert.py -- a Unity humanoid animation clip -> an emote the game plays on the player's Inkling (F5).

    python emote_convert.py <clip asset (.anim / unitypackage asset)> <game content folder> <out.emote> [--fps 30]

<game content folder>: the game's content\\ (or the update's), where Pack\\Player.pack is: the Inkling's
skeleton (bone names, rest pose) is read from the player's own dump at conversion time.

How the motion is moved onto the Inkling (its proportions are nothing like a person's):
  - body: RootQ (turned so the clip starts facing the Inkling's way) on joint_root; RootT's height
    change (crouches) and sway, scaled to the Inkling's legs.
  - spine / chest / neck / head: Unity's muscles (Front-Back, Left-Right, Twist / Nod, Tilt, Turn),
    +-40 degrees at +-1, as rotations of those bones.
  - arms and legs: two-bone IK to the clip's hand and foot targets (LeftHandT, LeftFootT, ...: body
    space, in units of the human's hip height), taken relative to a person's shoulders / hips and
    scaled to the Inkling's arm / leg length; elbows out and back, knees forward; feet kept flat.
Output (little-endian): "SEMO", version 1, fps (float), frames, bones, 32-byte bone names, then per
frame: joint_root's translation (3 floats) and per bone its local rotation (x, y, z, w).
"""
import struct, sys, math, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unity_anim

# --- small vector / quaternion helpers (x, y, z, w) ---
def v_add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def v_sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def v_mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def v_dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def v_cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def v_len(a): return math.sqrt(v_dot(a, a))
def v_norm(a):
    l = v_len(a)
    return v_mul(a, 1.0 / l) if l > 1e-9 else (0.0, 0.0, 0.0)
def q_mul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)
def q_conj(q): return (-q[0], -q[1], -q[2], q[3])
def q_rot(q, v):
    p = q_mul(q_mul(q, (v[0], v[1], v[2], 0.0)), q_conj(q))
    return (p[0], p[1], p[2])
def q_axis(axis, ang):
    s = math.sin(ang * 0.5)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(ang * 0.5))
def q_norm(q):
    l = math.sqrt(sum(c * c for c in q))
    return tuple(c / l for c in q) if l > 1e-12 else (0.0, 0.0, 0.0, 1.0)
def q_from_cols(c0, c1, c2):   # rotation matrix with these columns
    m00, m10, m20 = c0; m01, m11, m21 = c1; m02, m12, m22 = c2
    tr = m00 + m11 + m22
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2; return q_norm(((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s))
    if m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2; return q_norm((0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s))
    if m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2; return q_norm(((m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s))
    s = math.sqrt(1.0 + m22 - m00 - m11) * 2; return q_norm(((m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s))
def q_frame(u0, h0, u1, h1):   # the rotation taking the frame (u0, h0, u0 x h0) to (u1, h1, u1 x h1)
    a = q_from_cols(u0, h0, v_cross(u0, h0))
    b = q_from_cols(u1, h1, v_cross(u1, h1))
    return q_norm(q_mul(b, q_conj(a)))
def q_arc(a, b):   # the shortest rotation taking direction a to direction b
    a, b = v_norm(a), v_norm(b)
    c = v_cross(a, b); w = 1.0 + v_dot(a, b)
    if w < 1e-6:
        o = v_cross(a, X if abs(a[0]) < 0.9 else Y)
        return q_norm((o[0], o[1], o[2], 0.0))
    return q_norm((c[0], c[1], c[2], w))
def q_limit(q, max_ang):   # the same rotation, at most max_ang
    q = q if q[3] >= 0 else tuple(-c for c in q)
    ang = 2 * math.acos(max(-1.0, min(1.0, q[3])))
    if ang <= max_ang or ang < 1e-6: return q
    s = math.sin(ang / 2)
    ax = (q[0] / s, q[1] / s, q[2] / s)
    return q_axis(ax, max_ang)
def q_slerpish(a, b, t):   # nlerp along the short way
    if sum(x * y for x, y in zip(a, b)) < 0: b = tuple(-c for c in b)
    return q_norm(tuple(x + (y - x) * t for x, y in zip(a, b)))
def limit_twist(w_new, w_prev, axis_local, max_ang):
    """w_new, with its turn about its own bone axis since w_prev kept to max_ang (the swing is left as it is)."""
    if w_prev is None: return w_new
    a0, a1 = q_rot(w_prev, axis_local), q_rot(w_new, axis_local)
    swing = q_arc(a0, a1)
    twist = q_mul(q_conj(q_mul(swing, w_prev)), w_new)   # (about axis_local, in the bone's frame)
    return q_norm(q_mul(q_mul(swing, w_prev), q_limit(twist, max_ang)))
def q_euler_xyz(x, y, z): return q_mul(q_axis((0, 0, 1), z), q_mul(q_axis((0, 1, 0), y), q_axis((1, 0, 0), x)))
X, Y, Z = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)
DEG = math.pi / 180.0

# --- the Inkling's skeleton, from the player's own game files ---
def inkling_skeleton(content):
    import sarc_lite
    pack = os.path.join(content, 'Pack', 'Player.pack')
    model = sarc_lite.get(open(pack, 'rb').read(), 'Model/Player00.szs')
    bfres = sarc_lite.get(model, 'Output.bfres')
    return sarc_lite.fskl(bfres)

# --- Unity (x right, y up, z forward) -> the Inkling's model space (x = its left, y up, z forward) ---
def u_vec(x, y, z): return (-x, y, z)
def u_quat(x, y, z, w): return q_norm((x, -y, -z, w))

def two_bone(p0, target, a, b, pole, prev_pp=None):
    """Elbow / knee position and the reached end for a chain p0 -a- joint -b- end aiming at target."""
    dv = v_sub(target, p0)
    dl = v_len(dv)
    dirv = v_norm(dv) if dl > 1e-6 else v_norm(v_sub(target, v_add(p0, (0, 1e-3, 0))))
    d = min(max(dl, abs(a - b) + 1e-3), (a + b) * 0.9995)
    ca = (a * a + d * d - b * b) / (2 * a * d)
    ang = math.acos(max(-1.0, min(1.0, ca)))
    pp = v_sub(pole, v_mul(dirv, v_dot(pole, dirv)))
    if v_len(pp) < 1e-6: pp = v_cross(dirv, X if abs(dirv[0]) < 0.9 else Y)
    pp = v_norm(pp)
    if prev_pp is not None and v_dot(pp, prev_pp) < 0.0:   # (the pole swung to the other side in one frame: keep to last frame's side)
        pp2 = v_sub(prev_pp, v_mul(dirv, v_dot(prev_pp, dirv)))
        if v_len(pp2) > 1e-4: pp = v_norm(pp2)
    joint = v_add(p0, v_add(v_mul(dirv, a * math.cos(ang)), v_mul(pp, a * math.sin(ang))))
    end = v_add(p0, v_mul(dirv, d))
    return joint, end, dirv, pp

def convert(clip_path, content, out_path, fps=30.0):
    unity, objs = unity_anim.load(clip_path)
    clip = next(v for cid, t, v in objs if cid == 74)
    fc = unity_anim.float_curves(clip)
    length = max(k[-1][0] for k in fc.values())
    bones = inkling_skeleton(content)
    idx = {b['name']: i for i, b in enumerate(bones)}
    rest_t = [tuple(b['trans']) for b in bones]
    rest_q = [q_euler_xyz(*b['rot'][:3]) if (b['flags'] >> 12) & 1 else q_norm(tuple(b['rot'])) for b in bones]
    par = [b['parent'] for b in bones]
    # rest world rotations (model space)
    rest_w = []
    for i in range(len(bones)):
        rest_w.append(q_mul(rest_w[par[i]], rest_q[i]) if par[i] >= 0 else rest_q[i])
    def blen(name): return v_len(rest_t[idx[name]])

    S = rest_t[idx['joint_root']][1] - 1.5   # (the human's hip height in Inkling units: pelvis to ankle)
    ARM_A = blen('arm1sub_L') + blen('arm2_L'); ARM_B = blen('arm2sub_L') + blen('hand_L')
    LEG_A = blen('leg1sub_L') + blen('leg2_L'); LEG_B = blen('leg2sub_L') + blen('foot_L')
    K_ARM = (ARM_A + ARM_B) / (0.56 * S)    # (a person's shoulder-to-wrist: 0.56 hip heights)
    K_LEG = (LEG_A + LEG_B) / (0.90 * S)    # (hip joint to ankle: 0.9)

    def m(name, t):
        k = fc.get(name)
        return unity_anim.sample(k, t) if k else 0.0
    def muscles3(t, prefix, fb, lr, tw, lim):
        a = m(prefix + ' ' + fb, t) * lim * DEG
        b = m(prefix + ' ' + lr, t) * lim * DEG
        c = m(prefix + ' ' + tw, t) * lim * DEG
        return q_mul(q_axis(Y, -c), q_mul(q_axis(X, -a), q_axis(Z, b)))
    def uq(prefix, t): return u_quat(m(prefix + '.x', t), m(prefix + '.y', t), m(prefix + '.z', t), m(prefix + '.w', t) if (prefix + '.w') in fc else 1.0)
    def ut(prefix, t): return u_vec(m(prefix + '.x', t), m(prefix + '.y', t), m(prefix + '.z', t))

    r0 = uq('RootQ', 0.0)
    f0 = q_rot(r0, Z)
    q_ref = q_axis(Y, math.atan2(f0[0], f0[2]))
    root0 = ut('RootT', 0.0)

    driven = ['joint_root', 'hip', 'spine1', 'spine2', 'chest', 'neck', 'head',
              'arm1_L', 'arm1sub_L', 'arm2_L', 'arm2sub_L', 'hand_L', 'arm1_R', 'arm1sub_R', 'arm2_R', 'arm2sub_R', 'hand_R',
              'leg1_L', 'leg1sub_L', 'leg2_L', 'leg2sub_L', 'foot_L', 'leg1_R', 'leg1sub_R', 'leg2_R', 'leg2sub_R', 'foot_R']
    nframes = int(length * fps) + 1
    raw_pole, smooth_pole = {}, {}
    def smoothed(key, fi):
        acc, wsum = (0.0, 0.0, 0.0), 0.0
        for k in range(-4, 5):
            j = min(max(fi + k, 0), nframes - 1)
            v = raw_pole.get((key, j))
            if v is None: continue
            w = math.exp(-k * k / 8.0)
            acc = v_add(acc, v_mul(v_norm(v), w)); wsum += w
        return v_mul(acc, 1.0 / wsum) if wsum else None
    for pass_no in (0, 1):
      frames = []
      prev_pp, prev_w = {}, {}
      if pass_no == 1:
        for (key, j) in list(raw_pole): smooth_pole[(key, j)] = smoothed(key, j)
      for fi in range(nframes):
          t = fi / fps
          loc = {}
          body = q_norm(q_mul(q_conj(q_ref), uq('RootQ', t)))
          loc['joint_root'] = body
          dt = q_rot(q_conj(q_ref), v_sub(ut('RootT', t), root0))
          root_t = v_add(rest_t[idx['joint_root']], v_mul(dt, S * K_LEG))
          loc['hip'] = rest_q[idx['hip']]
          loc['spine1'] = muscles3(t, 'Spine', 'Front-Back', 'Left-Right', 'Twist Left-Right', 40)
          loc['spine2'] = muscles3(t, 'Chest', 'Front-Back', 'Left-Right', 'Twist Left-Right', 40)
          loc['chest'] = muscles3(t, 'UpperChest', 'Front-Back', 'Left-Right', 'Twist Left-Right', 20)
          loc['neck'] = muscles3(t, 'Neck', 'Nod Down-Up', 'Tilt Left-Right', 'Turn Left-Right', 40)
          loc['head'] = muscles3(t, 'Head', 'Nod Down-Up', 'Tilt Left-Right', 'Turn Left-Right', 40)
          # forward kinematics in body space (joint_root at the origin, unrotated)
          W, P = {}, {}
          def fk(name):
              if name in W: return
              i = idx[name]; pn = bones[par[i]]['name'] if par[i] >= 0 else None
              if name == 'joint_root' or pn is None:
                  W[name] = (0.0, 0.0, 0.0, 1.0); P[name] = (0.0, 0.0, 0.0); return
              fk(pn)
              P[name] = v_add(P[pn], q_rot(W[pn], rest_t[i]))
              W[name] = q_mul(W[pn], loc.get(name, rest_q[i]))
          chest_w = None
          for side, sx in (('L', 1.0), ('R', -1.0)):
              # arms
              fk('arm1_' + side)
              chest_w = W['chest']
              p0 = P['arm1_' + side]
              hn = 'LeftHand' if side == 'L' else 'RightHand'
              goal = v_mul(ut(hn + 'T', t), S)
              src_sh = q_rot(chest_w, (sx * 0.19 * S, 0.47 * S, 0.0))
              target = v_add(p0, v_mul(v_sub(goal, src_sh), K_ARM))
              fore = u_vec(*q_rot(q_norm((m(hn + 'Q.x', t), m(hn + 'Q.y', t), m(hn + 'Q.z', t), m(hn + 'Q.w', t))), X))   # (the clip's forearm, toward the hand)
              src_elbow = v_sub(goal, v_mul(fore, 0.27 * S))
              off = v_sub(src_elbow, v_mul(v_add(src_sh, goal), 0.5))
              pole = v_add(off, v_mul(q_rot(chest_w, v_norm((sx * 0.35, -0.55, -0.75))), 0.05 * S))
              raw_pole[('arm' + side, fi)] = pole
              if pass_no == 1 and smooth_pole.get(('arm' + side, fi)): pole = smooth_pole[('arm' + side, fi)]
              elbow, end, dirv, pp = two_bone(p0, target, ARM_A, ARM_B, pole, prev_pp.get('arm' + side))
              prev_pp['arm' + side] = pp
              d1 = v_norm(v_sub(elbow, p0)); d2 = v_norm(v_sub(end, elbow))
              hinge = v_norm(v_cross(pp, dirv))
              u0 = q_rot(rest_w[idx['arm1_' + side]], v_norm(rest_t[idx['arm1sub_' + side]]))
              h0 = v_norm(v_cross(u0, Z))
              w1 = q_mul(q_frame(u0, h0, d1, hinge), rest_w[idx['arm1_' + side]])
              w1 = limit_twist(w1, prev_w.get('arm' + side), v_norm(rest_t[idx['arm1sub_' + side]]), 12 * DEG)   # (no snapping of the upper arm about itself)
              prev_w['arm' + side] = w1
              w2 = q_mul(q_frame(u0, h0, d2, hinge), rest_w[idx['arm2_' + side]])
              loc['arm1_' + side] = q_norm(q_mul(q_conj(W['shoulder_' + side]), w1))
              loc['arm1sub_' + side] = (0.0, 0.0, 0.0, 1.0)
              loc['arm2_' + side] = q_norm(q_mul(q_conj(w1), w2))
              loc['arm2sub_' + side] = (0.0, 0.0, 0.0, 1.0)
              # (the wrist: the hand turned from the forearm toward the clip's hand direction, at most 60 degrees)
              loc['hand_' + side] = q_mul(q_limit(q_mul(q_conj(w2), q_mul(q_arc(d2, fore), w2)), 60 * DEG), rest_q[idx['hand_' + side]])
              # legs
              fk('leg1_' + side)
              p0 = P['leg1_' + side]
              goal = v_mul(ut(('LeftFootT' if side == 'L' else 'RightFootT'), t), S)
              src_hip = (sx * 0.10 * S, -0.05 * S, 0.0)
              target = v_add(p0, v_mul(v_sub(goal, src_hip), K_LEG))
              fn = 'LeftFoot' if side == 'L' else 'RightFoot'
              toe = v_norm(u_vec(*q_rot(q_norm((m(fn + 'Q.x', t), m(fn + 'Q.y', t), m(fn + 'Q.z', t), m(fn + 'Q.w', t))), (0.0, -1.0, 0.0))))
              pole = v_add(v_norm((toe[0], 0.0, toe[2])), (sx * 0.15, 0.0, 0.3))
              raw_pole[('leg' + side, fi)] = pole
              if pass_no == 1 and smooth_pole.get(('leg' + side, fi)): pole = smooth_pole[('leg' + side, fi)]
              knee, end, dirv, pp = two_bone(p0, target, LEG_A, LEG_B, pole, prev_pp.get('leg' + side))
              prev_pp['leg' + side] = pp
              d1 = v_norm(v_sub(knee, p0)); d2 = v_norm(v_sub(end, knee))
              hinge = v_norm(v_cross(pp, dirv))
              u0 = q_rot(rest_w[idx['leg1_' + side]], v_norm(rest_t[idx['leg1sub_' + side]]))
              h0 = v_norm(v_cross(u0, (0.0, 0.0, -1.0)))
              w1 = q_mul(q_frame(u0, h0, d1, hinge), rest_w[idx['leg1_' + side]])
              w2 = q_mul(q_frame(u0, h0, d2, hinge), rest_w[idx['leg2_' + side]])
              loc['leg1_' + side] = q_norm(q_mul(q_conj(W['crotch_' + side]), w1))
              loc['leg1sub_' + side] = (0.0, 0.0, 0.0, 1.0)
              loc['leg2_' + side] = q_norm(q_mul(q_conj(w1), w2))
              loc['leg2sub_' + side] = (0.0, 0.0, 0.0, 1.0)
              # the foot: its toes pointing like the clip's (from the rest foot-to-toe direction)
              rest_toe = q_rot(rest_w[idx['foot_' + side]], v_norm(rest_t[idx['toe_' + side]]))
              w_foot = q_mul(q_arc(rest_toe, toe), rest_w[idx['foot_' + side]])
              loc['foot_' + side] = q_norm(q_mul(q_conj(w2), w_foot))
          frames.append((root_t, [loc[n] for n in driven]))
    # (a 1-2-1 smoothing over neighbouring frames for the limbs: what jitter is left)
    limbs = [i for i, n in enumerate(driven) if n.startswith(('arm', 'leg', 'hand', 'foot'))]
    sm = []
    for fi in range(len(frames)):
        rt, qs = frames[fi]
        qs = list(qs)
        if 0 < fi < len(frames) - 1:
            for i in limbs:
                mid = q_slerpish(frames[fi - 1][1][i], frames[fi + 1][1][i], 0.5)
                qs[i] = q_slerpish(qs[i], mid, 0.5)
        sm.append((rt, qs))
    frames = sm
    with open(out_path, 'wb') as f:
        f.write(b'SEMO' + struct.pack('<IfII', 1, fps, len(frames), len(driven)))
        for n in driven: f.write(n.encode('ascii').ljust(32, b'\0'))
        for root_t, qs in frames:
            f.write(struct.pack('<3f', *root_t))
            for q in qs: f.write(struct.pack('<4f', *q))
    print('wrote %s: %d frames at %g fps (%.1f s), %d bones (clip "%s", Unity %s)' % (out_path, len(frames), fps, len(frames) / fps, len(driven), clip.get('m_Name'), unity))

if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    fps = 30.0
    if '--fps' in sys.argv: fps = float(sys.argv[sys.argv.index('--fps') + 1])
    convert(args[0], args[1], args[2], fps)
