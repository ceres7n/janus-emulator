#pragma once
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
//  The client's text codepage. The KR client runs its "korean" locale (client sub_77CA40 cases
//  0/1: data\libconfig.dat, _setmbcp(949), HANGUL_CHARSET) and parses / shows every string as
//  CP949 multibyte text (e.g. the NPC dialog split by _mbstok, sub_C04220). The db/*.yml files hold
//  UTF-8, so a text bound for the client is converted to CP949 (ASCII stays as it is); a character
//  CP949 lacks becomes '?'.
// ─────────────────────────────────────────────────────────────────────────────

std::string to_client_text(const std::string& utf8);
// The other way: a text the client typed (CP949) as the UTF-8 the database's utf8mb4 tables keep.
std::string from_client_text(const std::string& cp949);
