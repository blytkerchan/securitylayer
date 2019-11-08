/* Copyright 2019  Ronald Landheer-Cieslak
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); 
 * you may not use this file except in compliance with the License. 
 * You may obtain a copy of the License at 
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software 
 * distributed under the License is distributed on an "AS IS" BASIS, 
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
 * See the License for the specific language governing permissions and 
 * limitations under the License. */
#ifndef dnp3sav6_messages_hpp
#define dnp3sav6_messages_hpp

static_assert(DNP3SAV6_PROFILE_HPP_INCLUDED, "profile.hpp should be pre-included in CMakeLists.txt");

#include <cstdint>
#include "messages/error.hpp"
#include "messages/requestsessioninitiation.hpp"
#include "messages/sessionconfirmation.hpp"
#include "messages/sessionstartrequest.hpp"
#include "messages/sessionstartresponse.hpp"
#include "messages/setsessionkeys.hpp"
#include "messages/authenticatedapdu.hpp"

namespace DNP3SAv6 {
	enum struct Message : std::uint8_t
	{
		  request_session_initiation__	                    = 0x01
		, session_start_request__		                    = 0x02
		, session_start_response__		                    = 0x03
		, set_session_keys__			                    = 0x04
		, session_confirmation__		                    = 0x05
		, authenticated_apdu__			                    = 0x06
		, error__						                    = 0x20 // must be 0x20 to be able to mimic IIN2.5
	};
}

#endif



