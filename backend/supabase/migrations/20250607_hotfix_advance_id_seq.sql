-- ============================================================
-- Hotfix: Advance telemetry_computed id sequence past max(id)
-- Date:     2026-06-07
--
-- Why:
--   The previous migration (20250607_add_id_to_telemetry_computed.sql)
--   converted id to GENERATED ALWAYS AS IDENTITY without advancing
--   the sequence. So the next insert tried id=1, colliding with
--   the existing row that already has id=1.
--
--   Trigger now fails with "Key (id)=(N) already exists" for every
--   row insert, causing live rows to accumulate.
--
-- Fix:
--   setval(seq, max(id) + 1, false) — false means next nextval
--   returns exactly max(id) + 1, not max(id) + 1 + 1.
-- ============================================================

do $$
declare
    seq_name text;
    current_seq bigint;
    max_id bigint;
    new_seq bigint;
begin
    seq_name := pg_get_serial_sequence('public.telemetry_computed', 'id');

    if seq_name is null then
        raise notice 'No sequence attached to telemetry_computed.id (id is not an identity column?)';
        raise notice 'Re-run 20250607_add_id_to_telemetry_computed.sql first.';
        return;
    end if;

    select last_value into current_seq from pg_catalog.pg_sequences
    where schemaname = split_part(seq_name, '.', 1)
      and sequencename = split_part(seq_name, '.', 2);

    -- If pg_sequences lookup failed, try direct query
    if current_seq is null then
        execute format('select last_value from %I.%I',
            split_part(seq_name, '.', 1),
            split_part(seq_name, '.', 2))
        into current_seq;
    end if;

    select coalesce(max(id), 0) into max_id from public.telemetry_computed;
    new_seq := max_id + 1;

    raise notice 'Before fix: sequence % at %, max(id) = %', seq_name, current_seq, max_id;

    -- Set sequence to max(id) + 1, is_called=false so next nextval returns exactly new_seq
    perform setval(seq_name, new_seq, false);

    raise notice 'After fix: sequence % set to % (next insert will use id=%)',
        seq_name, new_seq, new_seq;
end $$;
