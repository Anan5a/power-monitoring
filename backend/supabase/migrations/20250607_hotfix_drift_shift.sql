-- ============================================================
-- Migration: Hotfix drifted timestamps (firmware not yet flashed)
-- Date:     2026-06-07
--
-- Why this exists:
--   The ESP32 TZ bug drifts the device clock ~42 min ahead.
--   The firmware fix is built but not yet flashed. To keep
--   data flowing without corrupting the time series, this
--   migration:
--
--   1. Restores the 1h future-guard in insert_telemetry
--      (was tightened to 5min in the previous migration).
--      Drifted inserts will be accepted again so the device
--      can keep emitting. The drifted rows are corrected by
--      this script.
--
--   2. Shifts drifted rows BACK by their drift amount.
--      Sensor values are preserved. recorded_at is corrected
--      to what the device's wall clock SHOULD have read.
--
--   3. Recomputes daily_battery_charge for today from the
--      now-correct timestamps.
--
-- Run once. After the firmware is flashed, re-run the previous
-- migration (20250607_fix_drift.sql) to re-tighten the guard
-- and re-recompute the integrator.
-- ============================================================

---------------------------------------------------------------
-- 1. Restore 1h future-guard (so device's drifted inserts
--    are accepted while we correct them here).
--    TEMPORARY: re-tighten to 5min after firmware is flashed.
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
    if not exists (
        select 1 from public.devices
        where device_key = p_device_key
          and device_api_key = p_device_api_key
    ) then
        raise exception 'Invalid device_key or device_api_key';
    end if;

    if p_recorded_at is not null then
        ts := to_timestamp(p_recorded_at)::timestamptz;
        -- TEMPORARY 1h guard: firmware not yet flashed, need to
        -- accept drifted inserts so we can correct them in SQL.
        -- Re-tighten to 5min AFTER firmware is flashed.
        if ts > now() + interval '1 hour' then
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
-- 2. PREVIEW: per-device drift windows (read-only)
--
--    Drift detection signal: a row in the FUTURE (recorded_at > now())
--    preceded by a row in the PAST (recorded_at <= now()).
--    The gap between them in the recorded_at sequence is
--    (D + 1 second), where D is the wall-clock jump.
--    We subtract (gap - 1s) = D from each drifted row so the
--    first drifted row lands 1s after the last clean row.
---------------------------------------------------------------
with ordered as (
    select
        device_key,
        recorded_at,
        lag(recorded_at) over (partition by device_key order by recorded_at) as prev_ts
    from public.telemetry_computed
    where recorded_at >= current_date
)
select
    'preview' as step,
    device_key,
    prev_ts as last_clean_ts,
    recorded_at as first_drifted_ts,
    (recorded_at - prev_ts - interval '1 second') as drift_offset,
    extract(epoch from (recorded_at - prev_ts - interval '1 second'))::int as drift_seconds
from ordered
where prev_ts is not null
  and prev_ts <= now()
  and recorded_at > now() + interval '5 minutes'
order by device_key, recorded_at asc;

---------------------------------------------------------------
-- 3. SHIFT: subtract drift_offset from every drifted row.
--
--    Strategy: for each device, find the first drifted row's
--    drift_offset (from preview above). Apply the same offset
--    to every drifted row for that device.
--
--    Wrapped in a transaction so it's all-or-nothing.
---------------------------------------------------------------
begin;

drop table if exists _drift_corrections;
create temp table _drift_corrections (
    device_key text,
    old_recorded_at timestamptz,
    new_recorded_at timestamptz,
    drift_offset interval
) on commit drop;

-- For each device, compute its drift offset once and apply to all drifted rows.
insert into _drift_corrections (device_key, old_recorded_at, new_recorded_at, drift_offset)
with device_drift as (
    select distinct on (device_key)
        device_key,
        (recorded_at - lag(recorded_at) over (partition by device_key order by recorded_at)) - interval '1 second' as drift_offset
    from public.telemetry_computed
    where recorded_at >= current_date
      and recorded_at > now() + interval '5 minutes'
    order by device_key, recorded_at asc
),
drifted_rows as (
    select
        tc.device_key,
        tc.recorded_at as old_recorded_at,
        tc.recorded_at - dd.drift_offset as new_recorded_at,
        dd.drift_offset
    from public.telemetry_computed tc
    join device_drift dd on dd.device_key = tc.device_key
    where tc.recorded_at >= current_date
      and tc.recorded_at > now() + interval '5 minutes'
      and dd.drift_offset > interval '0 seconds'
)
select * from drifted_rows;

---------------------------------------------------------------
-- 3a. AUDIT: show what will be shifted (read-only, runs as SELECT)
---------------------------------------------------------------
select
    'audit' as step,
    device_key,
    count(*) as rows,
    min(old_recorded_at) as min_old,
    max(old_recorded_at) as max_old,
    min(new_recorded_at) as min_new,
    max(new_recorded_at) as max_new,
    avg(extract(epoch from drift_offset))::int as avg_drift_sec
from _drift_corrections
group by device_key
order by device_key;

---------------------------------------------------------------
-- 3b. APPLY: update each row. Skip if the new timestamp collides
--    with an existing row at the same (device_key, recorded_at).
---------------------------------------------------------------
do $$
declare
    rec record;
    conflicts bigint := 0;
    applied bigint := 0;
begin
    for rec in select * from _drift_corrections loop
        if exists (
            select 1 from public.telemetry_computed
            where device_key = rec.device_key
              and recorded_at = rec.new_recorded_at
        ) then
            conflicts := conflicts + 1;
            continue;
        end if;

        update public.telemetry_computed
        set recorded_at = rec.new_recorded_at
        where device_key = rec.device_key
          and recorded_at = rec.old_recorded_at;

        applied := applied + 1;
    end loop;

    raise notice 'Applied: % rows. Conflicts (skipped): % rows.', applied, conflicts;
end $$;

commit;

---------------------------------------------------------------
-- 3c. APPLY RESULT: re-run the audit SELECT against the
--     actual table to show the post-shift state
---------------------------------------------------------------
with drifted_remaining as (
    select
        device_key,
        recorded_at,
        recorded_at - lag(recorded_at) over (partition by device_key order by recorded_at) as gap_from_prev
    from public.telemetry_computed
    where recorded_at >= current_date
)
select
    'post_shift' as step,
    count(*) filter (where recorded_at > now() + interval '5 minutes') as drifted_remaining,
    count(*) as total_rows_today
from public.telemetry_computed
where recorded_at >= current_date;

---------------------------------------------------------------
-- 4. Recompute daily_battery_charge for today from corrected
--    timestamps. Idempotent.
---------------------------------------------------------------
do $$
declare
    dev record;
begin
    for dev in select id, device_key from public.devices loop
        begin
            perform public.compute_daily_battery_charge(dev.id, current_date);
        exception when others then
            raise notice 'Skipped % (%): %', dev.device_key, dev.id, sqlerrm;
        end;
    end loop;
end $$;

---------------------------------------------------------------
-- 4a. RECOMPUTE RESULT: show today's battery state per device
---------------------------------------------------------------
select
    'recompute' as step,
    d.device_key,
    dbc.charge_wh,
    dbc.capacity_wh,
    dbc.energy_in_wh,
    dbc.energy_out_wh,
    dbc.last_recorded_at,
    dbc.last_recorded_at - now() as last_drift
from public.daily_battery_charge dbc
join public.devices d on d.id = dbc.device_id
where dbc.date = current_date
order by d.device_key;

---------------------------------------------------------------
-- 5. POST-AUDIT: print drift state after migration
---------------------------------------------------------------
select
    'post_audit' as step,
    (select count(*) from public.telemetry_computed
     where recorded_at > now() + interval '5 minutes') as drifted_rows,
    (select min(recorded_at) from public.telemetry_computed
     where recorded_at > now() + interval '5 minutes') as oldest_drift,
    (select max(recorded_at) from public.telemetry_computed
     where recorded_at > now() + interval '5 minutes') as newest_drift;
