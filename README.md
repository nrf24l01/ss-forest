# SS Forest

ESP-IDF project for an ESP32-C3 tree-shaped ESP-MESH network.

The repository has two firmware applications:

- `root/` - fixed ESP-MESH root node. It forwards mesh session events to the PC over serial and accepts serial commands.
- `node/` - child/repeater node. It participates in ESP-MESH, scans BLE on button press, checks the nearest device distance, reports the session to root, waits for a root response, and shows state on WS2812B.

Shared mesh code is in `components/ss_mesh/`.

## Hardware

- Target: ESP32-C3 on every device.
- Button: GPIO2, input pull-up. Connect button between GPIO2 and GND.
- WS2812B DIN: GPIO7.
- WS2812B power: use a suitable 5 V/3.3 V setup for your LED strip and common GND with ESP32-C3.

## How It Works

1. Node works as ESP-MESH repeater/client and connects to the root.
2. Button press starts BLE scan.
3. The strongest BLE advertising device is treated as the nearest device.
4. Distance is estimated from RSSI and accepted only when it is not above `CONFIG_SS_BLE_MAX_DISTANCE_CM`.
5. Node sends `NODE_REPORT` to root through ESP-MESH.
6. Root prints the report to serial and sends a response.
7. Node receives the response and finishes the session.

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

Root prints one line per completed BLE scan report:

```text
NODE_REPORT session=123456 node=aa:bb:cc:dd:ee:ff ble=11:22:33:44:55:66 rssi=-52 distance_cm=78 payload_hex=020106...
```

Root also prints UUID requests received from a PC through a node BLE GATT write:

```text
UUID_REQUEST session=123456 node=aa:bb:cc:dd:ee:ff uuid=550e8400-e29b-41d4-a716-446655440000 uuid_hex=353530...
```

Fields:

- `session` - node-generated session id.
- `node` - mesh STA MAC of the reporting node.
- `ble` - nearest BLE advertiser address.
- `rssi` - received signal strength.
- `distance_cm` - RSSI-based estimated distance.
- `payload_hex` - raw BLE advertising payload.

## Root Serial Commands

Use `idf.py monitor` or any serial terminal. Commands end with Enter.

```text
help
routes
reply <text>
color <#RRGGBB>
sendcolor <node_mac> <#RRGGBB>
send <node_mac> <text>
```

Commands:

- `help` - print available commands.
- `routes` - print current ESP-MESH routing table.
- `reply <text>` - send a response to the last node that produced `NODE_REPORT`.
- `color <#RRGGBB>` - send RGB color to the last node that produced `UUID_REQUEST` or `NODE_REPORT`.
- `send <node_mac> <text>` - send a response to a specific node MAC. This uses session id `0`, which nodes accept as an out-of-band response.
- `sendcolor <node_mac> <#RRGGBB>` - send RGB color to a specific node MAC. This uses session id `0` and immediately sets WS2812B on the node.

The root also sends `CONFIG_SS_ROOT_DEFAULT_RESPONSE` automatically for every `NODE_REPORT`.

## Configuration

Run menuconfig separately for root and node:

```bash
idf.py -C root set-target esp32c3
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
- `CONFIG_SS_BLE_MAX_DISTANCE_CM` - default `100` cm.
- `CONFIG_SS_BLE_RSSI_AT_ONE_METER` - RSSI calibration value at 1 m, default `-59`.
- `CONFIG_SS_BLE_PATH_LOSS_EXPONENT_X10` - path loss exponent multiplied by 10, default `20`.
- `CONFIG_SS_ROOT_RESPONSE_TIMEOUT_MS` - node wait timeout for root response, default `20000`.

Root-only options under `SS Forest Root Configuration`:

- `CONFIG_SS_ROOT_DEFAULT_RESPONSE` - automatic response payload, default `OK`.

## Build And Flash

Root:

```bash
idf.py -C root set-target esp32c3
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

Send a UUID from PC to a node over BLE GATT:

```bash
uv run ss-ble-uuid-test --uuid 550e8400-e29b-41d4-a716-446655440000
```

Expected flow:

1. `ble_uuid_test.py` writes UUID to BLE device `SS-FOREST-NODE`.
2. Node sends `SS_PACKET_UUID_REQUEST` through ESP-MESH to root.
3. Root prints `UUID_REQUEST ...` over USB serial.
4. `root_color_test.py` detects the line, looks up `uuid:color`, and sends `color <RRGGBB>` to root.
5. Root sends `SS_PACKET_COLOR_RESPONSE` through ESP-MESH to the node.
6. Node sets WS2812B to the received RGB color.

The BLE service UUID is `01008f7a-8e13-6e9b-8348-6df4029a6c70` and the writable characteristic UUID is `02008f7a-8e13-6e9b-8348-6df4029a6c70`.

## Notes

- All devices are ESP32-C3.
- The root firmware fixes itself as ESP-MESH root using `esp_mesh_fix_root(true)` and `esp_mesh_set_type(MESH_ROOT)`.
- Child nodes are not fixed root and can form the tree below root.
- BLE scanning and Wi-Fi mesh share the 2.4 GHz radio. Very long BLE scans can reduce mesh responsiveness.
