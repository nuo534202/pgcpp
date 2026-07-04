// crypt.h — Password hashing and verification (md5 / scram-sha-256 / plain).
//
// Converted from PostgreSQL 15's src/backend/libpq/crypt.c.
//
// PG stores password hashes in pg_authid.rolpassword using one of three
// formats:
//   * "md5" + 32 hex chars (MD5 of password+username)
//   * "SCRAM-SHA-256$<iter>:<salt>$<ck>:<sk>" (SCRAM-SHA-256)
//   * plain text (when `password_encryption = plain`, only for bootstrapping)
//
// This module provides:
//   - PasswordHash:        parsed representation of a stored password
//   - ParsePasswordHash:   detect the format and parse it
//   - Md5Encrypt:          compute the MD5 hash
//   - ScramSha256Hash:     compute the SCRAM-SHA-256 stored-key / server-key
//   - CryptVerify:         verify a plaintext password against a stored hash
//   - EncryptPassword:     hash a plaintext password using the configured method
//
// The implementation uses SHA-256 and MD5 from <openssl/evp.h> and <openssl/md5.h>.
// To avoid an OpenSSL build dependency for the unit tests, the .cpp provides a
// self-contained SHA-256 implementation and a minimal MD5 implementation.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::protocol {

// PasswordEncryptionAlgorithm — controls how new passwords are hashed.
// Mirrors PG's `password_encryption` GUC.
enum class PasswordEncryptionAlgorithm {
    kPlain = 0,        // store as plain text (testing only)
    kMd5 = 1,          // md5(password + username)
    kScramSha256 = 2,  // SCRAM-SHA-256 (default in PG 14+)
};

// PasswordHash — parsed representation of a stored password.
struct PasswordHash {
    PasswordEncryptionAlgorithm method = PasswordEncryptionAlgorithm::kPlain;
    // For md5: the 32-hex-char digest (without the "md5" prefix).
    std::string md5_digest;
    // For scram-sha-256: the parsed components.
    int scram_iterations = 0;
    std::string scram_salt_b64;       // base64-encoded salt
    std::string scram_storedkey_b64;  // base64-encoded StoredKey
    std::string scram_serverkey_b64;  // base64-encoded ServerKey
    // Raw shadow password string (as stored in pg_authid.rolpassword).
    std::string raw;
};

// ParsePasswordHash — parse a stored password string into its components.
// Returns true if the format is recognised (plain, md5:, or SCRAM-SHA-256$).
bool ParsePasswordHash(const std::string& shadow_pass, PasswordHash& out);

// Md5Encrypt — compute the MD5 hash of (password + username), prefixed with
// "md5". Returns the 35-char string ("md5" + 32 hex).
std::string Md5Encrypt(const std::string& passwd, const std::string& username);

// Md5Hex — compute the MD5 hash of `data` and return the 32 lowercase hex
// chars (no "md5" prefix). Used by the md5 auth handler to compute the
// inner hash md5(md5(password+username) + salt) without the empty-username
// hack that Md5Encrypt required.
std::string Md5Hex(const std::string& data);

// ScramSha256Hash — compute SCRAM-SHA-256 StoredKey and ServerKey from a
// plaintext password and salt. Uses PBKDF2 with the given iteration count.
// Outputs base64-encoded strings (matching PG's storage format).
void ScramSha256Hash(const std::string& password, const std::string& salt, int iterations,
                     std::string& stored_key_b64, std::string& server_key_b64);

// ScramSha256ClientKey — compute ClientKey from StoredKey (used during
// SCRAM auth exchange). Returns raw bytes.
std::vector<uint8_t> ScramSha256ClientKey(const std::vector<uint8_t>& stored_key);

// EncryptPassword — hash a plaintext password for storage, using the given
// algorithm. For SCRAM, a random salt is generated (deterministic when a
// salt is supplied for testing).
std::string EncryptPassword(const std::string& password, const std::string& username,
                            PasswordEncryptionAlgorithm method, const std::string& salt = "");

// CryptVerify — verify a plaintext password against a stored hash.
// Returns true if the password matches.
bool CryptVerify(const PasswordHash& hash, const std::string& password,
                 const std::string& username);

// Base64Encode / Base64Decode — helpers used by SCRAM password storage.
std::string Base64Encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> Base64Decode(const std::string& s);

// Sha256 — compute SHA-256 of `data` and return the 32 raw digest bytes.
// Exposed for the SCRAM-SHA-256 SASL exchange (auth.cpp) which needs to
// hash the recovered ClientKey during proof verification.
std::vector<uint8_t> Sha256(const std::string& data);

// HmacSha256 — compute HMAC-SHA-256 of `msg` under `key`. Returns the 32
// raw bytes. Used by the SCRAM exchange to compute ClientSignature and
// ServerSignature.
std::vector<uint8_t> HmacSha256(const std::vector<uint8_t>& key, const std::string& msg);

// Pbkdf2HmacSha256 — PBKDF2 key derivation using HMAC-SHA-256 (RFC 2898).
// Derives a 32-byte key from `password` and `salt` over `iterations`
// rounds. Exposed so tests acting as the SCRAM client can derive the
// SaltedPassword and compute the ClientProof.
std::vector<uint8_t> Pbkdf2HmacSha256(const std::string& password, const std::vector<uint8_t>& salt,
                                      int iterations);

}  // namespace pgcpp::protocol
