-- New battery profile tables and sync RPCs.
--
-- The new BatteryChemistryProfile shape (chemistry + rated_capacity_Ah + nominal/float/cutoff
-- voltages + c_rating + cycle_life) replaces the legacy BatteryProfile (system_voltage +
-- cell_count + capacity_mAh + initial_soc_pct). The legacy shape is still stored in the
-- device_channels.battery_profiles jsonb column for backward compatibility with old firmware;
-- new firmware writes only to the new tables.
--
-- The firmware POSTs to /rest/v1/rpc/sync_battery_profiles and /rest/v1/rpc/sync_battery_bindings
-- every 60s and eagerly on profile/binding change.

create table if not exists public.battery_profiles (
    device_key text not null references public.devices(device_key) on delete cascade,
    id smallint not null,
    name text not null,
    -- BatteryChemistryEnum: 0=LEAD_ACID, 1=LIION, 2=LFP, 3=LIPO, 4=NICD, 5=NIMH, 6=CUSTOM
    chemistry smallint not null,
    nominal_voltage real,
    rated_capacity_Ah real not null,
    c_rating real,
    cutoff_voltage real,
    float_voltage real,
    charge_efficiency real,
    cycle_life_rated smallint,
    min_soc_pct real,
    max_soc_pct real,
    updated_at timestamptz default now(),
    primary key (device_key, id)
);

create index if not exists idx_battery_profiles_device
    on public.battery_profiles (device_key);

alter table public.battery_profiles enable row level security;

create policy "own_battery_profiles_select" on public.battery_profiles
    for select to authenticated using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = battery_profiles.device_key
              and p.id = auth.uid()
        )
    );

create policy "own_battery_profiles_insert" on public.battery_profiles
    for insert to authenticated with check (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = battery_profiles.device_key
              and p.id = auth.uid()
        )
    );

create policy "own_battery_profiles_update" on public.battery_profiles
    for update to authenticated using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = battery_profiles.device_key
              and p.id = auth.uid()
        )
    ) with check (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = battery_profiles.device_key
              and p.id = auth.uid()
        )
    );

create policy "own_battery_profiles_delete" on public.battery_profiles
    for delete to authenticated using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = battery_profiles.device_key
              and p.id = auth.uid()
        )
    );

grant select, insert, update, delete on public.battery_profiles to authenticated;

-- Battery bindings: which channel uses which profile.
create table if not exists public.battery_bindings (
    device_key text not null references public.devices(device_key) on delete cascade,
    channel smallint not null,
    profile_id smallint not null,
    updated_at timestamptz default now(),
    primary key (device_key, channel)
);

create index if not exists idx_battery_bindings_device
    on public.battery_bindings (device_key);

alter table public.battery_bindings enable row level security;

create policy "own_battery_bindings_select" on public.battery_bindings
    for select to authenticated using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = battery_bindings.device_key
              and p.id = auth.uid()
        )
    );

create policy "own_battery_bindings_insert" on public.battery_bindings
    for insert to authenticated with check (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = battery_bindings.device_key
              and p.id = auth.uid()
        )
    );

create policy "own_battery_bindings_update" on public.battery_bindings
    for update to authenticated using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = battery_bindings.device_key
              and p.id = auth.uid()
        )
    ) with check (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = battery_bindings.device_key
              and p.id = auth.uid()
        )
    );

create policy "own_battery_bindings_delete" on public.battery_bindings
    for delete to authenticated using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = battery_bindings.device_key
              and p.id = auth.uid()
        )
    );

grant select, insert, update, delete on public.battery_bindings to authenticated;

-- sync_battery_profiles: full replace of the profile list for a device.
-- Called by the firmware every 60s and eagerly on profile change.
-- security definer: bypasses RLS (firmware doesn't have a user session).
create or replace function public.sync_battery_profiles(
    p_device_key text,
    p_profiles jsonb
) returns void language plpgsql security definer as $$
begin
    -- Validate device_key first.
    if not exists (select 1 from public.devices where device_key = p_device_key) then
        raise exception 'Unknown device_key: %', p_device_key;
    end if;

    delete from public.battery_profiles where device_key = p_device_key;

    if p_profiles is null or jsonb_array_length(p_profiles) = 0 then
        return;
    end if;

    insert into public.battery_profiles (
        device_key, id, name, chemistry,
        nominal_voltage, rated_capacity_Ah, c_rating,
        cutoff_voltage, float_voltage, charge_efficiency,
        cycle_life_rated, min_soc_pct, max_soc_pct
    )
    select
        p_device_key,
        (p->>'id')::smallint,
        p->>'name',
        (p->>'chemistry')::smallint,
        (p->>'nominal_voltage')::real,
        (p->>'rated_capacity_Ah')::real,
        (p->>'c_rating')::real,
        (p->>'cutoff_voltage')::real,
        (p->>'float_voltage')::real,
        (p->>'charge_efficiency')::real,
        (p->>'cycle_life_rated')::smallint,
        (p->>'min_soc_pct')::real,
        (p->>'max_soc_pct')::real
    from jsonb_array_elements(p_profiles) as p;
end;
$$;

grant execute on function public.sync_battery_profiles(text, jsonb) to authenticated;

-- sync_battery_bindings: full replace of the channel->profile bindings for a device.
-- Called by the firmware every 60s and eagerly on binding change.
create or replace function public.sync_battery_bindings(
    p_device_key text,
    p_bindings jsonb
) returns void language plpgsql security definer as $$
begin
    if not exists (select 1 from public.devices where device_key = p_device_key) then
        raise exception 'Unknown device_key: %', p_device_key;
    end if;

    delete from public.battery_bindings where device_key = p_device_key;

    if p_bindings is null or jsonb_array_length(p_bindings) = 0 then
        return;
    end if;

    insert into public.battery_bindings (device_key, channel, profile_id)
    select
        p_device_key,
        (p->>'channel')::smallint,
        (p->>'profile_id')::smallint
    from jsonb_array_elements(p_bindings) as p;
end;
$$;

grant execute on function public.sync_battery_bindings(text, jsonb) to authenticated;

-- Add to realtime publication so BatteryPage and other UIs can react to changes.
alter publication supabase_realtime add table public.battery_profiles;
alter publication supabase_realtime add table public.battery_bindings;
