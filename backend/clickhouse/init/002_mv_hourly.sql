-- ClickHouse hourly aggregate materialized view
CREATE MATERIALIZED VIEW IF NOT EXISTS telemetry_hourly
  ENGINE = AggregatingMergeTree
  PARTITION BY toYYYYMM(hour)
  ORDER BY (device_type, device_id, hour)
AS SELECT
    device_type,
    device_id,
    toStartOfHour(ts) AS hour,
    avgState(pv_power)         AS pv_power_avg,
    maxState(pv_power)         AS pv_power_max,
    avgState(battery_power)    AS battery_power_avg,
    avgState(inverter_power)   AS inverter_power_avg,
    avgState(dc_load_power)    AS dc_load_power_avg,
    argMaxState(total_energy_wh, ts) AS energy_last,
    minState(min_soc_pct)      AS min_soc_pct,
    maxState(max_soc_pct)      AS max_soc_pct,
    countState()               AS sample_count
FROM device_telemetry
GROUP BY device_type, device_id, hour;
