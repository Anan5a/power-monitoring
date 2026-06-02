-- Enable pg_cron for scheduled maintenance
create extension if not exists pg_cron;

---------------------------------------------------------------
-- 1. User profiles (extends auth.users)
---------------------------------------------------------------
create table public.profiles (
    id uuid primary key references auth.users(id) on delete cascade,
    display_name text,
    created_at timestamptz default now()
);

---------------------------------------------------------------
-- 2. Device registry
---------------------------------------------------------------
create table public.devices (
    id uuid default gen_random_uuid() primary key,
    user_id uuid not null references public.profiles(id) on delete cascade,
    device_name text not null,
    device_type text not null default 'generic',
    device_key text unique not null,
    device_api_key uuid unique not null default gen_random_uuid(),
    ble_pin text,
    is_online boolean default false,
    ble_pin text,
    updated_at timestamptz default now()
);

create index idx_devices_user_id on public.devices (user_id);
create index idx_devices_device_key on public.devices (device_key);

---------------------------------------------------------------
-- 3. Device profiles — defines which payload fields to chart per device type
---------------------------------------------------------------
create table public.device_profiles (
    id bigint generated always as identity primary key,
    device_type text unique not null,
    label text not null,
    fields jsonb not null default '[]'
);

---------------------------------------------------------------
-- 4. Telemetry — raw readings (hot, 7-day rolling retention)
---------------------------------------------------------------
create table public.telemetry_live (
    id bigint generated always as identity primary key,
    device_id text not null,
    recorded_at timestamptz default now() not null,
    payload jsonb not null default '{}',
    metadata jsonb default '{}'
);

alter table public.telemetry_live replica identity full;

create index idx_telemetry_live_device_time
    on public.telemetry_live (device_id, recorded_at desc);
create index idx_telemetry_live_recorded_at
    on public.telemetry_live (recorded_at desc);

---------------------------------------------------------------
-- 5. Relay states (for power-monitor device type)
---------------------------------------------------------------
create table public.relay_states (
    id bigint generated always as identity primary key,
    device_key text not null references public.devices(device_key) on delete cascade,
    relay_index smallint not null,
    channel smallint not null default 0,
    gpio_pin smallint not null,
    is_energized boolean default false,
    active_high boolean default true,
    last_tripped_at timestamptz,
    constraint unique_relay unique(device_key, relay_index)
);

---------------------------------------------------------------
-- Per-device channel configuration: groups, custom names, battery profiles
-- This allows per-device label overrides and battery chemistry config
---------------------------------------------------------------
create table public.device_channels (
    id bigint generated always as identity primary key,
    device_key text unique not null references public.devices(device_key) on delete cascade,
    channel_groups jsonb default '[]',
    channel_names jsonb default '[]',
    battery_profiles jsonb default '[]',
    channel_calibration jsonb default '{"volt_offset_mv":[0,0,0],"volt_gain":[1,1,1],"curr_offset_ma":[0,0,0],"curr_gain":[1,1,1]}'::jsonb,
    virtual_channels jsonb default '[]'::jsonb,
    ble_pin text,
    updated_at timestamptz default now()
);

alter table public.device_channels replica identity full;
alter table public.device_channels enable row level security;

-- Supabase realtime publication
drop publication if exists supabase_realtime;
create publication supabase_realtime for table public.telemetry_live, public.devices, public.device_channels;

create policy "own_device_channels" on public.device_channels
    for all to authenticated using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = device_channels.device_key
              and p.id = auth.uid()
        )
    );

-- ESP32 upserts device_channels row (security definer — bypasses RLS like claim_settings_command)
create or replace function public.sync_device_channels(p_device_key text, p_payload jsonb)
returns void language plpgsql security definer as $$
begin
    insert into public.device_channels (device_key, channel_names, battery_profiles, channel_calibration, virtual_channels, channel_groups)
    values (p_device_key, p_payload->'channel_names', p_payload->'battery_profiles',
            p_payload->'channel_calibration', p_payload->'virtual_channels', p_payload->'channel_groups')
    on conflict (device_key) do update
        set channel_names = EXCLUDED.channel_names,
            battery_profiles = EXCLUDED.battery_profiles,
            channel_calibration = EXCLUDED.channel_calibration,
            virtual_channels = EXCLUDED.virtual_channels,
            channel_groups = EXCLUDED.channel_groups,
            updated_at = now();
end;
$$;

-- ESP32 syncs just the calibration fields (partial update via security definer RPC)
create or replace function public.sync_device_calibration(p_device_key text, p_calibration jsonb)
returns void language plpgsql security definer as $$
begin
    update public.device_channels
    set channel_calibration = p_calibration,
        updated_at = now()
    where device_key = p_device_key;
end;
$$;

-- ESP32 upserts relay state (security definer — bypasses RLS)
create or replace function public.sync_relay_state(
    p_device_key text,
    p_relay_index smallint,
    p_gpio_pin smallint,
    p_is_energized boolean,
    p_active_high boolean default true,
    p_last_tripped_at timestamptz default now(),
    p_channel smallint default 0
) returns void language plpgsql security definer as $$
begin
    insert into public.relay_states (device_key, relay_index, channel, gpio_pin, is_energized, active_high, last_tripped_at)
    values (p_device_key, p_relay_index, p_channel, p_gpio_pin, p_is_energized, p_active_high, p_last_tripped_at)
    on conflict (device_key, relay_index) do update
        set gpio_pin = EXCLUDED.gpio_pin,
            is_energized = EXCLUDED.is_energized,
            active_high = EXCLUDED.active_high,
            channel = EXCLUDED.channel,
            last_tripped_at = EXCLUDED.last_tripped_at;
end;
$$;

-- ESP32 upserts sensor calibration status (security definer — bypasses RLS)
create or replace function public.sync_sensor_calibration_status(
    p_device_key text,
    p_calibrating boolean,
    p_baseline_tick smallint,
    p_baseline_stddev jsonb
) returns void language plpgsql security definer as $$
begin
    insert into public.sensor_calibration_status (device_key, calibrating, baseline_tick, baseline_stddev, updated_at)
    values (p_device_key, p_calibrating, p_baseline_tick, p_baseline_stddev, now())
    on conflict (device_key) do update
        set calibrating = EXCLUDED.calibrating,
            baseline_tick = EXCLUDED.baseline_tick,
            baseline_stddev = EXCLUDED.baseline_stddev,
            updated_at = now();
end;
$$;

-- ESP32 reads ble_pin from device_channels (security definer — bypasses RLS)
create or replace function public.get_device_ble_pin(p_device_key text)
returns text language plpgsql security definer as $$
declare
    pin_val text;
begin
    select ble_pin into pin_val from public.device_channels where device_key = p_device_key;
    return pin_val;
end;
$$;

-- ESP32 syncs ble_pin to device_channels (security definer — bypasses RLS)
create or replace function public.sync_ble_pin(p_device_key text, p_ble_pin text)
returns void language plpgsql security definer as $$
begin
    update public.device_channels
    set ble_pin = p_ble_pin, updated_at = now()
    where device_key = p_device_key;
end;
$$;

-- ble_pin readable to device owner (for UI dynamic PIN)
create policy "own_device_ble_pin" on public.devices
    for select to authenticated using (user_id = auth.uid());

---------------------------------------------------------------
-- Triggers
---------------------------------------------------------------

-- Auto-create profile when user signs up
create or replace function public.handle_new_user()
returns trigger language plpgsql security definer as $$
begin
    insert into public.profiles (id, display_name)
    values (
        new.id,
        coalesce(
            new.raw_user_meta_data->>'display_name',
            split_part(new.email, '@', 1)
        )
    );
    return new;
end;
$$;

create trigger on_auth_user_created
    after insert on auth.users
    for each row execute function public.handle_new_user();

-- Auto-update last_seen_at and is_online on telemetry insert
create or replace function public.handle_telemetry_insert()
returns trigger language plpgsql security definer as $$
begin
    update public.devices
    set last_seen_at = now(), is_online = true
    where device_key = new.device_id;
    return new;
end;
$$;

create trigger on_telemetry_insert
    after insert on public.telemetry_live
    for each row execute function public.handle_telemetry_insert();

---------------------------------------------------------------
-- Row Level Security
---------------------------------------------------------------
alter table public.profiles enable row level security;
alter table public.devices enable row level security;
alter table public.device_profiles enable row level security;
alter table public.telemetry_live enable row level security;
alter table public.relay_states enable row level security;

-- Profiles: users manage own profile
create policy "own_profile" on public.profiles
    for all to authenticated using (id = auth.uid());

-- Devices: users full access to own devices
create policy "own_devices" on public.devices
    for all to authenticated using (user_id = auth.uid());

-- Grant table-level permissions for authenticated role
-- (table grants needed even with RLS so the role can access the table at all)
grant select, insert, update, delete on public.devices to authenticated;
grant select on public.profiles to authenticated;
grant select on public.device_profiles to authenticated;
grant select, insert, update on public.device_channels to authenticated;
grant select on public.telemetry_live to authenticated;
grant select, insert, update on public.relay_states to authenticated;

-- Device profiles: all authenticated can read (for UI dropdown)
create policy "read_device_profiles" on public.device_profiles
    for select to authenticated using (true);

-- Telemetry live: users read own device data only
-- (device_id = device_key which maps to user_id via devices table)
create policy "own_telemetry_select" on public.telemetry_live
    for select to authenticated
    using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = telemetry_live.device_id
              and p.id = auth.uid()
        )
    );

-- Relay states: users manage own relays
create policy "own_relays" on public.relay_states
    for all to authenticated
    using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = relay_states.device_key
              and p.id = auth.uid()
        )
    );

---------------------------------------------------------------
-- Secure RPC: ESP32 inserts telemetry using per-device API key
-- The function validates both device_key AND device_api_key.
-- ESP32 uses device_api_key via the project's anon key (not service_role).
-- This means one compromised device cannot inject data for another device.
---------------------------------------------------------------
create or replace function public.insert_telemetry(
    p_device_key text,
    p_device_api_key uuid,
    p_payload jsonb,
    p_metadata jsonb default '{}',
    p_recorded_at bigint default null
) returns void language plpgsql security definer as $$
begin
    -- Verify device_key AND device_api_key both match the same device
    if not exists (
        select 1 from public.devices
        where device_key = p_device_key
          and device_api_key = p_device_api_key
    ) then
        raise exception 'Invalid device_key or device_api_key';
    end if;

    insert into public.telemetry_live (device_id, payload, metadata, recorded_at)
    values (
        p_device_key,
        p_payload,
        p_metadata,
        case
            when p_recorded_at is not null then to_timestamp(p_recorded_at)::timestamptz
            else now()
        end
    );
end;
$$;

-- Direct insert only allowed with valid device_api_key via the RPC function
-- (the RPC validates both keys, so a compromised anon key for one device
--  cannot insert for any other device)
create policy "anon_insert_telemetry" on public.telemetry_live
    for insert to authenticated
    with check (
        exists (
            select 1 from public.devices d
            where d.device_key = telemetry_live.device_id
              and d.device_api_key = current_setting('app.device_api_key', true)::uuid
        )
    );
create policy "no_direct_insert" on public.telemetry_live
    for insert to authenticated
    with check (false);

---------------------------------------------------------------
-- Telemetry retention: time-based strategy
--   telemetry_live     -> purge after 48h (raw payload copied to computed)
--   telemetry_computed -> keep 90 days (primary long-term store)
--   telemetry_backup   -> daily aggregates for data older than 90 days
-- Sensor calibration status: baseline noise collected per channel, updated during calibrate_baseline
---------------------------------------------------------------
create table public.sensor_calibration_status (
    id bigint generated always as identity primary key,
    device_key text unique not null references public.devices(device_key) on delete cascade,
    calibrating boolean default false,
    baseline_tick smallint default 0,
    baseline_stddev jsonb default '{}',
    updated_at timestamptz default now()
);
alter table public.sensor_calibration_status enable row level security;
grant select, insert, update on public.sensor_calibration_status to authenticated;

-- Supabase realtime
drop publication if exists supabase_realtime;
create publication supabase_realtime for table public.telemetry_live, public.devices, public.device_channels, public.sensor_calibration_status, public.relay_states;

---------------------------------------------------------------
-- Backup table: daily aggregates of computed telemetry older than 15 days
---------------------------------------------------------------
create table public.telemetry_backup (
    id bigint generated always as identity primary key,
    device_key text not null,
    day date not null,

    pv_power_avg float not null default 0,
    pv_power_min float not null default 0,
    pv_power_max float not null default 0,

    battery_power_avg float not null default 0,
    battery_power_min float not null default 0,
    battery_power_max float not null default 0,

    battery_charging_power_avg float not null default 0,
    battery_charging_power_max float not null default 0,

    battery_discharging_power_avg float not null default 0,
    battery_discharging_power_max float not null default 0,

    dc_load_power_avg float not null default 0,
    dc_load_power_max float not null default 0,

    inverter_power_avg float not null default 0,
    inverter_power_min float not null default 0,
    inverter_power_max float not null default 0,

    total_energy_wh_start float,
    total_energy_wh_end float,
    energy_wh_delta float,

    min_soc_pct float,
    max_soc_pct float,

    sample_count int not null default 0,
    created_at timestamptz not null default now(),

    unique (device_key, day)
);

create index idx_telemetry_backup_device_day
    on public.telemetry_backup (device_key, day desc);

alter table public.telemetry_backup enable row level security;

create policy "own_telemetry_backup" on public.telemetry_backup
    for select to authenticated using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = telemetry_backup.device_key
              and p.id = auth.uid()
        )
    );

grant select on public.telemetry_backup to authenticated;

---------------------------------------------------------------
-- Retention: telemetry_live deleted by trigger (passthrough only)
-- telemetry_computed: 0-48h raw 1/sec, then 2-sec rollup (via rollup_telemetry_computed)
-- telemetry_computed >15d: archived to telemetry_backup (via this job)
---------------------------------------------------------------
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

select cron.schedule(
    'telemetry-maintenance',
    '23 * * * *',
    'select public.archive_and_purge_telemetry()'
);

    ---------------------------------------------------------------
-- Seed data: power-monitor device profile
---------------------------------------------------------------
insert into public.device_profiles (device_type, label, fields) values (
    'power-monitor',
    'Power Monitor v2',
    '[
        {"key": "ina3221_v0", "label": "Ch0 Voltage", "unit": "V", "chart": true},
        {"key": "ina3221_i0", "label": "Ch0 Current", "unit": "A", "chart": true},
        {"key": "ina3221_v1", "label": "Ch1 Voltage", "unit": "V", "chart": true},
        {"key": "ina3221_i1", "label": "Ch1 Current", "unit": "A", "chart": true},
        {"key": "ina3221_v2", "label": "Ch2 Voltage", "unit": "V", "chart": true},
        {"key": "ina3221_i2", "label": "Ch2 Current", "unit": "A", "chart": true},
        {"key": "ina226_v", "label": "Ch3 Voltage", "unit": "V", "chart": true},
        {"key": "ina226_i", "label": "Ch3 Current", "unit": "A", "chart": true},
        {"key": "ina226_p", "label": "Ch3 Power", "unit": "W", "chart": true},
        {"key": "ads1115_0", "label": "ADC Ch0", "unit": "V", "chart": true},
        {"key": "ads1115_1", "label": "ADC Ch1", "unit": "V", "chart": false},
        {"key": "ads1115_2", "label": "ADC Ch2", "unit": "V", "chart": false},
        {"key": "ads1115_3", "label": "ADC Ch3", "unit": "V", "chart": false},
        {"key": "coulomb_mah0", "label": "Ch0 mAh", "unit": "mAh", "chart": false},
        {"key": "coulomb_mah1", "label": "Ch1 mAh", "unit": "mAh", "chart": false},
        {"key": "coulomb_mah2", "label": "Ch2 mAh", "unit": "mAh", "chart": false},
        {"key": "coulomb_mah3", "label": "Ch3 mAh", "unit": "mAh", "chart": false},
        {"key": "soc_pct0", "label": "Ch0 SoC", "unit": "%", "chart": true},
        {"key": "soc_pct1", "label": "Ch1 SoC", "unit": "%", "chart": true},
        {"key": "soc_pct2", "label": "Ch2 SoC", "unit": "%", "chart": true},
        {"key": "soc_pct3", "label": "Ch3 SoC", "unit": "%", "chart": true},
        {"key": "relay_states", "label": "Relay States", "unit": "", "chart": false},
        {"key": "log_entries", "label": "Log Entries", "unit": "", "chart": false},
        {"key": "log_overflow", "label": "Log Overflow", "unit": "", "chart": false}
    ]'::jsonb
);

---------------------------------------------------------------
-- Settings commands: ESP32 polls for pending config changes
---------------------------------------------------------------
create table public.settings_commands (
    id bigint generated always as identity primary key,
    device_key text not null,
    cmd_type text not null,
    payload jsonb not null default '{}',
    status text not null default 'pending',  -- pending | applied | failed
    created_at timestamptz default now(),
    applied_at timestamptz
);

create index idx_settings_cmds_device_status on public.settings_commands (device_key, status);

alter table public.settings_commands enable row level security;

-- Users write commands for their own devices
create policy "own_settings_commands_insert" on public.settings_commands
    for insert to authenticated
    with check (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = settings_commands.device_key
              and p.id = auth.uid()
        )
    );

-- ESP32 reads pending commands (device owns its own rows via device_key)
create policy "device_read_settings_commands" on public.settings_commands
    for select to authenticated
    using (device_key = current_setting('app.device_key', true));

-- ESP32 updates status to applied/failed
create policy "device_update_settings_commands" on public.settings_commands
    for update to authenticated
    using (device_key = current_setting('app.device_key', true));

-- Atomically claim the oldest pending command for a device
create or replace function public.claim_settings_command(p_device_key text)
returns jsonb language plpgsql security definer as $$
declare
    cmd_row record;
begin
    update public.settings_commands
    set status = 'applied', applied_at = now()
    where id = (
        select id from public.settings_commands
        where device_key = p_device_key and status = 'pending'
        order by created_at asc limit 1
    )
    returning cmd_type, payload into cmd_row;

    if cmd_row.cmd_type is null then
        return null;
    end if;

    return jsonb_build_object('cmd_type', cmd_row.cmd_type, 'payload', cmd_row.payload);
end;
$$;

---------------------------------------------------------------
-- Hourly energy aggregation: snapshot cumulative Wh per device per hour
-- Takes the latest energy_wh* values from telemetry_live each hour
-- Dashboard computes deltas by subtracting previous hour
---------------------------------------------------------------
create table public.energy_hourly (
    device_key text not null references public.devices(device_key) on delete cascade,
    hour timestamptz not null,
    energy_wh0 float default 0,
    energy_wh1 float default 0,
    energy_wh2 float default 0,
    energy_wh3 float default 0,
    primary key (device_key, hour)
);

create index idx_energy_hourly_device_hour on public.energy_hourly (device_key, hour desc);

alter table public.energy_hourly enable row level security;
grant select, insert, update on public.energy_hourly to authenticated;

-- Users read their own device energy data
create policy "own_energy_select" on public.energy_hourly
    for select to authenticated
    using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = energy_hourly.device_key
              and p.id = auth.uid()
        )
    );

-- Cron: snapshot latest energy values every hour at :05
create or replace function public.snapshot_hourly_energy()
returns void language plpgsql security definer as $$
declare
    rec record;
    bucket timestamptz := date_trunc('hour', now());
begin
    for rec in
        select distinct on (device_id) device_id, payload, recorded_at
        from public.telemetry_live
        where recorded_at >= bucket - interval '1 hour'
          and recorded_at < bucket + interval '1 hour'
        order by device_id, recorded_at desc
    loop
        insert into public.energy_hourly (device_key, hour, energy_wh0, energy_wh1, energy_wh2, energy_wh3)
        values (
            rec.device_id,
            bucket,
            coalesce((rec.payload->>'energy_wh0')::float, 0),
            coalesce((rec.payload->>'energy_wh1')::float, 0),
            coalesce((rec.payload->>'energy_wh2')::float, 0),
            coalesce((rec.payload->>'energy_wh3')::float, 0)
        )
        on conflict (device_key, hour) do update set
            energy_wh0 = excluded.energy_wh0,
            energy_wh1 = excluded.energy_wh1,
            energy_wh2 = excluded.energy_wh2,
            energy_wh3 = excluded.energy_wh3;
    end loop;
end;
$$;

select cron.schedule(
    'hourly-energy-snapshot',
    '5 * * * *',  -- 5 min past each hour (gives telemetry time to arrive)
    'select public.snapshot_hourly_energy()'
);

-- Add energy_hourly to realtime publication
alter publication supabase_realtime add table public.energy_hourly;

---------------------------------------------------------------
-- Telemetry Computed: trigger-computed enriched telemetry
-- One row per telemetry insert (append-only, not upsert)
-- 0-48h: raw 1/sec typed columns
-- 48h+: 2-sec rollup (via rollup_telemetry_computed cron job)
-- >15 days: daily backup (via archive_and_purge_telemetry cron job)
-- telemetry_live is a passthrough — deleted immediately by trigger
---------------------------------------------------------------

create table public.telemetry_computed (
    id bigint generated always as identity primary key,
    device_key text not null,
    recorded_at timestamptz not null,

    -- Power breakdowns
    pv_power float not null default 0,
    battery_power float not null default 0,       -- positive=discharge, negative=charge
    battery_charging_power float not null default 0,
    battery_discharging_power float not null default 0,
    dc_load_power float not null default 0,
    unclassified_power float not null default 0,

    -- Inverter net power
    inverter_power float not null default 0,

    -- System state
    system_status text not null default 'unknown',

    -- Aggregated SoC
    min_soc_pct float,
    max_soc_pct float,

    -- Energy
    total_energy_wh float not null default 0,

    -- Per-channel typed columns (replaces raw_payload JSONB)
    -- INA3221 bus voltages
    ina3221_v0 real,
    ina3221_v1 real,
    ina3221_v2 real,
    ina3221_i0 real,
    ina3221_i1 real,
    ina3221_i2 real,
    -- INA226
    ina226_v real,
    ina226_i real,
    ina226_p real,
    -- ADS1115
    ads1115_0 real,
    ads1115_1 real,
    ads1115_2 real,
    ads1115_3 real,
    -- Coulombs
    coulomb_mah0 real,
    coulomb_mah1 real,
    coulomb_mah2 real,
    coulomb_mah3 real,
    -- Energy per channel
    energy_wh0 real,
    energy_wh1 real,
    energy_wh2 real,
    energy_wh3 real,
    -- SoC per channel
    soc_pct0 real,
    soc_pct1 real,
    soc_pct2 real,
    soc_pct3 real,
    -- Relay states
    relay0 boolean,
    relay1 boolean,
    relay2 boolean,
    relay3 boolean,
    -- Virtual channels
    ch0_v real,
    ch0_i real,
    ch0_p real,
    ch1_v real,
    ch1_i real,
    ch1_p real,
    ch2_v real,
    ch2_i real,
    ch2_p real,
    ch3_v real,
    ch3_i real,
    ch3_p real,
    -- Spike flags
    ina3221_v0_spike boolean,
    ina3221_v1_spike boolean,
    ina3221_v2_spike boolean,
    ina3221_i0_spike boolean,
    ina3221_i1_spike boolean,
    ina3221_i2_spike boolean,

    created_at timestamptz not null default now(),

    unique (device_key, recorded_at)
);

alter table public.telemetry_computed enable row level security;

create policy "own_telemetry_computed" on public.telemetry_computed
    for all to authenticated using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = telemetry_computed.device_key
              and p.id = auth.uid()
        )
    );

grant select, insert on public.telemetry_computed to authenticated;

create index idx_telemetry_computed_device_time
    on public.telemetry_computed (device_key, recorded_at desc);

alter publication supabase_realtime add table public.telemetry_computed;

---------------------------------------------------------------
-- Trigger function: compute telemetry values from raw payload
-- Append-only: one computed row per telemetry_live insert
-- Defensive: all lookups use coalesce to avoid NULL propagation
---------------------------------------------------------------
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

        -- If we have channel_groups, use them for classification
        if cg_arr is not null and jsonb_array_length(cg_arr) > 0 then

            -- Iterate each group (SELECT syntax to avoid parser ambiguity)
            for grp in select elem from jsonb_array_elements(cg_arr) as elem loop
                grp_icon := (grp->>'icon')::int;
                grp_mask := (grp->>'channel_mask')::int;

                -- Check if this channel's bit is set in this group's mask
                if (grp_mask & (1 << ch_idx)) != 0 then
                    ch_in_any_group := true;

                    if grp_icon = 0 then
                        -- Solar: only count positive (generating)
                        pv_power_val := pv_power_val + greatest(ch_power, 0);
                    elsif grp_icon = 1 then
                        -- Battery: positive = charging, negative = discharging
                        if ch_power > 0 then
                            battery_charging := battery_charging + ch_power;
                        else
                            battery_discharging := battery_discharging + abs(ch_power);
                        end if;
                    elsif grp_icon = 2 then
                        -- Load: negative power = consuming (discharging the system)
                        dc_load_val := dc_load_val + case when ch_power < 0 then abs(ch_power) else 0 end;
                    else
                        -- Generic (icon 3) or unknown: treat as load
                        dc_load_val := dc_load_val + case when ch_power < 0 then abs(ch_power) else 0 end;
                    end if;

                    -- Channel found — don't check other groups (first match wins)
                    exit;
                end if;
            end loop;

        end if;

        -- Channel not in any group — fall back: check battery_profiles
        if not ch_in_any_group then
            declare
                bp_capacity float;
            begin
                -- Look for a battery profile for this channel with capacity > 0
                select (bp->>'capacity_mAh')::float into bp_capacity
                from (
                    select jsonb_array_elements(device_channels.battery_profiles) as bp,
                           (jsonb_array_elements(device_channels.battery_profiles)->>'channel')::int as bp_ch
                    from public.device_channels
                    where device_key = new.device_id
                ) sub
                where bp_ch = ch_idx
                  and bp_capacity > 0;

                if bp_capacity is not null and bp_capacity > 0 then
                    -- Fallback battery profile: positive = charging, negative = discharging
                    if ch_power > 0 then
                        battery_charging := battery_charging + ch_power;
                    else
                        battery_discharging := battery_discharging + abs(ch_power);
                    end if;
                    ch_in_any_group := true;
                end if;
            end;
        end if;

        -- Still not in any group — treat as unclassified
        if not ch_in_any_group then
            unclassified_val := unclassified_val + greatest(ch_power, 0);
        end if;
    end loop;

    -- Compute inverter power
    inv_power := pv_power_val + battery_discharging - battery_charging - dc_load_val;

    -- System status
    if battery_charging > 5 then
        sys_status := 'charging';
    elsif battery_discharging > 5 then
        sys_status := 'discharging';
    elsif abs(inv_power) <= 5 then
        sys_status := 'balanced';
    else
        sys_status := 'unknown';
    end if;

    -- SoC: extract soc_pct0..3, filter nulls
    select min(v), max(v) into min_soc, max_soc
    from unnest(array[
        (new.payload->>'soc_pct0')::float,
        (new.payload->>'soc_pct1')::float,
        (new.payload->>'soc_pct2')::float,
        (new.payload->>'soc_pct3')::float
    ]) as v where v is not null;

    -- Total energy
    total_energy := coalesce((new.payload->>'energy_wh0')::float, 0)
                  + coalesce((new.payload->>'energy_wh1')::float, 0)
                  + coalesce((new.payload->>'energy_wh2')::float, 0)
                  + coalesce((new.payload->>'energy_wh3')::float, 0);

    -- Insert computed row (append-only, no upsert)
    -- telemetry_live row is deleted by trigger after this insert
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

-- Trigger: fires AFTER each insert on telemetry_live
drop trigger if exists on_telemetry_computed_update on public.telemetry_live;
create trigger on_telemetry_computed_update
    after insert on public.telemetry_live
    for each row execute function public.compute_telemetry_row();

---------------------------------------------------------------
-- RPC: get_aggregated_telemetry
-- Time-bucket aggregation for chart long ranges
-- Reads typed columns from telemetry_computed (no raw_payload)
---------------------------------------------------------------
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
            ina226_v, ina226_i, ina226_p
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