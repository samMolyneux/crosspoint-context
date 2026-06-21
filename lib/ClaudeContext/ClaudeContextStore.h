#pragma once
#include <string>

/**
 * Singleton storing the configuration for pushing reading context to a Claude relay.
 *
 * Holds the relay URL and a write-only bearer token. The token is XOR-obfuscated with the
 * device's hardware MAC and base64-encoded before being written to JSON on the SD card —
 * the same scheme KOReaderCredentialStore uses for passwords (not cryptographically
 * secure, but prevents casual reading and ties the token to this device).
 */
class ClaudeContextStore {
 private:
  static ClaudeContextStore instance;
  std::string relayUrl;    // server origin, e.g. https://crosspoint-context-mcp.example.workers.dev
  std::string writeToken;  // bearer token; obfuscated at rest

  ClaudeContextStore() = default;

  // Fill the relay URL from the compile-time default (-DCLAUDE_DEFAULT_RELAY_URL) when unset.
  // No-op when the macro is undefined. There is deliberately no default write token.
  void applyCompileTimeDefaults();

 public:
  ClaudeContextStore(const ClaudeContextStore&) = delete;
  ClaudeContextStore& operator=(const ClaudeContextStore&) = delete;

  static ClaudeContextStore& getInstance() { return instance; }

  // Persistence (JSON on SD card)
  bool saveToFile() const;
  bool loadFromFile();

  // Configuration
  void setConfig(const std::string& url, const std::string& token);
  void setRelayUrl(const std::string& url);
  void setWriteToken(const std::string& token);
  const std::string& getRelayUrl() const { return relayUrl; }
  const std::string& getWriteToken() const { return writeToken; }

  // The configured server as a normalised origin: adds https:// if no scheme is present and
  // strips any path/trailing slash (callers append /ingest, /pair/start). Empty if unset.
  std::string getNormalisedUrl() const;

  // True when both the relay URL and the write token are set.
  bool isConfigured() const;

  void clear();
};

// Helper macro to access the store, mirroring KOREADER_STORE.
#define CLAUDE_CONTEXT_STORE ClaudeContextStore::getInstance()
