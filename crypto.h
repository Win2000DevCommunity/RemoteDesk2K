/*
 * crypto.h - Symmetric Encryption Module for RemoteDesk2K
 * 
 * Based on XOR cipher with enhancements:
 * - Multi-byte rotating key
 * - Byte substitution (S-Box)
 * - Position-dependent transformation
 * 
 * Used to encrypt Partner IDs for secure transmission
 */

#ifndef CRYPTO_H
#define CRYPTO_H

#include <windows.h>

/* Encryption key size (128 bits = 16 bytes for good security) */
#define CRYPTO_KEY_SIZE     16

/* Magic number for validation */
#define CRYPTO_MAGIC        0xRD2K

/* Error codes */
#define CRYPTO_SUCCESS      0
#define CRYPTO_ERR_INVALID  -1
#define CRYPTO_ERR_SIZE     -2

/*
 * Initialize the crypto module with a master key
 * If masterKey is NULL, uses built-in default key
 */
void Crypto_Init(const BYTE *masterKey);

/*
 * Cleanup the crypto module
 */
void Crypto_Cleanup(void);

/*
 * Set a new encryption key
 * key: pointer to CRYPTO_KEY_SIZE bytes
 */
void Crypto_SetKey(const BYTE *key);

/*
 * Get current key (for key exchange)
 * keyOut: buffer to receive CRYPTO_KEY_SIZE bytes
 */
void Crypto_GetKey(BYTE *keyOut);

/*
 * Encrypt data in place
 * data: buffer to encrypt
 * length: number of bytes
 * Returns: CRYPTO_SUCCESS or error code
 */
int Crypto_Encrypt(BYTE *data, DWORD length);

/*
 * Decrypt data in place
 * data: buffer to decrypt
 * length: number of bytes
 * Returns: CRYPTO_SUCCESS or error code
 */
int Crypto_Decrypt(BYTE *data, DWORD length);

/*
 * Encrypt a DWORD (like Partner ID)
 * Returns encrypted value
 */
DWORD Crypto_EncryptDWORD(DWORD value);

/*
 * Decrypt a DWORD (like Partner ID)
 * Returns decrypted value
 */
DWORD Crypto_DecryptDWORD(DWORD encrypted);

/*
 * Encrypt IP address (stored as DWORD)
 * plainIP: IP address in network byte order
 * Returns: encrypted IP as DWORD
 */
DWORD Crypto_EncryptIP(DWORD plainIP);

/*
 * Decrypt IP address
 * encryptedIP: encrypted IP value
 * Returns: original IP in network byte order
 */
DWORD Crypto_DecryptIP(DWORD encryptedIP);

/*
 * Encrypt a string (like ID display string "XXX XXX XXX")
 * plainText: input string
 * cipherText: output buffer (must be at least strlen(plainText)+1)
 * maxLen: size of output buffer
 * Returns: CRYPTO_SUCCESS or error code
 */
int Crypto_EncryptString(const char *plainText, char *cipherText, DWORD maxLen);

/*
 * Decrypt a string
 * cipherText: encrypted string
 * plainText: output buffer
 * maxLen: size of output buffer
 * Returns: CRYPTO_SUCCESS or error code
 */
int Crypto_DecryptString(const char *cipherText, char *plainText, DWORD maxLen);

/*
 * Generate a random session key
 * keyOut: buffer to receive CRYPTO_KEY_SIZE bytes
 */
void Crypto_GenerateSessionKey(BYTE *keyOut);

/*
 * Derive key from password using simple hash
 * password: null-terminated password string
 * keyOut: buffer to receive CRYPTO_KEY_SIZE bytes
 */
void Crypto_DeriveKeyFromPassword(const char *password, BYTE *keyOut);

/*
 * SERVER ID ENCODING/DECODING
 * 
 * Server ID is an obfuscated representation of IP:Port
 * Format: "XXXX-XXXX-XXXX" (alphanumeric, easy to type/share)
 * 
 * This hides the actual server IP from end users for security.
 * Server admin generates the ID, distributes to clients.
 */

/* Maximum Server ID string length (including null terminator) */
#define SERVER_ID_MAX_LEN   20

/*
 * Encode IP address and port into a Server ID string
 * ipAddress: IP address string (e.g., "192.168.1.100")
 * port: port number (0-65535)
 * serverIdOut: output buffer for Server ID (at least SERVER_ID_MAX_LEN bytes)
 * Returns: CRYPTO_SUCCESS or error code
 */
int Crypto_EncodeServerID(const char *ipAddress, WORD port, char *serverIdOut);

/*
 * Decode Server ID string back to IP address and port
 * serverId: Server ID string (e.g., "ABCD-EFGH-1234")
 * ipAddressOut: output buffer for IP (at least 16 bytes)
 * portOut: output pointer for port
 * Returns: CRYPTO_SUCCESS or error code
 */
int Crypto_DecodeServerID(const char *serverId, char *ipAddressOut, WORD *portOut);

/*
 * Validate Server ID format (quick check without decoding)
 * Returns: TRUE if format appears valid, FALSE otherwise
 */
BOOL Crypto_ValidateServerIDFormat(const char *serverId);

#endif /* CRYPTO_H */
