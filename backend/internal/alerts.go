// internal/alerts.go — Alert rule evaluation. Runs in the API server,
// triggered by each message on the live/# MQTT stream. Evaluates rules
// against the enriched payload and fires/resolves events.

package internal

import (
	"context"
	"fmt"
	"log/slog"
	"sync"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

// AlertEngine evaluates alert rules against live telemetry. It is
// constructed once in the API server and shared across all live/#
// subscribers; the RWMutex guards the cached rules and the per-device
// consecutive-match counters.
type AlertEngine struct {
	pg          *pgxpool.Pool
	email       *EmailService
	mu          sync.RWMutex
	counters    map[string]int // "rule_id:device_key" → consecutive match count
	rules       []alertRule    // cached rules, refreshed every 30s
	lastRefresh time.Time
}

// NewAlertEngine returns an engine with empty rule/counters maps. Rules
// are loaded lazily on the first Evaluate call.
func NewAlertEngine(pg *pgxpool.Pool, email *EmailService) *AlertEngine {
	return &AlertEngine{
		pg:       pg,
		email:    email,
		counters: make(map[string]int),
	}
}

// Evaluate checks all active rules against the enriched telemetry.
// Called by the live/# handler for each incoming message.
//
// Hysteresis model: a rule must match for `durationSec`'s worth of
// consecutive samples before it fires, and any single non-match clears
// the counter (and resolves an existing firing event). This prevents
// flapping from transient sensor spikes.
func (e *AlertEngine) Evaluate(ctx context.Context, deviceKey string, enriched *EnrichedTelemetry) {
	rules := e.getRules(ctx)

	for _, rule := range rules {
		// Skip rules scoped to a different device. A rule with both
		// DeviceKey and DeviceType empty matches every device.
		if !e.matchesDevice(rule, deviceKey, enriched.DeviceType) {
			continue
		}
		rawValue := e.getFieldValue(enriched, rule.Field)
		matched := e.evaluateCondition(rawValue, rule.Operator, rule.Value)

		// The counter key is rule+device so the same rule on two devices
		// is tracked independently.
		key := rule.ID + ":" + deviceKey
		e.mu.Lock()
		if matched {
			e.counters[key]++
		} else {
			// Any non-match immediately resets the streak, so a rule that
			// was about to fire must start over. This is intentional — we
			// prefer false negatives over false alarms from brief dips.
			delete(e.counters, key)
		}
		count := e.counters[key]
		e.mu.Unlock()

		// Fire only when the streak crosses the duration threshold; resolve
		// on the first non-match. The fire/resolve helpers are idempotent
		// (INSERT ... ON CONFLICT DO NOTHING / UPDATE ... WHERE status),
		// so redundant calls are cheap.
		if matched && count >= requiredSamples(rule.DurationSec) {
			e.fire(ctx, rule, deviceKey, rawValue)
		} else if !matched {
			e.resolve(ctx, rule, deviceKey, rawValue)
		}
	}
}

// getRules returns cached rules, refreshing from DB every 30 seconds.
//
// The 30s TTL bounds DB load: with many live subscribers all calling
// Evaluate per message, querying alert_rules on every frame would be
// prohibitive. A failed reload falls back to the stale cache (if any)
// rather than returning no rules, so a transient DB hiccup doesn't
// silently disable all alerting.
func (e *AlertEngine) getRules(ctx context.Context) []alertRule {
	e.mu.RLock()
	rules := e.rules
	lastRefresh := e.lastRefresh
	e.mu.RUnlock()

	if rules != nil && time.Since(lastRefresh) < 30*time.Second {
		return rules
	}

	loaded, err := e.loadRules(ctx)
	if err != nil {
		slog.Error("load rules", "error", err)
		if rules != nil {
			// Serve stale cache rather than empty rules on a transient error.
			return rules
		}
		return nil
	}

	e.mu.Lock()
	e.rules = loaded
	e.lastRefresh = time.Now()
	e.mu.Unlock()
	return loaded
}

// alertRule mirrors a row in alert_rules. Only enabled rules are loaded,
// so the Enabled field is effectively always true in-memory but kept for
// completeness and future use.
type alertRule struct {
	ID          string
	Name        string
	DeviceType  string
	DeviceKey   string
	Enabled     bool
	Field       string
	Operator    string
	Value       float64
	DurationSec int
	NotifyEmail bool
}

// loadRules reads all enabled alert rules from Postgres. Coalesce wraps
// the nullable device_type/device_key columns so we scan into plain
// strings without a sql.NullString dance.
func (e *AlertEngine) loadRules(ctx context.Context) ([]alertRule, error) {
	rows, err := e.pg.Query(ctx, `
		SELECT id, name, coalesce(device_type,''), coalesce(device_key,''),
		       enabled, field, operator, value, duration_sec, notify_email
		FROM alert_rules WHERE enabled = true`)
	if err != nil {
		return nil, fmt.Errorf("query rules: %w", err)
	}
	defer rows.Close()

	var rules []alertRule
	for rows.Next() {
		var r alertRule
		if err := rows.Scan(&r.ID, &r.Name, &r.DeviceType, &r.DeviceKey,
			&r.Enabled, &r.Field, &r.Operator, &r.Value, &r.DurationSec, &r.NotifyEmail); err != nil {
			return nil, fmt.Errorf("scan rule: %w", err)
		}
		rules = append(rules, r)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("load rules: %w", err)
	}
	return rules, nil
}

// matchesDevice reports whether a rule applies to the given device. An
// empty DeviceKey matches any device; a non-empty one must equal deviceKey.
// DeviceType follows the same convention. Both empty → matches all.
func (e *AlertEngine) matchesDevice(rule alertRule, deviceKey, deviceType string) bool {
	if rule.DeviceKey != "" && rule.DeviceKey != deviceKey {
		return false
	}
	if rule.DeviceType != "" && rule.DeviceType != deviceType {
		return false
	}
	return true
}

// getFieldValue extracts the metric named by rule.Field from the enriched
// payload. The well-known aggregate fields are switched first; anything
// else is looked up in the raw per-channel Fields map, so rules can
// reference raw firmware keys (e.g. "ch0_P") without special handling.
func (e *AlertEngine) getFieldValue(enriched *EnrichedTelemetry, field string) float64 {
	switch field {
	case "pv_power":
		return float64(enriched.PVPower)
	case "battery_power":
		return float64(enriched.BatteryPower)
	case "inverter_power":
		return float64(enriched.InverterPower)
	case "dc_load_power":
		return float64(enriched.DCLoadPower)
	case "min_soc_pct":
		return float64(enriched.MinSOCPct)
	case "max_soc_pct":
		return float64(enriched.MaxSOCPct)
	default:
		if v, ok := enriched.Fields[field]; ok {
			return v
		}
		return 0
	}
}

// evaluateCondition applies the rule's operator to (value, threshold).
// An unknown operator returns false rather than panicking so a corrupt
// rule row never takes down the live subscriber.
func (e *AlertEngine) evaluateCondition(value float64, op string, threshold float64) bool {
	switch op {
	case "gt":
		return value > threshold
	case "lt":
		return value < threshold
	case "gte":
		return value >= threshold
	case "lte":
		return value <= threshold
	case "eq":
		return value == threshold
	case "neq":
		return value != threshold
	default:
		return false
	}
}

// requiredSamples converts a rule's duration (in seconds) into the number
// of consecutive matching samples required before firing. The +4 / 5
// rounding matches the firmware's 5-second publish interval: each live/#
// message represents ~5s of data, so durationSec / 5 (rounded up) gives
// the sample count. A non-positive duration fires on the first match.
func requiredSamples(durationSec int) int {
	if durationSec <= 0 {
		return 1
	}
	return (durationSec + 4) / 5
}

// resolveOwnerEmail looks up the owner's email and user ID for a device.
// Returns "" for both on any error (including no owner); callers treat
// empty email as "skip notification" rather than failing the alert.
func (e *AlertEngine) resolveOwnerEmail(ctx context.Context, deviceKey string) (string, string) {
	var email, userID string
	err := e.pg.QueryRow(ctx,
		`SELECT u.email, u.id::text FROM users u
		 JOIN devices d ON d.owner_id = u.id
		 WHERE d.device_key = $1 AND d.owner_id IS NOT NULL`,
		deviceKey).Scan(&email, &userID)
	if err != nil {
		return "", ""
	}
	return email, userID
}

// shouldNotify reports whether the owner has opted into the given
// notification channel (e.g. "alert_fired_email"). A missing user_id or a
// missing preference row default to true so we err on the side of
// notifying; an explicit false preference disables the channel.
func (e *AlertEngine) shouldNotify(ctx context.Context, userID, prefField string) bool {
	if userID == "" {
		return true
	}
	var enabled bool
	err := e.pg.QueryRow(ctx,
		`SELECT `+prefField+` FROM notification_preferences WHERE user_id = $1`, userID).Scan(&enabled)
	if err != nil {
		return true
	}
	return enabled
}

// fire records a firing event for the rule/device and, if the rule has
// email notifications enabled, enqueues one. Idempotency and concurrency
// safety rely on the partial unique index documented inline below.
func (e *AlertEngine) fire(ctx context.Context, rule alertRule, deviceKey string, value float64) {
	// INSERT ... ON CONFLICT DO NOTHING against the partial unique index
	// uq_alert_events_firing (rule_id, device_key WHERE status='firing') makes
	// the check-and-insert atomic, so two concurrent live/# messages cannot
	// each insert a firing row for the same rule/device.
	tag, err := e.pg.Exec(ctx,
		`INSERT INTO alert_events (rule_id, device_key, status, fired_value)
		 VALUES ($1, $2, 'firing', $3)
		 ON CONFLICT (rule_id, device_key) WHERE status = 'firing' DO NOTHING`,
		rule.ID, deviceKey, value)
	if err != nil {
		slog.Warn("insert alert event", "rule", rule.Name, "error", err)
		return
	}
	if tag.RowsAffected() == 0 {
		// A firing event already exists for this rule/device — nothing to do.
		return
	}

	if rule.NotifyEmail {
		email, userID := e.resolveOwnerEmail(ctx, deviceKey)
		if email != "" && e.shouldNotify(ctx, userID, "alert_fired_email") {
			e.email.Enqueue(ctx, "alert_fired", email, userID, map[string]any{
				"RuleName":  rule.Name,
				"DeviceKey": deviceKey,
				"Value":     value,
				"Threshold": rule.Value,
			})
		}
	}
	slog.Warn("alert fired", "rule", rule.Name, "device", deviceKey, "value", value)
}

// resolve marks an existing firing event as resolved. The UPDATE only
// touches rows with status='firing', so resolving a rule that was never
// firing (or already resolved) affects zero rows and is a no-op.
// RowsAffected()==0 short-circuits the email so we don't notify on every
// non-matching sample after a rule has already resolved.
func (e *AlertEngine) resolve(ctx context.Context, rule alertRule, deviceKey string, value float64) {
	tag, err := e.pg.Exec(ctx,
		`UPDATE alert_events SET status = 'resolved', resolved_at = now(), resolved_value = $3
		 WHERE rule_id = $1 AND device_key = $2 AND status = 'firing'`,
		rule.ID, deviceKey, value)
	if err != nil || tag.RowsAffected() == 0 {
		return
	}
	if rule.NotifyEmail {
		email, userID := e.resolveOwnerEmail(ctx, deviceKey)
		if email != "" && e.shouldNotify(ctx, userID, "alert_resolved_email") {
			e.email.Enqueue(ctx, "alert_resolved", email, userID, map[string]any{
				"RuleName":  rule.Name,
				"DeviceKey": deviceKey,
				"Value":     value,
			})
		}
	}
	slog.Info("alert resolved", "rule", rule.Name, "device", deviceKey)
}
