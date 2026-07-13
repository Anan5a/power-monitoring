// internal/oauth.go — OAuth login with Google, GitHub, and OIDC SSO.
// Handles the redirect → callback → JWT flow.

package internal

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
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

	var info struct {
		ID      string `json:"id"`
		Email   string `json:"email"`
		Name    string `json:"name"`
		Login   string `json:"login"`
		Picture string `json:"picture"`
		Avatar  string `json:"avatar_url"`
	}
	json.NewDecoder(resp.Body).Decode(&info)
	if info.Name == "" {
		info.Name = info.Login
	}
	if info.Picture == "" {
		info.Picture = info.Avatar
	}

	var userID string
	err = h.pg.QueryRow(r.Context(),
		`SELECT user_id FROM user_oauth_accounts WHERE provider = $1 AND provider_id = $2`,
		provider, info.ID).Scan(&userID)
	if err != nil {
		h.pg.QueryRow(r.Context(),
			`INSERT INTO users (email, password_hash, display_name)
			 VALUES ($1, '', $2) RETURNING id`,
			info.Email, info.Name).Scan(&userID)
		h.pg.Exec(r.Context(),
			`INSERT INTO user_oauth_accounts (user_id, provider, provider_id, email, display_name, avatar_url)
			 VALUES ($1, $2, $3, $4, $5, $6)`,
			userID, provider, info.ID, info.Email, info.Name, info.Picture)
	}

	access, _ := h.jwt.IssueAccessToken(userID, "user")
	refresh, _ := h.jwt.IssueRefreshToken(userID)
	writeJSON(w, http.StatusOK, map[string]string{
		"access_token":  access,
		"refresh_token": refresh,
		"redirect":      "/dashboard",
	})
}

func generateState() string {
	b := make([]byte, 16)
	rand.Read(b)
	return hex.EncodeToString(b)
}
