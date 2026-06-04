-- get_hourly_generation: returns kWh per hour for a time range
-- Uses energy_wh1 delta (from ESP32 cumulative Wh) for accuracy
-- Returns rows newest-first; caller reverses for charts

drop function if exists public.get_hourly_generation(text, int);
drop function if exists public.get_hourly_generation(text, int, text);

create or replace function public.get_hourly_generation(
    p_device_key text,
    p_start_time timestamptz,
    p_end_time timestamptz
)
returns table (
    hour_start timestamptz,
    kwh float,
    is_partial boolean
) language sql security definer as $$
    with hourly as (
        select
            date_trunc('hour', recorded_at) as hr,
            (array_agg(energy_wh1 order by recorded_at asc))[1] as first_val,
            (array_agg(energy_wh1 order by recorded_at desc))[1] as last_val
        from public.telemetry_computed
        where device_key = p_device_key
          and recorded_at >= p_start_time
          and recorded_at <= p_end_time
          and energy_wh1 is not null
        group by date_trunc('hour', recorded_at)
    )
    select
        hr as hour_start,
        coalesce(greatest(0, last_val - first_val), 0) / 1000.0 as kwh,
        (hr = date_trunc('hour', p_end_time)) as is_partial
    from hourly
    order by hr desc;
$$;

grant execute on function public.get_hourly_generation(text, timestamptz, timestamptz) to authenticated;

-- get_daily_generation: returns kWh per day for a time range
-- Uses energy_wh1 delta per day for accuracy

drop function if exists public.get_daily_generation(text, int);

create or replace function public.get_daily_generation(
    p_device_key text,
    p_start_time timestamptz,
    p_end_time timestamptz
)
returns table (
    day date,
    kwh float,
    is_partial boolean
) language sql security definer as $$
    with daily as (
        select
            date_trunc('day', recorded_at)::date as d,
            (array_agg(energy_wh1 order by recorded_at asc))[1] as first_val,
            (array_agg(energy_wh1 order by recorded_at desc))[1] as last_val
        from public.telemetry_computed
        where device_key = p_device_key
          and recorded_at >= p_start_time
          and recorded_at <= p_end_time
          and energy_wh1 is not null
        group by date_trunc('day', recorded_at)::date
    )
    select
        d as day,
        coalesce(greatest(0, last_val - first_val), 0) / 1000.0 as kwh,
        (d = current_date) as is_partial
    from daily
    order by d desc;
$$;

grant execute on function public.get_daily_generation(text, timestamptz, timestamptz) to authenticated;