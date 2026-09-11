#pragma once
#include <string>

namespace Password {
std::string hash(const std::string& password);
bool verify(const std::string& password, const std::string& stored);
bool isEncoded(const std::string& stored);
bool validNew(const std::string& password);
std::string randomToken();
}
