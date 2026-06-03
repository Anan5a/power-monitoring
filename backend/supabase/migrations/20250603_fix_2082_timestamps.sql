-- Fix corrupted 2082 timestamps by shifting them back to 2026
-- Root cause: firmware log buffer replay with uninitialized/corrupt base timestamps

update public.telemetry_computed
set recorded_at = recorded_at - interval '56 years'
where extract(year from recorded_at) = 2082;

update public.telemetry_live
set recorded_at = recorded_at - interval '56 years'
where extract(year from recorded_at) = 2082;
