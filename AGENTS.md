# Agent Instructions — eink-devdash

## Devcontainer is verplicht

Alle firmware build-stappen en flash-server commando's moeten **altijd binnen de devcontainer** worden uitgevoerd. Voer commando's nooit direct op de host uit.

De devcontainer heeft de container naam `optimistic_hermann` (VS Code devcontainer). Controleer of hij draait:

```bash
docker ps --filter "name=optimistic_hermann" --format "{{.Names}}\t{{.Status}}"
```

Commando's uitvoeren in de devcontainer — **altijd als `node` user** met `-u node`:

```bash
docker exec -u node optimistic_hermann bash -c "<commando>"
```

> **Belangrijk:** zonder `-u node` draaien commando's als root en worden bestanden (build/, sdkconfig, dependencies.lock) root-eigenaar, waarna de node user niet meer kan builden.

ESP-IDF activeren is niet nodig via een apart `source`-commando — de tools staan al op `PATH` in de container. Gebruik anders expliciet:

```bash
docker exec -u node optimistic_hermann bash -c "source /etc/profile.d/esp-idf.sh && <commando>"
```

## Veelgebruikte commando's

Alle commando's worden uitgevoerd als `docker exec -u node optimistic_hermann bash -c "..."`.

| Taak | Commando (binnen de container) |
|------|-------------------------------|
| Firmware bouwen | `cd /workspaces/eink-devdash/firmware && idf.py build` |
| Target instellen (eenmalig na clean) | `idf.py set-target esp32s3` |
| Flash-server starten | `cd /workspaces/eink-devdash/flash-server && bash serve.sh` |
| API dependencies installeren | `cd /workspaces/eink-devdash/api && npm install` |
| API starten | `cd /workspaces/eink-devdash/api && npm start` |

## Projectstructuur

```
firmware/          # ESP-IDF firmware (ESP32-S3)
  main/            # Applicatiecode
  components/      # Custom component: eink_weact29
flash-server/      # Web flasher (ESP Web Tools, poort 8080)
  bins/            # Gegenereerd door serve.sh na idf.py build
api/               # Node.js API server (poort 3000)
scripts/           # Hulpscripts
```

## idf_component.yml

Gebruik **geen** `espressif/json` in `idf_component.yml` — `json` (cJSON) is een ingebakken ESP-IDF component en wordt alleen via `CMakeLists.txt` `REQUIRES json` toegevoegd.

## Git safe.directory

De devcontainer heeft een andere UID dan de host. Voeg dit eenmalig toe als git klaagt:

```bash
docker exec optimistic_hermann git config --global --add safe.directory /workspaces/eink-devdash
```
