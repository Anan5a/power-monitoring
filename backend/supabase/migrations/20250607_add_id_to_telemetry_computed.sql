-- ============================================================
-- Migration: Add id bigint primary key to telemetry_computed
-- Date:     2026-06-07
--
-- Why:
--   Production telemetry_computed has a composite primary key
--   on (device_key, recorded_at) and no surrogate id column.
--   This was different from the schema.sql definition which
--   includes an id bigint identity PK.
--
--   Adding a surrogate id PK:
--   1. Makes the trigger's UPSERT path more efficient (the
--      (device_key, recorded_at) index becomes a regular
--      unique index rather than the PK; less contention).
--   2. Allows drift correction migrations to target rows by
--      a single cheap key.
--   3. Matches the original schema intent.
--
-- Strategy:
--   1. Add id bigint as nullable + a sequence for backfill
--   2. Backfill id for all existing rows
--   3. Make id NOT NULL
--   4. Add unique constraint on (device_key, recorded_at)
--      (preserves the ON CONFLICT clause in the trigger)
--   5. Drop the composite PK
--   6. Add PK on id
--   7. Set id as GENERATED ALWAYS AS IDENTITY (so future
--      inserts auto-fill, and explicit inserts are blocked —
--      matches schema.sql intent)
--
-- Safety: All steps are idempotent. Each can be run on a
--         table that already has the change (no-op).
-- ============================================================

---------------------------------------------------------------
-- 1. Add id column + sequence (skip if already exists)
---------------------------------------------------------------
do $$
begin
    if not exists (
        select 1 from information_schema.columns
        where table_schema = 'public'
          and table_name = 'telemetry_computed'
          and column_name = 'id'
    ) then
        -- Add column nullable, with a sequence default
        create sequence if not exists public.telemetry_computed_id_seq;
        alter table public.telemetry_computed
            add column id bigint default nextval('public.telemetry_computed_id_seq');
    end if;
end $$;

---------------------------------------------------------------
-- 2. Backfill any null id values (only matters if step 1
--    was run on a table that already had some rows).
---------------------------------------------------------------
update public.telemetry_computed
set id = nextval('public.telemetry_computed_id_seq')
where id is null;

---------------------------------------------------------------
-- 3. Make id NOT NULL
---------------------------------------------------------------
alter table public.telemetry_computed
    alter column id set not null;

---------------------------------------------------------------
-- 4. Add unique constraint on (device_key, recorded_at).
--    This replaces the role of the composite PK for the
--    trigger's ON CONFLICT clause.
---------------------------------------------------------------
do $$
begin
    if not exists (
        select 1 from pg_constraint
        where conname = 'telemetry_computed_device_recorded_uniq'
    ) then
        alter table public.telemetry_computed
            add constraint telemetry_computed_device_recorded_uniq
            unique (device_key, recorded_at);
    end if;
end $$;

---------------------------------------------------------------
-- 5. Drop the composite PK
---------------------------------------------------------------
alter table public.telemetry_computed
    drop constraint if exists telemetry_computed_pkey;

---------------------------------------------------------------
-- 6. Add PK on id
---------------------------------------------------------------
do $$
begin
    if not exists (
        select 1 from pg_constraint
        where conname = 'telemetry_computed_pkey'
          and contype = 'p'
    ) then
        alter table public.telemetry_computed
            add primary key (id);
    end if;
end $$;

---------------------------------------------------------------
-- 7. Convert id to GENERATED ALWAYS AS IDENTITY.
--    This is the schema.sql-intended form: future inserts
--    auto-fill id, and explicit inserts are blocked.
--    AFTER the conversion, advance the sequence to max(id)+1
--    so future inserts don't collide with existing rows.
---------------------------------------------------------------
do $$
declare
    current_default text;
    seq_name text;
    next_id bigint;
begin
    select pg_get_expr(d.adbin, d.adrelid) into current_default
    from pg_attrdef d
    join pg_attribute a on a.attrelid = d.adrelid and a.attnum = d.adnum
    where a.attrelid = 'public.telemetry_computed'::regclass
      and a.attname = 'id';

    if current_default is null or current_default not like '%generated%' then
        -- Drop the sequence default first
        alter table public.telemetry_computed
            alter column id drop default;

        -- Find the sequence that was attached as the old default,
        -- so we can advance it AFTER the identity conversion.
        -- Identity columns get a new internal sequence; the old
        -- public.telemetry_computed_id_seq becomes orphaned but
        -- we leave it for hygiene.

        -- Convert to identity (creates new internal sequence)
        alter table public.telemetry_computed
            alter column id add generated always as identity;
    end if;

    -- CRITICAL: advance the identity sequence past max(id).
    -- Without this, the next insert will try to use id=1, which
    -- collides with the row that got id=1 during backfill.
    -- pg_get_serial_sequence() returns the sequence attached to
    -- the identity column.
    seq_name := pg_get_serial_sequence('public.telemetry_computed', 'id');
    if seq_name is not null then
        select coalesce(max(id), 0) + 1 into next_id from public.telemetry_computed;
        perform setval(seq_name, next_id, false);
        raise notice 'Advanced sequence % to %', seq_name, next_id;
    end if;
end $$;

---------------------------------------------------------------
-- 8. Verify final state
---------------------------------------------------------------
do $$
declare
    pk_cols text;
    has_unique boolean;
    row_count bigint;
    max_id bigint;
    min_id bigint;
    null_count bigint;
begin
    -- PK columns
    select string_agg(a.attname, ', ' order by array_position(con.conkey, a.attnum))
        into pk_cols
    from pg_constraint con
    join pg_class c on c.oid = con.conrelid
    join pg_attribute a on a.attrelid = c.oid and a.attnum = any(con.conkey)
    where c.relname = 'telemetry_computed' and con.contype = 'p';

    -- Check unique on (device_key, recorded_at)
    select exists (
        select 1 from pg_constraint con
        join pg_class c on c.oid = con.conrelid
        where c.relname = 'telemetry_computed'
          and con.contype = 'u'
          and array_length(con.conkey, 1) = 2
          and con.conkey[1] = (select attnum from pg_attribute
                               where attrelid = c.oid and attname = 'device_key')
          and con.conkey[2] = (select attnum from pg_attribute
                               where attrelid = c.oid and attname = 'recorded_at')
    ) into has_unique;

    select count(*), max(id), min(id), count(*) - count(id)
        into row_count, max_id, min_id, null_count
    from public.telemetry_computed;

    raise notice '=== Migration verification ===';
    raise notice '  rows total: %', row_count;
    raise notice '  id range: % to %', min_id, max_id;
    raise notice '  null ids: %', null_count;
    raise notice '  PK columns: %', pk_cols;
    raise notice '  has unique (device_key, recorded_at): %', has_unique;

    if pk_cols <> 'id' then
        raise exception 'PK is not on id alone: %', pk_cols;
    end if;
    if not has_unique then
        raise exception 'Missing unique constraint on (device_key, recorded_at)';
    end if;
    if null_count > 0 then
        raise exception 'Found % rows with null id', null_count;
    end if;
    raise notice 'Migration complete.';
end $$;

---------------------------------------------------------------
-- 9. ANALYZE for fresh stats
---------------------------------------------------------------
analyze public.telemetry_computed;
