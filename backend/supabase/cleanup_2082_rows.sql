-- Cleanup 2082 garbled rows from telemetry_computed and telemetry_live
-- Root cause already fixed: firmware timestamp formula (connectivity_manager.cpp:889)
-- and insert_telemetry validation (schema.sql).
-- These rows are not recoverable — timestamps are completely wrong.

select 'telemetry_computed with 2082 dates: ' || count(*) as count_before
from telemetry_computed where extract(year from recorded_at) = 2082;

delete from telemetry_computed
where extract(year from recorded_at) = 2082;

select 'telemetry_live with 2082 dates: ' || count(*) as count_before
from telemetry_live where extract(year from recorded_at) = 2082;

delete from telemetry_live
where extract(year from recorded_at) = 2082;