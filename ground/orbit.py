"""Ground station geometry and pass prediction (ICD 7.4).

- look angles (elevation, azimuth, range) from a ground station to a spacecraft position in Earth-fixed axes
- pass prediction: propagate the spacecraft from one 42 truth sample and find when it rises above the mask

42 propagates the NOS3 orbit as a two-body orbit (Orb_LEO.txt, perturbations off), so the predictor does too,
and its passes match the simulation's. The inertial (42 "N", J2000) to Earth-fixed ("W") rotation at the truth
sample is solved from the position and velocity pairs (TRIAD): it includes the ~0.36 deg of precession since
J2000, which a plain rotation about z would miss.
"""
import math

MU = 3.986004418e14       # m^3/s^2
OMEGA_E = 7.2921150e-5    # rad/s
WGS84_A = 6378137.0
WGS84_F = 1 / 298.257223563


# ---- Small vector helpers (pure Python: no numpy in the NOS3 image) ----

def add(a, b):
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]


def sub(a, b):
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


def scale(a, k):
    return [a[0] * k, a[1] * k, a[2] * k]


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def norm(a):
    return math.sqrt(dot(a, a))


def unit(a):
    return scale(a, 1.0 / norm(a))


def matvec(m, v):
    return [dot(m[0], v), dot(m[1], v), dot(m[2], v)]


def rot_z(angle, v):
    """Rotates the axes by angle about z (a vector's components in axes turned by +angle)."""
    c, s = math.cos(angle), math.sin(angle)
    return [c * v[0] + s * v[1], -s * v[0] + c * v[1], v[2]]


# ---- Ground station ----

class GroundStation:
    def __init__(self, lat_deg, lon_deg, alt_m=0.0, mask_deg=10.0):
        self.lat, self.lon, self.mask = math.radians(lat_deg), math.radians(lon_deg), mask_deg
        e2 = WGS84_F * (2 - WGS84_F)
        n = WGS84_A / math.sqrt(1 - e2 * math.sin(self.lat) ** 2)
        cl, sl, co, so = math.cos(self.lat), math.sin(self.lat), math.cos(self.lon), math.sin(self.lon)
        self.ecef = [(n + alt_m) * cl * co, (n + alt_m) * cl * so, (n * (1 - e2) + alt_m) * sl]
        self.east = [-so, co, 0.0]
        self.north = [-sl * co, -sl * so, cl]
        self.up = [cl * co, cl * so, sl]

    def look(self, sat_ecef):
        """(elevation deg, azimuth deg, range km) to a position in Earth-fixed axes (m)."""
        d = sub(sat_ecef, self.ecef)
        rng = norm(d)
        el = math.degrees(math.asin(max(-1.0, min(1.0, dot(d, self.up) / rng))))
        az = math.degrees(math.atan2(dot(d, self.east), dot(d, self.north))) % 360.0
        return el, az, rng / 1000.0


# ---- Propagation ----

def _accel(r):
    rn = norm(r)
    return scale(r, -MU / rn ** 3)


def _rk4(r, v, dt):
    a1 = _accel(r)
    r2, v2 = add(r, scale(v, dt / 2)), add(v, scale(a1, dt / 2))
    a2 = _accel(r2)
    r3, v3 = add(r, scale(v2, dt / 2)), add(v, scale(a2, dt / 2))
    a3 = _accel(r3)
    r4, v4 = add(r, scale(v3, dt)), add(v, scale(a3, dt))
    a4 = _accel(r4)
    r_new = add(r, scale(add(add(v, scale(v2, 2)), add(scale(v3, 2), v4)), dt / 6))
    v_new = add(v, scale(add(add(a1, scale(a2, 2)), add(scale(a3, 2), a4)), dt / 6))
    return r_new, v_new


def _triad(a_n, b_n, a_w, b_w):
    """Rotation matrix W <- N from two vector pairs (a is trusted most)."""
    def frame(a, b):
        t1 = unit(a)
        t2 = unit(cross(a, b))
        return [t1, t2, cross(t1, t2)]
    fn, fw = frame(a_n, b_n), frame(a_w, b_w)
    # R = sum_k fw_k fn_k^T
    return [[sum(fw[k][i] * fn[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


class Predictor:
    """Propagates from one truth sample: pos/vel inertial (N) and Earth-fixed (W), metres and m/s."""

    def __init__(self, pos_n, vel_n, pos_w, vel_w):
        self.r0, self.v0 = list(pos_n), list(vel_n)
        # In W the inertial velocity is the Earth-relative velocity plus the Earth's rotation
        v_inertial_w = add(list(vel_w), cross([0.0, 0.0, OMEGA_E], list(pos_w)))
        self.w_from_n = _triad(pos_n, vel_n, pos_w, v_inertial_w)

    def ecef(self, r_n, dt):
        """Earth-fixed position of inertial r_n, dt seconds after the truth sample."""
        return rot_z(OMEGA_E * dt, matvec(self.w_from_n, r_n))

    def passes(self, gs, horizon_s=43200.0, step_s=10.0):
        """Passes within the horizon: list of (aos_s, los_s, max_elevation_deg), times after the truth sample.
        A pass in progress at t=0 has aos_s 0."""
        out = []
        r, v, t = self.r0, self.v0, 0.0
        el = gs.look(self.ecef(r, t))[0]
        aos = 0.0 if el >= gs.mask else None
        max_el = el if aos is not None else -90.0
        prev_el = el
        while t < horizon_s:
            r, v = _rk4(r, v, step_s)
            t += step_s
            el = gs.look(self.ecef(r, t))[0]
            if aos is None and el >= gs.mask:
                aos = t - step_s * (el - gs.mask) / (el - prev_el)  # interpolate the crossing
                max_el = el
            elif aos is not None:
                max_el = max(max_el, el)
                if el < gs.mask:
                    los = t - step_s * (gs.mask - el) / (prev_el - el)
                    out.append((aos, los, max_el))
                    aos, max_el = None, -90.0
            prev_el = el
        return out
