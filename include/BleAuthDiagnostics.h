#pragma once

// Observe Bluedroid security without replacing keyboard/security callbacks.
void beginBleAuthDiagnostics();
void logBleBondCount(const char *stage);
