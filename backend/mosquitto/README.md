# Mosquitto (production)

The dev stack (`mosquitto/config/mosquitto-test.conf`) allows anonymous
connections for convenience. **Production must never allow anonymous
connections.** The production config (`mosquitto/config/mosquitto.conf`)
authenticates every device via the HTTP auth backend:

```
auth_plugin /usr/lib/mosquitto/auth_plugin_http.so
auth_plugin_http_host api
auth_plugin_http_port 8080
auth_plugin_http_path /api/v1/mqtt/auth
```

The stock `eclipse-mosquitto:2` image does **not** ship the HTTP auth plugin.
Build a custom image that compiles `mosquitto-auth-plug`'s HTTP backend and
installs it at `/usr/lib/mosquitto/auth_plugin_http.so`.

## Build

`docker-compose.prod.yml` builds this directory with a `Dockerfile`. Provide
one that:

1. Installs build deps (`gcc`, `make`, `libcurl-dev`, `openssl-dev`, plus the
   mosquitto dev headers matching the base image version).
2. Clones `https://github.com/ieuanraymond/mosquitto-auth-plug` (or a pinned
   fork) at a tagged commit.
3. Sets `BACKEND_HTTP` in `config.mk` and builds only the HTTP backend.
4. Copies the resulting `auth-plugin-http.so` to
   `/usr/lib/mosquitto/auth_plugin_http.so`.
5. Extends `eclipse-mosquitto:2` and runs the stock entrypoint.

A minimal `Dockerfile` skeleton is intentionally **not** committed here because
the exact plugin source/commit and base-image ABI change over time. Pick a
plugin version whose `libmosquitto` matches the `eclipse-mosquitto:2` minor you
pin in `docker-compose.prod.yml`, then commit your working `Dockerfile`.

## Auth contract

`POST /api/v1/mqtt/auth` (in `internal/mqttauth.go`) returns 200 when the
device_key + api_key pair is valid and the device is active, otherwise 403.
The plugin sends username=device_key, password=api_key.

## Notes

- Keep `1883` behind a firewall restricted to known device subnets.
- Persistence is enabled (`persistence true`) so queued messages survive
  broker restarts; the volume is `mqtt_data`.
- For TLS listener on 8883 with client certs, add a second `listener 8883`
  block and mount certs — recommended for internet-exposed brokers.