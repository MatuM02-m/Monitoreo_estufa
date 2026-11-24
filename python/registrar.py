"""Escucha MQTT y agrega cada muestra a datos/mqtt_AAAAMMDD_HHMMSS.csv."""
import configparser
import json
from datetime import datetime
from pathlib import Path

import paho.mqtt.client as mqtt

RAIZ = Path(__file__).resolve().parent.parent


def fila(payload):
    d = json.loads(payload)
    t = "nan" if d["t"] is None else f"{float(d['t']):.4f}"
    return f"{int(d['ensayo'])},{int(d['epoch'])},{int(d['s'])},{t}"


def main():
    cfg = configparser.ConfigParser(interpolation=None)
    if not cfg.read(RAIZ / "python" / "credenciales.ini", encoding="utf-8"):
        raise SystemExit("Falta python/credenciales.ini (copiar el .example)")
    c = cfg["mqtt"]
    (RAIZ / "datos").mkdir(exist_ok=True)
    salida = RAIZ / "datos" / f"mqtt_{datetime.now():%Y%m%d_%H%M%S}.csv"

    def al_conectar(cliente, _u, _f, codigo, _p):
        print("Conexión:", codigo)
        if not codigo.is_failure:
            cliente.subscribe([("estufa/temperatura", 1), ("estufa/estado", 1)])

    def al_recibir(_c, _u, msg):
        if msg.topic == "estufa/estado":
            print("# ESP32:", msg.payload.decode(errors="replace"))
            return
        try:
            linea = fila(msg.payload)
        except (ValueError, KeyError, TypeError) as e:
            print("# Mensaje inválido:", e)
            return
        with salida.open("a", encoding="utf-8") as f:
            f.write(linea + "\n")
        print(linea)

    cliente = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    cliente.username_pw_set(c["usuario"], c["clave"])
    cliente.tls_set()
    cliente.on_connect = al_conectar
    cliente.on_message = al_recibir
    cliente.connect(c["host"], c.getint("puerto", 8883))
    print("Guardando en", salida)
    try:
        cliente.loop_forever()
    except KeyboardInterrupt:
        print("Fin")


if __name__ == "__main__":
    main()
