.PHONY: build test lint clean up down migrate-up migrate-down swag-init swag-fmt

build:
	go build -o bin/api ./cmd/api
	go build -o bin/ingest ./cmd/ingest

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
	@read -p "DATABASE_URL: " dburl; \
	migrate -path migrations -database "$$dburl" up

migrate-down:
	@read -p "DATABASE_URL: " dburl; \
	migrate -path migrations -database "$$dburl" down 1

swag-init:
	swag init -g internal/handlers.go -o docs/api --parseDependency

swag-fmt:
	swag fmt -g internal/handlers.go

clean:
	rm -rf bin/ cover.out
