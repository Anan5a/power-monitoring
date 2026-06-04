-- Definitive get_aggregated_telemetry: compact wide format
-- This migration is the source of truth. It drops any existing version
-- (old tall format or previous compact) and recreates only the wide format.
--
-- Problem it solves: multiple migration files defined get_aggregated_telemetry
-- with the SAME signature (text, int, text) but DIFFERENT return types.
-- PostgreSQL CREATE OR REPLACE cannot change return type, so old files
-- that use CREATE OR REPLACE would fail or silently be skipped.
-- This file explicitly drops first, then creates the correct version.

drop function if exists public.get_aggregated_telemetry(text, int, text);

-- Compact wide-format: one row per bucket with all metric columns.
-- coalesce(..., 0) prevents NULL from leaking into chart tooltips.
create or replace function public.get_aggregated_telemetry(
    p_device_key text,
    p_hours int,
    p_metric text default 'power'
)
returns table (
    bucket timestamptz,
    "ch0_P" float, "ch1_P" float, "ch2_P" float, "ch3_P" float,
    ina226_p float,
    pv_power float, battery_power float, inverter_power float, dc_load_power float,
    soc_pct0 float,
    "ch0_V" float, "ch1_V" float, "ch2_V" float, "ch3_V" float,
    ina3221_v0 float, ina3221_v1 float, ina3221_v2 float, ina226_v float,
    "ch0_I" float, "ch1_I" float, "ch2_I" float, "ch3_I" float,
    ina3221_i0 float, ina3221_i1 float, ina3221_i2 float, ina226_i float
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
    select
        to_timestamp(
            floor(extract(epoch from t.recorded_at)::bigint
                / extract(epoch from bucket_interval)::bigint)
            * extract(epoch from bucket_interval)::bigint
        )::timestamptz as b,
        -- Power
        coalesce(case when p_metric = 'power' then avg(t.ch0_p)::float end, 0) as "ch0_P",
        coalesce(case when p_metric = 'power' then avg(t.ch1_p)::float end, 0) as "ch1_P",
        coalesce(case when p_metric = 'power' then avg(t.ch2_p)::float end, 0) as "ch2_P",
        coalesce(case when p_metric = 'power' then avg(t.ch3_p)::float end, 0) as "ch3_P",
        coalesce(case when p_metric = 'power' then avg(t.ina226_p)::float end, 0) as ina226_p,
        coalesce(case when p_metric = 'power' then avg(t.pv_power)::float end, 0) as pv_power,
        coalesce(case when p_metric = 'power' then avg(t.battery_power)::float end, 0) as battery_power,
        coalesce(case when p_metric = 'power' then avg(t.inverter_power)::float end, 0) as inverter_power,
        coalesce(case when p_metric = 'power' then avg(t.dc_load_power)::float end, 0) as dc_load_power,
        coalesce(case when p_metric = 'power' then avg(t.soc_pct0)::float end, 0) as soc_pct0,
        -- Voltage
        coalesce(case when p_metric = 'voltage' then avg(t.ch0_v)::float end, 0) as "ch0_V",
        coalesce(case when p_metric = 'voltage' then avg(t.ch1_v)::float end, 0) as "ch1_V",
        coalesce(case when p_metric = 'voltage' then avg(t.ch2_v)::float end, 0) as "ch2_V",
        coalesce(case when p_metric = 'voltage' then avg(t.ch3_v)::float end, 0) as "ch3_V",
        coalesce(case when p_metric = 'voltage' then avg(t.ina3221_v0)::float end, 0) as ina3221_v0,
        coalesce(case when p_metric = 'voltage' then avg(t.ina3221_v1)::float end, 0) as ina3221_v1,
        coalesce(case when p_metric = 'voltage' then avg(t.ina3221_v2)::float end, 0) as ina3221_v2,
        coalesce(case when p_metric = 'voltage' then avg(t.ina226_v)::float end, 0) as ina226_v,
        -- Current
        coalesce(case when p_metric = 'current' then avg(t.ch0_i)::float end, 0) as "ch0_I",
        coalesce(case when p_metric = 'current' then avg(t.ch1_i)::float end, 0) as "ch1_I",
        coalesce(case when p_metric = 'current' then avg(t.ch2_i)::float end, 0) as "ch2_I",
        coalesce(case when p_metric = 'current' then avg(t.ch3_i)::float end, 0) as "ch3_I",
        coalesce(case when p_metric = 'current' then avg(t.ina3221_i0)::float end, 0) as ina3221_i0,
        coalesce(case when p_metric = 'current' then avg(t.ina3221_i1)::float end, 0) as ina3221_i1,
        coalesce(case when p_metric = 'current' then avg(t.ina3221_i2)::float end, 0) as ina3221_i2,
        coalesce(case when p_metric = 'current' then avg(t.ina226_i)::float end, 0) as ina226_i
    from public.telemetry_computed t
    where t.device_key = p_device_key and t.recorded_at >= since
    group by b
    order by b;
end;
$$;

grant execute on function public.get_aggregated_telemetry(text, int, text) to authenticated;
