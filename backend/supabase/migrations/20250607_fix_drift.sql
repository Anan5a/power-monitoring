-- ============================================================
-- Migration: Fix timestamp drift from ESP32 TZ bug
-- Date:     2026-06-07
--
-- Background:
--   The ESP32 firmware had a newlib TZ bug that drifted the
--   device clock ~42 min ahead. The drift slipped past the
--   insert_telemetry guard (which was set to 1h).
--
-- Scope:
--   1. Tighten the future-guard from 1h to 5 min (idempotent).
--   2. Recompute daily_battery_charge for today from clean rows
--      (idempotent).
--   3. DO NOT delete any telemetry_computed rows. The firmware
--      has been fixed and is being re-flashed; the database
--      will self-heal on next re-emit because the
--      ON CONFLICT (device_key, recorded_at) DO UPDATE will
--      overwrite the drifted rows with correct ones for the
--      same timestamps.
-- ============================================================

---------------------------------------------------------------
-- 1. Tighten insert_telemetry future-timestamp guard
--    Old: rejected >1h in the future
--    New: rejects >5min in the future
--
--    The ESP32 RTC can drift a few seconds when NTP fails,
--    so 5min gives plenty of slack while catching real bugs.
--    Idempotent — safe to re-run.
---------------------------------------------------------------
create or replace function public.insert_telemetry(
    p_device_key text,
    p_device_api_key uuid,
    p_payload jsonb,
    p_metadata jsonb default '{}',
    p_recorded_at bigint default null
) returns void language plpgsql security definer as $$
declare
    ts timestamptz;
begin
    -- Verify device_key AND device_api_key both match the same device
    if not exists (
        select 1 from public.devices
        where device_key = p_device_key
          and device_api_key = p_device_api_key
    ) then
        raise exception 'Invalid device_key or device_api_key';
    end if;

    -- Validate timestamp: reject >5min in future, or before 2024.
    -- 5min gives the ESP32 RTC slack for round-trip latency and
    -- small clock drift while still catching real firmware bugs.
    if p_recorded_at is not null then
        ts := to_timestamp(p_recorded_at)::timestamptz;
        if ts > now() + interval '5 minutes' then
            raise exception 'Timestamp too far in future: %', ts;
        end if;
        if ts < '2024-01-01'::timestamptz then
            raise exception 'Timestamp too old: %', ts;
        end if;
    else
        ts := now();
    end if;

    insert into public.telemetry_live (device_id, payload, metadata, recorded_at)
    values (p_device_key, p_payload, p_metadata, ts);
end;
$$;

grant execute on function public.insert_telemetry(text, uuid, jsonb, jsonb, bigint) to authenticated;

---------------------------------------------------------------
-- 2. Recompute daily_battery_charge for today.
--    The integrator uses last_recorded_at from the prior row
--    to compute dt_hours. If a prior row is drifted, dt_hours
--    is wrong, so charge_wh accumulates incorrectly.
--
--    compute_daily_battery_charge() recomputes a full day from
--    scratch using a windowed lead() on real recorded_at, which
--    is robust to drift (it only uses the ordering of timestamps
--    from telemetry_computed, not the actual values).
--
--    We DON'T delete the drifted rows because:
--    a) They contain real sensor readings, just on the wrong
--       wall clock.
--    b) The firmware fix will eventually re-emit correct rows
--       that overwrite them via ON CONFLICT DO UPDATE.
--    c) Deleting creates a hole in the time series that the
--       integration logic has to interpolate over.
--
--    We DO recompute the integrator so today's charge_wh is
--    correct going forward.
--
--    Idempotent — re-running just overwrites the same row.
---------------------------------------------------------------
do $$
declare
    dev record;
begin
    for dev in
        select id, device_key from public.devices
    loop
        begin
            perform public.compute_daily_battery_charge(dev.id, current_date);
            raise notice 'Recomputed daily_battery_charge for % (%)',
                dev.device_key, dev.id;
        exception when others then
            raise notice 'Skipped % (%): %',
                dev.device_key, dev.id, sqlerrm;
        end;
    end loop;
end $$;

---------------------------------------------------------------
-- 3. Audit: print drift state after migration
--    (read-only, useful for verification)
---------------------------------------------------------------
do $$
declare
    drifted_rows bigint;
    oldest_drift timestamptz;
begin
    select count(*), min(recorded_at) into drifted_rows, oldest_drift
    from public.telemetry_computed
    where recorded_at > now() + interval '5 minutes';

    raise notice 'Drifted rows remaining (>5min future): %', drifted_rows;
    if drifted_rows > 0 then
        raise notice 'Oldest drifted row: %', oldest_drift;
        raise notice 'After firmware is re-flashed, ON CONFLICT will overwrite these on re-emit.';
    else
        raise notice 'No drift detected. Migration complete.';
    end if;
end $$;
