-- 006: Prevent duplicate "firing" alert events.
-- Two concurrent live/# messages for the same rule/device could both pass the
-- "no existing firing row" check and INSERT, producing duplicate firing events.
-- A partial unique index lets us INSERT ... ON CONFLICT DO NOTHING atomically.

-- Clean up any duplicates that already exist before adding the constraint.
DELETE FROM alert_events a USING alert_events b
WHERE a.id > b.id
  AND a.rule_id = b.rule_id
  AND a.device_key = b.device_key
  AND a.status = 'firing' AND b.status = 'firing';

CREATE UNIQUE INDEX IF NOT EXISTS uq_alert_events_firing
    ON alert_events (rule_id, device_key)
    WHERE status = 'firing';