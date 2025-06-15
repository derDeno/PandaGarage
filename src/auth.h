#pragma once

#include "mbedtls/sha256.h"

extern AppConfig appConfig;


String sha256(const String& input) {
    uint8_t hash[32];
    mbedtls_sha256_context ctx;

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts_ret(&ctx, 0); // SHA-256 (not 224)
    mbedtls_sha256_update_ret(&ctx, (const unsigned char*)input.c_str(), input.length());
    mbedtls_sha256_finish_ret(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char output[65];
    for (int i = 0; i < 32; i++) {
        sprintf(output + i * 2, "%02x", hash[i]);
    }
    output[64] = 0;
    return String(output);
}

bool verifyPasswordHash(const String& hashToCheck) {
    String stored = appConfig.adminPwd;
    return stored.length() > 0 && stored == hashToCheck;
}