# IoT Dashboard — Supabase Setup Guide

## 1. Create Supabase Project

1. Go to [supabase.com](https://supabase.com) and sign up/login
2. Click **New Project** → give it a name (e.g. `power-monitor`)
3. Select a region closest to you
4. Save the **Database Password** securely
5. Wait for the project to provision (~2 minutes)

## 2. Get Your Credentials

After the project is created, go to **Settings → API**:

- **Project URL**: `https://<your-project-id>.supabase.co`
- **service_role (jwt)**: `eyJhbGc...` — used by ESP32 only
- **anon public**: `eyJhbGc...` — used by React frontend

You will also need the **service_role key** for the ESP32 (not the anon key). Find it in the same API settings page under `service_role` section.

## 3. Run the Schema

1. In Supabase dashboard, go to **SQL Editor** (left sidebar)
2. Click **New Query**
3. Open `backend/supabase/schema.sql` from this project
4. Copy-paste the entire contents
5. Click **Run**

Verify success: you should see the tables created without errors.

## 4. Verify Tables

Go to **Table Editor** in the left sidebar. You should see these tables:
- `profiles`
- `devices`
- `device_profiles`
- `telemetry_live`
- `telemetry_archive`
- `relay_states`

Also verify under **Database → Extensions** that `pg_cron` is enabled.

## 5. Configure Authentication

1. Go to **Authentication → Providers → Email**
2. Enable **Email/Password** if not already enabled
3. Optionally uncheck **Allow new registrations** if you only want invited users

## 6. Deploy the React Frontend

### Option A: Cloudflare Pages (recommended, free)

1. Push this project to a GitHub repository
2. Go to [cloudflare.com/pages](https://pages.cloudflare.com)
3. Select your GitHub repo
4. Set build command: `npm run build`
5. Set output directory: `dist`
6. Add environment variables:
   - `VITE_SUPABASE_URL` = your Supabase project URL
   - `VITE_SUPABASE_ANON_KEY` = your anon public key
7. Deploy

### Option B: Vercel (free)

1. Push to GitHub
2. Go to [vercel.com](https://vercel.com)
3. Import the repo
4. Add environment variables in project settings:
   - `VITE_SUPABASE_URL`
   - `VITE_SUPABASE_ANON_KEY`
5. Deploy

## 7. Provision Your First Device

1. Open the deployed React frontend
2. Create an account via the login page
3. Go to **Admin → Add Device**
4. Copy the generated `device_key`
5. Connect to your ESP32 via BLE using the **Provisioning** page
6. Enter WiFi credentials and Supabase URL + service_role_key + device_key
7. Reboot the ESP32 — it should start posting telemetry

## 8. Verify Data Flow

1. In Supabase dashboard → **Table Editor → telemetry_live** — you should see rows appearing every 5 minutes
2. In the React dashboard, select your device — charts should populate within seconds of each POST
3. The **live indicator** on the device card should turn green

## Troubleshooting

**No data appearing:**
- Check ESP32 serial output for HTTP response codes
- Verify the service_role key is correct (not the anon key)
- Check **Authentication → Logs** for blocked requests
- Verify RLS isn't blocking — run `select * from telemetry_live limit 10` in SQL Editor (as your user)

**pg_cron not running:**
- Supabase free tier has limitations on pg_cron. The archive function may need to be triggered manually or via a Supabase Pro plan for reliable scheduling.

**BLE provisioning not working:**
- Use Chrome or Edge browser (Web Bluetooth required)
- Ensure Bluetooth is enabled on your laptop/phone
- Move ESP32 close to the device running the browser