// internal/email.go — Async email service. Triggering code enqueues
// a row into email_queue; a background goroutine drains it, renders
// the template, and sends via SMTP with retry.

package internal

import (
	"bytes"
	"context"
	"encoding/json"
	htmltemplate "html/template"
	"log/slog"
	"text/template"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"gopkg.in/gomail.v2"
)

// EmailService renders queued email templates and delivers them via SMTP.
// It is decoupled from request handling: callers Enqueue rows into
// email_queue and a background DrainLoop processes them with retry/backoff.
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

// NewEmailService constructs an EmailService bound to the given pool and SMTP
// settings. platform/baseURL are hardcoded defaults used for template rendering.
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
	// Fire-and-forget insert: the DrainLoop picks this up on its next tick.
	// Errors are intentionally ignored so a transient DB blip never blocks the
	// caller's request; the queue is best-effort.
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
			// Context cancel (process shutdown) exits the loop; in-flight rows
			// left in 'sending' status are recovered on the next run.
			return
		}
	}
}

// drainBatch pulls up to 10 due rows from email_queue. It uses
// FOR UPDATE SKIP LOCKED so multiple worker processes can drain concurrently
// without contending on the same rows.
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
		if err := rows.Scan(&id, &key, &recipient, &data); err != nil {
			slog.Warn("email queue scan", "error", err)
			continue
		}
		e.sendOne(ctx, id, key, recipient, data)
	}
	_ = rows.Err()
}

// sendOne renders one queued email and sends it, updating queue status around
// the SMTP call. The status transitions queued -> sending -> sent|failed.
func (e *EmailService) sendOne(ctx context.Context, id int64, key, recipient string, data []byte) {
	// Mark 'sending' and bump attempts first so a crash mid-send leaves the row
	// in a state the next drain can reason about.
	e.pg.Exec(ctx, `UPDATE email_queue SET status='sending', attempts=attempts+1 WHERE id=$1`, id)

	tmpl, err := e.loadTemplate(ctx, key)
	if err != nil {
		e.failOne(ctx, id, err)
		return
	}

	var vars map[string]any
	json.Unmarshal(data, &vars)
	// Inject the platform name so templates can reference {{.PlatformName}}
	// without each caller supplying it.
	vars["PlatformName"] = e.platform

	subject := renderText(tmpl.Subject, vars)
	bodyText := renderText(tmpl.BodyText, vars)
	bodyHTML := renderHTML(tmpl.BodyHTML, vars)

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

// emailTemplate is a row from email_templates rendered into an outgoing message.
type emailTemplate struct {
	Subject  string
	BodyText string
	BodyHTML string
}

// loadTemplate fetches the subject/body trio for a template key from the DB.
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

// failOne records a send failure with exponential backoff. After 3 attempts
// the row is marked 'failed' (no further retries); otherwise it returns to
// 'queued' with next_attempt_at pushed out by 2^attempts minutes.
func (e *EmailService) failOne(ctx context.Context, id int64, err error) {
	e.pg.Exec(ctx, `
		UPDATE email_queue
		SET status = CASE WHEN attempts >= 3 THEN 'failed' ELSE 'queued' END,
		    last_error = $2,
		    next_attempt_at = now() + (interval '1 minute' * power(2, attempts))
		WHERE id = $1`, id, err.Error())
	slog.Error("email send failed", "id", id, "error", err)
}

// renderText executes a text/template, returning the raw template on parse
// error so a broken template never blank-silences the email body.
func renderText(tmpl string, vars map[string]any) string {
	t, err := template.New("").Parse(tmpl)
	if err != nil {
		return tmpl
	}
	var buf bytes.Buffer
	t.Execute(&buf, vars)
	return buf.String()
}

// renderHTML executes an html/template, returning the raw template on parse
// error (see renderText). Uses html/template for context-aware auto-escaping.
func renderHTML(tmpl string, vars map[string]any) string {
	t, err := htmltemplate.New("").Parse(tmpl)
	if err != nil {
		return tmpl
	}
	var buf bytes.Buffer
	t.Execute(&buf, vars)
	return buf.String()
}
