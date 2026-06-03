-- get_hourly_generation: returns kWh per hour for the last N hours
-- Uses energy_wh1 delta (from ESP32 cumulative Wh) for accuracy

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
    since timestamptz;
    now_local timestamptz;
    cur_hour timestamptz;
    first_energy float;
    max_energy float;
    result_kwh float;
begin
    since := now() - (p_hours || ' hours')::interval;
    now_local := now();  -- UTC

    -- Iterate from current hour going back p_hours entries
    cur_hour := date_trunc('hour', now_local);

    for i in 0..(p_hours - 1) loop
        -- Completed hour (not current)
        if i < p_hours - 1 then
            select
                (array_agg(energy_wh1 order by recorded_at limit 1))[1],
                max(energy_wh1)
            into first_energy, max_energy
            from public.telemetry_computed
            where device_key = p_device_key
              and recorded_at >= cur_hour - interval '1 hour'
              and recorded_at < cur_hour
              and energy_wh1 is not null;

            result_kwh := greatest(0, coalesce(max_energy, 0) - coalesce(first_energy, 0)) / 1000.0;
            hour_start := cur_hour - interval '1 hour';
            kwh := result_kwh;
            is_partial := false;

        -- Current (partial) hour
        else
            select
                (array_agg(energy_wh1 order by recorded_at limit 1))[1],
                max(energy_wh1)
            into first_energy, max_energy
            from public.telemetry_computed
            where device_key = p_device_key
              and recorded_at >= cur_hour
              and recorded_at <= now_local
              and energy_wh1 is not null;

            result_kwh := greatest(0, coalesce(max_energy, 0) - coalesce(first_energy, 0)) / 1000.0;
            hour_start := cur_hour;
            kwh := result_kwh;
            is_partial := true;
        end if;

        return next;
        cur_hour := cur_hour - interval '1 hour';
    end loop;
end;
$$;

grant execute on function public.get_hourly_generation(text, int) to authenticated;