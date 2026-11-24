# Monitoreo de estufa

ESP32 + DS18B20: una muestra cada 10 s, guardada en flash y enviada por MQTT
con TLS (HiveMQ Cloud). Conexión: DQ a GPIO25, 4,7 kΩ entre DQ y 3,3 V.

1. Completar `include/secrets.h` y cargar el firmware desde PlatformIO.
2. `python -m pip install -r python/requirements.txt`
3. Copiar `python/credenciales.ini.example` como `credenciales.ini` y completar.
4. `python python/registrar.py`
5. `python python/analizar.py datos/archivo.csv --setpoint 37 --desde 120`

Formato: `ensayo,epoch,segundos,temp_C`. Cada arranque crea un ensayo nuevo.
`epoch` = 0 sin hora de red; `nan` = lectura inválida.

Recuperar la flash: `pio device monitor -f log2file`, enviar `d` y esperar `# FIN`.
El volcado pausa la medición: hacerlo al terminar. Analizar el `.log` que se crea.
Borrar ensayos: enviar `BORRAR` y Enter. Cargar el firmware no los borra;
"Erase Flash" o cambiar las particiones, sí.

No abrir el monitor durante el ensayo: puede reiniciar el ESP32.

## Panel web (sin PC encendida)

`docs/index.html` muestra el ensayo leyendo HiveMQ por WSS (puerto 8884). Con
`?demo` usa datos simulados. El ESP32 publica mensajes retenidos en
`estufa/hist/<ensayo % 4>/<hora>`: todas las lecturas, hasta 36 h y 4 ensayos.
El CSV que descarga sirve para `analizar.py`. La copia completa queda en la flash.
