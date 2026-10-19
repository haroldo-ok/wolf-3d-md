"""Colour quantisation helpers: Genesis 9-bit colours, Lab k-means palettes."""
import numpy as np

LEVELS = np.array([0, 36, 73, 109, 146, 182, 219, 255], dtype=np.float64)   # 3 bit DAC


def rgb_to_lab(rgb):
    rgb = np.asarray(rgb, dtype=np.float64) / 255.0
    a = np.where(rgb > 0.04045, ((rgb + 0.055) / 1.055) ** 2.4, rgb / 12.92)
    m = np.array([[0.4124564, 0.3575761, 0.1804375],
                  [0.2126729, 0.7151522, 0.0721750],
                  [0.0193339, 0.1191920, 0.9503041]])
    xyz = a @ m.T
    xyz /= np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16.0 / 116.0)
    L = 116 * f[..., 1] - 16
    A = 500 * (f[..., 0] - f[..., 1])
    B = 200 * (f[..., 1] - f[..., 2])
    return np.stack([L, A, B], axis=-1)


# all 512 Genesis colours
GEN = np.array([(r, g, b) for b in range(8) for g in range(8) for r in range(8)])
GEN_RGB = LEVELS[GEN]
GEN_LAB = rgb_to_lab(GEN_RGB)


def snap(rgb):
    """nearest Genesis colour (3 bit triple) for an 8 bit rgb triple"""
    lab = rgb_to_lab(np.asarray(rgb, dtype=np.float64))
    i = np.argmin(((GEN_LAB - lab) ** 2).sum(axis=1))
    return tuple(GEN[i])


def snap_many(rgbs):
    lab = rgb_to_lab(np.asarray(rgbs, dtype=np.float64))
    d = ((lab[:, None, :] - GEN_LAB[None, :, :]) ** 2).sum(axis=2)
    return GEN[np.argmin(d, axis=1)]


def kmeans_palette(colors, weights, k, fixed=(), iters=40, seed=1):
    """colors: (n,3) 8-bit rgb of distinct colours, weights (n,)
    fixed: list of 3-bit triples that must be present (they stay fixed).
    returns list of k 3-bit triples (fixed first)."""
    rng = np.random.RandomState(seed)
    lab = rgb_to_lab(colors)
    w = np.asarray(weights, dtype=np.float64)
    fixedlab = [rgb_to_lab(LEVELS[np.array(f)]) for f in fixed]
    nf = len(fixed)
    nfree = k - nf
    # init: k-means++ on weights
    cent = []
    idx = rng.choice(len(lab), p=w / w.sum())
    cent.append(lab[idx])
    allc = list(fixedlab) + cent
    for _ in range(nfree - 1):
        d = np.min(((lab[:, None, :] - np.array(allc)[None, :, :]) ** 2).sum(axis=2), axis=1)
        p = d * w
        if p.sum() <= 0:
            p = w
        idx = rng.choice(len(lab), p=p / p.sum())
        cent.append(lab[idx])
        allc = list(fixedlab) + cent
    cent = np.array(cent)
    for _ in range(iters):
        allc = np.array(list(fixedlab) + list(cent))
        d = ((lab[:, None, :] - allc[None, :, :]) ** 2).sum(axis=2)
        a = np.argmin(d, axis=1)
        newc = cent.copy()
        for j in range(nfree):
            m = a == nf + j
            if m.any():
                newc[j] = (lab[m] * w[m, None]).sum(axis=0) / w[m].sum()
        # snap to the Genesis grid each iteration
        sn = ((newc[:, None, :] - GEN_LAB[None, :, :]) ** 2).sum(axis=2)
        newc = GEN_LAB[np.argmin(sn, axis=1)]
        if np.allclose(newc, cent):
            break
        cent = newc
    out = [tuple(f) for f in fixed]
    sn = ((cent[:, None, :] - GEN_LAB[None, :, :]) ** 2).sum(axis=2)
    for i in np.argmin(sn, axis=1):
        out.append(tuple(GEN[i]))
    return out


def palette_lab(pal3):
    return rgb_to_lab(LEVELS[np.array(pal3)])


def nearest_index(lab_colors, pal3):
    pl = palette_lab(pal3)
    d = ((lab_colors[:, None, :] - pl[None, :, :]) ** 2).sum(axis=2)
    return np.argmin(d, axis=1), d.min(axis=1)


def best_kmeans(colors, weights, k, fixed=(), tries=6):
    best = None
    lab = rgb_to_lab(colors)
    for s in range(tries):
        p = kmeans_palette(colors, weights, k, fixed, seed=s + 1)
        _, err = nearest_index(lab, p)
        e = float((err * weights).sum() / weights.sum())
        if best is None or e < best[0]:
            best = (e, p)
    return best[1], best[0]
