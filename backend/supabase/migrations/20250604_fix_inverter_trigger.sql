-- Fix deployed compute_telemetry_row trigger
-- The deployed version had a broken inverter_power formula:
--   inv_power := dc_load_val + unclassified_val
-- This just made inverter_power always equal dc_load_power.
--
-- Correct formula:
--   inv_power := pv_power_val + battery_discharging - battery_charging - dc_load_val

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
        ina3221_v0, ina3221_v1, ina3221_v2,
        ina3221_i0, ina3221_i1, ina3221_i2,
        ina226_v, ina226_i, ina226_p,
        ads1115_0, ads1115_1, ads1115_2, ads1115_3,
        coulomb_mah0, coulomb_mah1, coulomb_mah2, coulomb_mah3,
        energy_wh0, energy_wh1, energy_wh2, energy_wh3,
        soc_pct0, soc_pct1, soc_pct2, soc_pct3,
        relay0, relay1, relay2, relay3,
        ch0_v, ch0_i, ch0_p,
        ch1_v, ch1_i, ch1_p,
        ch2_v, ch2_i, ch2_p,
        ch3_v, ch3_i, ch3_p,
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
        (new.payload->>'ina3221_v0')::real,
        (new.payload->>'ina3221_v1')::real,
        (new.payload->>'ina3221_v2')::real,
        (new.payload->>'ina3221_i0')::real,
        (new.payload->>'ina3221_i1')::real,
        (new.payload->>'ina3221_i2')::real,
        (new.payload->>'ina226_v')::real,
        (new.payload->>'ina226_i')::real,
        (new.payload->>'ina226_p')::real,
        (new.payload->'ads1115'->0)::real,
        (new.payload->'ads1115'->1)::real,
        (new.payload->'ads1115'->2)::real,
        (new.payload->'ads1115'->3)::real,
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
