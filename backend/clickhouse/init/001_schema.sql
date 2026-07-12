-- Phase 1: core telemetry table. MVs and TTL added in later phases.
CREATE TABLE IF NOT EXISTS device_telemetry (
    device_id       String,
    device_type     String,
    ts              DateTime64(3),

    -- Common metadata
    rssi            Int8,
    uptime_ms       UInt32,

    -- Computed fields (enriched by ingest worker)
    pv_power        Float32,
    battery_power   Float32,
    inverter_power  Float32,
    dc_load_power   Float32,
    system_status   UInt8,
    min_soc_pct     Float32,
    max_soc_pct     Float32,
    total_energy_wh Float32,

    -- Raw device-specific measurements
    fields          Map(String, Float64),

    ingested_at     DateTime DEFAULT now()
) ENGINE = MergeTree
  PARTITION BY toYYYYMM(ts)
  ORDER BY (device_type, device_id, ts)
  SETTINGS index_granularity = 8192;
