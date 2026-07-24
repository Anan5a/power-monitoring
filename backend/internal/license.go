// internal/license.go — License plan enforcement.
// Tracks per-user device counts and enforces plan limits when devices are claimed.

package internal

import (
	"context"
	"fmt"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// LicenseChecker enforces per-user plan limits on device ownership. It is the
// single authority for whether a user may claim another device.
type LicenseChecker struct {
	pg *pgxpool.Pool
}

// freePlanName is the plan assigned to users with no explicit license.
const freePlanName = "free"

// NewLicenseChecker constructs a LicenseChecker backed by the given pool.
func NewLicenseChecker(pg *pgxpool.Pool) *LicenseChecker {
	return &LicenseChecker{pg: pg}
}

// CanAddDevice returns true if the user has not reached their plan's max_devices limit.
// A user with no license row is treated as being on the implicit free plan
// (allowed one device); the returned *LicensePlan is nil in that case.
func (lc *LicenseChecker) CanAddDevice(ctx context.Context, userID string) (bool, *LicensePlan, error) {
	if lc.pg == nil {
		return false, nil, fmt.Errorf("database unavailable")
	}
	var plan LicensePlan
	var deviceCount int
	err := lc.pg.QueryRow(ctx, `
		SELECT lp.id, lp.name, lp.audience, lp.max_devices, lp.retention_days, lp.features, lp.price_monthly,
		       COALESCE(ul.device_count, 0)
		FROM user_licenses ul
		JOIN license_plans lp ON lp.id = ul.plan_id
		WHERE ul.user_id = $1`, userID).Scan(
		&plan.ID, &plan.Name, &plan.Audience, &plan.MaxDevices, &plan.RetentionDays, &plan.Features, &plan.PriceMonthly,
		&deviceCount)
	if err == pgx.ErrNoRows {
		// No license row: treat as free plan with 1 device. The count is 0 by
		// definition (no row), so the user is always allowed here.
		return deviceCount == 0, nil, nil
	}
	if err != nil {
		return false, nil, fmt.Errorf("lookup license: %w", err)
	}
	return deviceCount < plan.MaxDevices, &plan, nil
}

// IncrementDeviceCount increases the user's device_count by one. It upserts a
// user_licenses row, creating the free-plan default for first-time users.
func (lc *LicenseChecker) IncrementDeviceCount(ctx context.Context, userID string) error {
	if lc.pg == nil {
		return fmt.Errorf("database unavailable")
	}
	_, err := lc.pg.Exec(ctx, `
		INSERT INTO user_licenses (user_id, plan_id, device_count)
		VALUES ($1, (SELECT id FROM license_plans WHERE name = $2), 1)
		ON CONFLICT (user_id) DO UPDATE SET device_count = user_licenses.device_count + 1, updated_at = now()`,
		userID, freePlanName)
	if err != nil {
		return fmt.Errorf("increment device count: %w", err)
	}
	return nil
}

// ClaimDevice atomically checks the license cap and increments the device
// count under a row lock on user_licenses, preventing concurrent claims from
// exceeding the plan limit. Returns ErrLicenseCapReached when the user is at
// their plan's max_devices (with the offending plan), or ErrDeviceAlreadyClaimed
// when the device was already claimed by someone else between the check and the
// update. deviceAPIKey authenticates the claim against devices.api_key.
func (lc *LicenseChecker) ClaimDevice(ctx context.Context, userID, deviceKey, deviceAPIKey string) (*LicensePlan, error) {
	if lc.pg == nil {
		return nil, fmt.Errorf("database unavailable")
	}
	tx, err := lc.pg.Begin(ctx)
	if err != nil {
		return nil, fmt.Errorf("begin tx: %w", err)
	}
	defer tx.Rollback(ctx)

	// Lock the user's license row (or create the free-plan default) so concurrent
	// claims serialize on it.
	var plan LicensePlan
	var deviceCount int
	err = tx.QueryRow(ctx, `
		INSERT INTO user_licenses (user_id, plan_id, device_count)
		VALUES ($1, (SELECT id FROM license_plans WHERE name = $2), 0)
		ON CONFLICT (user_id) DO UPDATE SET updated_at = user_licenses.updated_at
		RETURNING (SELECT lp.id FROM license_plans lp WHERE lp.id = user_licenses.plan_id),
		          (SELECT lp.name FROM license_plans lp WHERE lp.id = user_licenses.plan_id),
		          (SELECT lp.audience FROM license_plans lp WHERE lp.id = user_licenses.plan_id),
		          (SELECT lp.max_devices FROM license_plans lp WHERE lp.id = user_licenses.plan_id),
		          (SELECT lp.retention_days FROM license_plans lp WHERE lp.id = user_licenses.plan_id),
		          (SELECT lp.features FROM license_plans lp WHERE lp.id = user_licenses.plan_id),
		          (SELECT lp.price_monthly FROM license_plans lp WHERE lp.id = user_licenses.plan_id),
		          device_count`,
		userID, freePlanName).Scan(
		&plan.ID, &plan.Name, &plan.Audience, &plan.MaxDevices, &plan.RetentionDays,
		&plan.Features, &plan.PriceMonthly, &deviceCount)
	if err != nil {
		return nil, fmt.Errorf("lock license: %w", err)
	}

	if deviceCount >= plan.MaxDevices {
		// Return the offending plan so callers can surface which limit was hit.
		return &plan, ErrLicenseCapReached
	}

	tag, err := tx.Exec(ctx,
		`UPDATE devices SET owner_id = $1, device_name = 'My ' || device_type
		 WHERE device_key = $2 AND owner_id IS NULL AND api_key::text = $3`,
		userID, deviceKey, deviceAPIKey)
	if err != nil {
		return nil, fmt.Errorf("claim device: %w", err)
	}
	if tag.RowsAffected() == 0 {
		// Zero rows means the device was either missing, already claimed, or the
		// API key didn't match — all surface as ErrDeviceAlreadyClaimed so we do
		// not leak which one (avoids enumerating devices for an attacker).
		return nil, ErrDeviceAlreadyClaimed
	}

	if _, err := tx.Exec(ctx,
		`UPDATE user_licenses SET device_count = device_count + 1, updated_at = now()
		 WHERE user_id = $1`, userID); err != nil {
		return nil, fmt.Errorf("increment count: %w", err)
	}

	if err := tx.Commit(ctx); err != nil {
		return nil, fmt.Errorf("commit: %w", err)
	}
	return &plan, nil
}

// Sentinel errors for ClaimDevice. Callers test these with errors.Is rather
// than string-matching the returned error.
var (
	// ErrLicenseCapReached is returned when the user's plan max_devices would
	// be exceeded; the accompanying *LicensePlan identifies the plan.
	ErrLicenseCapReached = fmt.Errorf("device limit reached for plan")
	// ErrDeviceAlreadyClaimed is returned when the claim UPDATE matched no row,
	// i.e. the device was unknown, already owned, or the API key was wrong.
	ErrDeviceAlreadyClaimed = fmt.Errorf("device not found or already claimed")
)

// DecrementDeviceCount decreases the user's device_count by one, never below zero.
func (lc *LicenseChecker) DecrementDeviceCount(ctx context.Context, userID string) error {
	if lc.pg == nil {
		return fmt.Errorf("database unavailable")
	}
	_, err := lc.pg.Exec(ctx, `
		UPDATE user_licenses
		SET device_count = GREATEST(user_licenses.device_count - 1, 0),
		    updated_at = now()
		WHERE user_id = $1`, userID)
	if err != nil {
		return fmt.Errorf("decrement device count: %w", err)
	}
	return nil
}

// ListPlans returns all available license plans.
func (lc *LicenseChecker) ListPlans(ctx context.Context) ([]LicensePlan, error) {
	if lc.pg == nil {
		return nil, fmt.Errorf("database unavailable")
	}
	rows, err := lc.pg.Query(ctx, `
		SELECT id, name, audience, max_devices, retention_days, features, price_monthly
		FROM license_plans ORDER BY price_monthly`)
	if err != nil {
		return nil, fmt.Errorf("list plans: %w", err)
	}
	defer rows.Close()

	plans := []LicensePlan{}
	for rows.Next() {
		var p LicensePlan
		if err := rows.Scan(&p.ID, &p.Name, &p.Audience, &p.MaxDevices, &p.RetentionDays, &p.Features, &p.PriceMonthly); err != nil {
			return nil, fmt.Errorf("scan plan: %w", err)
		}
		plans = append(plans, p)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("list plans: %w", err)
	}
	return plans, nil
}
