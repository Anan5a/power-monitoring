.PHONY: build build-seed test test-integration test-cover lint clean up down migrate-up migrate-down seed-dev swag-init swag-fmt

build:
	go build -o bin/api ./cmd/api
	go build -o bin/ingest ./cmd/ingest

build-seed:
	go build -o bin/seed ./cmd/seed

test:
	go test ./...

test-integration:
	go test -tags=integration ./...

test-cover:
	go test -coverprofile=cover.out ./internal/...
	go tool cover -html=cover.out

lint:
	golangci-lint run ./...

up:
	docker compose up -d

down:
	docker compose down

migrate-up:
	migrate -path migrations -database "postgres://powermon:powermon@localhost:5432/powermon?sslmode=disable" up

migrate-down:
	migrate -path migrations -database "postgres://powermon:powermon@localhost:5432/powermon?sslmode=disable" down 1

seed-dev: build-seed
	DATABASE_URL=postgres://powermon:powermon@localhost:5432/powermon?sslmode=disable \
	JWT_SECRET=dev-secret-32-chars-minimum-for-hs256 \
	CLICKHOUSE_URL=clickhouse://localhost:9000/powermon?dial_timeout=10s \
	MQTT_BROKER=tcp://localhost:1883 \
	./bin/seed

smoke: seed-dev
	@echo "Smoke-test credentials: test@example.com / TestPass123!"

swag-init:
	swag init -g internal/handlers.go -o docs/api --parseDependency

swag-fmt:
	swag fmt -g internal/handlers.go

clean:
	rm -rf bin/ cover.out
