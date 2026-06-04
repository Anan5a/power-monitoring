-- get_hourly_generation: returns kWh per hour for the last N hours
-- Uses energy_wh1 delta (from ESP32 cumulative Wh) for accuracy
-- Returns rows newest-first (current partial hour first); caller reverses for charts

drop function if exists public.get_hourly_generation(text, int);

create or replace function public.get_hourly_generation(
    p_device_key text,
    p_hours int
)
returns table (
    hour_start timestamptz,
    kwh float,
    is_partial boolean
) language plpgsql security definer as $$
declare
    now_local timestamptz;
    current_hour timestamptz;
    first_val float;
    last_val float;
    h timestamptz;
    i int;
begin
    now_local := now();
    current_hour := date_trunc('hour', now_local);

    -- 1. Current partial hour (newest)
    select
        (select energy_wh1 from public.telemetry_computed
         where device_key = p_device_key
           and recorded_at >= current_hour
           and recorded_at <= now_local
           and energy_wh1 is not null
         order by recorded_at asc limit 1),
        (select energy_wh1 from public.telemetry_computed
         where device_key = p_device_key
           and recorded_at >= current_hour
           and recorded_at <= now_local
           and energy_wh1 is not null
         order by recorded_at desc limit 1)
    into first_val, last_val;

    hour_start := current_hour;
    kwh := coalesce(greatest(0, last_val - first_val), 0) / 1000.0;
    is_partial := true;
    return next;

    -- 2. Completed hours going backward
    for i in 1..(p_hours - 1) loop
        h := current_hour - (i || ' hours')::interval;

        select
            (select energy_wh1 from public.telemetry_computed
             where device_key = p_device_key
               and recorded_at >= h
               and recorded_at < h + interval '1 hour'
               and energy_wh1 is not null
             order by recorded_at asc limit 1),
            (select energy_wh1 from public.telemetry_computed
             where device_key = p_device_key
               and recorded_at >= h
               and recorded_at < h + interval '1 hour'
               and energy_wh1 is not null
             order by recorded_at desc limit 1)
        into first_val, last_val;

        hour_start := h;
        kwh := coalesce(greatest(0, last_val - first_val), 0) / 1000.0;
        is_partial := false;
        return next;
    end loop;
end;
$$;

grant execute on function public.get_hourly_generation(text, int) to authenticated;

-- get_daily_generation: returns kWh per day for the last N days
-- Uses energy_wh1 delta per day for accuracy

create or replace function public.get_daily_generation(
    p_device_key text,
    p_days int
)
returns table (
    day date,
    kwh float,
    is_partial boolean
) language plpgsql security definer as $$
declare
    now_local timestamptz;
    current_day date;
    first_val float;
    last_val float;
    d date;
    i int;
begin
    now_local := now();
    current_day := current_date;

    -- 1. Current partial day (newest)
    select
        (select energy_wh1 from public.telemetry_computed
         where device_key = p_device_key
           and recorded_at >= date_trunc('day', now_local)
           and recorded_at <= now_local
           and energy_wh1 is not null
         order by recorded_at asc limit 1),
        (select energy_wh1 from public.telemetry_computed
         where device_key = p_device_key
           and recorded_at >= date_trunc('day', now_local)
           and recorded_at <= now_local
           and energy_wh1 is not null
         order by recorded_at desc limit 1)
    into first_val, last_val;

    day := current_day;
    kwh := coalesce(greatest(0, last_val - first_val), 0) / 1000.0;
    is_partial := true;
    return next;

    -- 2. Completed days going backward
    for i in 1..(p_days - 1) loop
        d := current_day - i;

        select
            (select energy_wh1 from public.telemetry_computed
             where device_key = p_device_key
               and recorded_at >= d::timestamptz
               and recorded_at < (d + 1)::timestamptz
               and energy_wh1 is not null
             order by recorded_at asc limit 1),
            (select energy_wh1 from public.telemetry_computed
             where device_key = p_device_key
               and recorded_at >= d::timestamptz
               and recorded_at < (d + 1)::timestamptz
               and energy_wh1 is not null
             order by recorded_at desc limit 1)
        into first_val, last_val;

        day := d;
        kwh := coalesce(greatest(0, last_val - first_val), 0) / 1000.0;
        is_partial := false;
        return next;
    end loop;
end;
$$;

grant execute on function public.get_daily_generation(text, int) to authenticated;