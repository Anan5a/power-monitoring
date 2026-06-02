-- ============================================================
-- Migration: Typed columns + 2-sec rollup + telemetry_live purge
-- Date: 2026-06-02
--
-- Strategy:
--   telemetry_live     -> passthrough only (deleted immediately after trigger)
--   telemetry_computed -> 48h raw 1/sec typed columns, then 2-sec rollup
--   telemetry_backup   -> daily aggregates after 15 days
--
-- Storage (1 device, 1/sec, 500 MB free tier):
--   telemetry_live:     ~0 MB (rows deleted immediately)
--   telemetry_computed: 48h * 86400 * 350 B + 13d * 43200 * 350 B ≈ 245 MB
--   telemetry_backup:   ~0 MB (daily aggregates)
-- ============================================================

-- ============================================================
-- 1. Add typed columns to telemetry_computed
--    Drop raw_payload/raw_metadata, add per-channel typed columns
-- ============================================================

-- Add all typed per-channel columns (will be back-filled by a one-time backfill)
alter table public.telemetry_computed add column if not exists ina3221_v0 real;
alter table public.telemetry_computed add column if not exists ina3221_v1 real;
alter table public.telemetry_computed add column if not exists ina3221_v2 real;
alter table public.telemetry_computed add column if not exists ina226_v real;
alter table public.telemetry_computed add column if not exists ina226_i real;
alter table public.telemetry_computed add column if not exists ina226_p real;
alter table public.telemetry_computed add column if not exists ads1115_0 real;
alter table public.telemetry_computed add column if not exists ads1115_1 real;
alter table public.telemetry_computed add column if not exists ads1115_2 real;
alter table public.telemetry_computed add column if not exists ads1115_3 real;
alter table public.telemetry_computed add column if not exists coulomb_mah0 real;
alter table public.telemetry_computed add column if not exists coulomb_mah1 real;
alter table public.telemetry_computed add column if not exists coulomb_mah2 real;
alter table public.telemetry_computed add column if not exists coulomb_mah3 real;
alter table public.telemetry_computed add column if not exists energy_wh0 real;
alter table public.telemetry_computed add column if not exists energy_wh1 real;
alter table public.telemetry_computed add column if not exists energy_wh2 real;
alter table public.telemetry_computed add column if not exists energy_wh3 real;
alter table public.telemetry_computed add column if not exists soc_pct0 real;
alter table public.telemetry_computed add column if not exists soc_pct1 real;
alter table public.telemetry_computed add column if not exists soc_pct2 real;
alter table public.telemetry_computed add column if not exists soc_pct3 real;
alter table public.telemetry_computed add column if not exists relay0 boolean;
alter table public.telemetry_computed add column if not exists relay1 boolean;
alter table public.telemetry_computed add column if not exists relay2 boolean;
alter table public.telemetry_computed add column if not exists relay3 boolean;
alter table public.telemetry_computed add column if not exists ch0_v real;
alter table public.telemetry_computed add column if not exists ch0_i real;
alter table public.telemetry_computed add column if not exists ch0_p real;
alter table public.telemetry_computed add column if not exists ch1_v real;
alter table public.telemetry_computed add column if not exists ch1_i real;
alter table public.telemetry_computed add column if not exists ch1_p real;
alter table public.telemetry_computed add column if not exists ch2_v real;
alter table public.telemetry_computed add column if not exists ch2_i real;
alter table public.telemetry_computed add column if not exists ch2_p real;
alter table public.telemetry_computed add column if not exists ch3_v real;
alter table public.telemetry_computed add column if not exists ch3_i real;
alter table public.telemetry_computed add column if not exists ch3_p real;
alter table public.telemetry_computed add column if not exists ina3221_v0_spike boolean;
alter table public.telemetry_computed add column if not exists ina3221_v1_spike boolean;
alter table public.telemetry_computed add column if not exists ina3221_v2_spike boolean;
alter table public.telemetry_computed add column if not exists ina3221_i0_spike boolean;
alter table public.telemetry_computed add column if not exists ina3221_i1_spike boolean;
alter table public.telemetry_computed add column if not exists ina3221_i2_spike boolean;

-- Mark telemetry_live as ephemeral: rows deleted immediately after trigger
-- No longer needed as a long-term store

-- ============================================================
-- 2. Rewrite compute_telemetry_row trigger
--    Extract all typed columns from payload, delete telemetry_live
--    after insert (no raw_payload stored)
-- ============================================================

create or replace function public.compute_telemetry_row()
returns trigger language plpgsql security definer as $$
declare
    cg_arr jsonb;
    pv_power_val float := 0;
    battery_charging float := 0;
    battery_discharging float := 0;
    dc_load_val float := 0;
    unclassified_val float := 0;
    ch_idx int;
    ch_power float;
    grp jsonb;
    grp_icon int;
    grp_mask int;
    ch_in_any_group boolean := false;
    min_soc float;
    max_soc float;
    total_energy float;
    inv_power float;
    sys_status text;
begin
    -- Read channel_groups for this device
    select channel_groups into cg_arr
    from public.device_channels
    where device_key = new.device_id;

    -- Iterate each channel (0-3)
    for ch_idx in 0..3 loop
        ch_power := coalesce((new.payload->>('ch' || ch_idx || '_P'))::float, 0);
        ch_in_any_group := false;

        if cg_arr is not null and jsonb_array_length(cg_arr) > 0 then
            for grp in select elem from jsonb_array_elements(cg_arr) as elem loop
                grp_icon := (grp->>'icon')::int;
                grp_mask := (grp->>'channel_mask')::int;

                if (grp_mask & (1 << ch_idx)) != 0 then
                    ch_in_any_group := true;

                    if grp_icon = 0 then
                        pv_power_val := pv_power_val + greatest(ch_power, 0);
                    elsif grp_icon = 1 then
                        if ch_power > 0 then
                            battery_charging := battery_charging + ch_power;
                        else
                            battery_discharging := battery_discharging + abs(ch_power);
                        end if;
                    elsif grp_icon = 2 then
                        dc_load_val := dc_load_val + case when ch_power < 0 then abs(ch_power) else 0 end;
                    else
                        dc_load_val := dc_load_val + case when ch_power < 0 then abs(ch_power) else 0 end;
                    end if;

                    exit;
                end if;
            end loop;
        end if;

        if not ch_in_any_group then
            declare
                bp_capacity float;
            begin
                select (bp->>'capacity_mAh')::float into bp_capacity
                from (
                    select jsonb_array_elements(device_channels.battery_profiles) as bp,
                           (jsonb_array_elements(device_channels.battery_profiles)->>'channel')::int as bp_ch
                    from public.device_channels
                    where device_key = new.device_id
                ) sub
                where bp_ch = ch_idx and bp_capacity > 0;

                if bp_capacity is not null and bp_capacity > 0 then
                    if ch_power > 0 then
                        battery_charging := battery_charging + ch_power;
                    else
                        battery_discharging := battery_discharging + abs(ch_power);
                    end if;
                    ch_in_any_group := true;
                end if;
            end;
        end if;

        if not ch_in_any_group then
            unclassified_val := unclassified_val + greatest(ch_power, 0);
        end if;
    end loop;

    inv_power := pv_power_val + battery_discharging - battery_charging - dc_load_val;

    if battery_charging > 5 then
        sys_status := 'charging';
    elsif battery_discharging > 5 then
        sys_status := 'discharging';
    elsif abs(inv_power) <= 5 then
        sys_status := 'balanced';
    else
        sys_status := 'unknown';
    end if;

    select min(v), max(v) into min_soc, max_soc
    from unnest(array[
        (new.payload->>'soc_pct0')::float,
        (new.payload->>'soc_pct1')::float,
        (new.payload->>'soc_pct2')::float,
        (new.payload->>'soc_pct3')::float
    ]) as v where v is not null;

    total_energy := coalesce((new.payload->>'energy_wh0')::float, 0)
                  + coalesce((new.payload->>'energy_wh1')::float, 0)
                  + coalesce((new.payload->>'energy_wh2')::float, 0)
                  + coalesce((new.payload->>'energy_wh3')::float, 0);

    insert into public.telemetry_computed (
        device_key, recorded_at,
        pv_power, battery_power,
        battery_charging_power, battery_discharging_power,
        dc_load_power, unclassified_power, inverter_power,
        system_status,
        min_soc_pct, max_soc_pct,
        total_energy_wh,

        -- INA3221 bus voltages (from JSON array, index by channel)
        ina3221_v0, ina3221_v1, ina3221_v2,
        -- INA226
        ina226_v, ina226_i, ina226_p,
        -- ADS1115
        ads1115_0, ads1115_1, ads1115_2, ads1115_3,
        -- Coulombs
        coulomb_mah0, coulomb_mah1, coulomb_mah2, coulomb_mah3,
        -- Energy
        energy_wh0, energy_wh1, energy_wh2, energy_wh3,
        -- SoC
        soc_pct0, soc_pct1, soc_pct2, soc_pct3,
        -- Relays
        relay0, relay1, relay2, relay3,
        -- Virtual channels
        ch0_v, ch0_i, ch0_p,
        ch1_v, ch1_i, ch1_p,
        ch2_v, ch2_i, ch2_p,
        ch3_v, ch3_i, ch3_p,
        -- Spike flags
        ina3221_v0_spike, ina3221_v1_spike, ina3221_v2_spike,
        ina3221_i0_spike, ina3221_i1_spike, ina3221_i2_spike
    ) values (
        new.device_id, new.recorded_at,
        pv_power_val,
        battery_charging - battery_discharging,
        battery_charging,
        battery_discharging,
        dc_load_val, unclassified_val, inv_power,
        sys_status,
        case when min_soc isnull then null else min_soc end,
        case when max_soc isnull then null else max_soc end,
        total_energy,

        -- Extract typed values from payload JSONB
        (new.payload->'ina3221'->0->>'v')::real,
        (new.payload->'ina3221'->1->>'v')::real,
        (new.payload->'ina3221'->2->>'v')::real,
        (new.payload->>'ina226_v')::real,
        (new.payload->>'ina226_i')::real,
        (new.payload->>'ina226_p')::real,
        (new.payload->>'ads1115_0')::real,
        (new.payload->>'ads1115_1')::real,
        (new.payload->>'ads1115_2')::real,
        (new.payload->>'ads1115_3')::real,
        (new.payload->>'coulomb_mah0')::real,
        (new.payload->>'coulomb_mah1')::real,
        (new.payload->>'coulomb_mah2')::real,
        (new.payload->>'coulomb_mah3')::real,
        (new.payload->>'energy_wh0')::real,
        (new.payload->>'energy_wh1')::real,
        (new.payload->>'energy_wh2')::real,
        (new.payload->>'energy_wh3')::real,
        (new.payload->>'soc_pct0')::real,
        (new.payload->>'soc_pct1')::real,
        (new.payload->>'soc_pct2')::real,
        (new.payload->>'soc_pct3')::real,
        (new.payload->>'relay0')::boolean,
        (new.payload->>'relay1')::boolean,
        (new.payload->>'relay2')::boolean,
        (new.payload->>'relay3')::boolean,
        (new.payload->>'ch0_V')::real,
        (new.payload->>'ch0_I')::real,
        (new.payload->>'ch0_P')::real,
        (new.payload->>'ch1_V')::real,
        (new.payload->>'ch1_I')::real,
        (new.payload->>'ch1_P')::real,
        (new.payload->>'ch2_V')::real,
        (new.payload->>'ch2_I')::real,
        (new.payload->>'ch2_P')::real,
        (new.payload->>'ch3_V')::real,
        (new.payload->>'ch3_I')::real,
        (new.payload->>'ch3_P')::real,
        (new.payload->>'ina3221_v0_spike')::boolean,
        (new.payload->>'ina3221_v1_spike')::boolean,
        (new.payload->>'ina3221_v2_spike')::boolean,
        (new.payload->>'ina3221_i0_spike')::boolean,
        (new.payload->>'ina3221_i1_spike')::boolean,
        (new.payload->>'ina3221_i2_spike')::boolean
    );

    -- Delete telemetry_live immediately (passthrough only)
    delete from public.telemetry_live where id = new.id;

    return new;
end;
$$;

-- Trigger: fires AFTER each insert on telemetry_live
drop trigger if exists on_telemetry_computed_update on public.telemetry_live;
create trigger on_telemetry_computed_update
    after insert on public.telemetry_live
    for each row execute function public.compute_telemetry_row();

-- ============================================================
-- 3. Rollup function: 1-sec rows -> 2-sec buckets
--    Runs every minute via cron
--    Aggregates rows older than 2 minutes (to ensure all 1-sec
--    rows in the last 2-sec window are complete before rolling up)
-- ============================================================

create or replace function public.rollup_telemetry_computed()
returns void language plpgsql security definer as $$
declare
    bucket_size_sec int := 2;
    raw_window interval := interval '48 hours';  -- keep 48h of raw 1-sec
    cutoff timestamptz := now() - raw_window - (bucket_size_sec || ' seconds')::interval;
    rolled int;
    deleted int;
begin
    -- Only roll up rows older than 48 hours
    -- Rows between now-48h and now are kept as 1-sec raw for detailed charts

    with rolled_up as (
        select
            device_key,
            (date_trunc('second', recorded_at)::timestamptz
                + (floor(extract(epoch from recorded_at)::bigint / bucket_size_sec) * bucket_size_sec)::bigint * interval '1 second') as bucket_ts,

            avg(pv_power) as pv_power,
            min(pv_power) as pv_power_min,
            max(pv_power) as pv_power_max,

            avg(battery_power) as battery_power,
            min(battery_power) as battery_power_min,
            max(battery_power) as battery_power_max,

            avg(battery_charging_power) as battery_charging_power,
            max(battery_charging_power) as battery_charging_power_max,

            avg(battery_discharging_power) as battery_discharging_power,
            max(battery_discharging_power) as battery_discharging_power_max,

            avg(dc_load_power) as dc_load_power,
            max(dc_load_power) as dc_load_power_max,

            avg(unclassified_power) as unclassified_power,

            avg(inverter_power) as inverter_power,
            min(inverter_power) as inverter_power_min,
            max(inverter_power) as inverter_power_max,

            (array_agg(total_energy_wh order by recorded_at desc))[1] as total_energy_wh,

            min(min_soc_pct) as min_soc_pct,
            max(max_soc_pct) as max_soc_pct,

            avg(ch0_v) as ch0_v, min(ch0_v) as ch0_v_min, max(ch0_v) as ch0_v_max,
            avg(ch0_i) as ch0_i, min(ch0_i) as ch0_i_min, max(ch0_i) as ch0_i_max,
            avg(ch0_p) as ch0_p, min(ch0_p) as ch0_p_min, max(ch0_p) as ch0_p_max,
            avg(ch1_v) as ch1_v, min(ch1_v) as ch1_v_min, max(ch1_v) as ch1_v_max,
            avg(ch1_i) as ch1_i, min(ch1_i) as ch1_i_min, max(ch1_i) as ch1_i_max,
            avg(ch1_p) as ch1_p, min(ch1_p) as ch1_p_min, max(ch1_p) as ch1_p_max,
            avg(ch2_v) as ch2_v, min(ch2_v) as ch2_v_min, max(ch2_v) as ch2_v_max,
            avg(ch2_i) as ch2_i, min(ch2_i) as ch2_i_min, max(ch2_i) as ch2_i_max,
            avg(ch2_p) as ch2_p, min(ch2_p) as ch2_p_min, max(ch2_p) as ch2_p_max,
            avg(ch3_v) as ch3_v, min(ch3_v) as ch3_v_min, max(ch3_v) as ch3_v_max,
            avg(ch3_i) as ch3_i, min(ch3_i) as ch3_i_min, max(ch3_i) as ch3_i_max,
            avg(ch3_p) as ch3_p, min(ch3_p) as ch3_p_min, max(ch3_p) as ch3_p_max,

            (array_agg(energy_wh0 order by recorded_at desc))[1] as energy_wh0,
            (array_agg(energy_wh1 order by recorded_at desc))[1] as energy_wh1,
            (array_agg(energy_wh2 order by recorded_at desc))[1] as energy_wh2,
            (array_agg(energy_wh3 order by recorded_at desc))[1] as energy_wh3,

            (array_agg(soc_pct0 order by recorded_at desc))[1] as soc_pct0,
            (array_agg(soc_pct1 order by recorded_at desc))[1] as soc_pct1,
            (array_agg(soc_pct2 order by recorded_at desc))[1] as soc_pct2,
            (array_agg(soc_pct3 order by recorded_at desc))[1] as soc_pct3,

            (array_agg(coulomb_mah0 order by recorded_at desc))[1] as coulomb_mah0,
            (array_agg(coulomb_mah1 order by recorded_at desc))[1] as coulomb_mah1,
            (array_agg(coulomb_mah2 order by recorded_at desc))[1] as coulomb_mah2,
            (array_agg(coulomb_mah3 order by recorded_at desc))[1] as coulomb_mah3,

            avg(ina3221_v0) as ina3221_v0, min(ina3221_v0) as ina3221_v0_min, max(ina3221_v0) as ina3221_v0_max,
            avg(ina3221_v1) as ina3221_v1, min(ina3221_v1) as ina3221_v1_min, max(ina3221_v1) as ina3221_v1_max,
            avg(ina3221_v2) as ina3221_v2, min(ina3221_v2) as ina3221_v2_min, max(ina3221_v2) as ina3221_v2_max,
            avg(ina226_v) as ina226_v, min(ina226_v) as ina226_v_min, max(ina226_v) as ina226_v_max,
            avg(ina226_i) as ina226_i, min(ina226_i) as ina226_i_min, max(ina226_i) as ina226_i_max,
            avg(ina226_p) as ina226_p, min(ina226_p) as ina226_p_min, max(ina226_p) as ina226_p_max,

            avg(ads1115_0) as ads1115_0, min(ads1115_0) as ads1115_0_min, max(ads1115_0) as ads1115_0_max,
            avg(ads1115_1) as ads1115_1, min(ads1115_1) as ads1115_1_min, max(ads1115_1) as ads1115_1_max,
            avg(ads1115_2) as ads1115_2, min(ads1115_2) as ads1115_2_min, max(ads1115_2) as ads1115_2_max,
            avg(ads1115_3) as ads1115_3, min(ads1115_3) as ads1115_3_min, max(ads1115_3) as ads1115_3_max,

            bool_or(ina3221_v0_spike) as ina3221_v0_spike,
            bool_or(ina3221_v1_spike) as ina3221_v1_spike,
            bool_or(ina3221_v2_spike) as ina3221_v2_spike,
            bool_or(ina3221_i0_spike) as ina3221_i0_spike,
            bool_or(ina3221_i1_spike) as ina3221_i1_spike,
            bool_or(ina3221_i2_spike) as ina3221_i2_spike,

            count(*) as sample_count
        from public.telemetry_computed
        where recorded_at < cutoff  -- only rows older than 48h
        group by
            device_key,
            date_trunc('second', recorded_at)::timestamptz
                + (floor(extract(epoch from recorded_at)::bigint / bucket_size_sec) * bucket_size_sec)::bigint * interval '1 second'
    )
    insert into public.telemetry_computed (
        device_key, recorded_at,
        pv_power, battery_power,
        battery_charging_power, battery_discharging_power,
        dc_load_power, unclassified_power, inverter_power,
        system_status, min_soc_pct, max_soc_pct, total_energy_wh,
        ch0_v, ch0_i, ch0_p,
        ch1_v, ch1_i, ch1_p,
        ch2_v, ch2_i, ch2_p,
        ch3_v, ch3_i, ch3_p,
        energy_wh0, energy_wh1, energy_wh2, energy_wh3,
        soc_pct0, soc_pct1, soc_pct2, soc_pct3,
        coulomb_mah0, coulomb_mah1, coulomb_mah2, coulomb_mah3,
        ina3221_v0, ina3221_v1, ina3221_v2,
        ina226_v, ina226_i, ina226_p,
        ads1115_0, ads1115_1, ads1115_2, ads1115_3,
        ina3221_v0_spike, ina3221_v1_spike, ina3221_v2_spike,
        ina3221_i0_spike, ina3221_i1_spike, ina3221_i2_spike
    )
    select
        device_key, bucket_ts,
        pv_power, battery_power,
        battery_charging_power, battery_discharging_power,
        dc_load_power, unclassified_power, inverter_power,
        case
            when battery_charging_power > 5 then 'charging'
            when battery_discharging_power > 5 then 'discharging'
            when abs(inverter_power) <= 5 then 'balanced'
            else 'unknown'
        end,
        min_soc_pct, max_soc_pct, total_energy_wh,
        ch0_v, ch0_i, ch0_p,
        ch1_v, ch1_i, ch1_p,
        ch2_v, ch2_i, ch2_p,
        ch3_v, ch3_i, ch3_p,
        energy_wh0, energy_wh1, energy_wh2, energy_wh3,
        soc_pct0, soc_pct1, soc_pct2, soc_pct3,
        coulomb_mah0, coulomb_mah1, coulomb_mah2, coulomb_mah3,
        ina3221_v0, ina3221_v1, ina3221_v2,
        ina226_v, ina226_i, ina226_p,
        ads1115_0, ads1115_1, ads1115_2, ads1115_3,
        ina3221_v0_spike, ina3221_v1_spike, ina3221_v2_spike,
        ina3221_i0_spike, ina3221_i1_spike, ina3221_i2_spike
    from rolled_up
    on conflict (device_key, recorded_at) do nothing
    returning 1;

    get diagnostics rolled = row_count;

    -- Delete the original 1-sec rows that were rolled up
    delete from public.telemetry_computed
    where id in (
        select id from public.telemetry_computed
        where recorded_at < cutoff
    );
    get diagnostics deleted = row_count;

    raise log 'rollup: inserted=% rolled rows, deleted=% original rows', rolled, deleted;
end;
$$;

-- Schedule rollup every minute
do $$
begin
    perform cron.unschedule('telemetry-rollup');
exception when others then null;
end
$$;
select cron.schedule(
    'telemetry-rollup',
    '* * * * *',
    'select public.rollup_telemetry_computed()'
);

-- ============================================================
-- 4. Update get_aggregated_telemetry RPC
--    Read typed columns instead of raw_payload JSONB
-- ============================================================

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
begin
    bucket_interval := case
        when p_hours <= 1 then '30 seconds'::interval
        when p_hours <= 6 then '1 minute'::interval
        when p_hours <= 24 then '5 minutes'::interval
        when p_hours <= 168 then '15 minutes'::interval
        else '1 hour'::interval
    end;

    since := now() - (p_hours || ' hours')::interval;

    return query
    with buckets as (
        select
            date_trunc('epoch', recorded_at)::timestamptz
                + (floor(extract(epoch from recorded_at)::bigint
                    / extract(epoch from bucket_interval)::bigint)
                * extract(epoch from bucket_interval)::bigint
                * interval '1 second') as b,
            ch0_p, ch0_v, ch0_i,
            ch1_p, ch1_v, ch1_i,
            ch2_p, ch2_v, ch2_i,
            ch3_p, ch3_v, ch3_i
        from public.telemetry_computed
        where device_key = p_device_key and recorded_at >= since
    )
    select b as bucket, 'ch0_P'::text, avg(ch0_p)::float, min(ch0_p)::float, max(ch0_p)::float
    from buckets where ch0_p is not null and p_metric = 'power' group by b
    union all select b, 'ch1_P', avg(ch1_p)::float, min(ch1_p)::float, max(ch1_p)::float
    from buckets where ch1_p is not null and p_metric = 'power' group by b
    union all select b, 'ch2_P', avg(ch2_p)::float, min(ch2_p)::float, max(ch2_p)::float
    from buckets where ch2_p is not null and p_metric = 'power' group by b
    union all select b, 'ch3_P', avg(ch3_p)::float, min(ch3_p)::float, max(ch3_p)::float
    from buckets where ch3_p is not null and p_metric = 'power' group by b
    union all select b, 'ch0_V', avg(ch0_v)::float, min(ch0_v)::float, max(ch0_v)::float
    from buckets where ch0_v is not null and p_metric = 'voltage' group by b
    union all select b, 'ch1_V', avg(ch1_v)::float, min(ch1_v)::float, max(ch1_v)::float
    from buckets where ch1_v is not null and p_metric = 'voltage' group by b
    union all select b, 'ch2_V', avg(ch2_v)::float, min(ch2_v)::float, max(ch2_v)::float
    from buckets where ch2_v is not null and p_metric = 'voltage' group by b
    union all select b, 'ch3_V', avg(ch3_v)::float, min(ch3_v)::float, max(ch3_v)::float
    from buckets where ch3_v is not null and p_metric = 'voltage' group by b
    union all select b, 'ch0_I', avg(ch0_i)::float, min(ch0_i)::float, max(ch0_i)::float
    from buckets where ch0_i is not null and p_metric = 'current' group by b
    union all select b, 'ch1_I', avg(ch1_i)::float, min(ch1_i)::float, max(ch1_i)::float
    from buckets where ch1_i is not null and p_metric = 'current' group by b
    union all select b, 'ch2_I', avg(ch2_i)::float, min(ch2_i)::float, max(ch2_i)::float
    from buckets where ch2_i is not null and p_metric = 'current' group by b
    union all select b, 'ch3_I', avg(ch3_i)::float, min(ch3_i)::float, max(ch3_i)::float
    from buckets where ch3_i is not null and p_metric = 'current' group by b
    order by bucket, key;
end;
$$;

-- ============================================================
-- 5. Update archive_and_purge_telemetry
--    telemetry_live already deleted by trigger (passthrough only)
--    rollup handles 0-48h; archive_and_purge handles >15 days backup
-- ============================================================

create or replace function public.archive_and_purge_telemetry()
returns void language plpgsql security definer as $$
declare
    archived_rows int;
    deleted_computed int;
begin
    -- Step 1: Archive computed data older than 15 days to daily aggregates
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
        where recorded_at < now() - interval '15 days'
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

    -- Step 2: Delete archived computed rows
    delete from public.telemetry_computed
    where recorded_at < now() - interval '15 days';
    get diagnostics deleted_computed = row_count;

    -- Step 3: Mark devices offline if no telemetry in last 24 hours
    update public.devices d
    set is_online = false
    where d.last_seen_at < current_timestamp - interval '1 day';

    raise log 'telemetry retention: archived_computed=%, deleted_computed=%',
        archived_rows, deleted_computed;
end;
$$;

do $$
begin
    perform cron.unschedule('telemetry-maintenance');
exception when others then null;
end
$$;
select cron.schedule(
    'telemetry-maintenance',
    '23 * * * *',
    'select public.archive_and_purge_telemetry()'
);