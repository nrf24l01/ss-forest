# SS Forest
*Summer school forest project*

---
ESP-IDF project for a tree-shaped low-power wireless network.

The root target is ESP32-WROOM-32 and the node target is ESP32-C3. The firmware transport is ESP-MESH over Wi-Fi, so the root must use a Wi-Fi-capable target.

The repository has two firmware applications:

- `root/` - fixed ESP-MESH root node. It forwards mesh session events to the PC over serial and accepts serial commands.
- `node/` - child/repeater node. It participates in ESP-MESH, scans BLE on button press, checks the nearest device distance, reports the session to root, waits for a root response, and shows state on WS2812B.

Shared mesh code is in `components/ss_mesh/`.

## Hardware

- Target: ESP32-WROOM-32 for the root and ESP32-C3 for nodes.
- Node button and WS2812B pins are configured with `CONFIG_SS_BUTTON_GPIO` and `CONFIG_SS_LED_GPIO`.
- WS2812B power: use a suitable 5 V/3.3 V setup for your LED strip and common GND with ESP32-C3.

## How It Works

1. Node works as an ESP-MESH repeater/client and connects to the root.
2. Button press opens its BLE GATT connectability window.
3. A user writes an 18-byte value: raw 16-byte team UUID followed by little-endian `uint16_t` attack points.
4. Node accepts the GATT write only when the client's RSSI-estimated distance is within `CONFIG_SS_BLE_MAX_DISTANCE_CM` (100 cm by default).
5. Node sends its MAC address, team UUID, attack points, RSSI, estimated distance, and a session ID to root through ESP-MESH.
6. Root prints the request and accepts one `color <node_mac> <#RRGGBB>` reply for it.
7. Node accepts the matching session color and updates LED0.

BLE distance from RSSI is approximate. Calibrate `CONFIG_SS_BLE_RSSI_AT_ONE_METER` for your beacon/device and environment before relying on the 1 m threshold.

## LED States

WS2812B uses two LEDs when `CONFIG_SS_LED_COUNT >= 2`:

- LED0 (team/ble):
  - Idle: last color received from root, or black if none.
  - Blue fast blink: BLE scan is running.
  - Blue slow blink: no valid BLE device found or nearest device is farther than threshold.
  - Yellow solid: valid BLE device found within threshold.
  - Green fast blink: sending session data to root.
  - Green solid: root response received.
  - Red solid: mesh send failed or root response timed out.
- LED1 (mesh):
  - Purple blink: mesh is trying to connect.
  - Green solid: mesh connected.
  - Red solid: mesh disconnected after being connected.

## Root Serial Output

Root prints UUID requests received from a user through a node BLE GATT write:

```text
UUID_REQUEST session=123456 node=aa:bb:cc:dd:ee:ff uuid=550e8400e29b41d4a716446655440000 attack_points=42 uuid_hex=550e8400e29b41d4a716446655440000
```

Fields:

- `session` - node-generated session id.
- `node` - mesh STA MAC of the reporting node.
- `uuid` - raw 16-byte team UUID in hexadecimal.
- `attack_points` - unsigned 16-bit count supplied by the user.
- `rssi` and `distance_cm` - BLE client signal strength and RSSI-based distance estimate used for the acceptance gate.

## Root Serial Commands

Use `idf.py monitor` or any serial terminal. Commands end with Enter.

```text
help
routes
nodes
tree
getcolor <node_mac>
color <node_mac> <#RRGGBB>
```

Commands:

- `help` - print available commands.
- `routes` - print current ESP-MESH routing table.
- `nodes` - print all currently connected node MAC addresses.
- `tree` - print a snapshot of the mesh tree. Direct-child relationships are marked as known; routed descendants use the root as a safe fallback parent.
- `getcolor <node_mac>` - request the current displayed LED0 color from any connected node.
- `color <node_mac> <#RRGGBB>` - send RGB color only for that node's outstanding UUID/attack-points request.

## Configuration

Run menuconfig separately for root and node:

```bash
idf.py -C root set-target esp32
idf.py -C root menuconfig
idf.py -C node set-target esp32c3
idf.py -C node menuconfig
```

Important shared mesh options are under `SS Forest Mesh Configuration`:

- `CONFIG_MESH_ROUTER_SSID` - upstream Wi-Fi router SSID used by root.
- `CONFIG_MESH_ROUTER_PASSWD` - upstream Wi-Fi password.
- `CONFIG_MESH_CHANNEL` - mesh/router channel. `0` lets ESP-MESH scan.
- `CONFIG_MESH_AP_PASSWD` - password used by mesh child connections.
- `CONFIG_MESH_MAX_LAYER` - max tree depth.
- `CONFIG_MESH_ROUTE_TABLE_SIZE` - max number of mesh devices in routing table.

Node-only options under `SS Forest Node Configuration`:

- `CONFIG_SS_BUTTON_GPIO` - default `2`.
- `CONFIG_SS_LED_GPIO` - default `7`.
- `CONFIG_SS_LED_COUNT` - default `2`.
- `CONFIG_SS_BLE_SCAN_SECONDS` - BLE scan duration per button press.
- `CONFIG_SS_BLE_CONNECT_WINDOW_SECONDS` - BLE discoverable/connectable window after button press.
- `CONFIG_SS_BLE_MAX_DISTANCE_CM` - default `100` cm.
- `CONFIG_SS_BLE_RSSI_AT_ONE_METER` - RSSI calibration value at 1 m, default `-59`.
- `CONFIG_SS_BLE_PATH_LOSS_EXPONENT_X10` - path loss exponent multiplied by 10, default `20`.
- `CONFIG_SS_ROOT_RESPONSE_TIMEOUT_MS` - node wait timeout for root response, default `20000`.

Root-only options under `SS Forest Root Configuration`:

- `CONFIG_SS_ROOT_DEFAULT_RESPONSE` - automatic response payload, default `OK`.

## Build And Flash

For repeatable builds and parallel flashing, use `tools/firmware.py`. Each
`--node-path` is paired with the corresponding `--node-port`:

```bash
python tools/firmware.py \
  --root-path root --root-port /dev/ttyUSB0 \
  --node-path node --node-port /dev/ttyUSB1 \
  --node-path node --node-port /dev/ttyUSB2 \
  --jobs 4
```

Use `--build-only` to build without flashing, or `--no-build` to flash
already-built images. Set `IDF_PY` or pass `--idf` when `idf.py` is not on
`PATH`.

Root:

```bash
idf.py -C root set-target esp32
idf.py -C root build
idf.py -C root -p /dev/ttyUSB0 flash monitor
```

Node:

```bash
idf.py -C node set-target esp32c3
idf.py -C node build
idf.py -C node -p /dev/ttyUSB1 flash monitor
```

Replace serial ports with your actual ports.

## PC Test Scripts

Install Python dependencies on the PC with `uv`:

```bash
uv sync
```

Run the root serial color responder. It listens for `UUID_REQUEST` from root and sends a color from a UUID-to-color table:

```bash
uv run ss-root-color-test --port /dev/ttyUSB0 --map 550e8400-e29b-41d4-a716-446655440000:ff0000 --print-all
```

You can also use a JSON table:

```bash
uv run ss-root-color-test --port /dev/ttyUSB0 --table-file tools/uuid_colors.example.json --color 00ff00 --print-all
```

The mesh API server handles both operations at the same time. It continuously watches root serial output, answers UUID color requests, periodically refreshes the tree into a buffer, and persists color changes to a JSON file:

```bash
uv run ss-mesh-api server \
  --port /dev/ttyUSB0 \
  --table-file tools/uuid_colors.json \
  --api-port 8080
```

Clients can configure colors without touching the serial port:

```bash
uv run ss-mesh-client color 550e8400-e29b-41d4-a716-446655440000 ff0000
uv run ss-mesh-client colors '{"550e8400-e29b-41d4-a716-446655440000":"ff0000","other-uuid":"00ff00"}'
uv run ss-mesh-client tree
```

API endpoints are `GET /api/tree`, `GET /api/colors`, `PUT /api/colors/{uuid}` with `{"color":"RRGGBB"}`, and `PATCH /api/colors` with a UUID-to-color JSON object.

## Go Mesh API Tool

The same watcher and API are also implemented in Go under `tools/go/`:

```bash
cd tools/go
go run . server --port /dev/ttyUSB0 --table-file ../../tools/uuid_colors.json
```

Use the Go client in another terminal:

```bash
cd tools/go
go run . client color 550e8400-e29b-41d4-a716-446655440000 ff0000
go run . client colors '{"uuid-one":"ff0000","uuid-two":"00ff00"}'
go run . client tree
```

The Go server owns the serial port, responds to UUID requests, continuously buffers the latest tree, and exposes the same HTTP API as the Python implementation.

Send a UUID from PC to a node over BLE GATT:

```bash
uv run ss-ble-uuid-test --uuid 550e8400-e29b-41d4-a716-446655440000 --attack-points 42
```

Expected flow:

1. `ble_uuid_test.py` writes UUID to BLE device `SS-FOREST-NODE`.
2. Node sends `SS_PACKET_UUID_REQUEST` through ESP-MESH to root.
3. Root prints `UUID_REQUEST ...` over USB serial.
4. `root_color_test.py` detects the line, looks up `uuid:color`, and sends `color <node_mac> <RRGGBB>` to root.
5. Root sends `SS_PACKET_COLOR_RESPONSE` through ESP-MESH to the node.
6. Node sets WS2812B to the received RGB color.

The BLE service UUID is `01008f7a-8e13-6e9b-8348-6df4029a6c70` and the writable characteristic UUID is `02008f7a-8e13-6e9b-8348-6df4029a6c70`.

## Notes

- The root is configured for ESP32-WROOM-32 and nodes for ESP32-C3 because ESP-MESH requires Wi-Fi.
- The root firmware fixes itself as ESP-MESH root using `esp_mesh_fix_root(true)` and `esp_mesh_set_type(MESH_ROOT)`.
- Child nodes are not fixed root and can form the tree below root.
