-- Fix rollup_telemetry_computed: "returning 1" causes "query has no destination for result data"
-- Fix also adds limit to delete to prevent memory issues on large datasets

create or replace function public.rollup_telemetry_computed()
returns void language plpgsql security definer as $$
declare
    bucket_size_sec int := 2;
    raw_window interval := interval '48 hours';
    cutoff timestamptz := now() - raw_window - (bucket_size_sec || ' seconds')::interval;
    rolled int;
    deleted int;
begin
    with rolled_up as (
        select
            device_key,
            (date_trunc('second', recorded_at)::timestamptz
                + (floor(extract(epoch from recorded_at)::bigint / bucket_size_sec) * bucket_size_sec)::bigint * interval '1 second') as bucket_ts,

            avg(pv_power) as pv_power,
            avg(battery_power) as battery_power,
            avg(battery_charging_power) as battery_charging_power,
            max(battery_charging_power) as battery_charging_power_max,
            avg(battery_discharging_power) as battery_discharging_power,
            max(battery_discharging_power) as battery_discharging_power_max,
            avg(dc_load_power) as dc_load_power,
            avg(unclassified_power) as unclassified_power,
            avg(inverter_power) as inverter_power,
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
            avg(ina3221_i0) as ina3221_i0, min(ina3221_i0) as ina3221_i0_min, max(ina3221_i0) as ina3221_i0_max,
            avg(ina3221_i1) as ina3221_i1, min(ina3221_i1) as ina3221_i1_min, max(ina3221_i1) as ina3221_i1_max,
            avg(ina3221_i2) as ina3221_i2, min(ina3221_i2) as ina3221_i2_min, max(ina3221_i2) as ina3221_i2_max,
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
        where recorded_at < cutoff
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
        ina3221_i0, ina3221_i1, ina3221_i2,
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
        ina3221_i0, ina3221_i1, ina3221_i2,
        ina226_v, ina226_i, ina226_p,
        ads1115_0, ads1115_1, ads1115_2, ads1115_3,
        ina3221_v0_spike, ina3221_v1_spike, ina3221_v2_spike,
        ina3221_i0_spike, ina3221_i1_spike, ina3221_i2_spike
    from rolled_up
    on conflict (device_key, recorded_at) do nothing
    )
    select count(*) into rolled from upserted;

    -- Delete old 1-sec rows in batches
    delete from public.telemetry_computed
    where recorded_at < cutoff
      and id in (
        select id from public.telemetry_computed
        where recorded_at < cutoff
        limit 10000
    );
    get diagnostics deleted = row_count;

    raise log 'rollup: inserted=% rolled rows, deleted=% original rows', rolled, deleted;
end;
$$;