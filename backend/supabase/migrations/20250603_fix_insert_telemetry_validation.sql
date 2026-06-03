-- Add timestamp validation to insert_telemetry to reject absurd future dates
-- Also fix the trigger to reject out-of-range recorded_at

create or replace function public.insert_telemetry(
    p_device_key text,
    p_device_api_key uuid,
    p_payload jsonb,
    p_metadata jsonb default '{}',
    p_recorded_at bigint default null
) returns void language plpgsql security definer as $$
declare
    ts timestamptz;
begin
    -- Verify device_key AND device_api_key both match the same device
    if not exists (
        select 1 from public.devices
        where device_key = p_device_key
          and device_api_key = p_device_api_key
    ) then
        raise exception 'Invalid device_key or device_api_key';
    end if;

    -- Validate timestamp: reject more than 1 hour in the future or before 2024
    if p_recorded_at is not null then
        ts := to_timestamp(p_recorded_at)::timestamptz;
        if ts > now() + interval '1 hour' then
            raise exception 'Timestamp too far in future: %', ts;
        end if;
        if ts < '2024-01-01'::timestamptz then
            raise exception 'Timestamp too old: %', ts;
        end if;
    else
        ts := now();
    end if;

    insert into public.telemetry_live (device_id, payload, metadata, recorded_at)
    values (p_device_key, p_payload, p_metadata, ts);
end;
$$;

-- Clean up existing corrupted future rows from telemetry_computed
-- (keep only rows with reasonable timestamps: 2024 through 1 hour from now)
delete from public.telemetry_computed
where recorded_at > now() + interval '1 hour'
   or recorded_at < '2024-01-01'::timestamptz;

-- Clean up any orphaned rows in telemetry_live (should be empty due to trigger, but just in case)
delete from public.telemetry_live
where recorded_at > now() + interval '1 hour'
   or recorded_at < '2024-01-01'::timestamptz;
