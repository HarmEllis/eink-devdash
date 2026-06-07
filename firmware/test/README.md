# Firmware host unit tests

Host-side unit tests for the pure OTA trust-anchor helpers and runtime policies in
`firmware/main/ota_version.c` (`ota_download_url_is_canonical`,
`ota_version_is_newer`) plus `clock_should_apply` and `api_url_is_relay`.
The helpers have no ESP-IDF HTTP/TLS dependency, so
they compile and run on the ESP-IDF `linux` target without flashing hardware.

Run inside the devcontainer as the `node` user (per `AGENTS.md`):

```bash
docker exec -u node optimistic_hermann bash -c "source /etc/profile.d/esp-idf.sh && \
  cd /workspaces/eink-devdash/firmware/test && \
  idf.py --preview set-target linux && idf.py build && \
  ./build/ota_version_host_test.elf"
```

The test runner's `app_main` exits non-zero if any Unity case fails, so the
final command's exit status gates the suite.
