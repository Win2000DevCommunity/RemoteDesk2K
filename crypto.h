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

#endif /* CRYPTO_H */
