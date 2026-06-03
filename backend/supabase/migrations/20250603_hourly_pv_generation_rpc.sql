-- RPC: get_hourly_pv_generation
-- Returns hourly kWh for a device on a given date
-- Much more efficient than fetching all rows (avoids 1000 row limit)

create or replace function public.get_hourly_pv_generation(
    p_device_key text,
    p_date date default current_date
)
returns table (
    hour timestamptz,
    kwh numeric
) language sql security definer as $$
    select
        date_trunc('hour', recorded_at) as hour,
        (sum(pv_power) / count(*) / 3600.0 / 1000)::numeric(10,4) as kwh
    from telemetry_computed
    where device_key = p_device_key
      and recorded_at >= p_date
      and recorded_at < p_date + interval '1 day'
    group by date_trunc('hour', recorded_at)
    order by hour
$$;

grant execute on function public.get_hourly_pv_generation(text, date) to authenticated;