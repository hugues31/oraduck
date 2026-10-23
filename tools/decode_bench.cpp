// Micro-benchmark of the NUMBER decoders: ns per value over 10,000,000 pre-encoded values per case.
// Build and run: make decode-bench   (pins the CPUs with taskset -c 0-7)
#include "oraduck/oracle_decoding.hpp"
#include "oraduck/oracle_encoding.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace oraduck;

namespace {

constexpr size_t kValues = 10000000;
constexpr size_t kStride = 24;

uint64_t g_state = 0x9E3779B97F4A7C15ULL;

uint64_t NextRandom() { // xorshift64*
	g_state ^= g_state >> 12;
	g_state ^= g_state << 25;
	g_state ^= g_state >> 27;
	return g_state * 0x2545F4914F6CDD1DULL;
}

// Random decimal digits (all non zero) -> unsigned 128-bit magnitude.
OracleUint128 RandomMagnitude(int digits) {
	OracleUint128 value {0, 0};
	for (int d = 0; d < digits; d++) {
		const uint32_t digit = static_cast<uint32_t>(NextRandom() >> 33) % 9 + 1;
		uint32_t limbs[4] = {static_cast<uint32_t>(value.hi >> 32), static_cast<uint32_t>(value.hi),
		                     static_cast<uint32_t>(value.lo >> 32), static_cast<uint32_t>(value.lo)};
		uint64_t carry = digit;
		for (int i = 3; i >= 0; i--) {
			const uint64_t current = static_cast<uint64_t>(limbs[i]) * 10 + carry;
			limbs[i] = static_cast<uint32_t>(current);
			carry = current >> 32;
		}
		value.hi = (static_cast<uint64_t>(limbs[0]) << 32) | limbs[1];
		value.lo = (static_cast<uint64_t>(limbs[2]) << 32) | limbs[3];
	}
	return value;
}

struct Column {
	std::vector<uint8_t> bytes = std::vector<uint8_t>(kValues * kStride);
	std::vector<uint8_t> lens = std::vector<uint8_t>(kValues);
};

// digits_min..digits_max random digits, stored with the given scale
void Fill(Column &c, int digits_min, int digits_max, uint8_t scale) {
	for (size_t i = 0; i < kValues; i++) {
		const int digits = digits_min + static_cast<int>((NextRandom() >> 40) % (digits_max - digits_min + 1));
		const bool negative = (NextRandom() >> 63) != 0;
		const size_t len = EncodeOracleNumber(RandomMagnitude(digits), negative, scale, &c.bytes[i * kStride]);
		if (len == 0) {
			std::fprintf(stderr, "value not encodable (%d digits, scale %d)\n", digits, scale);
			std::exit(1);
		}
		c.lens[i] = static_cast<uint8_t>(len);
	}
}

template <class F>
void Run(const char *name, const Column &c, F decode) {
	uint64_t checksum = 0;
	const auto start = std::chrono::steady_clock::now();
	for (size_t i = 0; i < kValues; i++) {
		checksum += decode(&c.bytes[i * kStride], c.lens[i]);
	}
	const auto stop = std::chrono::steady_clock::now();
	const double ns = std::chrono::duration<double, std::nano>(stop - start).count() / kValues;
	std::printf("%-44s %8.2f ns/value   checksum %llu\n", name, ns, static_cast<unsigned long long>(checksum));
}

} // namespace

int main() {
	Column c;
	Fill(c, 18, 18, 0);
	Run("Scaled64  NUMBER 18 digits, scale 0", c, [](const uint8_t *b, size_t n) {
		int64_t v = 0;
		DecodeOracleNumberScaled64(b, n, 0, false, v);
		return static_cast<uint64_t>(v);
	});
	Fill(c, 20, 38, 0);
	Run("Scaled128 NUMBER(38,0) 20-38 digits", c, [](const uint8_t *b, size_t n) {
		OracleInt128 v {0, 0};
		DecodeOracleNumberScaled128(b, n, 0, false, v);
		return v.lo ^ static_cast<uint64_t>(v.hi);
	});
	Fill(c, 25, 25, 9);
	Run("Scaled128 NUMBER(25,9) 25 digits, scale 9", c, [](const uint8_t *b, size_t n) {
		OracleInt128 v {0, 0};
		DecodeOracleNumberScaled128(b, n, 9, false, v);
		return v.lo ^ static_cast<uint64_t>(v.hi);
	});
	Fill(c, 25, 25, 4);
	Run("Text      25 digits, scale 4", c, [](const uint8_t *b, size_t n) {
		const std::string s = DecodeOracleNumberText(b, n);
		return static_cast<uint64_t>(s.size()) + static_cast<uint8_t>(s[s.size() / 2]);
	});
	return 0;
}
