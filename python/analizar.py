"""Indicadores y gráfico del ensayo. Acepta CSV de MQTT o volcado serie.

python python/analizar.py datos/archivo.csv --setpoint 37 --desde 120 --hasta 1080
"""
import argparse
import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Admite un prefijo de hora del monitor serie
LINEA = re.compile(r"(?:^|\s)(\d+),(\d+),(\d+),(nan|-?\d+(?:\.\d+)?)\s*$")


def leer(rutas):
    ensayos = {}
    for r in rutas:
        texto = Path(r).read_text(encoding="utf-8", errors="replace")
        for linea in texto.splitlines():
            m = LINEA.search(linea)
            if m:
                ensayos.setdefault(int(m[1]), {})[int(m[3])] = float(m[4])
    return ensayos


def ciclos(x, y, banda=0.15):
    """Período y amplitud pico a pico, con cruces por la tendencia con histéresis."""
    resid = y - np.polyval(np.polyfit(x, y, 1), x)
    arriba, cruces = None, []
    for i, v in enumerate(resid):
        if v > banda / 2:
            if arriba is False:
                cruces.append(i)
            arriba = True
        elif v < -banda / 2:
            arriba = False
    if len(cruces) < 3:
        return None
    amp = [np.ptp(y[a:b]) for a, b in zip(cruces, cruces[1:])]
    return np.median(np.diff(x[cruces])), np.median(amp), len(amp)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("archivos", nargs="+")
    p.add_argument("--setpoint", type=float, required=True)
    p.add_argument("--tol", type=float, default=1.0)
    p.add_argument("--desde", type=float, default=120, help="min: inicio del régimen")
    p.add_argument("--hasta", type=float, help="min: fin del régimen (antes de abrir)")
    p.add_argument("--ensayo", type=int)
    a = p.parse_args()

    ensayos = leer(a.archivos)
    if not ensayos:
        raise SystemExit("No hay muestras")
    for k, d in sorted(ensayos.items()):
        print(f"Ensayo {k}: {len(d)} muestras, {(max(d) - min(d)) / 3600:.2f} h")
    n = a.ensayo if a.ensayo is not None else max(ensayos, key=lambda k: len(ensayos[k]))
    if n not in ensayos:
        raise SystemExit(f"No existe el ensayo {n}")
    if len(ensayos) > 1:
        print(f"Hay varios (¿reinicios?). Se usa el {n}; elegir otro con --ensayo")

    s = np.array(sorted(ensayos[n]), dtype=float)
    t = np.array([ensayos[n][int(v)] for v in s])
    invalidas = int(np.isnan(t).sum())
    x, t = (s - s[0])[~np.isnan(t)] / 60, t[~np.isnan(t)]
    if len(x) < 2:
        raise SystemExit("No hay suficientes lecturas válidas")
    hueco = np.diff(x).max() * 60
    hasta = a.hasta if a.hasta is not None else x[-1]
    reg = (x >= a.desde) & (x <= hasta)
    if reg.sum() < 10:
        raise SystemExit("Pocas muestras en el régimen: revisar --desde/--hasta")
    xr, r = x[reg], t[reg]

    print(f"\nInválidas: {invalidas} | hueco máximo: {hueco:.0f} s")
    print(f"Régimen: {xr[0]:.0f}-{xr[-1]:.0f} min ({len(r)} muestras)")
    print(f"Media: {r.mean():.2f} °C | sesgo: {r.mean() - a.setpoint:+.2f} °C")
    print(f"Desvío estándar: {r.std(ddof=1):.3f} °C")
    print(f"Mínimo: {r.min():.2f} °C | máximo: {r.max():.2f} °C")
    dentro = np.mean(np.abs(r - a.setpoint) <= a.tol) * 100
    print(f"Dentro de ±{a.tol:g} °C: {dentro:.1f} % de las muestras")
    print(f"Deriva: {np.polyfit(xr, r, 1)[0] * 60:+.4f} °C/h")
    c = ciclos(xr, r)
    print("Ciclos: no detectados" if c is None else
          f"Ciclos: período {c[0]:.1f} min, {c[1]:.2f} °C pico a pico ({c[2]} ciclos)")

    previo = x <= hasta
    ini = (x < a.desde) & previo
    if ini.any():
        print(f"Arranque: máximo {t[ini].max():.2f} °C "
              f"({t[ini].max() - a.setpoint:+.2f} °C respecto del setpoint)")
    fuera = np.flatnonzero((np.abs(t - a.setpoint) > a.tol) & previo)
    if not len(fuera):
        print("Estabilización: siempre dentro de tolerancia")
    elif x[fuera[-1]] >= x[previo][-1]:
        print("Estabilización: termina fuera de tolerancia")
    else:
        print(f"Estabilización: última salida de tolerancia a los {x[fuera[-1]]:.0f} min")

    fig, ejes = plt.subplots(2, 1, figsize=(10, 7), constrained_layout=True)
    for eje, m in ((ejes[0], np.ones_like(x, bool)), (ejes[1], reg)):
        eje.axhspan(a.setpoint - a.tol, a.setpoint + a.tol, color="0.9")
        eje.axhline(a.setpoint, color="0.5", ls="--", lw=1)
        eje.plot(x[m] / 60, t[m], lw=0.9, color="#1f5fa8")
        eje.set_ylabel("Temperatura (°C)")
        eje.set_xlabel("Tiempo (h)")
        eje.grid(alpha=0.3)
    ejes[0].axvline(a.desde / 60, color="0.3", ls=":", lw=1)
    if a.hasta is not None:
        ejes[0].axvline(a.hasta / 60, color="0.3", ls=":", lw=1)
    salida = Path(a.archivos[0]).with_suffix(".png")
    fig.savefig(salida, dpi=150)
    print("\nGráfico:", salida)


if __name__ == "__main__":
    main()
