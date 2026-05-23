# Authentication & Security Design

## Overview

This system uses two authentication paths:
1. **User authentication** — Supabase email/password for the React dashboard
2. **Device authentication** — `device_key` for ESP32 telemetry POSTs

Both are decoupled. Users never share credentials with devices.

---

## User Authentication (React Dashboard)

### How It Works

1. User opens the dashboard at `yourapp.pages.dev`
2. Clicks **Sign In** → enters email + password
3. Supabase Auth validates credentials, returns a **session** (JWT)
4. The session is stored in `localStorage` via `@supabase/supabase-js`
5. All subsequent API calls from the browser include the session token automatically
6. RLS on all tables checks `auth.uid()` against the user's `user_id`

### Session Flow

```
User signs in → Supabase returns { access_token, refresh_token }
↓
Browser stores in localStorage
↓
@supabase/supabase-js attaches to all requests
↓
API request includes: Authorization: Bearer <access_token>
↓
Supabase validates → returns auth.uid() for RLS checks
```

### Signing Out

When the user signs out:
```typescript
supabase.auth.signOut()
```
This clears localStorage and the next navigation to `/dashboard` redirects to `/login`.

### Row Level Security (RLS)

All data tables have RLS enabled. Example for `telemetry_live`:

```sql
create policy "own_telemetry_select" on public.telemetry_live
    for select to authenticated
    using (
        exists (
            select 1 from public.devices d
            join public.profiles p on p.id = d.user_id
            where d.device_key = telemetry_live.device_id
              and p.id = auth.uid()   -- auth.uid() from the session JWT
        )
    );
```

This ensures User A cannot read telemetry from User B's devices, even if they somehow obtain the device_key.

---

## Device Authentication (ESP32)

### How It Works

1. User creates a device in the Admin panel → a `device_key` (UUID) is generated
2. User provisions the ESP32 via BLE with:
   - WiFi credentials
   - Supabase project URL
   - `service_role` key
   - `device_key`
3. ESP32 stores all three in NVS
4. Every telemetry POST includes the `device_key` in the payload

### Why `service_role` Key for ESP32?

The ESP32 cannot perform browser OAuth. It uses the `service_role` key directly because:
- `service_role` bypasses RLS at the connection level
- But the `insert_telemetry` RPC function **validates the device_key** before inserting
- So even with a compromised service_role key, an attacker cannot inject data for arbitrary devices

### How the RPC Validates

```sql
create or replace function public.insert_telemetry(
    p_device_key text,
    p_payload jsonb,
    p_metadata jsonb default '{}'
) returns void language plpgsql security definer as $$
begin
    -- Verify device_key exists before inserting
    if not exists (
        select 1 from public.devices
        where device_key = p_device_key
    ) then
        raise exception 'Invalid device_key';
    end if;

    insert into public.telemetry_live (device_id, payload, metadata)
    values (p_device_key, p_payload, p_metadata);
end;
$$;
```

If an attacker tries to POST with a random `device_key`, the function rejects it.

### ESP32 Payload Example

```json
{
  "p_device_key": "abc123-device-key",
  "p_payload": { "temperature": 25.4 },
  "p_metadata": { "rssi": -60 }
}
```

The `device_key` in the payload maps to a `user_id` through the `devices` table. The user's RLS policies then control who can read that telemetry.

---

## Security Comparison

| Vector | Protection |
|---|---|
| User steals another user's device_key | RLS blocks reading — device_key alone is insufficient |
| Attacker obtains service_role key | RPC validates device_key belongs to a real device |
| ESP32 posts for wrong device | Function rejects if device_key not in `devices` table |
| User A spoofs User B's user_id | RLS checks `auth.uid()` which is cryptographically verified |
| Browser XSS | HttpOnly cookies not used — Supabase uses localStorage (standard for SPAs) |

---

## Key Storage

| Credential | Where Stored | Who Has Access |
|---|---|---|
| User password | Supabase Auth (hashed) | User only |
| User session JWT | Browser localStorage | User's browser only |
| Anon key | React app (VITE_ env var) | Public — needed for unauthenticated reads |
| Service role key | ESP32 NVS (BLE provisioned) | ESP32 device only |
| Device key | ESP32 NVS + Supabase devices table | User (admin panel), ESP32 |

---

## RLS Policy Reference

| Table | Select | Insert | Update | Delete |
|---|---|---|---|---|
| `profiles` | Own user only (`id = auth.uid()`) | Auto via trigger | Own user only | Own user only |
| `devices` | Own devices (`user_id = auth.uid()`) | Via user signup flow | Own devices | Own devices |
| `device_profiles` | All authenticated | Admin only | Admin only | Admin only |
| `telemetry_live` | Own devices (via device_key→user_id) | RPC only | — | — |
| `telemetry_archive` | Own devices (via device_key→user_id) | — | — | — |
| `relay_states` | Own devices (via device_key→user_id) | Own devices | Own devices | Own devices |