# ELF-only ABI probes. Run under WSL:
#
#   wsl -- bash -lc 'cd /mnt/c/.../plugin-dispatch-kata/cpp/abi-probe && make run'
#
# -fvisibility=hidden on the shared object is the realistic setting and the one
# that produces the interesting result. Turning it off changes the answer, which
# is the point.

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra

all: abi_probe libhandler.so

libhandler.so: handler.cpp shared.hpp
	$(CXX) $(CXXFLAGS) -fPIC -fvisibility=hidden -shared -o $@ handler.cpp

abi_probe: probe.cpp shared.hpp
	$(CXX) $(CXXFLAGS) -o $@ probe.cpp -ldl

run: all
	./abi_probe

# The std::string ABI break, demonstrated as a link failure rather than a
# silent one. Two translation units that disagree about _GLIBCXX_USE_CXX11_ABI
# mangle std::string differently, so the symbol simply is not there. This is the
# GOOD case: the failure is loud. The bad case is the same disagreement reached
# through a dlopen, where there is no link step to catch it.
abi-break:
	@$(CXX) $(CXXFLAGS) -D_GLIBCXX_USE_CXX11_ABI=1 -c string_api.cpp -o /tmp/swy_new.o
	@$(CXX) $(CXXFLAGS) -D_GLIBCXX_USE_CXX11_ABI=0 -c string_user.cpp -o /tmp/swy_old.o
	@echo "--- linking a CXX11-ABI library against a pre-CXX11-ABI caller ---"
	@if $(CXX) /tmp/swy_new.o /tmp/swy_old.o -o /tmp/swy_break 2>/tmp/swy_err; then \
	  echo "LINKED — unexpected; the ABI break was not detected"; exit 1; \
	else \
	  head -3 /tmp/swy_err; \
	  echo "--- link failed, which is the GOOD case: the mismatch was caught."; \
	  echo "    Reached through a dlopen instead, there is no link step to catch it."; \
	fi

clean:
	rm -f abi_probe libhandler.so

.PHONY: all run clean abi-break
