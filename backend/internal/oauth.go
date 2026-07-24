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

// OAuthHandler drives the OAuth/OIDC login flow (Google and GitHub) and
// exchanges the provider's authorization code for local JWTs. The in-memory
// states map holds short-lived CSRF tokens keyed by the random state sent to
// the provider, so a forged callback cannot mint tokens without first
// obtaining a state issued by this server.
type OAuthHandler struct {
	pg      *pgxpool.Pool
	jwt     *JWTManager
	refresh *RefreshTokenStore
	configs map[string]*oauth2.Config
	states  sync.Map
}

// NewOAuthHandler constructs an OAuthHandler with the supplied provider
// credentials and a base URL used to build the per-provider callback URLs.
func NewOAuthHandler(pg *pgxpool.Pool, jwt *JWTManager, refresh *RefreshTokenStore, googleID, googleSecret, githubID, githubSecret, baseURL string) *OAuthHandler {
	h := &OAuthHandler{
		pg:      pg,
		jwt:     jwt,
		refresh: refresh,
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
	// States live at most 10 minutes so a leaked state value is only useful
	// for a short window; sync.Map keeps this lock-free across concurrent logins.
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
// @Success      200  {object}  map[string]string
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
		// Unknown state: either forged, replayed after deletion, or from a
		// server restart. Refuse rather than risk accepting a crafted callback.
		writeError(w, "unauthorized", "invalid state parameter", http.StatusUnauthorized)
		return
	}
	// Delete before consuming so a parallel replay of the same state fails.
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

	access, err := h.jwt.IssueAccessToken(userID, "user")
	if err != nil {
		writeError(w, "internal_error", "failed to issue access token", http.StatusInternalServerError)
		return
	}
	refresh, err := h.refresh.Issue(r.Context(), userID)
	if err != nil {
		writeError(w, "internal_error", "failed to issue refresh token", http.StatusInternalServerError)
		return
	}
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

	// No linked OAuth account. Try to find an existing user by email — but only
	// link to it if that account has no password set. Linking an OAuth identity
	// to a pre-existing password account would let whoever registered the email
	// first (possibly an attacker, since emails are not verified at registration)
	// take over the OAuth user's data. Accounts created by a prior OAuth login
	// have an empty password_hash and are safe to re-link.
	if email != "" {
		err = h.pg.QueryRow(ctx,
			`SELECT id FROM users WHERE email = $1 AND (password_hash IS NULL OR password_hash = '')`,
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
		// Most likely the email already belongs to a password account. Do not
		// silently merge — surface a clear error instead of a generic 500.
		var exists bool
		if qerr := h.pg.QueryRow(ctx,
			`SELECT EXISTS (SELECT 1 FROM users WHERE email = $1 AND password_hash <> '')`,
			email).Scan(&exists); qerr == nil && exists {
			return "", fmt.Errorf("account exists with password: log in with email/password and link %s from settings", provider)
		}
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
	if _, err := rand.Read(b); err != nil {
		// crypto/rand should not fail in practice; panic rather than emit a
		// predictable (all-zero) CSRF state.
		panic(fmt.Sprintf("crypto/rand: %v", err))
	}
	return hex.EncodeToString(b)
}
