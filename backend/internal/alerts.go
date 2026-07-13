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

type AlertEngine struct {
	pg          *pgxpool.Pool
	email       *EmailService
	mu          sync.RWMutex
	counters    map[string]int // "rule_id:device_key" → consecutive match count
	rules       []alertRule    // cached rules, refreshed every 30s
	lastRefresh time.Time
}

func NewAlertEngine(pg *pgxpool.Pool, email *EmailService) *AlertEngine {
	return &AlertEngine{
		pg:       pg,
		email:    email,
		counters: make(map[string]int),
	}
}

// Evaluate checks all active rules against the enriched telemetry.
// Called by the live/# handler for each incoming message.
func (e *AlertEngine) Evaluate(ctx context.Context, deviceKey string, enriched *EnrichedTelemetry) {
	rules := e.getRules(ctx)

	for _, rule := range rules {
		if !e.matchesDevice(rule, deviceKey) {
			continue
		}
		rawValue := e.getFieldValue(enriched, rule.Field)
		matched := e.evaluateCondition(rawValue, rule.Operator, rule.Value)

		key := rule.ID + ":" + deviceKey
		e.mu.Lock()
		if matched {
			e.counters[key]++
		} else {
			delete(e.counters, key)
		}
		count := e.counters[key]
		e.mu.Unlock()

		if matched && count >= requiredSamples(rule.DurationSec) {
			e.fire(ctx, rule, deviceKey, rawValue)
		} else if !matched {
			e.resolve(ctx, rule, deviceKey, rawValue)
		}
	}
}

// getRules returns cached rules, refreshing from DB every 30 seconds.
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
		rows.Scan(&r.ID, &r.Name, &r.DeviceType, &r.DeviceKey,
			&r.Enabled, &r.Field, &r.Operator, &r.Value, &r.DurationSec, &r.NotifyEmail)
		rules = append(rules, r)
	}
	return rules, nil
}

func (e *AlertEngine) matchesDevice(rule alertRule, deviceKey string) bool {
	if rule.DeviceKey != "" && rule.DeviceKey != deviceKey {
		return false
	}
	return true
}

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

func requiredSamples(durationSec int) int {
	if durationSec <= 0 {
		return 1
	}
	return (durationSec + 4) / 5
}

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

func (e *AlertEngine) fire(ctx context.Context, rule alertRule, deviceKey string, value float64) {
	var existing string
	err := e.pg.QueryRow(ctx,
		`SELECT id FROM alert_events WHERE rule_id = $1 AND device_key = $2 AND status = 'firing' LIMIT 1`,
		rule.ID, deviceKey).Scan(&existing)
	if err == nil {
		return
	}

	e.pg.Exec(ctx,
		`INSERT INTO alert_events (rule_id, device_key, status, fired_value) VALUES ($1, $2, 'firing', $3)`,
		rule.ID, deviceKey, value)

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
