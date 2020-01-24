#include "securitylayer.hpp"
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
#include "securitylayer.hpp"
#include "exceptions/contract.hpp"
#include <openssl/crypto.h>
#include "messages.hpp"
#include "hmac.hpp"
#include "aead.hpp"

static_assert(DNP3SAV6_PROFILE_HPP_INCLUDED, "profile.hpp should be pre-included in CMakeLists.txt");

using namespace std;
using namespace boost::asio;

namespace DNP3SAv6 {
SecurityLayer::SecurityLayer(
	  boost::asio::io_context &io_context
    , std::uint16_t association_id
	, Config config
	, Details::IRandomNumberGenerator &random_number_generator
	)
	: config_(config)
	, random_number_generator_(random_number_generator)
	, timeout_(io_context)
    , association_id_(association_id)
{
	memset(statistics_, 0, sizeof(statistics_));
}

void SecurityLayer::onLinkLost() noexcept
{
	reset();
}
void SecurityLayer::onApplicationReset() noexcept
{
	reset();
}
void SecurityLayer::onAPDUTimeout() noexcept
{
    cancelPendingAPDU();
}
void SecurityLayer::cancelPendingAPDU() noexcept
{
	switch (state_)
	{
	case initial__ :
	case expect_session_start_request__ :
		reset();
		break;
	case expect_session_start_response__ :
	case expect_set_keys__ :
	case expect_session_confirmation__ :
	case active__ :
		discardAPDU();
		// no state change
		break;
	}
}
void SecurityLayer::postAPDU(const_buffer const &apdu) noexcept
{
	pre_condition(apdu.size() <= sizeof(outgoing_apdu_buffer_));

	// if any APDU is pending, it must be discarded: we cannot have more than one APDU at a 
	// time and the Application Layer must be in charge of what gets sent.
	discardAPDU();
	queueAPDU(apdu);
	onPostAPDU(apdu);
}
void SecurityLayer::postSPDU(const_buffer const &spdu) noexcept
{
	pre_condition(spdu.size() <= sizeof(incoming_spdu_buffer_));

	incrementStatistic(Statistics::total_messages_received__);

	memcpy(incoming_spdu_buffer_, spdu.data(), spdu.size());
	incoming_spdu_ = const_buffer(incoming_spdu_buffer_, spdu.size());

	parseIncomingSPDU();
}

bool SecurityLayer::pollAPDU() const noexcept
{
	return incoming_apdu_.size() != 0;
}
bool SecurityLayer::pollSPDU() const noexcept
{
	return outgoing_spdu_.size() != 0;
}

const_buffer SecurityLayer::getAPDU() noexcept
{
	const_buffer const retval(incoming_apdu_);
	incoming_apdu_ = const_buffer();

	return retval;
}
const_buffer SecurityLayer::getSPDU() noexcept
{
	const_buffer const retval(outgoing_spdu_);
	outgoing_spdu_= const_buffer();

	return retval;
}

std::pair< SecurityLayer::UpdateResult, boost::asio::steady_timer::duration> SecurityLayer::update() noexcept
{
    if (pollSPDU())
    {
        return make_pair(UpdateResult::spdu_ready__, boost::asio::steady_timer::duration(0));
    }
    else if (pollAPDU())
    {
        return make_pair(UpdateResult::apdu_ready__, boost::asio::steady_timer::duration(0));
    }
    if (state_ == State::active__)
    {
        if (outgoing_apdu_.size())
        {
            onPostAPDU(outgoing_apdu_);
            return update();
        }
        else
        { /* no APDU to handle */ }
    }
    else
    { /* SA protocol is still driving */ }
    return make_pair(UpdateResult::wait__, timeout_.expires_from_now());
}

/*virtual */bool SecurityLayer::acceptKeyWrapAlgorithm(KeyWrapAlgorithm incoming_kwa) const noexcept
{
	return true;
}

/*virtual */bool SecurityLayer::acceptMACAlgorithm(AEADAlgorithm incoming_mal) const noexcept
{
	return true;
}

/*virtual */KeyWrapAlgorithm SecurityLayer::getPreferredKeyWrapAlgorithm() const noexcept
{
	return KeyWrapAlgorithm::rfc3394_aes256_key_wrap__;
}

/*virtual */AEADAlgorithm SecurityLayer::getPreferredMACAlgorithm() const noexcept
{
	return AEADAlgorithm::hmac_sha_256_truncated_16__;
}

void SecurityLayer::reset() noexcept
{
	discardAPDU();
	state_ = initial__;
	incoming_apdu_ = const_buffer();
	incoming_spdu_ = const_buffer();
	outgoing_apdu_ = const_buffer();
	outgoing_spdu_ = const_buffer();
	seq_ = 0;
	seq_validator_.reset();
	//TODO make sure incoming authenticated APDUs don't affect this if we don't have a session 
	//TODO make sure this is reset if a new session is created (I think it already is)
	session_.reset();
	// don't touch statistics
	//TODO reset the timer
	
}

void SecurityLayer::setOutgoingSPDU(
	  boost::asio::const_buffer const &spdu
	, boost::asio::steady_timer::duration const &timeout/* = std::chrono::milliseconds(0)*/
	) noexcept
{
	timeout_.expires_after(timeout);
	outgoing_spdu_ = spdu;
}

void SecurityLayer::discardAPDU() noexcept
{
	if (outgoing_apdu_.size())
	{
		incrementStatistic(Statistics::discarded_messages__);
	}
	else
	{ /* no pending APDU - nothing to increment */ }
	outgoing_apdu_ = const_buffer();
}

void SecurityLayer::queueAPDU(boost::asio::const_buffer const &apdu) noexcept
{
	memcpy(outgoing_apdu_buffer_, apdu.data(), apdu.size());
	outgoing_apdu_ = const_buffer(outgoing_apdu_buffer_, apdu.size());
}

boost::asio::const_buffer SecurityLayer::formatAuthenticatedAPDU(Direction direction, boost::asio::const_buffer const &apdu) noexcept
{
    invariant(getSession().valid());

    size_t const needed_space((8/*SPDU header size*/) + sizeof(Messages::AuthenticatedAPDU) + apdu.size() + getAEADAlgorithmAuthenticationTagSize(session_.getAEADAlgorithm()));

    pre_condition(needed_space <= sizeof(outgoing_spdu_buffer_));

	outgoing_spdu_size_ = 0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0xC0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x80;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x01;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = static_cast< unsigned char >(Message::secure_message__);

    static_assert(sizeof(association_id_) == 2, "wrong size (type) for association_id_");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &association_id_, sizeof(association_id_));
	outgoing_spdu_size_ += sizeof(association_id_);

    const_buffer associated_data_buffer(outgoing_spdu_buffer_, outgoing_spdu_size_);

	static_assert(sizeof(seq_) == 2, "wrong size (type) for seq_");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &seq_, sizeof(seq_));
    const_buffer nonce_buffer(outgoing_spdu_buffer_ + outgoing_spdu_size_, sizeof(seq_));
	outgoing_spdu_size_ += sizeof(seq_);

    assert(outgoing_spdu_size_ == (8/*SPDU header size*/));

    pre_condition(apdu.size() < numeric_limits< decltype(Messages::AuthenticatedAPDU::apdu_length_) >::max());
    Messages::AuthenticatedAPDU authenticated_apdu(static_cast< decltype(Messages::AuthenticatedAPDU::apdu_length_) >(apdu.size()));
    unsigned char authenticated_apdu_serialized[sizeof(authenticated_apdu)];
    memcpy(authenticated_apdu_serialized, &authenticated_apdu, sizeof(authenticated_apdu));
    const_buffer authenticated_apdu_serialized_buffer(authenticated_apdu_serialized, sizeof(authenticated_apdu_serialized));

    mutable_buffer output_buffer(outgoing_spdu_buffer_ + outgoing_spdu_size_, needed_space - outgoing_spdu_size_);
    auto encrypt_result(
          encrypt(
              output_buffer
            , getSession().getAEADAlgorithm()
            , direction == Direction::controlling__ ? getSession().getControlDirectionSessionKey() : getSession().getMonitoringDirectionSessionKey()
            , nonce_buffer
            , associated_data_buffer
            , authenticated_apdu_serialized_buffer
            , apdu
            )
        );
    outgoing_spdu_size_ += encrypt_result.size();

	return const_buffer(outgoing_spdu_buffer_, outgoing_spdu_size_);
}

boost::asio::const_buffer SecurityLayer::format(Messages::RequestSessionInitiation const &rsi) noexcept
{
    invariant(!getSession().valid());

	static_assert(sizeof(outgoing_spdu_buffer_) >= (8/*SPDU header size*/), "buffer too small");
	outgoing_spdu_size_ = 0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0xC0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x80;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x01;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = static_cast< unsigned char >(Message::session_initiation__);

    static_assert(sizeof(association_id_) == 2, "wrong size (type) for association_id_");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &association_id_, sizeof(association_id_));
	outgoing_spdu_size_ += sizeof(association_id_);

    static_assert(sizeof(seq_) == 2, "wrong size (type) for seq_");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &seq_, sizeof(seq_));
	outgoing_spdu_size_ += sizeof(seq_);

    assert(outgoing_spdu_size_ == (8/*SPDU header size*/));

	return const_buffer(outgoing_spdu_buffer_, outgoing_spdu_size_);
}

boost::asio::const_buffer SecurityLayer::format(Messages::SessionStartRequest const &ssr) noexcept
{
	static_assert(sizeof(outgoing_spdu_buffer_) >= (8/*SPDU header size*/) + sizeof(ssr), "buffer too small");
	outgoing_spdu_size_ = 0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0xC0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x80;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x01;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = static_cast< unsigned char >(Message::session_start_request__);

    static_assert(sizeof(association_id_) == 2, "wrong size (type) for association_id_");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &association_id_, sizeof(association_id_));
	outgoing_spdu_size_ += sizeof(association_id_);
    
    static_assert(sizeof(seq_) == 2, "wrong size (type) for seq_");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &seq_, sizeof(seq_));
	outgoing_spdu_size_ += sizeof(seq_);
	
    assert(outgoing_spdu_size_ == (8/*SPDU header size*/));

	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &ssr, sizeof(ssr));
	outgoing_spdu_size_ += sizeof(ssr);
	assert(outgoing_spdu_size_ == (8/*SPDU header size*/) + sizeof(ssr));

	return const_buffer(outgoing_spdu_buffer_, outgoing_spdu_size_);
}

boost::asio::const_buffer SecurityLayer::format(std::uint16_t seq, Messages::SessionStartResponse const &ssr, const_buffer const &nonce) noexcept
{
	pre_condition(sizeof(outgoing_spdu_buffer_) >= (8/*SPDU header size*/) + sizeof(ssr) + nonce.size());
	
	outgoing_spdu_size_ = 0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0xC0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x80;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x01;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = static_cast< unsigned char >(Message::session_start_response__);

    static_assert(sizeof(association_id_) == 2, "wrong size (type) for association_id_");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &association_id_, sizeof(association_id_));
	outgoing_spdu_size_ += sizeof(association_id_);

	static_assert(sizeof(seq) == sizeof(seq_), "wrong size (type) for seq");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &seq, sizeof(seq));
	outgoing_spdu_size_ += sizeof(seq);

	assert(outgoing_spdu_size_ == (8/*SPDU header size*/));

	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &ssr, sizeof(ssr));
	outgoing_spdu_size_ += sizeof(ssr);
	assert(outgoing_spdu_size_ == (8/*SPDU header size*/) + sizeof(ssr));

	assert(ssr.challenge_data_length_ == nonce.size());
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, nonce.data(), nonce.size());
	outgoing_spdu_size_ += nonce.size();

	return const_buffer(outgoing_spdu_buffer_, outgoing_spdu_size_);
}

const_buffer SecurityLayer::format(Messages::SetSessionKeys const &sk, const_buffer const &wrapped_key_data) noexcept
{
	pre_condition(sizeof(outgoing_spdu_buffer_) >= (8/*SPDU header size*/) + sizeof(sk) + wrapped_key_data.size());

	outgoing_spdu_size_ = 0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0xC0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x80;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x01;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = static_cast< unsigned char >(Message::session_key_change__);

    static_assert(sizeof(association_id_) == 2, "wrong size (type) for association_id_");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &association_id_, sizeof(association_id_));
	outgoing_spdu_size_ += sizeof(association_id_);

	static_assert(sizeof(seq_) == 2, "wrong size (type) for seq_");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &seq_, sizeof(seq_));
	outgoing_spdu_size_ += sizeof(seq_);

	assert(outgoing_spdu_size_ == (8/*SPDU header size*/));

	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &sk, sizeof(sk));
	outgoing_spdu_size_ += sizeof(sk);
	assert(outgoing_spdu_size_ == (8/*SPDU header size*/) + sizeof(sk));

	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, wrapped_key_data.data(), wrapped_key_data.size());
	outgoing_spdu_size_ += wrapped_key_data.size();
	assert(outgoing_spdu_size_ == (8/*SPDU header size*/) + sizeof(sk) + wrapped_key_data.size());

	return const_buffer(outgoing_spdu_buffer_, outgoing_spdu_size_);
}

boost::asio::const_buffer SecurityLayer::format(std::uint16_t seq, Messages::SessionConfirmation const &sc, boost::asio::const_buffer const &digest) noexcept
{
	pre_condition(sizeof(outgoing_spdu_buffer_) >= (8/*SPDU header size*/) + sizeof(sc) + sc.mac_length_);
    pre_condition(sc.mac_length_ <= digest.size());
	outgoing_spdu_size_ = 0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0xC0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x80;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x01;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = static_cast< unsigned char >(Message::session_key_change_confirmation__);

    static_assert(sizeof(association_id_) == 2, "wrong size (type) for association_id_");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &association_id_, sizeof(association_id_));
	outgoing_spdu_size_ += sizeof(association_id_);

    static_assert(sizeof(seq) == sizeof(seq_), "wrong size (type) for seq");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &seq, sizeof(seq));
	outgoing_spdu_size_ += sizeof(seq);

	assert(outgoing_spdu_size_ == (8/*SPDU header size*/));
	
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &sc, sizeof(sc));
	outgoing_spdu_size_ += sizeof(sc);
	assert(outgoing_spdu_size_ == (8/*SPDU header size*/) + sizeof(sc));

    memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, digest.data(), sc.mac_length_);
    outgoing_spdu_size_ += sc.mac_length_;

	return const_buffer(outgoing_spdu_buffer_, outgoing_spdu_size_);
}

boost::asio::const_buffer SecurityLayer::format(Messages::Error const &e) noexcept
{
	return format(seq_, e);
}

boost::asio::const_buffer SecurityLayer::format(std::uint16_t seq, Messages::Error const &e) noexcept
{
	static_assert(sizeof(outgoing_spdu_buffer_) >= (8/*SPDU header size*/) + sizeof(e), "buffer too small");
	outgoing_spdu_size_ = 0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0xC0;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x80;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = 0x01;
	outgoing_spdu_buffer_[outgoing_spdu_size_++] = static_cast< unsigned char >(Message::session_initiation__);

    static_assert(sizeof(association_id_) == 2, "wrong size (type) for association_id_");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &association_id_, sizeof(association_id_));
	outgoing_spdu_size_ += sizeof(association_id_);

	static_assert(sizeof(seq) == sizeof(seq_), "wrong size (type) for seq");
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &seq, sizeof(seq));
	outgoing_spdu_size_ += sizeof(seq);
	
    assert(outgoing_spdu_size_ == (8/*SPDU header size*/));
	
	memcpy(outgoing_spdu_buffer_ + outgoing_spdu_size_, &e, sizeof(e));
	outgoing_spdu_size_ += sizeof(e);
	assert(outgoing_spdu_size_ == (8/*SPDU header size*/) + sizeof(e));

	return const_buffer(outgoing_spdu_buffer_, outgoing_spdu_size_);
}

void SecurityLayer::incrementStatistic(Statistics statistic) noexcept
{
	++statistics_[static_cast< int >(statistic)];
}

unsigned int SecurityLayer::getStatistic(Statistics statistic) noexcept
{
	return statistics_[static_cast< int >(statistic)];
}

/*virtual */void SecurityLayer::rxRequestSessionInitiation(uint32_t incoming_seq, boost::asio::const_buffer const &spdu) noexcept
{
	incrementStatistic(Statistics::unexpected_messages__);
}
/*virtual */void SecurityLayer::rxSessionStartRequest(uint32_t incoming_seq, Messages::SessionStartRequest const &incoming_ssr, boost::asio::const_buffer const &spdu) noexcept
{
	incrementStatistic(Statistics::unexpected_messages__);
}
/*virtual */void SecurityLayer::rxSessionStartResponse(uint32_t incoming_seq, Messages::SessionStartResponse const &incoming_ssr, boost::asio::const_buffer const &nonce, boost::asio::const_buffer const &spdu) noexcept
{
	incrementStatistic(Statistics::unexpected_messages__);
}
/*virtual */void SecurityLayer::rxSetSessionKeys(uint32_t incoming_seq, Messages::SetSessionKeys const& incoming_ssk, boost::asio::const_buffer const& incoming_key_wrap_data, boost::asio::const_buffer const& spdu) noexcept
{
    incrementStatistic(Statistics::unexpected_messages__);
}

/*virtual */void SecurityLayer::rxSessionConfirmation(std::uint32_t incoming_seq, Messages::SessionConfirmation const &incoming_sc, boost::asio::const_buffer const &incoming_mac, boost::asio::const_buffer const& spdu) noexcept
{
    incrementStatistic(Statistics::unexpected_messages__);
}

void SecurityLayer::rxAuthenticatedAPDU(std::uint32_t incoming_seq, boost::asio::const_buffer const& incoming_nonce, boost::asio::const_buffer const& incoming_associated_data, boost::asio::const_buffer const& incoming_payload, boost::asio::const_buffer const& incoming_spdu) noexcept
{
    /* NOTE we don't check the state here: if we have a valid session, we use it.
     *      This means we can receive authenticated APDUs while a new session is being built.
     *      Whether the other side is actually capable of sending them in the current state
     *      is another matter. */
    if (!getSession().valid())
    {
        incrementStatistic(Statistics::unexpected_messages__);
        return;
    }
    else
    { /* all is well */ }

    using Details::SEQValidator;
    switch (seq_validator_.validateSEQ(incoming_seq))
    {
    case SEQValidator::invalid_seq__ :
        incrementStatistic(Statistics::discarded_messages__);
        //TODO send an error in this case?
        return;
    case SEQValidator::old_seq__     :
        // either a replay attack or a network error. We shouldn't send an error message in either case
        //TODO log?
        incrementStatistic(Statistics::discarded_messages__); //TODO unexpected?
        return;
    case SEQValidator::repeat_seq__  :
        //TODO increment stats?
        return;
    case SEQValidator::next_seq__    :
    case SEQValidator::new_seq__     :
        break;
    }

    auto incoming_apdu(
          decrypt(
              mutable_buffer(incoming_apdu_buffer_, sizeof(incoming_apdu_buffer_))
            , getSession().getAEADAlgorithm()
            , getIncomingDirection() == Direction::controlling__ ? getSession().getControlDirectionSessionKey() : getSession().getMonitoringDirectionSessionKey()
            , incoming_nonce
            , incoming_associated_data
            , incoming_payload
            )
        );

    if (!incoming_apdu.data()) // auth failure
    {   //TODO message if in maintenance mode
        //TODO statistics
        return;
    }
    else
    { /* all is well */ }

    assert(incoming_apdu.data() == incoming_apdu_buffer_);
    assert(incoming_apdu.size() <= sizeof(incoming_apdu_buffer_));

    unsigned char const *curr(static_cast< unsigned char const * >(incoming_apdu.data()));
    unsigned char const *const end(curr + incoming_apdu.size());

    Messages::AuthenticatedAPDU incoming_aa;
    if (static_cast< size_t >(distance(curr, end)) < sizeof(incoming_aa)) // invalid message
    {   //TODO message if in maintenance mode
        //TODO statistics
        return;
    }
    else
    { /* all is well */ }
    memcpy(&incoming_aa, curr, sizeof(incoming_aa));
    curr += sizeof(incoming_aa);

    if (incoming_aa.apdu_length_ != distance(curr, end))
    { //TODO handle maintenance mode
        const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
        setOutgoingSPDU(response_spdu);
        incrementStatistic(Statistics::error_messages_sent__);
        incrementStatistic(Statistics::total_messages_sent__);
        return;
    }
    else
    { /* all is well */ }

    incoming_apdu_ = const_buffer(curr, incoming_aa.apdu_length_);
    seq_validator_.setLatestIncomingSEQ(incoming_seq);
}

void SecurityLayer::parseIncomingSPDU() noexcept
{
	unsigned char const *incoming_spdu_data(static_cast< unsigned char const* >(incoming_spdu_.data()));
	unsigned char const *curr(incoming_spdu_data);
	unsigned char const *const end(curr + incoming_spdu_.size());

    unsigned char const *const associated_data_begin(curr);
	if (distance(curr, end) < (8/*SPDU header size*/))
	{
		const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
		setOutgoingSPDU(response_spdu);
		incrementStatistic(Statistics::error_messages_sent__);
		incrementStatistic(Statistics::total_messages_sent__);
	}
	else
	{ /* it's at least big enough to be a valid SPDU */ }

	static const unsigned char preamble__[] = { 0xC0, 0x80, 0x01 };
	if (!equal(curr, curr + sizeof(preamble__), preamble__))
	{
		const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
		setOutgoingSPDU(response_spdu);
		incrementStatistic(Statistics::error_messages_sent__);
		incrementStatistic(Statistics::total_messages_sent__);
	}
	else
	{ /* preamble is OK */ }
	curr += 3;

	unsigned char const incoming_function_code(*curr++);

    std::uint16_t incoming_association_id;
    static_assert(sizeof(incoming_association_id) == 2, "wrong size (type) for incoming_association_id");
    memcpy(&incoming_association_id, curr, sizeof(incoming_association_id));
    curr += 2;

    if (incoming_association_id != association_id_)
    {   //TODO stat? log? just ignore?
        return;
    }
    else
    { /* all is well */ }

    unsigned char const *const associated_data_end(curr);

	decltype(seq_) incoming_seq;
	memcpy(&incoming_seq, curr, sizeof(incoming_seq));
    unsigned char const *nonce_begin(curr);
	curr += sizeof(incoming_seq);
    unsigned char const *nonce_end(curr);

	switch (incoming_function_code)
	{
	case static_cast< uint8_t >(Message::session_initiation__) :
		rxRequestSessionInitiation(incoming_seq, incoming_spdu_);
		break;
	case static_cast< uint8_t >(Message::session_start_request__) :
		// check the SPDU size to see if it's big enough to hold a SessionStartRequest message
		// if so, parse into a SessionStartRequest object and call rxSessionStartRequest(incoming_seq, incoming_ssr);
		if (incoming_spdu_.size() == sizeof(Messages::SessionStartRequest) + (8/*header size*/))
		{
			Messages::SessionStartRequest incoming_ssr;
			assert(distance(curr, end) == sizeof(incoming_ssr));
			incoming_ssr.version_ = *curr++;
			if (incoming_ssr.version_ != 6)
			{
				const_buffer response_spdu(format(Messages::Error(Messages::Error::unsupported_version__)));
				setOutgoingSPDU(response_spdu);
				incrementStatistic(Statistics::error_messages_sent__);
				incrementStatistic(Statistics::total_messages_sent__);
				break;
			}
			else
			{ /* all is well as far as the version is concerned */ }
 			incoming_ssr.flags_ = *curr++;
    
			incoming_ssr.key_wrap_algorithm_ = *curr++;
			incoming_ssr.aead_algorithm_ = *curr++;
			memcpy(&incoming_ssr.session_key_change_interval_, curr, 4);
			curr += 4;
			memcpy(&incoming_ssr.session_key_change_count_, curr, 2);
			curr += 2;
			assert(curr == end);
			rxSessionStartRequest(incoming_seq, incoming_ssr, incoming_spdu_);
		}
		else
		{
			const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
			setOutgoingSPDU(response_spdu);
			incrementStatistic(Statistics::error_messages_sent__);
			incrementStatistic(Statistics::total_messages_sent__);
		}
		break;
	case static_cast< uint8_t >(Message::session_start_response__) :
	{
		unsigned int const min_expected_spdu_size((8/*header size*/) + sizeof(Messages::SessionStartResponse) + Config::min_nonce_size__);
		unsigned int const max_expected_spdu_size((8/*header size*/) + sizeof(Messages::SessionStartResponse) + Config::max_nonce_size__);
		if ((incoming_spdu_.size() >= min_expected_spdu_size) && (incoming_spdu_.size() <= max_expected_spdu_size))
		{
			Messages::SessionStartResponse incoming_ssr;
			assert(static_cast< size_t >(distance(curr, end)) > sizeof(incoming_ssr));
			memcpy(&incoming_ssr, curr, sizeof(incoming_ssr));
			curr += sizeof(incoming_ssr);

			if (incoming_ssr.challenge_data_length_ == distance(curr, end))
			{
				const_buffer nonce(curr, distance(curr, end));
				rxSessionStartResponse(incoming_seq, incoming_ssr, nonce, incoming_spdu_);
			}
			else
			{
				const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
				setOutgoingSPDU(response_spdu);
				incrementStatistic(Statistics::error_messages_sent__);
				incrementStatistic(Statistics::total_messages_sent__);
			}
		}
		else
		{
			const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
			setOutgoingSPDU(response_spdu);
			incrementStatistic(Statistics::error_messages_sent__);
			incrementStatistic(Statistics::total_messages_sent__);
		}

		// check the SPDU size to see if it's big enough to hold a SessionStartResponse message
		// if so, parse into a SessionStartResponse object and call rxSessionStartResponse(incoming_seq, incoming_ssr);
		break;
	}
	case static_cast< uint8_t >(Message::session_key_change__) :
    {
        // check the SPDU size to see if it's big enough to hold a SetSessionKeys message
        unsigned int const min_expected_spdu_size((8/*header size*/) + sizeof(Messages::SetSessionKeys));
        unsigned int const max_expected_spdu_size((8/*header size*/) + sizeof(Messages::SetSessionKeys) + Config::max_key_wrap_data_size__);
        if ((incoming_spdu_.size() >= min_expected_spdu_size) && (incoming_spdu_.size() <= max_expected_spdu_size))
        {
            Messages::SetSessionKeys incoming_ssk;
            assert(static_cast< size_t >(distance(curr, end)) >= sizeof(incoming_ssk));
            memcpy(&incoming_ssk, curr, sizeof(incoming_ssk));
            curr += sizeof(incoming_ssk);

            if (incoming_ssk.key_wrap_data_length_ == distance(curr, end))
            {
                const_buffer incoming_key_wrap_data(curr, distance(curr, end));
                rxSetSessionKeys(incoming_seq, incoming_ssk, incoming_key_wrap_data, incoming_spdu_);
            }
            else
            {
                const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
                setOutgoingSPDU(response_spdu);
                incrementStatistic(Statistics::error_messages_sent__);
                incrementStatistic(Statistics::total_messages_sent__);
            }
        }
        else
        {
            const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
            setOutgoingSPDU(response_spdu);
            incrementStatistic(Statistics::error_messages_sent__);
            incrementStatistic(Statistics::total_messages_sent__);
        }
        break;
    }
    case static_cast< uint8_t >(Message::session_key_change_confirmation__) :
    {
        unsigned int const min_expected_spdu_size((8/*header size*/) + sizeof(Messages::SessionConfirmation));
        unsigned int const max_expected_spdu_size((8/*header size*/) + sizeof(Messages::SessionConfirmation) + Config::max_digest_size__);
        if ((incoming_spdu_.size() >= min_expected_spdu_size) && (incoming_spdu_.size() <= max_expected_spdu_size))
        {
            Messages::SessionConfirmation incoming_sc;
            assert(static_cast< size_t >(distance(curr, end)) > sizeof(incoming_sc));
            memcpy(&incoming_sc, curr, sizeof(incoming_sc));
            curr += sizeof(incoming_sc);

            if (incoming_sc.mac_length_ == distance(curr, end))
            {
                const_buffer incoming_mac(curr, distance(curr, end));
                rxSessionConfirmation(incoming_seq, incoming_sc, incoming_mac, incoming_spdu_);
            }
            else
            {
                const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
                setOutgoingSPDU(response_spdu);
                incrementStatistic(Statistics::error_messages_sent__);
                incrementStatistic(Statistics::total_messages_sent__);
            }
        }
        else
        {
            const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
            setOutgoingSPDU(response_spdu);
            incrementStatistic(Statistics::error_messages_sent__);
            incrementStatistic(Statistics::total_messages_sent__);
        }
		break;
    }
	case static_cast< uint8_t >(Message::secure_message__) :
    {
        unsigned int const min_expected_spdu_size((8/*header size*/) + sizeof(Messages::AuthenticatedAPDU) + 2/*minimal size of an APDU in either direction is two bytes, for the header with no objects*/ + getAEADAlgorithmAuthenticationTagSize(getSession().getAEADAlgorithm()));
        unsigned int const max_expected_spdu_size(Config::max_spdu_size__);
        if ((incoming_spdu_.size() >= min_expected_spdu_size) && (incoming_spdu_.size() <= max_expected_spdu_size))
        {
            const_buffer incoming_nonce(nonce_begin, distance(nonce_begin, nonce_end));
            const_buffer incoming_associated_data(associated_data_begin, distance(associated_data_begin, associated_data_end));
            const_buffer incoming_payload(curr, distance(curr, end));
            rxAuthenticatedAPDU(incoming_seq, incoming_nonce, incoming_associated_data, incoming_payload, incoming_spdu_);
        }
        else
        {
            const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
            setOutgoingSPDU(response_spdu);
            incrementStatistic(Statistics::error_messages_sent__);
            incrementStatistic(Statistics::total_messages_sent__);
        }
		break;
    }
	case static_cast< uint8_t >(Message::error__) :
        //TODO
		// check the SPDU size to see if it's big enough to hold an Error message
		// if so, parse into a Error object and call rxError(incoming_seq, incoming_error);
		break;
	default :
		const_buffer response_spdu(format(Messages::Error(Messages::Error::invalid_spdu__)));
		setOutgoingSPDU(response_spdu);
		incrementStatistic(Statistics::error_messages_sent__);
		incrementStatistic(Statistics::total_messages_sent__);
	}
}
}


