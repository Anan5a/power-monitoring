DROP INDEX IF EXISTS idx_device_groups_owner;
ALTER TABLE device_groups DROP COLUMN IF EXISTS owner_id;