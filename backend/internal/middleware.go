// internal/middleware.go — HTTP middleware for the API server.
// Auth middleware validates JWT tokens. Logger middleware adds request
// context. CORS middleware allows the web UI to connect.

package internal

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"github.com/go-chi/cors"
)

type contextKey string

const (
	ContextUserID  contextKey = "user_id"
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
			ctx := context.WithValue(r.Context(), ContextUserID, claims.UserID)
			ctx = context.WithValue(ctx, ContextUserRole, claims.Role)
			next.ServeHTTP(w, r.WithContext(ctx))
		})
	}
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

type responseWriter struct {
	http.ResponseWriter
	statusCode int
}

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
	json.NewEncoder(w).Encode(APIResponse{Data: data})
}
