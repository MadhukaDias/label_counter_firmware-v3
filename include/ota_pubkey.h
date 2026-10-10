#pragma once
// Public half of the OTA signing key. Safe to commit; the private key lives
// only in the GitHub secret OTA_SIGNING_KEY.
static const char OTA_PUBKEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE9jZDr62xgvSBcYc88pTrqY6LKQAz\n"
    "8pw7Q3HlJi9tn/w1M8DrWlIOB4jHUjCP0CLQE+5U+CSBBRzd8huJh3AMuQ==\n"
    "-----END PUBLIC KEY-----\n";
