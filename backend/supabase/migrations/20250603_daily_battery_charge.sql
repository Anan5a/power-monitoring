-- daily_battery_charge: daily energy flow tracking for battery
-- Uses voltage-based full-charge detection to anchor SOC when firmware coulomb is unreliable
-- When firmware is fixed, switch to coulomb counter (computed_from = 'coulomb')

drop table if exists public.daily_battery_charge;
drop function if exists public.compute_daily_battery_charge(uuid, date);
drop function if exists public.get_battery_charge(uuid, int);
drop function if exists public.trg_on_telemetry_computed_insert();
drop trigger if exists trg_telemetry_computed_charge on telemetry_computed;

create table public.daily_battery_charge (
    id bigint generated always as identity primary key,
    device_id uuid not null references devices(id) on delete cascade,
    date date not null,
    capacity_wh float not null default 0,
    energy_in_wh float not null default 0,
    energy_out_wh float not null default 0,
    last_voltage float null,
    full_charge_voltage float null,
    is_full_charge_day boolean not null default false,
    charge_wh float not null default 0,
    last_recorded_at timestamptz null,
    computed_from text not null default 'battery_power',
    created_at timestamptz default now(),
    unique (device_id, date)
);

comment on table public.daily_battery_charge is 'Daily battery energy in/out with voltage-based SOC anchoring';

create index idx_daily_battery_charge_device_date on daily_battery_charge (device_id, date desc);

-- battery_calibration: per-device battery capacity and voltage thresholds
drop table if exists public.battery_calibration;

create table public.battery_calibration (
    device_id uuid primary key references devices(id) on delete cascade,
    full_charge_voltage float not null default 29.2,
    nominal_capacity_wh float not null default 1000,
    last_calibrated_at timestamptz default now()
);

comment on table public.battery_calibration is 'Battery capacity and full-charge voltage threshold per device';

-- Trigger: increment energy in/out as telemetry rows arrive
-- Anchor charge from previous day's charge_wh when starting a new day
create or replace function public.trg_on_telemetry_computed_insert()
returns trigger language plpgsql security definer as $$
declare
    dt_hours float;
    prev_charge float;
    prev_last_ts timestamptz;
    capacity float;
    fcv float;
    dev_id uuid;
begin
    dev_id := (select id from devices where device_key = NEW.device_key);

    -- Get calibration
    select nominal_capacity_wh, full_charge_voltage
    into capacity, fcv
    from battery_calibration
    where device_id = dev_id;

    if capacity is null or capacity = 0 then
        return new;
    end if;

    -- Get previous last_recorded_at and charge_wh for TODAY (if exists)
    select last_recorded_at, charge_wh
    into prev_last_ts, prev_charge
    from daily_battery_charge
    where device_id = dev_id and date = current_date;

    -- If no row for today yet, seed from yesterday's charge_wh
    if prev_last_ts is null then
        select charge_wh into prev_charge
        from daily_battery_charge
        where device_id = dev_id and date < current_date
        order by date desc limit 1;

        prev_charge := coalesce(prev_charge, capacity * 0.5); -- fallback to 50% if no history
        prev_last_ts := null; -- no dt possible for first row of new day
    end if;

    -- Compute dt from previous row
    if prev_last_ts is not null then
        dt_hours := EXTRACT(EPOCH FROM (NEW.recorded_at - prev_last_ts)) / 3600;
        dt_hours := least(dt_hours, 1.0); -- cap at 1h to avoid reconnect spikes
    else
        dt_hours := 0;
    end if;

    -- Detect full-charge voltage
    if NEW.ch1_v is not null and fcv is not null and NEW.ch1_v >= fcv then
        prev_charge := capacity; -- voltage anchor: set to 100%
    end if;

    -- Upsert today's row
    insert into daily_battery_charge
        (device_id, date, capacity_wh, energy_in_wh, energy_out_wh,
         last_voltage, full_charge_voltage, is_full_charge_day,
         charge_wh, last_recorded_at, computed_from)
    values (dev_id, current_date, capacity,
            case when NEW.battery_power > 0 then NEW.battery_power * dt_hours else 0 end,
            case when NEW.battery_power < 0 then abs(NEW.battery_power) * dt_hours else 0 end,
            NEW.ch1_v, fcv,
            case when NEW.ch1_v is not null and fcv is not null and NEW.ch1_v >= fcv then true else false end,
            case
                when NEW.ch1_v is not null and fcv is not null and NEW.ch1_v >= fcv then capacity
                else greatest(0, least(capacity, prev_charge + case when NEW.battery_power > 0 then NEW.battery_power * dt_hours else -abs(NEW.battery_power) * dt_hours end))
            end,
            NEW.recorded_at,
            'battery_power')
    on conflict (device_id, date) do update set
        last_recorded_at = NEW.recorded_at,
        last_voltage = NEW.ch1_v,
        energy_in_wh = daily_battery_charge.energy_in_wh + case when NEW.battery_power > 0 then NEW.battery_power * dt_hours else 0 end,
        energy_out_wh = daily_battery_charge.energy_out_wh + case when NEW.battery_power < 0 then abs(NEW.battery_power) * dt_hours else 0 end,
        is_full_charge_day = daily_battery_charge.is_full_charge_day or (NEW.ch1_v is not null and fcv is not null and NEW.ch1_v >= fcv),
        charge_wh = case
            when NEW.ch1_v is not null and fcv is not null and NEW.ch1_v >= fcv then capacity
            else greatest(0, least(capacity, daily_battery_charge.charge_wh + case when NEW.battery_power > 0 then NEW.battery_power * dt_hours else -abs(NEW.battery_power) * dt_hours end))
        end;

    return new;
end;
$$;

create trigger trg_telemetry_computed_charge
after insert on telemetry_computed
for each row execute function public.trg_on_telemetry_computed_insert();

-- compute_daily_battery_charge: full recompute for a given day (for backfill/recovery)
create or replace function public.compute_daily_battery_charge(
    p_device_id uuid,
    p_date date default current_date
) returns daily_battery_charge language plpgsql security definer as $$
declare
    start_dt timestamptz := p_date::timestamptz;
    end_dt timestamptz := (p_date + 1)::timestamptz;
    cal_row record;
    row_data record;
    energy_in float := 0;
    energy_out float := 0;
    last_v float;
    max_v float;
    charge_wh float := 0;
    result_row daily_battery_charge%rowtype;
    prev_charge float := 0;
begin
    select full_charge_voltage, nominal_capacity_wh
    into cal_row
    from battery_calibration
    where device_id = p_device_id;

    if cal_row.nominal_capacity_wh is null or cal_row.nominal_capacity_wh = 0 then
        raise exception 'Battery calibration not found for device id: %', p_device_id;
    end if;

    select charge_wh into prev_charge
    from daily_battery_charge
    where device_id = p_device_id and date < p_date
    order by date desc limit 1;

    if prev_charge is null then
        prev_charge := cal_row.nominal_capacity_wh * 0.5;
    end if;

    select max(ch1_v), max(recorded_at) into max_v, last_v
    from telemetry_computed tc
    join devices d on d.device_key = tc.device_key
    where d.id = p_device_id
      and tc.recorded_at >= start_dt and tc.recorded_at < end_dt
      and tc.ch1_v is not null;

    for row_data in (
        select tc.recorded_at, tc.battery_power
        from telemetry_computed tc
        join devices d on d.device_key = tc.device_key
        where d.id = p_device_id
          and tc.recorded_at >= start_dt and tc.recorded_at < end_dt
          and tc.battery_power is not null
        order by tc.recorded_at asc
    ) loop
        if row_data.battery_power > 0 then
            energy_in := energy_in + row_data.battery_power;
        else
            energy_out := energy_out + abs(row_data.battery_power);
        end if;
    end loop;

    charge_wh := greatest(0, prev_charge + energy_in - energy_out);
    charge_wh := least(cal_row.nominal_capacity_wh, charge_wh);

    if max_v >= cal_row.full_charge_voltage then
        charge_wh := cal_row.nominal_capacity_wh;
    end if;

    insert into daily_battery_charge
        (device_id, date, capacity_wh, energy_in_wh, energy_out_wh, last_voltage,
         full_charge_voltage, is_full_charge_day, charge_wh, computed_from)
    values (p_device_id, p_date, cal_row.nominal_capacity_wh, energy_in, energy_out,
            last_v, cal_row.full_charge_voltage,
            max_v >= cal_row.full_charge_voltage, charge_wh, 'battery_power')
    on conflict (device_id, date) do update set
        capacity_wh = cal_row.nominal_capacity_wh,
        energy_in_wh = energy_in,
        energy_out_wh = energy_out,
        last_voltage = last_v,
        full_charge_voltage = cal_row.full_charge_voltage,
        is_full_charge_day = max_v >= cal_row.full_charge_voltage,
        charge_wh = charge_wh,
        computed_from = 'battery_power',
        created_at = now();

    select * into result_row
    from daily_battery_charge
    where device_id = p_device_id and date = p_date;

    return result_row;
end;
$$;

-- get_battery_charge: returns current battery state
create or replace function public.get_battery_charge(
    p_device_id uuid,
    p_hours int default 24
) returns table (
    charge_wh float,
    capacity_wh float,
    energy_in_24h float,
    energy_out_24h float,
    soc_pct float,
    is_full_charge_today boolean
) language plpgsql security definer as $$
declare
    cal_row record;
    last_row daily_battery_charge%rowtype;
    device_key_val text;
begin
    select device_key into device_key_val
    from devices where id = p_device_id;

    select full_charge_voltage, nominal_capacity_wh
    into cal_row
    from battery_calibration
    where device_id = p_device_id;

    select * into last_row
    from daily_battery_charge
    where device_id = p_device_id and date = current_date;

    with bounds as (
        select now() - (p_hours || ' hours')::interval as ts_start
    )
    select
        coalesce(sum(case when battery_power > 0 then abs(battery_power) end), 0),
        coalesce(sum(case when battery_power < 0 then abs(battery_power) end), 0)
    into energy_in_24h, energy_out_24h
    from (
        select battery_power,
               lead(recorded_at) over (order by recorded_at) as next_ts,
               EXTRACT(EPOCH FROM (lead(recorded_at) over (order by recorded_at) - recorded_at)) / 3600 as dt_hrs
        from telemetry_computed
        where device_key = device_key_val
          and recorded_at >= (now() - (p_hours || ' hours')::interval)
          and battery_power is not null
    ) sub
    where dt_hrs > 0 and dt_hrs < 2;

    charge_wh := coalesce(last_row.charge_wh, cal_row.nominal_capacity_wh * 0.5);
    capacity_wh := coalesce(cal_row.nominal_capacity_wh, 1000);
    soc_pct := (charge_wh / nullif(capacity_wh, 0)) * 100;
    is_full_charge_today := coalesce(last_row.is_full_charge_day, false);

    return next;
end;
$$;

grant execute on function public.trg_on_telemetry_computed_insert() to authenticated;
grant execute on function public.compute_daily_battery_charge(uuid, date) to authenticated;
grant execute on function public.get_battery_charge(uuid, int) to authenticated;