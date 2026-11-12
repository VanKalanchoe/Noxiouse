"""Draws the Project Manager's template pictures (NoxEditor/assets/Templates/Basic.png, Blank.png; 384 x 216, 16:9).

Each template ships a picture of what it starts with, as Unreal's templates do (their Media/ images): Basic, the view the editor opens
a new Basic project on (EditorLayer::ApplyBasicSceneView: 35 degrees round), framed lower so the sky shows, of the 50 m floor under
the procedural sky with its 54-degree sun; Blank, an empty dark scene. A project shows its own screenshot once
it has one (Saved/Thumbnail.png). To use real screenshots instead, overwrite the PNGs (any 16:9 picture).

    python3 scripts/make_template_pictures.py
"""
import os

import numpy as np
from PIL import Image

W, H, SS = 384, 216, 3  # size, supersampling per axis
OUT = os.path.join(os.path.dirname(__file__), "..", "NoxEditor", "assets", "Templates")


def camera_rays(distance, pitch_deg, yaw_deg, fov_deg):
    pitch, yaw = np.radians(pitch_deg), np.radians(yaw_deg)
    # Orbit camera round the origin: back along its view direction by `distance`, looking down by `pitch`.
    forward = np.array([-np.sin(yaw) * np.cos(pitch), -np.sin(pitch), -np.cos(yaw) * np.cos(pitch)])
    position = -forward * distance
    right = np.cross(forward, [0.0, 1.0, 0.0])
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    w, h = W * SS, H * SS
    tan = np.tan(np.radians(fov_deg) * 0.5)
    xs = (np.arange(w) + 0.5) / w * 2.0 - 1.0
    ys = 1.0 - (np.arange(h) + 0.5) / h * 2.0
    x, y = np.meshgrid(xs * tan * (W / H), ys * tan)
    d = forward[None, None, :] + x[..., None] * right[None, None, :] + y[..., None] * up[None, None, :]
    d /= np.linalg.norm(d, axis=-1, keepdims=True)
    return position, d


def value_noise(p, seed):
    rng = np.random.default_rng(seed)
    table = rng.random((256, 256))
    i = np.floor(p).astype(int)
    f = p - i
    f = f * f * (3.0 - 2.0 * f)
    def at(dx, dy):
        return table[(i[..., 0] + dx) & 255, (i[..., 1] + dy) & 255]
    a, b, c, d = at(0, 0), at(1, 0), at(0, 1), at(1, 1)
    return (a * (1 - f[..., 0]) + b * f[..., 0]) * (1 - f[..., 1]) + (c * (1 - f[..., 0]) + d * f[..., 0]) * f[..., 1]


def fbm(p, octaves=5):
    total, amplitude = 0.0, 0.5
    for o in range(octaves):
        total = total + value_noise(p, 7 + o) * amplitude
        p = p * 2.03
        amplitude *= 0.5
    return total


def tonemap(c):
    # ACES fit (Narkowicz), then sRGB.
    c = np.clip((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def downsample(img):
    return img.reshape(H, SS, W, SS, 3).mean(axis=(1, 3))


def save(img, name):
    os.makedirs(OUT, exist_ok=True)
    Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(OUT, name))


def basic():
    # Framed lower and farther back than the editor's opening view (25 degrees down shows a sliver of sky): the floor and the sky.
    position, d = camera_rays(32.0, 9.0, 35.0, 55.0)
    sun_elevation, sun_azimuth = np.radians(54.0), np.radians(-40.0)
    sun = np.array([np.cos(sun_elevation) * np.sin(sun_azimuth), np.sin(sun_elevation), -np.cos(sun_elevation) * np.cos(sun_azimuth)])

    # Sky: a blue that pales towards the horizon, the sun's glow, soft clouds on a layer 2 km up.
    elevation = np.clip(d[..., 1], 0.0, 1.0)
    zenith, horizon = np.array([0.10, 0.26, 0.80]), np.array([0.62, 0.76, 0.95])
    t = np.power(elevation, 0.45)[..., None]
    sky = horizon * (1 - t) + zenith * t
    cos_sun = np.clip(np.sum(d * sun, axis=-1), -1.0, 1.0)[..., None]
    sky = sky + np.array([1.0, 0.9, 0.7]) * (np.power(np.clip(cos_sun, 0, 1), 64) * 0.6 + np.power(np.clip(cos_sun, 0, 1), 6) * 0.12)
    sky = sky + (cos_sun > 0.9995) * 30.0
    with np.errstate(divide="ignore", invalid="ignore"):
        travel = np.where(d[..., 1] > 0.02, 2000.0 / d[..., 1], 0.0)
    cloud_uv = (d[..., [0, 2]] * travel[..., None]) / 900.0
    density = np.clip((fbm(cloud_uv + 3.0) - 0.48) * 3.2, 0.0, 1.0) * np.clip(d[..., 1] * 6.0, 0.0, 1.0)
    cloud_color = np.array([1.0, 1.0, 1.0]) * (0.95 + 0.25 * np.clip(cos_sun, 0, 1))
    sky = sky * (1 - density[..., None] * 0.85) + cloud_color * density[..., None] * 0.85

    # Below the horizon, beyond the floor: the sky's ground, hazy.
    below = np.clip(-d[..., 1], 0.0, 1.0)[..., None]
    ground = horizon * 0.55 * (1 - np.power(below, 0.3)) + np.array([0.18, 0.19, 0.21]) * np.power(below, 0.3)

    # The 50 m floor (light grey, lit by the sun and a little sky), with its edge.
    with np.errstate(divide="ignore", invalid="ignore"):
        hit_t = np.where(d[..., 1] < -1e-4, -position[1] / d[..., 1], np.inf)
    hit = np.nan_to_num(position[None, None, :] + d * hit_t[..., None], nan=0.0, posinf=0.0, neginf=0.0)
    on_floor = np.isfinite(hit_t) & (np.abs(hit[..., 0]) <= 25.0) & (np.abs(hit[..., 2]) <= 25.0)
    albedo = 0.42
    floor = albedo * (np.array([1.0, 0.96, 0.9]) * sun[1] * 1.15 + np.array([0.35, 0.45, 0.65]) * 0.35)
    # A faint 1 m grid, as the editor's, fading with distance.
    gx = np.abs(hit[..., 0] - np.round(hit[..., 0]))
    gz = np.abs(hit[..., 2] - np.round(hit[..., 2]))
    line = (np.minimum(gx, gz) < 0.025) * np.clip(1.0 - hit_t / 45.0, 0.0, 1.0)
    floor_img = floor * (1.0 - 0.18 * line[..., None])
    haze = np.clip(hit_t / 300.0, 0.0, 0.3)[..., None]
    floor_img = floor_img * (1 - haze) + horizon * haze

    img = np.where((d[..., 1] >= 0)[..., None], sky, ground)
    img = np.where(on_floor[..., None], floor_img, img)
    save(downsample(tonemap(img * 0.8)), "Basic.png")


def blank():
    position, d = camera_rays(15.0, 25.0, 35.0, 60.0)
    img = np.zeros(d.shape) + np.array([0.012, 0.013, 0.016])
    with np.errstate(divide="ignore", invalid="ignore"):
        hit_t = np.where(d[..., 1] < -1e-4, -position[1] / d[..., 1], np.inf)
    hit = np.nan_to_num(position[None, None, :] + d * hit_t[..., None], nan=0.0, posinf=0.0, neginf=0.0)
    gx = np.abs(hit[..., 0] - np.round(hit[..., 0]))
    gz = np.abs(hit[..., 2] - np.round(hit[..., 2]))
    fade = np.where(np.isfinite(hit_t), np.clip(1.0 - hit_t / 60.0, 0.0, 1.0), 0.0)
    line = (np.minimum(gx, gz) < 0.02) * fade
    img = img + np.array([0.03, 0.033, 0.04]) * line[..., None]
    save(downsample(np.clip(img, 0, 1) ** (1 / 2.2)), "Blank.png")


if __name__ == "__main__":
    basic()
    blank()
    print("wrote", os.path.abspath(OUT))
