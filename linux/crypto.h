/*
 * crypto.h - Encryption Module for RemoteDesk2K Linux Relay
 */

#ifndef _RD2K_CRYPTO_H_
#define _RD2K_CRYPTO_H_

#include "common.h"

#define CRYPTO_KEY_SIZE     16
#define CRYPTO_SUCCESS      0
#define CRYPTO_ERR_INVALID  (-1)
#define CRYPTO_ERR_SIZE     (-2)

#define SERVER_ID_MAX_LEN   20

/* Initialize crypto module */
void Crypto_Init(const BYTE *masterKey);

/* Cleanup crypto module */
void Crypto_Cleanup(void);

/* Encrypt data in place */
int Crypto_Encrypt(BYTE *data, DWORD length);

/* Decrypt data in place */
int Crypto_Decrypt(BYTE *data, DWORD length);

/* Server ID encoding/decoding */
int Crypto_EncodeServerID(const char *ipAddress, WORD port, char *serverIdOut);
int Crypto_DecodeServerID(const char *serverId, char *ipAddressOut, WORD *portOut);
BOOL Crypto_ValidateServerIDFormat(const char *serverId);

#endif /* _RD2K_CRYPTO_H_ */
