// internal/middleware.go — HTTP middleware for the API server.
// Auth middleware validates JWT tokens. Logger middleware adds request
// context. CORS middleware allows the web UI to connect.

package internal

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"github.com/go-chi/cors"
	"github.com/jackc/pgx/v5/pgxpool"
)

// contextKey is an unexported string type used for context.WithValue keys
// to avoid collisions with keys defined in other packages (per the context
// package's documentation, callers should not use built-in string types).
type contextKey string

const (
	// ContextUserID carries the authenticated user's ID through the request
	// lifecycle after AuthMiddleware has validated a JWT.
	ContextUserID contextKey = "user_id"
	// ContextUserRole carries the authenticated user's role ("admin" or "user")
	// so downstream handlers (e.g. AdminOnly) can authorize without re-parsing
	// the token.
	ContextUserRole contextKey = "user_role"
)

// AuthMiddleware validates the Bearer token and injects user info into context.
func AuthMiddleware(jwt *JWTManager) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			auth := r.Header.Get("Authorization")
			if !strings.HasPrefix(auth, "Bearer ") {
				writeError(w, "unauthorized", "missing or malformed authorization header", http.StatusUnauthorized)
				return
			}
			token := strings.TrimPrefix(auth, "Bearer ")
			claims, err := jwt.ValidateToken(token)
			if err != nil {
				writeError(w, "unauthorized", "invalid or expired token", http.StatusUnauthorized)
				return
			}
			if claims.UserID == "" {
				// A token with an empty subject is structurally valid but
				// meaningless; reject it rather than letting handlers treat a
				// missing identity as "authenticated as nobody".
				writeError(w, "unauthorized", "invalid token claims", http.StatusUnauthorized)
				return
			}
			ctx := context.WithValue(r.Context(), ContextUserID, claims.UserID)
			ctx = context.WithValue(ctx, ContextUserRole, claims.Role)
			next.ServeHTTP(w, r.WithContext(ctx))
		})
	}
}

// ContextDeviceKey holds the authenticated device_key set by DeviceAuthMiddleware.
const ContextDeviceKey contextKey = "device_key"

// DeviceAuthMiddleware authenticates firmware requests using device_key and
// api_key (the same credentials the device uses for MQTT), accepted via
// X-Device-Key/X-Api-Key headers or HTTP Basic auth. When device credentials
// are present they are validated and the device_key is placed in the request
// context; when absent the request passes through so a user Bearer token
// (handled by the route handler) may be used instead. Either way, the handler
// must still authorize the caller.
func DeviceAuthMiddleware(pg *pgxpool.Pool) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			deviceKey, apiKey := r.Header.Get("X-Device-Key"), r.Header.Get("X-Api-Key")
			if deviceKey == "" || apiKey == "" {
				// Fall back to HTTP Basic auth so firmware using libcurl-style
				// clients can send credentials without custom headers.
				if u, p, ok := r.BasicAuth(); ok {
					deviceKey, apiKey = u, p
				}
			}
			if deviceKey == "" || apiKey == "" {
				// No device credentials presented: pass through so a user Bearer
				// token (validated by the route handler) can still be used. This
				// middleware only enforces device auth when device creds are
				// present; it never authenticates by itself alone.
				next.ServeHTTP(w, r)
				return
			}
			if pg == nil {
				writeError(w, "internal_error", "database unavailable", http.StatusInternalServerError)
				return
			}
			var storedKey string
			err := pg.QueryRow(r.Context(),
				`SELECT api_key::text FROM devices WHERE device_key = $1 AND is_active = true`,
				deviceKey).Scan(&storedKey)
			// Compare in constant time so a request for a missing device key
			// (storedKey == "") takes the same path as a wrong password, and the
			// comparison is not short-circuited by the equality operator.
			if err != nil || subtle.ConstantTimeCompare([]byte(storedKey), []byte(apiKey)) != 1 {
				writeError(w, "unauthorized", "invalid device credentials", http.StatusUnauthorized)
				return
			}
			ctx := context.WithValue(r.Context(), ContextDeviceKey, deviceKey)
			next.ServeHTTP(w, r.WithContext(ctx))
		})
	}
}

// AdminOnly rejects requests from non-admin users. Must run after AuthMiddleware.
func AdminOnly(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		role, _ := r.Context().Value(ContextUserRole).(string)
		if role != "admin" {
			writeError(w, "forbidden", "admin privileges required", http.StatusForbidden)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// maxBodyBytes caps request body size to protect JSON decoders from
// memory-exhaustion DoS. Applied globally; handlers that accept larger
// uploads (e.g. firmware binaries) should bypass it.
const maxBodyBytes = 1 << 20 // 1 MiB

// MaxBodySize wraps r.Body with http.MaxBytesReader so any single request body
// larger than maxBodyBytes is rejected. Defense against unbounded JSON bodies.
func MaxBodySize(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		r.Body = http.MaxBytesReader(w, r.Body, maxBodyBytes)
		next.ServeHTTP(w, r)
	})
}

// LoggerMiddleware logs each request with method, path, status, and duration.
func LoggerMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		wrapped := &responseWriter{ResponseWriter: w, statusCode: 200}
		next.ServeHTTP(wrapped, r)
		slog.Info("request",
			"method", r.Method,
			"path", r.URL.Path,
			"status", wrapped.statusCode,
			"duration", time.Since(start).String(),
		)
	})
}

// CORSMiddleware allows the web UI origin.
func CORSMiddleware(allowedOrigins []string) func(http.Handler) http.Handler {
	return cors.Handler(cors.Options{
		AllowedOrigins:   allowedOrigins,
		AllowedMethods:   []string{"GET", "POST", "PATCH", "DELETE", "OPTIONS"},
		AllowedHeaders:   []string{"Authorization", "Content-Type", "X-Request-Id"},
		AllowCredentials: true,
		MaxAge:           300,
	})
}

// responseWriter wraps http.ResponseWriter to capture the status code for
// request logging. The embedded ResponseWriter keeps the original methods
// (Header, Write) so only WriteHeader is intercepted.
type responseWriter struct {
	http.ResponseWriter
	statusCode int
}

// WriteHeader records the status code then delegates to the wrapped writer.
func (rw *responseWriter) WriteHeader(code int) {
	rw.statusCode = code
	rw.ResponseWriter.WriteHeader(code)
}

// writeError sends a standard error response.
func writeError(w http.ResponseWriter, code, message string, status int) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(APIError{
		Error: APIErrorDetail{
			Code:    code,
			Message: message,
		},
	})
}

// writeJSON sends a standard success response.
func writeJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(data)
}
