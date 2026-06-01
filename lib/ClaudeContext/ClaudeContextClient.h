#pragma once
#include <cstddef>

/**
 * Uploads the extracted reading context to the configured Claude relay.
 *
 * The body (the book-so-far, already truncated at the reading position) is staged to a
 * file on the SD card first, then POSTed with a known Content-Length and streamed off the
 * card in small chunks. Doing it in two phases keeps the EPUB and the network stack from
 * being resident at the same time (the TLS handshake alone needs ~55 KB of heap, more than
 * is free while a book is loaded) and avoids holding the whole body in RAM.
 */
class ClaudeContextClient {
 public:
  enum Error {
    OK,
    NOT_CONFIGURED,
    LOW_MEMORY,
    NETWORK_ERROR,
    UNAUTHORIZED,
    TOO_LARGE,
    SERVER_ERROR,
  };

  // POSTs the file at `path` to the configured relay as the request body. WiFi must already
  // be connected. Streams the file in chunks; only one small buffer is resident.
  static Error postFile(const char* path);

  static const char* errorString(Error error);

  // HTTP status code of the most recent request (0 if the request never completed).
  static int lastHttpCode;
};
