-- ============================================================
-- Audit script: characterize timestamp drift in the database
-- Read-only — does NOT modify any data
--
-- Background:
--   ESP32 firmware had a TZ-bug that drifted the device clock.
--   `p_recorded_at` (Unix epoch) is shifted forward by the bug.
--   `insert_telemetry` only rejects rows >1h in the future, so
--   42-min drift sneaks in. Need to know the damage before
--   deciding on a cleanup.
--
-- Run in Supabase SQL editor. Inspect the output. Re-run
-- periodically to track drift if the firmware bug returns.
-- ============================================================

---------------------------------------------------------------
-- 1. Year-bucket distribution
--    Any rows in 2082? Any in 1970 (epoch 0)?
---------------------------------------------------------------
select
    extract(year from recorded_at)::int as yr,
    count(*) as rows
from public.telemetry_computed
group by 1
order by 1;

---------------------------------------------------------------
-- 2. Drift distribution per device (telemetry_computed)
--    Drift = recorded_at - now()
--    Grouped into buckets. Positive = clock was ahead.
---------------------------------------------------------------
select
    device_key,
    case
        when recorded_at < '2024-01-01'::timestamptz then 'A. pre-2024'
        when extract(year from recorded_at) = 2082 then 'B. year 2082'
        when recorded_at > now() + interval '5 minutes' then 'C. future >5min (drifted)'
        when recorded_at > now() + interval '1 minute' then 'D. future 1-5min (mild)'
        when recorded_at > now() then 'E. future <1min (clock skew, normal)'
        else 'F. past (normal)'
    end as bucket,
    count(*) as rows,
    min(recorded_at) as min_ts,
    max(recorded_at) as max_ts
from public.telemetry_computed
group by device_key, bucket
order by device_key, bucket;

---------------------------------------------------------------
-- 3. Continuous drift histogram (15-min buckets)
--    Use this to see whether drift is constant or variable.
---------------------------------------------------------------
select
    device_key,
    width_bucket(
        extract(epoch from (recorded_at - now()))::numeric,
        -86400, 86400, 96  -- 96 buckets of 30 min from -24h to +24h
    ) as bucket_idx,
    -- Translate to readable label
    (extract(epoch from (recorded_at - now())) / 60)::int
        as drift_min_approx,
    count(*) as rows
from public.telemetry_computed
where recorded_at > now() - interval '1 day'
   or recorded_at > now()  -- future rows too
group by device_key, bucket_idx, drift_min_approx
order by device_key, bucket_idx;

---------------------------------------------------------------
-- 4. Per-day row count for the last 7 days
--    Sudden drop in today's count = drift cutoff likely.
--    Sudden jump = drift started.
---------------------------------------------------------------
select
    device_key,
    (recorded_at at time zone 'UTC')::date as day,
    count(*) as rows,
    min(recorded_at) as first,
    max(recorded_at) as last,
    max(recorded_at) - min(recorded_at) as span
from public.telemetry_computed
where recorded_at > now() - interval '7 days'
   or recorded_at > now()
group by device_key, day
order by device_key, day desc;

---------------------------------------------------------------
-- 5. drift seconds per row — top 20 worst offenders (future)
---------------------------------------------------------------
select
    device_key,
    recorded_at,
    recorded_at - now() as drift,
    recorded_at - lag(recorded_at) over (
        partition by device_key order by recorded_at
    ) as gap_from_prev
from public.telemetry_computed
where recorded_at > now()
order by recorded_at desc
limit 20;

---------------------------------------------------------------
-- 6. Was the drift ever in the past? (clock behind reality)
--    Less likely with this bug, but check anyway.
---------------------------------------------------------------
select
    device_key,
    count(*) as rows,
    min(now() - recorded_at) as max_age,
    avg(now() - recorded_at) as avg_age
from public.telemetry_computed
where recorded_at < now() - interval '1 day'
group by device_key;

---------------------------------------------------------------
-- 7. telemetry_live: should be empty (passthrough)
--    Any rows here = trigger failed to delete.
---------------------------------------------------------------
select
    count(*) as live_rows,
    min(recorded_at) as oldest,
    max(recorded_at) as newest,
    (extract(year from min(recorded_at)))::int as oldest_year,
    (extract(year from max(recorded_at)))::int as newest_year
from public.telemetry_live;

---------------------------------------------------------------
-- 8. daily_battery_charge for today — is the integrator sane?
--    Large dt_hours accumulated = drift is poisoning the integration.
---------------------------------------------------------------
select
    dbc.device_id,
    d.device_key,
    dbc.date,
    dbc.charge_wh,
    dbc.capacity_wh,
    dbc.energy_in_wh,
    dbc.energy_out_wh,
    dbc.last_recorded_at,
    dbc.last_recorded_at - now() as last_drift,
    dbc.computed_from
from public.daily_battery_charge dbc
join public.devices d on d.id = dbc.device_id
where dbc.date = current_date
order by d.device_key;

---------------------------------------------------------------
-- 9. Device last_seen_at — does it match the latest clean row?
---------------------------------------------------------------
select
    d.device_key,
    d.last_seen_at,
    d.last_seen_at - now() as last_seen_drift,
    (select max(recorded_at) from public.telemetry_computed tc
        where tc.device_key = d.device_key
          and tc.recorded_at <= now() + interval '5 minutes') as latest_clean_row,
    (select max(recorded_at) from public.telemetry_computed tc
        where tc.device_key = d.device_key) as latest_any_row
from public.devices d
order by d.device_key;

---------------------------------------------------------------
-- 10. insert_telemetry behavior check (dry-run, no insert)
--     Tests whether the drifted values would be REJECTED by
--     the current validation (>1h future).
--     If the answer is "no" (drift < 1h), the validation
--     is too loose — that's the schema fix to apply.
---------------------------------------------------------------
do $$
declare
    test_ts timestamptz := now() + interval '42 minutes';
    test_epoch bigint := extract(epoch from test_ts)::bigint;
    would_be_rejected boolean;
begin
    would_be_rejected := test_ts > now() + interval '1 hour';
    raise notice 'Drift of 42 min (test epoch=%) → rejected by current 1h guard? %',
        test_epoch, would_be_rejected;
    raise notice '→ A 42-min drift slips through. Tighten guard to 5 min.';
end $$;

---------------------------------------------------------------
-- 11. Cron job status — are the maintenance jobs still running?
--     If rollup/maintenance is failing, drift would never get
--     purged by the normal pipeline.
---------------------------------------------------------------
select jobname, schedule, active, last_started_time, last_run_status
from cron.job_run_details jrd
join cron.job j on j.jobid = jrd.jobid
order by last_started_time desc nulls last
limit 20;

---------------------------------------------------------------
-- 12. Rollup state — what is the cutoff?
--     Rows older than 48h should already be rolled up to 2-sec
--     and the originals deleted. If they aren't, the rollup
--     cron is broken.
---------------------------------------------------------------
select
    count(*) filter (where recorded_at < now() - interval '48 hours') as raw_over_48h,
    count(*) filter (where recorded_at >= now() - interval '48 hours') as raw_under_48h,
    count(*) as total
from public.telemetry_computed;

---------------------------------------------------------------
-- 13. Year-2082 row check (separate from #1, in case the
--     cleanup migration was already applied and we want to
--     verify it stuck).
---------------------------------------------------------------
select
    'telemetry_computed year=2082' as what,
    count(*) as rows
from public.telemetry_computed
where extract(year from recorded_at) = 2082
union all
select
    'telemetry_live year=2082',
    count(*)
from public.telemetry_live
where extract(year from recorded_at) = 2082;
