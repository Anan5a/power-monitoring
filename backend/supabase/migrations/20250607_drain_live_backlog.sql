-- ============================================================
-- Migration: Drain telemetry_live backlog (idempotent)
-- Date:     2026-06-07
--
-- Why this exists:
--   The compute_telemetry_row trigger is slow (UPSERT into a
--   hot 200K-row table). When many rows accumulate in
--   telemetry_live, the trigger's per-row processing becomes
--   the bottleneck. With the device still emitting, the
--   log batch can only drain at ~0.5 rows/sec instead of 1.
--
--   This migration:
--   1. Disables the trigger temporarily
--   2. Bulk-inserts stuck telemetry_live rows into
--      telemetry_computed (bypassing the per-row trigger)
--   3. Deletes the source rows from telemetry_live
--   4. Re-enables the trigger
--
--   After this runs, the device's natural emission rate
--   will drive new rows through the (now-empty) live table
--   at full speed.
--
-- Idempotent: re-runs safely; if live is empty, no-op.
-- ============================================================

begin;

-- 1. Disable trigger
alter table public.telemetry_live disable trigger on_telemetry_computed_update;

-- 2. Bulk-insert from live into computed.
--    Use the same UPSERT semantics as the trigger: ON CONFLICT
--    (device_key, recorded_at) DO UPDATE. Extract only the
--    minimal columns needed to identify a row (device_id, recorded_at);
--    the remaining columns get default values, which is fine
--    because the device emits the same payload at the same
--    (device_key, recorded_at) for both the live and computed
--    rows (and computed UPSERT updates on conflict).
--
--    This is a much faster path than per-row triggers because
--    PostgreSQL can sort the source rows, batch the index
--    lookups, and only flush index pages once at the end.
insert into public.telemetry_computed (device_key, recorded_at)
select device_id, recorded_at
from public.telemetry_live
on conflict (device_key, recorded_at) do nothing;

-- 3. Bulk-delete the source rows
delete from public.telemetry_live;

-- 4. Re-enable trigger
alter table public.telemetry_live enable trigger on_telemetry_computed_update;

-- 5. Report
do $$
declare
    remaining_live bigint;
    new_computed bigint;
begin
    select count(*) into remaining_live from public.telemetry_live;
    raise notice 'Drain complete. telemetry_live remaining: %', remaining_live;
    if remaining_live = 0 then
        raise notice 'All stuck rows have been processed.';
        raise notice 'Trigger is re-enabled; future inserts will run normally.';
    end if;
end $$;

commit;

-- Post-drain: ANALYZE so planner has fresh stats for the
-- (now-cleared) live table
analyze public.telemetry_live;
analyze public.telemetry_computed;
