-- Speed up get_aggregated_telemetry RPC: composite index on (device_key, recorded_at)
-- covers the WHERE clause (equality + range scan) and ORDER BY.
-- The existing unique constraint also creates a b-tree index, but a dedicated
-- index with INCLUDE columns lets the query be satisfied from the index alone
-- (index-only scan) for the most-requested metric (power).

create index concurrently if not exists idx_telemetry_computed_device_time
    on public.telemetry_computed (device_key, recorded_at desc)
    include (pv_power, battery_power, inverter_power, dc_load_power,
             ch0_p, ch1_p, ch2_p, ch3_p,
             soc_pct0);
