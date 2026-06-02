-- ============================================================
-- Migration: Time-based retention strategy
-- Date: 2026-06-02
--
-- Strategy:
--   telemetry_live     -> purge after 48h (raw payload already copied to computed)
--   telemetry_computed -> keep 90 days (primary long-term store, includes raw_payload)
--   telemetry_backup   -> daily aggregates for data older than 90 days
-- ============================================================

-- 1. Create backup table for daily aggregates of old computed data
-------------------------------------------------------------
create table if not exists public.telemetry_backup (
    id bigint generated always as identity primary key,
    device_key text not null,
    day date not null,

    -- Daily aggregates of computed power values
    pv_power_avg float not null default 0,
    pv_power_min float not null default 0,
    pv_power_max float not null default 0,

    battery_power_avg float not null default 0,
    battery_power_min float not null default 0,
    battery_power_max float not null default 0,

    battery_charging_power_avg float not null default 0,
    battery_charging_power_max float not null default 0,

    battery_discharging_power_avg float not null default 0,
    battery_discharging_power_max float not null default 0,

    dc_load_power_avg float not null default 0,
    dc_load_power_max float not null default 0,

    inverter_power_avg float not null default 0,
    inverter_power_min float not null default 0,
    inverter_power_max float not null default 0,

    total_energy_wh_start float,   -- first reading of the day
    total_energy_wh_end float,     -- last reading of the day
    energy_wh_delta float,         -- end - start

    min_soc_pct float,
    max_soc_pct float,

    -- Count of original computed rows this aggregate covers
    sample_count int not null default 0,

    created_at timestamptz not null default now(),

    unique (device_key, day)
);

-- Index for fast lookups
create index if not exists idx_telemetry_backup_device_day
    on public.telemetry_backup (device_key, day desc);

-- RLS
alter table public.telemetry_backup enable row level security;

drop policy if exists "own_telemetry_backup" on public.telemetry_backup;
create policy "own_telemetry_backup" on public.telemetry_backup
    for select to authenticated using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = telemetry_backup.device_key
              and p.id = auth.uid()
        )
    );

grant select on public.telemetry_backup to authenticated;

-- 2. Rewrite retention function: time-based, not size-based
-------------------------------------------------------------
create or replace function public.archive_and_purge_telemetry()
returns void language plpgsql security definer as $$
declare
    archived_rows int;
    deleted_live int;
    deleted_computed int;
begin
    -- Step 1: Aggressively purge telemetry_live after 48 hours
    -- (raw payload already copied to telemetry_computed via trigger)
    delete from public.telemetry_live
    where recorded_at < now() - interval '48 hours';
    get diagnostics deleted_live = row_count;

    -- Step 2: Archive computed data older than 90 days to daily aggregates
    -- then delete the original computed rows
    with old_data as (
        select
            device_key,
            date_trunc('day', recorded_at)::date as day,
            avg(pv_power) as pv_power_avg,
            min(pv_power) as pv_power_min,
            max(pv_power) as pv_power_max,
            avg(battery_power) as battery_power_avg,
            min(battery_power) as battery_power_min,
            max(battery_power) as battery_power_max,
            avg(battery_charging_power) as battery_charging_power_avg,
            max(battery_charging_power) as battery_charging_power_max,
            avg(battery_discharging_power) as battery_discharging_power_avg,
            max(battery_discharging_power) as battery_discharging_power_max,
            avg(dc_load_power) as dc_load_power_avg,
            max(dc_load_power) as dc_load_power_max,
            avg(inverter_power) as inverter_power_avg,
            min(inverter_power) as inverter_power_min,
            max(inverter_power) as inverter_power_max,
            (array_agg(total_energy_wh order by recorded_at asc))[1] as total_energy_wh_start,
            (array_agg(total_energy_wh order by recorded_at desc))[1] as total_energy_wh_end,
            min(min_soc_pct) as min_soc_pct,
            max(max_soc_pct) as max_soc_pct,
            count(*) as sample_count
        from public.telemetry_computed
        where recorded_at < now() - interval '90 days'
        group by device_key, date_trunc('day', recorded_at)::date
    ),
    upserted as (
        insert into public.telemetry_backup (
            device_key, day,
            pv_power_avg, pv_power_min, pv_power_max,
            battery_power_avg, battery_power_min, battery_power_max,
            battery_charging_power_avg, battery_charging_power_max,
            battery_discharging_power_avg, battery_discharging_power_max,
            dc_load_power_avg, dc_load_power_max,
            inverter_power_avg, inverter_power_min, inverter_power_max,
            total_energy_wh_start, total_energy_wh_end, energy_wh_delta,
            min_soc_pct, max_soc_pct,
            sample_count
        )
        select
            device_key, day,
            pv_power_avg, pv_power_min, pv_power_max,
            battery_power_avg, battery_power_min, battery_power_max,
            battery_charging_power_avg, battery_charging_power_max,
            battery_discharging_power_avg, battery_discharging_power_max,
            dc_load_power_avg, dc_load_power_max,
            inverter_power_avg, inverter_power_min, inverter_power_max,
            total_energy_wh_start, total_energy_wh_end,
            coalesce(total_energy_wh_end, 0) - coalesce(total_energy_wh_start, 0),
            min_soc_pct, max_soc_pct,
            sample_count
        from old_data
        on conflict (device_key, day) do update set
            pv_power_avg = excluded.pv_power_avg,
            pv_power_min = least(telemetry_backup.pv_power_min, excluded.pv_power_min),
            pv_power_max = greatest(telemetry_backup.pv_power_max, excluded.pv_power_max),
            battery_power_avg = excluded.battery_power_avg,
            battery_power_min = least(telemetry_backup.battery_power_min, excluded.battery_power_min),
            battery_power_max = greatest(telemetry_backup.battery_power_max, excluded.battery_power_max),
            battery_charging_power_avg = excluded.battery_charging_power_avg,
            battery_charging_power_max = greatest(telemetry_backup.battery_charging_power_max, excluded.battery_charging_power_max),
            battery_discharging_power_avg = excluded.battery_discharging_power_avg,
            battery_discharging_power_max = greatest(telemetry_backup.battery_discharging_power_max, excluded.battery_discharging_power_max),
            dc_load_power_avg = excluded.dc_load_power_avg,
            dc_load_power_max = greatest(telemetry_backup.dc_load_power_max, excluded.dc_load_power_max),
            inverter_power_avg = excluded.inverter_power_avg,
            inverter_power_min = least(telemetry_backup.inverter_power_min, excluded.inverter_power_min),
            inverter_power_max = greatest(telemetry_backup.inverter_power_max, excluded.inverter_power_max),
            total_energy_wh_start = coalesce(telemetry_backup.total_energy_wh_start, excluded.total_energy_wh_start),
            total_energy_wh_end = excluded.total_energy_wh_end,
            energy_wh_delta = excluded.total_energy_wh_end - coalesce(telemetry_backup.total_energy_wh_start, excluded.total_energy_wh_start),
            min_soc_pct = least(telemetry_backup.min_soc_pct, excluded.min_soc_pct),
            max_soc_pct = greatest(telemetry_backup.max_soc_pct, excluded.max_soc_pct),
            sample_count = telemetry_backup.sample_count + excluded.sample_count,
            created_at = now()
        returning 1
    )
    select count(*) into archived_rows from upserted;

    -- Step 3: Delete archived computed rows
    delete from public.telemetry_computed
    where recorded_at < now() - interval '90 days';
    get diagnostics deleted_computed = row_count;

    -- Step 4: Mark devices offline if no telemetry in last 24 hours
    update public.devices d
    set is_online = false
    where d.last_seen_at < current_timestamp - interval '1 day';

    -- Log summary (appears in Supabase logs)
    raise log 'telemetry retention: deleted_live=%, archived_computed=%, deleted_computed=%',
        deleted_live, archived_rows, deleted_computed;
end;
$$;

-- Ensure cron job is scheduled hourly
select cron.unschedule('telemetry-maintenance');
select cron.schedule(
    'telemetry-maintenance',
    '17 * * * *',  -- run at :17 to avoid cron stampede
    'select public.archive_and_purge_telemetry()'
);

-- 3. Update RPC: query telemetry_computed.raw_payload instead of telemetry_live
--    (telemetry_live is ephemeral; computed has the long-term raw_payload copy)
-------------------------------------------------------------
create or replace function public.get_aggregated_telemetry(
    p_device_key text,
    p_hours int,
    p_metric text default 'power'
)
returns table (
    bucket timestamptz,
    key text,
    avg_val float,
    min_val float,
    max_val float
) language plpgsql security definer as $$
declare
    bucket_interval interval;
    since timestamptz;
    key_pattern text;
begin
    -- Auto-select bucket size based on range
    bucket_interval := case
        when p_hours <= 1 then '30 seconds'::interval
        when p_hours <= 6 then '1 minute'::interval
        when p_hours <= 24 then '5 minutes'::interval
        when p_hours <= 168 then '15 minutes'::interval
        else '1 hour'::interval
    end;

    since := now() - (p_hours || ' hours')::interval;

    -- Match payload keys by metric type in raw_payload
    key_pattern := case p_metric
        when 'power' then 'ch%_P'
        when 'voltage' then 'ch%_V'
        when 'current' then 'ch%_I'
        else 'ch%_P'
    end;

    return query
    with buckets as (
        select
            date_trunc('epoch', recorded_at)::timestamptz
                + (extract(epoch from recorded_at)::bigint / extract(epoch from bucket_interval)::bigint)
                * bucket_interval as b,
            key,
            (raw_payload->>key)::float as val
        from public.telemetry_computed,
             lateral jsonb_object_keys(raw_payload) as key
        where device_key = p_device_key
          and recorded_at >= since
          and key like key_pattern
    )
    select
        b as bucket,
        key,
        avg(val)::float as avg_val,
        min(val)::float as min_val,
        max(val)::float as max_val
    from buckets
    where val is not null
    group by b, key
    order by b, key;
end;
$$;
