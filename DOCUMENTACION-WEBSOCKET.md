# Documentación: Control del carrito ESP32-CAM vía WebSocket

> Documento para el equipo de backend. Describe el protocolo exacto que implementa el firmware del carrito (ESP32-CAM) para verificar la integración.

---

## 1. Arquitectura

```mermaid
flowchart LR
    F[Frontend (usuario remoto)] -->|comandos| B[Backend Node<br/>puerto 3000]
    B <-->|WebSocket wss://:443| E[ESP32-CAM<br/>CLIENTE WebSocket]
    E --> M[Motores L298N + LED]
```

- El **ESP32 es un cliente WebSocket**, no un servidor. Es él quien inicia la conexión saliente.
- El backend es quien acepta la conexión, identifica al carro y le reenvía comandos.
- El carrito se conecta **solo** a la red WiFi de casa (modo STA). No crea red WiFi propia (AP) ni servidor web local: el control es únicamente por WebSocket.

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
| Video (opcional) | frames JPEG binarios                | **Desactivado** (`WS_ENVIAR_VIDEO = 0`) |

Notas:

- El carrito **no envía telemetría**: no tiene sensores de distancia ni proximidad.
- El backend solo recibirá el registro y, si se activa, frames de video.

---

## 4. Mensajes Backend → Carrito (comandos)

Deben enviarse como **frames de texto** con JSON válido (`ws.send(string)`). Los frames binarios son ignorados por el firmware.

### 4.1 Comandos principales (`cmd`)

| Comando         | JSON                            | Efecto                              |
| --------------- | ------------------------------- | ----------------------------------- |
| Avanzar         | `{"cmd":"adelante"}`            | Avanza                              |
| Retroceder      | `{"cmd":"atras"}`               | Retrocede                           |
| Girar izquierda | `{"cmd":"izquierda"}`           | Giro en sitio a la izquierda        |
| Girar derecha   | `{"cmd":"derecha"}`             | Giro en sitio a la derecha          |
| Parar           | `{"cmd":"parar"}`               | Frena los dos motores               |
| Velocidad       | `{"cmd":"velocidad","val":200}` | Ajusta velocidad PWM, `val` = 0–255 |
| Luz LED         | `{"cmd":"luz","val":255}`       | LED pin 4 por PWM, `val` = 0–255    |

### 4.2 Formato alternativo (`var`/`val`)

El firmware también acepta este formato alternativo, útil si el frontend ya lo usa:

| Variable                  | Valor     | Efecto    |
| ------------------------- | --------- | --------- |
| `{"var":"car","val":1}`   | 1         | Adelante  |
| `{"var":"car","val":2}`   | 2         | Derecha   |
| `{"var":"car","val":3}`   | 3         | Parar     |
| `{"var":"car","val":4}`   | 4         | Izquierda |
| `{"var":"car","val":5}`   | 5         | Atrás     |
| `{"var":"speed","val":N}` | N (0–255) | Velocidad |
| `{"var":"flash","val":N}` | N (0–255) | Luz LED   |

### 4.3 Reglas de parseo

- Campo `val` opcional; si falta, se toma `0`.
- Comandos desconocidos: el carrito los ignora y los registra en su serial (`[WS] Comando desconocido: ...`). **No** responden error.
- Los valores de `velocidad` y `luz` se limitan internamente a 0–255 (`constrain`).

---

## 5. Comportamientos de seguridad y conexión

- **Freno por pérdida de conexión**: si la conexión WebSocket se cae, el carrito frena los motores automáticamente (`WStype_DISCONNECTED` → `parar`).
- **Reconexión automática**: cada 5 s.
- **Heartbeat**: el cliente envía ping cada 15 s, espera pong 3 s, y se desconecta tras 2 fallos. El backend debe responder los pings normalmente (lo hace `ws` automáticamente).

---

## 6. Checklist de verificación del backend

- [ ] Acepta conexiones `wss://` en `arduino.libardo-apps.es:443` (proxy → Node :3000).
- [ ] Al recibir `{"tipo":"hola","nombre":"carro1"}` guarda/actualiza el socket del carro.
- [ ] Puede enviar frames de texto al socket del carro con los JSON de la sección 4.
- [ ] Al reconectarse el carro (socket nuevo), usa el socket actualizado.
- [ ] Prueba E2E: enviar `{"cmd":"parar"}` y ver en el monitor serial del carrito `[WS] Ejecutando: PARAR`.

### Pruebas recomendadas (con el carrito conectado al monitor serial 115200)

| Enviar                          | Esperado en el serial del carrito |
| ------------------------------- | --------------------------------- |
| `{"cmd":"adelante"}`            | `[WS] Ejecutando: ADELANTE...`    |
| `{"cmd":"atras"}`               | `[WS] Ejecutando: ATRAS`          |
| `{"cmd":"parar"}`               | `[WS] Ejecutando: PARAR`          |
| `{"cmd":"izquierda"}`           | `[WS] Ejecutando: IZQUIERDA`      |
| `{"cmd":"derecha"}`             | `[WS] Ejecutando: DERECHA`        |
| `{"var":"car","val":1}`         | mismo efecto que adelante         |
| `{"cmd":"velocidad","val":180}` | `[WS] Velocidad ajustada a 180`   |
| `{"cmd":"luz","val":255}`       | `[WS] Luz LED: 255`               |

---

## 7. Notas adicionales

- Firmware: `ESP32CAM_L298N_WebcarritounexpoG.ino` + `CameraWebServer.cpp` (sin servidor web local; `app_httpd.cpp` fue eliminado).
- El envío de video por WebSocket está desactivado en el firmware (`WS_ENVIAR_VIDEO 0`). Si se activa, llegarían frames JPEG binarios cada ~500 ms; el backend debe estar preparado para ignorarlos o enrutarlos aparte.
- Si el carrito no aparece conectado, verificar que en su monitor serial diga `WiFi OK` (conectado a la red WiFi de casa) y `[WS] Conectado al servidor`.
