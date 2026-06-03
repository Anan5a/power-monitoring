-- Update get_aggregated_telemetry RPC to include computed power columns

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
            to_timestamp(
                floor(extract(epoch from recorded_at)::bigint
                    / extract(epoch from bucket_interval)::bigint)
                * extract(epoch from bucket_interval)::bigint
            )::timestamptz as b,
            ch0_p, ch0_v, ch0_i,
            ch1_p, ch1_v, ch1_i,
            ch2_p, ch2_v, ch2_i,
            ch3_p, ch3_v, ch3_i,
            ina3221_v0, ina3221_v1, ina3221_v2,
            ina3221_i0, ina3221_i1, ina3221_i2,
            ina226_v, ina226_i, ina226_p,
            pv_power, battery_power, inverter_power, dc_load_power
        from public.telemetry_computed
        where device_key = p_device_key and recorded_at >= since
    )
    select * from (
        select b as bucket, 'ch0_P'::text as key, avg(ch0_p)::float as avg_val, min(ch0_p)::float as min_val, max(ch0_p)::float as max_val
        from buckets where ch0_p is not null and p_metric = 'power' group by b
        union all select b, 'ch1_P', avg(ch1_p)::float, min(ch1_p)::float, max(ch1_p)::float
        from buckets where ch1_p is not null and p_metric = 'power' group by b
        union all select b, 'ch2_P', avg(ch2_p)::float, min(ch2_p)::float, max(ch2_p)::float
        from buckets where ch2_p is not null and p_metric = 'power' group by b
        union all select b, 'ch3_P', avg(ch3_p)::float, min(ch3_p)::float, max(ch3_p)::float
        from buckets where ch3_p is not null and p_metric = 'power' group by b
        union all select b, 'ina226_p', avg(ina226_p)::float, min(ina226_p)::float, max(ina226_p)::float
        from buckets where ina226_p is not null and p_metric = 'power' group by b
        union all select b, 'pv_power', avg(pv_power)::float, min(pv_power)::float, max(pv_power)::float
        from buckets where pv_power is not null and p_metric = 'power' group by b
        union all select b, 'battery_power', avg(battery_power)::float, min(battery_power)::float, max(battery_power)::float
        from buckets where battery_power is not null and p_metric = 'power' group by b
        union all select b, 'inverter_power', avg(inverter_power)::float, min(inverter_power)::float, max(inverter_power)::float
        from buckets where inverter_power is not null and p_metric = 'power' group by b
        union all select b, 'dc_load_power', avg(dc_load_power)::float, min(dc_load_power)::float, max(dc_load_power)::float
        from buckets where dc_load_power is not null and p_metric = 'power' group by b
        union all select b, 'ch0_V', avg(ch0_v)::float, min(ch0_v)::float, max(ch0_v)::float
        from buckets where ch0_v is not null and p_metric = 'voltage' group by b
        union all select b, 'ch1_V', avg(ch1_v)::float, min(ch1_v)::float, max(ch1_v)::float
        from buckets where ch1_v is not null and p_metric = 'voltage' group by b
        union all select b, 'ch2_V', avg(ch2_v)::float, min(ch2_v)::float, max(ch2_v)::float
        from buckets where ch2_v is not null and p_metric = 'voltage' group by b
        union all select b, 'ch3_V', avg(ch3_v)::float, min(ch3_v)::float, max(ch3_v)::float
        from buckets where ch3_v is not null and p_metric = 'voltage' group by b
        union all select b, 'ina3221_v0', avg(ina3221_v0)::float, min(ina3221_v0)::float, max(ina3221_v0)::float
        from buckets where ina3221_v0 is not null and p_metric = 'voltage' group by b
        union all select b, 'ina3221_v1', avg(ina3221_v1)::float, min(ina3221_v1)::float, max(ina3221_v1)::float
        from buckets where ina3221_v1 is not null and p_metric = 'voltage' group by b
        union all select b, 'ina3221_v2', avg(ina3221_v2)::float, min(ina3221_v2)::float, max(ina3221_v2)::float
        from buckets where ina3221_v2 is not null and p_metric = 'voltage' group by b
        union all select b, 'ina226_v', avg(ina226_v)::float, min(ina226_v)::float, max(ina226_v)::float
        from buckets where ina226_v is not null and p_metric = 'voltage' group by b
        union all select b, 'ch0_I', avg(ch0_i)::float, min(ch0_i)::float, max(ch0_i)::float
        from buckets where ch0_i is not null and p_metric = 'current' group by b
        union all select b, 'ch1_I', avg(ch1_i)::float, min(ch1_i)::float, max(ch1_i)::float
        from buckets where ch1_i is not null and p_metric = 'current' group by b
        union all select b, 'ch2_I', avg(ch2_i)::float, min(ch2_i)::float, max(ch2_i)::float
        from buckets where ch2_i is not null and p_metric = 'current' group by b
        union all select b, 'ch3_I', avg(ch3_i)::float, min(ch3_i)::float, max(ch3_i)::float
        from buckets where ch3_i is not null and p_metric = 'current' group by b
        union all select b, 'ina3221_i0', avg(ina3221_i0)::float, min(ina3221_i0)::float, max(ina3221_i0)::float
        from buckets where ina3221_i0 is not null and p_metric = 'current' group by b
        union all select b, 'ina3221_i1', avg(ina3221_i1)::float, min(ina3221_i1)::float, max(ina3221_i1)::float
        from buckets where ina3221_i1 is not null and p_metric = 'current' group by b
        union all select b, 'ina3221_i2', avg(ina3221_i2)::float, min(ina3221_i2)::float, max(ina3221_i2)::float
        from buckets where ina3221_i2 is not null and p_metric = 'current' group by b
        union all select b, 'ina226_i', avg(ina226_i)::float, min(ina226_i)::float, max(ina226_i)::float
        from buckets where ina226_i is not null and p_metric = 'current' group by b
    ) sub
    order by bucket, key;
end;
$$;

grant execute on function public.get_aggregated_telemetry(text, int, text) to authenticated;