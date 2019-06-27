#ifndef dnp3sav6_keywrapalgorithm_hpp
#define dnp3sav6_keywrapalgorithm_hpp

static_assert(DNP3SAV6_PROFILE_HPP_INCLUDED, "profile.hpp should be pre-included in CMakeLists.txt");

#include <cstdint>

namespace DNP3SAv6 {
	enum struct KeyWrapAlgorithm : std::uint8_t
	{
		// key wrap as defined by NIST SP800-38F using AES-256 in GCM mode.
		  nist_sp800_38f_aes_256__ = 2
	};
}

#endif



