/*
 * crypto.c - Symmetric Encryption Module for RemoteDesk2K
 * 
 * Implementation of XOR-based symmetric encryption with enhancements:
 * - Multi-byte rotating key (16 bytes / 128 bits)
 * - S-Box substitution for non-linearity
 * - Position-dependent byte transformation
 * - Bit rotation for diffusion
 * 
 * Algorithm:
 * 1. XOR with rotating key
 * 2. Apply S-Box substitution
 * 3. Rotate bits based on position
 * 4. XOR with position-derived value
 * 
 * Decryption reverses these steps
 */

#include "crypto.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/* S-Box for byte substitution (adds non-linearity) */
static const BYTE g_SBox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16
};

/* Inverse S-Box for decryption */
static const BYTE g_InvSBox[256] = {
    0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38, 0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7, 0xFB,
    0x7C, 0xE3, 0x39, 0x82, 0x9B, 0x2F, 0xFF, 0x87, 0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE, 0xE9, 0xCB,
    0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2, 0x23, 0x3D, 0xEE, 0x4C, 0x95, 0x0B, 0x42, 0xFA, 0xC3, 0x4E,
    0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2, 0x76, 0x5B, 0xA2, 0x49, 0x6D, 0x8B, 0xD1, 0x25,
    0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xD4, 0xA4, 0x5C, 0xCC, 0x5D, 0x65, 0xB6, 0x92,
    0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA, 0x5E, 0x15, 0x46, 0x57, 0xA7, 0x8D, 0x9D, 0x84,
    0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A, 0xF7, 0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06,
    0xD0, 0x2C, 0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02, 0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B,
    0x3A, 0x91, 0x11, 0x41, 0x4F, 0x67, 0xDC, 0xEA, 0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73,
    0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD, 0x35, 0x85, 0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E,
    0x47, 0xF1, 0x1A, 0x71, 0x1D, 0x29, 0xC5, 0x89, 0x6F, 0xB7, 0x62, 0x0E, 0xAA, 0x18, 0xBE, 0x1B,
    0xFC, 0x56, 0x3E, 0x4B, 0xC6, 0xD2, 0x79, 0x20, 0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD, 0x5A, 0xF4,
    0x1F, 0xDD, 0xA8, 0x33, 0x88, 0x07, 0xC7, 0x31, 0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F,
    0x60, 0x51, 0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D, 0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF,
    0xA0, 0xE0, 0x3B, 0x4D, 0xAE, 0x2A, 0xF5, 0xB0, 0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6, 0x26, 0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C, 0x7D
};

/* Default master key (should be changed for production) */
static const BYTE g_DefaultKey[CRYPTO_KEY_SIZE] = {
    0x52, 0x44, 0x32, 0x4B,  /* "RD2K" */
    0xDE, 0xAD, 0xBE, 0xEF,  /* 0xDEADBEEF */
    0xCA, 0xFE, 0xBA, 0xBE,  /* 0xCAFEBABE */
    0x20, 0x00, 0x20, 0x26   /* Year reference */
};

/* Current encryption key */
static BYTE g_CurrentKey[CRYPTO_KEY_SIZE];
static BOOL g_bInitialized = FALSE;

/* Helper: Rotate byte left */
static BYTE RotateLeft(BYTE value, int shift)
{
    shift &= 7;  /* Keep shift in 0-7 range */
    return (BYTE)((value << shift) | (value >> (8 - shift)));
}

/* Helper: Rotate byte right */
static BYTE RotateRight(BYTE value, int shift)
{
    shift &= 7;
    return (BYTE)((value >> shift) | (value << (8 - shift)));
}

/* Initialize crypto module */
void Crypto_Init(const BYTE *masterKey)
{
    if (masterKey) {
        memcpy(g_CurrentKey, masterKey, CRYPTO_KEY_SIZE);
    } else {
        memcpy(g_CurrentKey, g_DefaultKey, CRYPTO_KEY_SIZE);
    }
    g_bInitialized = TRUE;
    
    /* Seed random for session key generation */
    srand((unsigned int)time(NULL) ^ GetTickCount());
}

/* Cleanup */
void Crypto_Cleanup(void)
{
    /* Zero out key for security */
    ZeroMemory(g_CurrentKey, CRYPTO_KEY_SIZE);
    g_bInitialized = FALSE;
}

/* Set new key */
void Crypto_SetKey(const BYTE *key)
{
    if (key) {
        memcpy(g_CurrentKey, key, CRYPTO_KEY_SIZE);
    }
}

/* Get current key */
void Crypto_GetKey(BYTE *keyOut)
{
    if (keyOut) {
        memcpy(keyOut, g_CurrentKey, CRYPTO_KEY_SIZE);
    }
}

/*
 * Core encryption function
 * For each byte:
 * 1. XOR with key[position % keysize]
 * 2. Apply S-Box substitution
 * 3. Rotate bits based on position
 * 4. XOR with (position * prime) for additional diffusion
 */
int Crypto_Encrypt(BYTE *data, DWORD length)
{
    DWORD i;
    BYTE keyByte, temp;
    int rotAmount;
    
    if (!data || length == 0) {
        return CRYPTO_ERR_INVALID;
    }
    
    if (!g_bInitialized) {
        Crypto_Init(NULL);
    }
    
    for (i = 0; i < length; i++) {
        /* Step 1: XOR with rotating key */
        keyByte = g_CurrentKey[i % CRYPTO_KEY_SIZE];
        temp = data[i] ^ keyByte;
        
        /* Step 2: S-Box substitution */
        temp = g_SBox[temp];
        
        /* Step 3: Rotate bits based on position (1-7 bits) */
        rotAmount = ((i + 1) % 7) + 1;
        temp = RotateLeft(temp, rotAmount);
        
        /* Step 4: XOR with position-derived value (prime = 37) */
        temp ^= (BYTE)((i * 37) & 0xFF);
        
        data[i] = temp;
    }
    
    return CRYPTO_SUCCESS;
}

/*
 * Core decryption function
 * Reverse of encryption:
 * 1. XOR with (position * prime)
 * 2. Rotate bits right
 * 3. Apply inverse S-Box
 * 4. XOR with key
 */
int Crypto_Decrypt(BYTE *data, DWORD length)
{
    DWORD i;
    BYTE keyByte, temp;
    int rotAmount;
    
    if (!data || length == 0) {
        return CRYPTO_ERR_INVALID;
    }
    
    if (!g_bInitialized) {
        Crypto_Init(NULL);
    }
    
    for (i = 0; i < length; i++) {
        temp = data[i];
        
        /* Step 1: XOR with position-derived value (reverse step 4) */
        temp ^= (BYTE)((i * 37) & 0xFF);
        
        /* Step 2: Rotate bits right (reverse step 3) */
        rotAmount = ((i + 1) % 7) + 1;
        temp = RotateRight(temp, rotAmount);
        
        /* Step 3: Inverse S-Box (reverse step 2) */
        temp = g_InvSBox[temp];
        
        /* Step 4: XOR with key (reverse step 1) */
        keyByte = g_CurrentKey[i % CRYPTO_KEY_SIZE];
        temp ^= keyByte;
        
        data[i] = temp;
    }
    
    return CRYPTO_SUCCESS;
}

/* Encrypt a DWORD value */
DWORD Crypto_EncryptDWORD(DWORD value)
{
    BYTE buffer[4];
    
    /* Convert to bytes (little-endian) */
    buffer[0] = (BYTE)(value & 0xFF);
    buffer[1] = (BYTE)((value >> 8) & 0xFF);
    buffer[2] = (BYTE)((value >> 16) & 0xFF);
    buffer[3] = (BYTE)((value >> 24) & 0xFF);
    
    /* Encrypt */
    Crypto_Encrypt(buffer, 4);
    
    /* Convert back to DWORD */
    return (DWORD)buffer[0] | 
           ((DWORD)buffer[1] << 8) |
           ((DWORD)buffer[2] << 16) |
           ((DWORD)buffer[3] << 24);
}

/* Decrypt a DWORD value */
DWORD Crypto_DecryptDWORD(DWORD encrypted)
{
    BYTE buffer[4];
    
    /* Convert to bytes */
    buffer[0] = (BYTE)(encrypted & 0xFF);
    buffer[1] = (BYTE)((encrypted >> 8) & 0xFF);
    buffer[2] = (BYTE)((encrypted >> 16) & 0xFF);
    buffer[3] = (BYTE)((encrypted >> 24) & 0xFF);
    
    /* Decrypt */
    Crypto_Decrypt(buffer, 4);
    
    /* Convert back to DWORD */
    return (DWORD)buffer[0] | 
           ((DWORD)buffer[1] << 8) |
           ((DWORD)buffer[2] << 16) |
           ((DWORD)buffer[3] << 24);
}

/* Encrypt IP address */
DWORD Crypto_EncryptIP(DWORD plainIP)
{
    return Crypto_EncryptDWORD(plainIP);
}

/* Decrypt IP address */
DWORD Crypto_DecryptIP(DWORD encryptedIP)
{
    return Crypto_DecryptDWORD(encryptedIP);
}

/* Encrypt string */
int Crypto_EncryptString(const char *plainText, char *cipherText, DWORD maxLen)
{
    DWORD len;
    
    if (!plainText || !cipherText || maxLen == 0) {
        return CRYPTO_ERR_INVALID;
    }
    
    len = (DWORD)strlen(plainText);
    if (len >= maxLen) {
        return CRYPTO_ERR_SIZE;
    }
    
    /* Copy then encrypt in place */
    memcpy(cipherText, plainText, len + 1);
    return Crypto_Encrypt((BYTE*)cipherText, len);
}

/* Decrypt string */
int Crypto_DecryptString(const char *cipherText, char *plainText, DWORD maxLen)
{
    DWORD len;
    
    if (!cipherText || !plainText || maxLen == 0) {
        return CRYPTO_ERR_INVALID;
    }
    
    len = (DWORD)strlen(cipherText);
    if (len >= maxLen) {
        return CRYPTO_ERR_SIZE;
    }
    
    /* Copy then decrypt in place */
    memcpy(plainText, cipherText, len + 1);
    return Crypto_Decrypt((BYTE*)plainText, len);
}

/* Generate random session key */
void Crypto_GenerateSessionKey(BYTE *keyOut)
{
    int i;
    DWORD tick;
    
    if (!keyOut) return;
    
    tick = GetTickCount();
    
    for (i = 0; i < CRYPTO_KEY_SIZE; i++) {
        /* Mix multiple sources of entropy */
        keyOut[i] = (BYTE)(rand() ^ (tick >> (i % 4)) ^ g_DefaultKey[i]);
        tick = tick * 1103515245 + 12345;  /* LCG step */
    }
}

/* Derive key from password using simple hash */
void Crypto_DeriveKeyFromPassword(const char *password, BYTE *keyOut)
{
    DWORD hash = 5381;  /* djb2 hash initial value */
    int i, c;
    const char *p;
    
    if (!password || !keyOut) return;
    
    /* Start with default key */
    memcpy(keyOut, g_DefaultKey, CRYPTO_KEY_SIZE);
    
    /* Hash password using djb2 algorithm */
    p = password;
    while ((c = *p++) != 0) {
        hash = ((hash << 5) + hash) + c;  /* hash * 33 + c */
    }
    
    /* Mix hash into key */
    for (i = 0; i < CRYPTO_KEY_SIZE; i++) {
        keyOut[i] ^= (BYTE)((hash >> (i % 4)) & 0xFF);
        hash = hash * 1103515245 + 12345;
    }
}

/* ============================================================
 * SERVER ID ENCODING/DECODING FUNCTIONS
 * 
 * Server ID hides the actual IP:Port from end users.
 * Format: "XXXX-XXXX-XXXX" using base32-like encoding
 * ============================================================ */

/* Base32-like alphabet (no confusing chars like 0/O, 1/I/L) */
static const char g_ServerIdAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
#define ALPHABET_SIZE 32

/* Helper: Parse IP address string to 4 bytes */
static BOOL ParseIPAddress(const char *ipStr, BYTE *ipBytes)
{
    int parts[4];
    int i;
    
    if (!ipStr || !ipBytes) return FALSE;
    
    /* Parse dotted decimal format */
    if (sscanf(ipStr, "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]) != 4) {
        return FALSE;
    }
    
    /* Validate each octet */
    for (i = 0; i < 4; i++) {
        if (parts[i] < 0 || parts[i] > 255) {
            return FALSE;
        }
        ipBytes[i] = (BYTE)parts[i];
    }
    
    return TRUE;
}

/* Helper: Format IP bytes to string */
static void FormatIPAddress(const BYTE *ipBytes, char *ipStr)
{
    if (ipBytes && ipStr) {
        sprintf(ipStr, "%d.%d.%d.%d", ipBytes[0], ipBytes[1], ipBytes[2], ipBytes[3]);
    }
}

/* Helper: Encode 6 bytes to base32-like string (produces ~10 chars) */
static void EncodeBase32(const BYTE *data, int dataLen, char *output)
{
    int bitBuffer = 0;
    int bitCount = 0;
    int outIdx = 0;
    int i;
    
    for (i = 0; i < dataLen; i++) {
        bitBuffer = (bitBuffer << 8) | data[i];
        bitCount += 8;
        
        while (bitCount >= 5) {
            bitCount -= 5;
            output[outIdx++] = g_ServerIdAlphabet[(bitBuffer >> bitCount) & 0x1F];
        }
    }
    
    /* Handle remaining bits */
    if (bitCount > 0) {
        output[outIdx++] = g_ServerIdAlphabet[(bitBuffer << (5 - bitCount)) & 0x1F];
    }
    
    output[outIdx] = '\0';
}

/* Helper: Decode base32-like string to bytes */
static int DecodeBase32(const char *input, BYTE *output, int maxOutLen)
{
    int bitBuffer = 0;
    int bitCount = 0;
    int outIdx = 0;
    int i;
    char c;
    int charVal;
    const char *found;
    
    for (i = 0; input[i] != '\0' && outIdx < maxOutLen; i++) {
        c = input[i];
        
        /* Skip dashes */
        if (c == '-') continue;
        
        /* Convert to uppercase */
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        
        /* Find character in alphabet */
        found = strchr(g_ServerIdAlphabet, c);
        if (!found) {
            return -1;  /* Invalid character */
        }
        charVal = (int)(found - g_ServerIdAlphabet);
        
        bitBuffer = (bitBuffer << 5) | charVal;
        bitCount += 5;
        
        while (bitCount >= 8 && outIdx < maxOutLen) {
            bitCount -= 8;
            output[outIdx++] = (BYTE)((bitBuffer >> bitCount) & 0xFF);
        }
    }
    
    return outIdx;
}

/* Helper: Insert dashes into server ID for readability: XXXX-XXXX-XXXX */
static void FormatServerID(const char *raw, char *formatted)
{
    int len;
    int i, j;
    
    len = (int)strlen(raw);
    j = 0;
    
    for (i = 0; i < len && j < SERVER_ID_MAX_LEN - 1; i++) {
        if (i > 0 && (i % 4) == 0) {
            formatted[j++] = '-';
        }
        formatted[j++] = raw[i];
    }
    formatted[j] = '\0';
}

/* Helper: Remove dashes from server ID */
static void UnformatServerID(const char *formatted, char *raw)
{
    int i, j;
    
    j = 0;
    for (i = 0; formatted[i] != '\0'; i++) {
        if (formatted[i] != '-') {
            raw[j++] = formatted[i];
        }
    }
    raw[j] = '\0';
}

/*
 * Encode IP address and port into obfuscated Server ID
 * 
 * Structure: [4 bytes IP] + [2 bytes Port] = 6 bytes
 * These are encrypted, then base32-encoded
 */
int Crypto_EncodeServerID(const char *ipAddress, WORD port, char *serverIdOut)
{
    BYTE rawData[8];  /* Extra padding for encryption */
    BYTE encryptedData[8];
    char rawId[20];
    
    if (!ipAddress || !serverIdOut) {
        return CRYPTO_ERR_INVALID;
    }
    
    /* Zero out buffers */
    ZeroMemory(rawData, sizeof(rawData));
    
    /* Parse IP address to bytes */
    if (!ParseIPAddress(ipAddress, rawData)) {
        return CRYPTO_ERR_INVALID;
    }
    
    /* Add port (big-endian for consistent encoding) */
    rawData[4] = (BYTE)((port >> 8) & 0xFF);
    rawData[5] = (BYTE)(port & 0xFF);
    
    /* Add checksum byte for validation */
    rawData[6] = (BYTE)(rawData[0] ^ rawData[1] ^ rawData[2] ^ rawData[3] ^ rawData[4] ^ rawData[5]);
    rawData[7] = 0x2A;  /* Magic marker byte (RD2K) */
    
    /* Copy for encryption (encrypt in place would modify original) */
    memcpy(encryptedData, rawData, 8);
    
    /* Encrypt the data */
    if (!g_bInitialized) {
        Crypto_Init(NULL);
    }
    Crypto_Encrypt(encryptedData, 8);
    
    /* Encode to base32-like string */
    EncodeBase32(encryptedData, 8, rawId);
    
    /* Format with dashes for readability */
    FormatServerID(rawId, serverIdOut);
    
    return CRYPTO_SUCCESS;
}

/*
 * Decode Server ID back to IP address and port
 */
int Crypto_DecodeServerID(const char *serverId, char *ipAddressOut, WORD *portOut)
{
    char rawId[20];
    BYTE encryptedData[8];
    BYTE decryptedData[8];
    int decodedLen;
    BYTE checksum;
    
    if (!serverId || !ipAddressOut || !portOut) {
        return CRYPTO_ERR_INVALID;
    }
    
    /* Remove formatting dashes */
    UnformatServerID(serverId, rawId);
    
    /* Decode from base32 */
    ZeroMemory(encryptedData, sizeof(encryptedData));
    decodedLen = DecodeBase32(rawId, encryptedData, 8);
    
    if (decodedLen < 6) {
        return CRYPTO_ERR_INVALID;
    }
    
    /* Copy for decryption */
    memcpy(decryptedData, encryptedData, 8);
    
    /* Decrypt the data */
    if (!g_bInitialized) {
        Crypto_Init(NULL);
    }
    Crypto_Decrypt(decryptedData, 8);
    
    /* Verify checksum */
    checksum = (BYTE)(decryptedData[0] ^ decryptedData[1] ^ decryptedData[2] ^ 
                      decryptedData[3] ^ decryptedData[4] ^ decryptedData[5]);
    if (checksum != decryptedData[6]) {
        return CRYPTO_ERR_INVALID;  /* Checksum mismatch - invalid ID */
    }
    
    /* Extract IP address */
    FormatIPAddress(decryptedData, ipAddressOut);
    
    /* Extract port (big-endian) */
    *portOut = (WORD)((decryptedData[4] << 8) | decryptedData[5]);
    
    return CRYPTO_SUCCESS;
}

/*
 * Validate Server ID format (quick check without full decoding)
 */
BOOL Crypto_ValidateServerIDFormat(const char *serverId)
{
    int len;
    int alphaCount = 0;
    int dashCount = 0;
    int i;
    char c;
    const char *found;
    
    if (!serverId) return FALSE;
    
    len = (int)strlen(serverId);
    
    /* Server ID should be around 15-17 chars with dashes */
    if (len < 10 || len > 20) return FALSE;
    
    for (i = 0; i < len; i++) {
        c = serverId[i];
        
        if (c == '-') {
            dashCount++;
            continue;
        }
        
        /* Convert to uppercase for check */
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        
        /* Check if valid alphabet character */
        found = strchr(g_ServerIdAlphabet, c);
        if (!found) {
            return FALSE;
        }
        alphaCount++;
    }
    
    /* Expect ~13 alphabet chars and 2-3 dashes */
    return (alphaCount >= 10 && alphaCount <= 16 && dashCount >= 1);
}
