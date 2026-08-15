# Documentación: Control del carrito ESP32-CAM vía WebSocket

> Documento para el equipo de backend. Describe el protocolo exacto que implementa el firmware del carrito (ESP32-CAM) para verificar la integración.

---

## 1. Arquitectura

```mermaid
flowchart LR
    F[Frontend (usuario remoto)] -->|comandos| B[Backend Node<br/>puerto 3000]
    B <-->|WebSocket wss://:443| E[ESP32-CAM<br/>CLIENTE WebSocket]
    E --> M[Motores L298N + LED]
    E --> U[Sensor HC-SR04]
```

- El **ESP32 es un cliente WebSocket**, no un servidor. Es él quien inicia la conexión saliente.
- El backend es quien acepta la conexión, identifica al carro y le reenvía comandos.
- El carrito se conecta **solo** a la red WiFi de casa (modo STA). Ya no crea red WiFi propia (AP). Su web local queda accesible en la IP que obtenga de la red de casa.

### Datos de conexión del carrito

| Parámetro    | Valor                                      |
| ------------ | ------------------------------------------ |
| Host         | `arduino.libardo-apps.es`                  |
| Puerto       | `443` (TLS)                                |
| Path         | `/`                                        |
| Protocolo    | `wss://` (WebSocket seguro)                |
| Backend real | Node en puerto `3000`, detrás de proxy TLS |

> ⚠️ El ESP32 **no valida el certificado TLS** (conexión insegura para pruebas). No exigir certificados de cliente; solo aceptar `wss` normal.

---

## 2. Identificación del carro

Al conectarse (y cada vez que reconecta), el carrito envía **inmediatamente**:

```json
{ "tipo": "hola", "nombre": "carro1" }
```

**Requisito para el backend:**

- Guardar/actualizar el socket asociado a `carro1` cada vez que se reciba este mensaje.
- El carrito se reconecta solo cada 5 s tras una caída. Cada reconexión genera un **socket nuevo**, así que la referencia anterior queda obsoleta. **Siempre sobrescribir** con el socket del último `"hola"`.

---

## 3. Mensajes Carrito → Backend

| Tipo             | Formato                             | Frecuencia                              |
| ---------------- | ----------------------------------- | --------------------------------------- |
| Registro         | `{"tipo":"hola","nombre":"carro1"}` | Al conectar/reconectar                  |
| Telemetría       | `{"distancia":<int cm>}`            | Cada 1 s (solo si está conectado)       |
| Video (opcional) | frames JPEG binarios                | **Desactivado** (`WS_ENVIAR_VIDEO = 0`) |

Notas:

- `distancia` es la lectura del sensor ultrasónico HC-SR04 en centímetros.
- Si no hay eco válido, el firmware reporta `400` (valor centinela = "sin lectura").
- El backend puede reenviar esta telemetría al frontend que esté suscrito al carro.

---

## 4. Mensajes Backend → Carrito (comandos)

Deben enviarse como **frames de texto** con JSON válido (`ws.send(string)`). Los frames binarios son ignorados por el firmware.

### 4.1 Comandos principales (`cmd`)

| Comando         | JSON                            | Efecto                                                 |
| --------------- | ------------------------------- | ------------------------------------------------------ |
| Avanzar         | `{"cmd":"adelante"}`            | Avanza, con freno automático si hay obstáculo a <15 cm |
| Retroceder      | `{"cmd":"atras"}`               | Retrocede (sin freno por sensor)                       |
| Girar izquierda | `{"cmd":"izquierda"}`           | Giro en sitio a la izquierda                           |
| Girar derecha   | `{"cmd":"derecha"}`             | Giro en sitio a la derecha                             |
| Parar           | `{"cmd":"parar"}`               | Frena los dos motores                                  |
| Velocidad       | `{"cmd":"velocidad","val":200}` | Ajusta velocidad PWM, `val` = 0–255                    |
| Luz LED         | `{"cmd":"luz","val":255}`       | LED pin 4 por PWM, `val` = 0–255                       |

### 4.2 Formato compatible con la web local (`var`/`val`)

El firmware también acepta el protocolo de su web local, útil si el frontend ya lo usa:

| Variable                  | Valor     | Efecto                          |
| ------------------------- | --------- | ------------------------------- |
| `{"var":"car","val":1}`   | 1         | Adelante (con freno por sensor) |
| `{"var":"car","val":2}`   | 2         | Derecha                         |
| `{"var":"car","val":3}`   | 3         | Parar                           |
| `{"var":"car","val":4}`   | 4         | Izquierda                       |
| `{"var":"car","val":5}`   | 5         | Atrás                           |
| `{"var":"speed","val":N}` | N (0–255) | Velocidad                       |
| `{"var":"flash","val":N}` | N (0–255) | Luz LED                         |

### 4.3 Reglas de parseo

- Campo `val` opcional; si falta, se toma `0`.
- Comandos desconocidos: el carrito los ignora y los registra en su serial (`[WS] Comando desconocido: ...`). **No** responden error.
- Los valores de `velocidad` y `luz` se limitan internamente a 0–255 (`constrain`).

---

## 5. Comportamientos de seguridad y conexión

- **Freno por pérdida de conexión**: si la conexión WebSocket se cae, el carrito frena los motores automáticamente (`WStype_DISCONNECTED` → `parar`).
- **Freno por obstáculo**: en "adelante", si el sensor mide <15 cm, no avanza (frena solo).
- **Reconexión automática**: cada 5 s.
- **Heartbeat**: el cliente envía ping cada 15 s, espera pong 3 s, y se desconecta tras 2 fallos. El backend debe responder los pings normalmente (lo hace `ws` automáticamente).

---

## 6. Checklist de verificación del backend

- [ ] Acepta conexiones `wss://` en `arduino.libardo-apps.es:443` (proxy → Node :3000).
- [ ] Al recibir `{"tipo":"hola","nombre":"carro1"}` guarda/actualiza el socket del carro.
- [ ] Puede enviar frames de texto al socket del carro con los JSON de la sección 4.
- [ ] Reenvía la telemetría `{"distancia":N}` a los frontends suscritos.
- [ ] Al reconectarse el carro (socket nuevo), usa el socket actualizado.
- [ ] Prueba E2E: enviar `{"cmd":"parar"}` y ver en el monitor serial del carrito `Stop`.

### Pruebas recomendadas (con el carrito conectado al monitor serial 115200)

| Enviar                          | Esperado en el serial del carrito       |
| ------------------------------- | --------------------------------------- |
| `{"cmd":"adelante"}`            | avanza (si hay >15 cm libres)           |
| `{"cmd":"atras"}`               | `Backward`                              |
| `{"cmd":"parar"}`               | `Stop`                                  |
| `{"cmd":"izquierda"}`           | `TurnLeft`                              |
| `{"cmd":"derecha"}`             | `TurnRight`                             |
| `{"var":"car","val":1}`         | mismo efecto que adelante               |
| `{"cmd":"velocidad","val":180}` | cambia velocidad (visible en la marcha) |
| `{"cmd":"luz","val":255}`       | LED encendido                           |

Además, cada segundo debe llegar al backend `{"distancia":<cm>}`.

---

## 7. Notas adicionales

- Firmware: `ESP32CAM_L298N_WebcarritounexpoG.ino` + `app_httpd.cpp` + `CameraWebServer.cpp`.
- Librerías: `arduinoWebSockets` (cliente) y `ArduinoJson`.
- El envío de video por WebSocket está desactivado en el firmware (`WS_ENVIAR_VIDEO 0`). Si se activa, llegarían frames JPEG binarios cada ~500 ms; el backend debe estar preparado para ignorarlos o enrutarlos aparte.
- Si el carrito no aparece conectado, verificar que en su monitor serial diga `WiFi OK` (conectado a la red WiFi de casa) y `[WS] Conectado al servidor`.
