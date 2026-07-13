package main

import (
	"bytes"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	_ "modernc.org/sqlite"
)

func TestUploadPackageUpgradesMatchingAppOnlyRelease(t *testing.T) {
	dataDir := t.TempDir()
	firmwareDir := filepath.Join(dataDir, "firmware")
	packagesDir := filepath.Join(dataDir, "packages")
	if err := os.MkdirAll(firmwareDir, 0750); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(packagesDir, 0750); err != nil {
		t.Fatal(err)
	}
	db, err := sql.Open("sqlite", filepath.Join(dataDir, "test.sqlite3"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	if _, err = db.Exec(`CREATE TABLE releases (
		id INTEGER PRIMARY KEY,
		board_type TEXT NOT NULL,
		version TEXT NOT NULL,
		channel TEXT NOT NULL,
		filename TEXT NOT NULL,
		sha256 TEXT NOT NULL,
		size INTEGER NOT NULL,
		notes TEXT NOT NULL,
		created_at INTEGER NOT NULL,
		url TEXT NOT NULL DEFAULT '',
		UNIQUE(board_type,version,channel)
	)`); err != nil {
		t.Fatal(err)
	}

	app := []byte("matching application image")
	digest := sha256.Sum256(app)
	appSHA := hex.EncodeToString(digest[:])
	oldName := "gezipai-0.6.0-old.bin"
	if err = os.WriteFile(filepath.Join(firmwareDir, oldName), app, 0640); err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec(`INSERT INTO releases(board_type,version,channel,filename,sha256,size,notes,created_at,url) VALUES(?,?,?,?,?,?,?,?,?)`,
		"gezipai", "0.6.0", "stable", oldName, appSHA, len(app), "app only", 1, "/firmware/"+oldName); err != nil {
		t.Fatal(err)
	}

	s := &server{db: db, firmwareDir: firmwareDir, packagesDir: packagesDir, adminToken: "admin"}
	meta := packageMeta{
		Board: "gezipai", Version: "0.6.0", Channel: "stable", Notes: "complete",
		ChipFamily: "ESP32-S3", AppOffset: 0x10000,
		Parts: []packagePart{
			{Offset: 0, Name: "bootloader.bin"},
			{Offset: 0x10000, Name: "nrl-esp32.bin"},
		},
	}
	request := packageRequest(t, meta, map[string][]byte{
		"bootloader.bin": []byte("bootloader"),
		"nrl-esp32.bin":  app,
	})
	recorder := httptest.NewRecorder()
	s.uploadPackage(recorder, request)
	if recorder.Code != http.StatusCreated {
		t.Fatalf("status = %d, body = %s", recorder.Code, recorder.Body.String())
	}

	var gotURL, gotSHA string
	if err = db.QueryRow(`SELECT url,sha256 FROM releases WHERE board_type=? AND version=? AND channel=?`,
		"gezipai", "0.6.0", "stable").Scan(&gotURL, &gotSHA); err != nil {
		t.Fatal(err)
	}
	if want := "/packages/gezipai/0.6.0/nrl-esp32.bin"; gotURL != want {
		t.Fatalf("url = %q, want %q", gotURL, want)
	}
	if gotSHA != appSHA {
		t.Fatalf("sha256 = %q, want %q", gotSHA, appSHA)
	}
	if _, err = os.Stat(filepath.Join(packagesDir, "gezipai", "0.6.0", "package.json")); err != nil {
		t.Fatalf("package metadata was not stored: %v", err)
	}
	if _, err = os.Stat(filepath.Join(firmwareDir, oldName)); !os.IsNotExist(err) {
		t.Fatalf("old app-only image still exists, stat error: %v", err)
	}
}

func packageRequest(t *testing.T, meta packageMeta, files map[string][]byte) *http.Request {
	t.Helper()
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	metaField, err := writer.CreateFormField("meta")
	if err != nil {
		t.Fatal(err)
	}
	if err = json.NewEncoder(metaField).Encode(meta); err != nil {
		t.Fatal(err)
	}
	for _, part := range meta.Parts {
		field, err := writer.CreateFormFile(part.Name, part.Name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = field.Write(files[part.Name]); err != nil {
			t.Fatal(err)
		}
	}
	if err = writer.Close(); err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, "/api/v1/admin/flash-package", &body)
	request.Header.Set("Content-Type", writer.FormDataContentType())
	request.Header.Set("X-OTA-Token", "admin")
	return request
}
