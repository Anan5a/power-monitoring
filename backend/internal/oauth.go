// internal/oauth.go — OAuth login with Google, GitHub, and OIDC SSO.
// Handles the redirect → callback → JWT flow.

package internal

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5/pgxpool"
	"golang.org/x/oauth2"
	"golang.org/x/oauth2/github"
	"golang.org/x/oauth2/google"
)

type OAuthHandler struct {
	pg      *pgxpool.Pool
	jwt     *JWTManager
	configs map[string]*oauth2.Config
	states  sync.Map
}

func NewOAuthHandler(pg *pgxpool.Pool, jwt *JWTManager, googleID, googleSecret, githubID, githubSecret, baseURL string) *OAuthHandler {
	h := &OAuthHandler{
		pg:      pg,
		jwt:     jwt,
		configs: make(map[string]*oauth2.Config),
	}
	h.configs["google"] = &oauth2.Config{
		ClientID:     googleID,
		ClientSecret: googleSecret,
		RedirectURL:  baseURL + "/api/v1/auth/oauth/google/callback",
		Scopes:       []string{"openid", "profile", "email"},
		Endpoint:     google.Endpoint,
	}
	h.configs["github"] = &oauth2.Config{
		ClientID:     githubID,
		ClientSecret: githubSecret,
		RedirectURL:  baseURL + "/api/v1/auth/oauth/github/callback",
		Scopes:       []string{"user:email"},
		Endpoint:     github.Endpoint,
	}
	return h
}

// Redirect initiates OAuth login flow
// @Summary      Initiate OAuth login
// @Tags         Auth
// @Produce      json
// @Param        provider  path  string  true  "Provider (google, github)"
// @Success      307  "Redirect to OAuth provider"
// @Failure      404  {object}  APIError
// @Router       /auth/oauth/{provider} [get]
func (h *OAuthHandler) Redirect(w http.ResponseWriter, r *http.Request) {
	provider := chi.URLParam(r, "provider")
	config, ok := h.configs[provider]
	if !ok {
		writeError(w, "not_found", "unsupported provider", http.StatusNotFound)
		return
	}
	state := generateState()
	h.states.Store(state, time.Now().Add(10*time.Minute))
	url := config.AuthCodeURL(state, oauth2.AccessTypeOffline)
	http.Redirect(w, r, url, http.StatusTemporaryRedirect)
}

// Callback handles the OAuth provider callback
// @Summary      OAuth callback
// @Tags         Auth
// @Produce      json
// @Param        provider  path  string  true  "Provider (google, github)"
// @Param        code      query string  true  "Authorization code"
// @Param        state     query string  true  "CSRF state token"
// @Success      200  {object}  AuthResponse
// @Failure      401  {object}  APIError
// @Router       /auth/oauth/{provider}/callback [get]
func (h *OAuthHandler) Callback(w http.ResponseWriter, r *http.Request) {
	provider := chi.URLParam(r, "provider")
	config, ok := h.configs[provider]
	if !ok {
		writeError(w, "not_found", "unsupported provider", http.StatusNotFound)
		return
	}

	state := r.URL.Query().Get("state")
	expiry, ok := h.states.Load(state)
	if !ok {
		writeError(w, "unauthorized", "invalid state parameter", http.StatusUnauthorized)
		return
	}
	h.states.Delete(state)
	if expiry.(time.Time).Before(time.Now()) {
		writeError(w, "unauthorized", "state expired", http.StatusUnauthorized)
		return
	}

	code := r.URL.Query().Get("code")
	token, err := config.Exchange(r.Context(), code)
	if err != nil {
		writeError(w, "unauthorized", "oauth exchange failed", http.StatusUnauthorized)
		return
	}

	client := config.Client(r.Context(), token)
	userInfoURL := "https://www.googleapis.com/oauth2/v2/userinfo"
	if provider == "github" {
		userInfoURL = "https://api.github.com/user"
	}
	resp, err := client.Get(userInfoURL)
	if err != nil {
		writeError(w, "internal_error", "failed to fetch user info", http.StatusInternalServerError)
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		writeError(w, "internal_error", "provider returned non-200 status", http.StatusInternalServerError)
		return
	}

	var info struct {
		ID      string `json:"id"`
		Email   string `json:"email"`
		Name    string `json:"name"`
		Login   string `json:"login"`
		Picture string `json:"picture"`
		Avatar  string `json:"avatar_url"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&info); err != nil {
		writeError(w, "internal_error", "failed to parse user info", http.StatusInternalServerError)
		return
	}
	if info.ID == "" {
		writeError(w, "unauthorized", "provider did not return user id", http.StatusUnauthorized)
		return
	}
	if info.Name == "" {
		info.Name = info.Login
	}
	if info.Name == "" {
		info.Name = "OAuth User"
	}
	if info.Picture == "" {
		info.Picture = info.Avatar
	}

	userID, err := h.findOrCreateOAuthUser(r.Context(), provider, info.ID, info.Email, info.Name, info.Picture)
	if err != nil {
		slog.Error("oauth user lookup/creation", "error", err)
		writeError(w, "internal_error", "failed to create or link account", http.StatusInternalServerError)
		return
	}

	access, _ := h.jwt.IssueAccessToken(userID, "user")
	refresh, _ := h.jwt.IssueRefreshToken(userID)
	writeJSON(w, http.StatusOK, map[string]string{
		"access_token":  access,
		"refresh_token": refresh,
		"redirect":      "/dashboard",
	})
}

// findOrCreateOAuthUser returns the local user ID for the OAuth account,
// creating a new user and linking the account if necessary.
func (h *OAuthHandler) findOrCreateOAuthUser(ctx context.Context, provider, providerID, email, displayName, avatarURL string) (string, error) {
	var userID string
	err := h.pg.QueryRow(ctx,
		`SELECT user_id FROM user_oauth_accounts WHERE provider = $1 AND provider_id = $2`,
		provider, providerID).Scan(&userID)
	if err == nil {
		return userID, nil
	}

	// No linked OAuth account. Try to find an existing user by email.
	if email != "" {
		err = h.pg.QueryRow(ctx,
			`SELECT id FROM users WHERE email = $1`,
			email).Scan(&userID)
		if err == nil {
			_, err = h.pg.Exec(ctx,
				`INSERT INTO user_oauth_accounts (user_id, provider, provider_id, email, display_name, avatar_url)
				 VALUES ($1, $2, $3, $4, $5, $6)
				 ON CONFLICT (provider, provider_id) DO NOTHING`,
				userID, provider, providerID, email, displayName, avatarURL)
			if err != nil {
				return "", fmt.Errorf("link oauth account: %w", err)
			}
			return userID, nil
		}
	}

	// Create a new user.
	err = h.pg.QueryRow(ctx,
		`INSERT INTO users (email, password_hash, display_name)
		 VALUES ($1, '', $2) RETURNING id`,
		email, displayName).Scan(&userID)
	if err != nil {
		return "", fmt.Errorf("create user: %w", err)
	}
	_, err = h.pg.Exec(ctx,
		`INSERT INTO user_oauth_accounts (user_id, provider, provider_id, email, display_name, avatar_url)
		 VALUES ($1, $2, $3, $4, $5, $6)`,
		userID, provider, providerID, email, displayName, avatarURL)
	if err != nil {
		return "", fmt.Errorf("insert oauth account: %w", err)
	}
	return userID, nil
}

func generateState() string {
	b := make([]byte, 16)
	rand.Read(b)
	return hex.EncodeToString(b)
}
