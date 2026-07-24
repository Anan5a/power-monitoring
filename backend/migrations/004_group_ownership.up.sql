-- 004: Per-user device group ownership.
-- device_groups was global; any user could see/modify every group. Add an
-- owner_id column so groups are scoped to the user who created them. Existing
-- rows are left NULL (treated as unowned/legacy) and only visible to admins.

ALTER TABLE device_groups
    ADD COLUMN IF NOT EXISTS owner_id UUID REFERENCES users(id) ON DELETE SET NULL;

CREATE INDEX IF NOT EXISTS idx_device_groups_owner ON device_groups(owner_id);