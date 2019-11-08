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
#include "session.hpp"

namespace DNP3SAv6 { 
KeyWrapAlgorithm Session::getKeyWrapAlgorithm() const noexcept
{
    return key_wrap_algorithm_;
}

MACAlgorithm Session::getMACAlgorithm() const noexcept
{
    return mac_algorithm_;
}

boost::asio::const_buffer Session::getControlDirectionSessionKey() const noexcept
{
    return boost::asio::const_buffer(control_direction_session_key_, control_direction_session_key_size_);
}

boost::asio::const_buffer Session::getMonitoringDirectionSessionKey() const noexcept
{
    return boost::asio::const_buffer(monitoring_direction_session_key_, monitoring_direction_session_key_size_);
}

bool Session::valid() const noexcept
{
    return valid_;
}

void Session::reset() noexcept
{
    *this = Session();
}

}
