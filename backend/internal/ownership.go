// internal/ownership.go — Shared device-ownership helper used by handlers
// that must scope device access to the authenticated user.

package internal

import (
	"context"

	"github.com/jackc/pgx/v5/pgxpool"
)

// IsDeviceOwner returns true if the device exists and is owned by userID.
// A nil pool or nonexistent/unclaimed device returns false. Handlers call this
// before reading or acting on a device so that a user can never operate on a
// device that has not been claimed by (or has been revoked from) them.
func IsDeviceOwner(ctx context.Context, pg *pgxpool.Pool, deviceKey, userID string) bool {
	if pg == nil || deviceKey == "" || userID == "" {
		// Fail closed on missing inputs — never default to "allowed".
		return false
	}
	var ownerID *string
	err := pg.QueryRow(ctx,
		`SELECT owner_id::text FROM devices WHERE device_key = $1`, deviceKey).Scan(&ownerID)
	if err != nil {
		// Device missing or query error: deny rather than leak existence via
		// a timing/error difference.
		return false
	}
	// ownerID is NULL for unclaimed devices; only an exact match authorizes.
	return ownerID != nil && *ownerID == userID
}
