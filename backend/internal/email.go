// internal/email.go — Async email service. Triggering code enqueues
// a row into email_queue; a background goroutine drains it, renders
// the template, and sends via SMTP with retry.

package internal

import (
	"bytes"
	"context"
	"encoding/json"
	"html/template"
	"log/slog"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"gopkg.in/gomail.v2"
)

type EmailService struct {
	pg       *pgxpool.Pool
	fromAddr string
	smtpHost string
	smtpPort int
	smtpUser string
	smtpPass string
	platform string
	baseURL  string
}

func NewEmailService(pg *pgxpool.Pool, fromAddr, smtpHost string, smtpPort int, smtpUser, smtpPass string) *EmailService {
	return &EmailService{
		pg:       pg,
		fromAddr: fromAddr,
		smtpHost: smtpHost,
		smtpPort: smtpPort,
		smtpUser: smtpUser,
		smtpPass: smtpPass,
		platform: "IoT Platform",
		baseURL:  "http://localhost:3000",
	}
}

// Enqueue inserts an email into the queue. Returns immediately.
func (e *EmailService) Enqueue(ctx context.Context, templateKey, recipient, userID string, data map[string]any) {
	payload, _ := json.Marshal(data)
	e.pg.Exec(ctx,
		`INSERT INTO email_queue (template_key, recipient, user_id, data) VALUES ($1, $2, $3, $4)`,
		templateKey, recipient, userID, payload)
}

// DrainLoop runs as a background goroutine, polling the queue every 5s.
func (e *EmailService) DrainLoop(ctx context.Context) {
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			e.drainBatch(ctx)
		case <-ctx.Done():
			return
		}
	}
}

func (e *EmailService) drainBatch(ctx context.Context) {
	rows, err := e.pg.Query(ctx, `
		SELECT id, template_key, recipient, data FROM email_queue
		WHERE status = 'queued' AND next_attempt_at <= now()
		ORDER BY next_attempt_at LIMIT 10
		FOR UPDATE SKIP LOCKED`)
	if err != nil {
		return
	}
	defer rows.Close()

	for rows.Next() {
		var id int64
		var key, recipient string
		var data []byte
		rows.Scan(&id, &key, &recipient, &data)
		e.sendOne(ctx, id, key, recipient, data)
	}
}

func (e *EmailService) sendOne(ctx context.Context, id int64, key, recipient string, data []byte) {
	e.pg.Exec(ctx, `UPDATE email_queue SET status='sending', attempts=attempts+1 WHERE id=$1`, id)

	tmpl, err := e.loadTemplate(ctx, key)
	if err != nil {
		e.failOne(ctx, id, err)
		return
	}

	var vars map[string]any
	json.Unmarshal(data, &vars)
	vars["PlatformName"] = e.platform

	subject := renderText(tmpl.Subject, vars)
	bodyHTML := renderText(tmpl.BodyHTML, vars)
	bodyText := renderText(tmpl.BodyText, vars)

	msg := gomail.NewMessage()
	msg.SetHeader("From", e.fromAddr)
	msg.SetHeader("To", recipient)
	msg.SetHeader("Subject", subject)
	msg.SetBody("text/plain", bodyText)
	msg.AddAlternative("text/html", bodyHTML)

	dialer := gomail.NewDialer(e.smtpHost, e.smtpPort, e.smtpUser, e.smtpPass)
	if err := dialer.DialAndSend(msg); err != nil {
		e.failOne(ctx, id, err)
		return
	}
	e.pg.Exec(ctx, `UPDATE email_queue SET status='sent', sent_at=now() WHERE id=$1`, id)
}

type emailTemplate struct {
	Subject  string
	BodyText string
	BodyHTML string
}

func (e *EmailService) loadTemplate(ctx context.Context, key string) (*emailTemplate, error) {
	var t emailTemplate
	err := e.pg.QueryRow(ctx,
		`SELECT subject, body_text, body_html FROM email_templates WHERE template_key = $1`, key).
		Scan(&t.Subject, &t.BodyText, &t.BodyHTML)
	if err != nil {
		return nil, err
	}
	return &t, nil
}

func (e *EmailService) failOne(ctx context.Context, id int64, err error) {
	e.pg.Exec(ctx, `
		UPDATE email_queue
		SET status = CASE WHEN attempts >= 3 THEN 'failed' ELSE 'queued' END,
		    last_error = $2,
		    next_attempt_at = now() + (interval '1 minute' * (5 ^ attempts))
		WHERE id = $1`, id, err.Error())
	slog.Error("email send failed", "id", id, "error", err)
}

func renderText(tmpl string, vars map[string]any) string {
	t, err := template.New("").Parse(tmpl)
	if err != nil {
		return tmpl
	}
	var buf bytes.Buffer
	t.Execute(&buf, vars)
	return buf.String()
}
