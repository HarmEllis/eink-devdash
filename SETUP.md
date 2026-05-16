# Devcontainer + Remote SSH + ESP32 flash setup

## Vereisten

| Machine | Software |
|---------|----------|
| Lokaal  | VS Code + [Remote SSH](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-ssh) + [Dev Containers](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) extensie |
| Remote  | Docker, `usbip` (`sudo apt install usbip linux-tools-generic`) |
| Lokaal  | `usbip` (`sudo apt install usbip`) |

## Eenmalige setup: usbip

### 1. Lokale machine — USB delen
```bash
# Eenmalig: modprobe persistent maken
echo "usbip_host" | sudo tee /etc/modules-load.d/usbip.conf

# ESP32 pluggen, dan:
./scripts/usbip-share.sh
# Script zoekt automatisch het ESP32/serial device en deelt het
# Laat dit terminal venster open
```

### 2. Remote host — USB attachen (voor devcontainer start)
```bash
./scripts/usbip-attach.sh <ip-lokale-machine>
# Bijv: ./scripts/usbip-attach.sh 192.168.1.50
# Controleer: ls /dev/ttyACM0
```

### 3. VS Code openen
```
VS Code → Remote SSH → verbind met remote host → open /home/.../eink-devdash
→ "Reopen in Container"
```

De devcontainer heeft `--device=/dev/ttyACM0` en kan dan direct flashen.

## Flashen vanuit de devcontainer

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Of via de ESP-IDF VS Code extensie (knop in statusbalk).

## API server starten

```bash
cd api
npm run dev
# Luistert op localhost:3000 (geforward naar lokale machine)
```

Test: `curl -H "Authorization: Bearer <token>" http://localhost:3000/api/dashboard`

## Troubleshooting

**`/dev/ttyACM0` niet gevonden in container**
- Controleer op remote host: `ls /dev/ttyACM*`
- Als niet aanwezig: usbip-attach.sh opnieuw uitvoeren
- Container herstarten na attach (device mount bij container start)

**ESP32-S3 Super Mini herkend als `/dev/ttyUSB0`** (CH340 chip)
- Pas `idf.port` in devcontainer.json aan naar `/dev/ttyUSB0`
- En `--device=/dev/ttyUSB0` in `runArgs`

**usbip verbinding verloren**
- Lokaal: `./scripts/usbip-share.sh` opnieuw
- Remote: `sudo usbip detach -p 0 && ./scripts/usbip-attach.sh <ip>`
