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
    last_seen_at timestamptz default now(),
    created_at timestamptz default now()
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
-- 5. Daily archive (cold, indefinite) — aggregates all numeric payload fields
---------------------------------------------------------------
create table public.telemetry_archive (
    id bigint generated always as identity primary key,
    device_id text not null,
    log_date date not null,
    sample_count bigint not null,
    payload_agg jsonb not null default '{}',
    constraint unique_device_date unique(device_id, log_date)
);

create index idx_telemetry_archive_device_date
    on public.telemetry_archive (device_id, log_date desc);

---------------------------------------------------------------
-- 6. Relay states (for power-monitor device type)
---------------------------------------------------------------
create table public.relay_states (
    id bigint generated always as identity primary key,
    device_key text not null references public.devices(device_key) on delete cascade,
    relay_index smallint not null,
    gpio_pin smallint not null,
    is_energized boolean default false,
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
    updated_at timestamptz default now()
);

alter table public.device_channels replica identity full;
alter table public.device_channels enable row level security;

-- Supabase realtime publication (create after all tables exist)
drop publication if exists supabase_realtime;
create publication supabase_realtime for table public.telemetry_live, public.devices, public.telemetry_archive, public.device_channels;

create policy "own_device_channels" on public.device_channels
    for all to authenticated using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = device_channels.device_key
              and p.id = auth.uid()
        )
    );

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
alter table public.telemetry_archive enable row level security;
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
grant select on public.telemetry_archive to authenticated;
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

-- Telemetry archive: same auth pattern
create policy "own_archive_select" on public.telemetry_archive
    for select to authenticated
    using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = telemetry_archive.device_id
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
-- pg_cron: Daily maintenance at midnight UTC
-- Adaptive retention: keeps at least 7 days, targets 80% storage capacity
---------------------------------------------------------------
create or replace function public.archive_and_purge_telemetry()
returns void language plpgsql security definer as $$
declare
    yd date := current_date - interval '1 day';
    row_ct bigint;
    size_mb numeric;
    cutoff_ts timestamptz;
begin
    -- Step 1: Archive yesterday's telemetry into daily aggregates
    insert into public.telemetry_archive (device_id, log_date, sample_count, payload_agg)
    select
        device_id,
        yd,
        count(id),
        jsonb_object(array_agg(key || '_avg'), array_agg(avg_val::text))
    from (
        select
            t.device_id,
            (each(t.payload)).key,
            avg((each(t.payload)).value::numeric) as avg_val
        from public.telemetry_live t
        where t.recorded_at >= yd and t.recorded_at < current_date
        group by t.device_id, (each(t.payload)).key
    ) sub
    group by device_id
    on conflict (device_id, log_date) do nothing;

    get diagnostics row_ct = row_count;

    -- Step 2: Adaptive retention based on actual storage used
    -- Free tier = 500 MB. Start pruning beyond 400 MB (80%).
    -- Always keep minimum 7 days. Target: stay under 350 MB (70%).
    select pg_total_relation_size('public.telemetry_live') / (1024 * 1024) into size_mb;

    if size_mb > 400 then
        -- Delete oldest rows in chunks until under threshold or 7-day minimum hit
        while size_mb > 350 loop
            select recorded_at into cutoff_ts
            from public.telemetry_live
            where recorded_at < current_timestamp - interval '7 days'
            order by recorded_at asc
            limit 1;

            exit when cutoff_ts is null;

            delete from public.telemetry_live
            where recorded_at <= cutoff_ts;

            select pg_total_relation_size('public.telemetry_live') / (1024 * 1024) into size_mb;
        end loop;
    end if;

    -- Step 3: Mark devices offline if no telemetry in last 24 hours
    update public.devices d
    set is_online = false
    where d.last_seen_at < current_timestamp - interval '1 day';
end;
$$;

select cron.schedule(
    'telemetry-maintenance',
    '0 0 * * *',
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