// internal/oauth.go — OAuth login with Google, GitHub, and OIDC SSO.
// Handles the redirect → callback → JWT flow.

package internal

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"net/http"

	"github.com/jackc/pgx/v5/pgxpool"
	"golang.org/x/oauth2"
	"golang.org/x/oauth2/github"
	"golang.org/x/oauth2/google"
)

type OAuthHandler struct {
	pg      *pgxpool.Pool
	jwt     *JWTManager
	configs map[string]*oauth2.Config
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

func (h *OAuthHandler) Redirect(w http.ResponseWriter, r *http.Request) {
	provider := r.PathValue("provider")
	config, ok := h.configs[provider]
	if !ok {
		writeError(w, "not_found", "unsupported provider", http.StatusNotFound)
		return
	}
	state := generateState()
	url := config.AuthCodeURL(state, oauth2.AccessTypeOffline)
	http.Redirect(w, r, url, http.StatusTemporaryRedirect)
}

func (h *OAuthHandler) Callback(w http.ResponseWriter, r *http.Request) {
	provider := r.PathValue("provider")
	config, ok := h.configs[provider]
	if !ok {
		writeError(w, "not_found", "unsupported provider", http.StatusNotFound)
		return
	}

	code := r.URL.Query().Get("code")
	token, err := config.Exchange(r.Context(), code)
	if err != nil {
		writeError(w, "unauthorized", "oauth exchange failed", http.StatusUnauthorized)
		return
	}

	// Fetch user info from provider
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
		Login   string `json:"login"`    // GitHub uses "login" instead of "name"
		Picture string `json:"picture"`
		Avatar  string `json:"avatar_url"` // GitHub uses "avatar_url"
	}
	json.NewDecoder(resp.Body).Decode(&info)
	// GitHub uses "login" for display name and "avatar_url" for picture
	if info.Name == "" {
		info.Name = info.Login
	}
	if info.Picture == "" {
		info.Picture = info.Avatar
	}

	// Find or create user
	var userID string
	err = h.pg.QueryRow(r.Context(),
		`SELECT user_id FROM user_oauth_accounts WHERE provider = $1 AND provider_id = $2`,
		provider, info.ID).Scan(&userID)
	if err != nil {
		// New user — create account
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
