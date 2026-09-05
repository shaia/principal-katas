// Exports a function taking std::string. Which std::string depends on
// _GLIBCXX_USE_CXX11_ABI, and the two mangle differently.
#include <string>
int consume(const std::string& s);
int consume(const std::string& s) { return static_cast<int>(s.size()); }
