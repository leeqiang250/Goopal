#include <client/ClientImpl.hpp>

namespace goopal { namespace client { namespace detail {

#define UPDATE_CONFIG(name, value) DelegateConfig config = _delegate_config; \
                                   config.name = value; \
                                   config.validate(); \
                                   _delegate_config = config

fc::variant ClientImpl::delegate_get_config()const
{ try {
    return fc::variant( _delegate_config );
} FC_CAPTURE_AND_RETHROW() }

void ClientImpl::delegate_set_network_min_connection_count( uint32_t count )
{ try {
    UPDATE_CONFIG( network_min_connection_count, count );
} FC_CAPTURE_AND_RETHROW( (count) ) }

void ClientImpl::delegate_set_block_max_transaction_count( uint32_t count )
{ try {
    UPDATE_CONFIG( block_max_transaction_count, count );
} FC_CAPTURE_AND_RETHROW( (count) ) }

void ClientImpl::delegate_set_block_max_size( uint32_t size )
{ try {
    UPDATE_CONFIG( block_max_size, size );
} FC_CAPTURE_AND_RETHROW( (size) ) }

// void client_impl::delegate_set_block_max_production_time( uint64_t time )
// { try {
//     UPDATE_CONFIG( block_max_production_time, fc::microseconds( time ) );
// } FC_CAPTURE_AND_RETHROW( (time) ) }
void ClientImpl::delegate_set_imessage_fee_coe(const string& fee_coe)
{
	auto ipos = fee_coe.find(".");
	if (ipos != string::npos)
	{
		string str = fee_coe.substr(ipos + 1);
		int64_t precision_input = pow(10, str.size());
		FC_ASSERT((precision_input <= GOP_BLOCKCHAIN_PRECISION), "Precision is not correct");
	}
	double dFee = std::stod(fee_coe);
	ImessageLengthIdType feecoe = floor(dFee * GOP_BLOCKCHAIN_PRECISION + 0.5);
    try {
        UPDATE_CONFIG(transaction_imessage_min_fee_coe, feecoe);
    } FC_CAPTURE_AND_RETHROW((feecoe))
}
void ClientImpl::delegate_set_soft_max_imessage_length(int64_t soft_length)
{
    try {
        UPDATE_CONFIG(transaction_imessage_max_soft_length, soft_length);
    } FC_CAPTURE_AND_RETHROW((soft_length))
}
void ClientImpl::delegate_set_transaction_max_size( uint32_t size )
{ try {
    UPDATE_CONFIG( transaction_max_size, size );
} FC_CAPTURE_AND_RETHROW( (size) ) }

void ClientImpl::delegate_set_transaction_canonical_signatures_required( bool required )
{ try {
    UPDATE_CONFIG( transaction_canonical_signatures_required, required );
} FC_CAPTURE_AND_RETHROW( (required) ) }

void ClientImpl::delegate_set_transaction_min_fee( uint64_t fee )
{ try {
    UPDATE_CONFIG( transaction_min_fee, fee );
} FC_CAPTURE_AND_RETHROW( (fee) ) }

void ClientImpl::delegate_blacklist_add_transaction( const TransactionIdType& id )
{ try {
    _delegate_config.transaction_blacklist.insert( id );
} FC_CAPTURE_AND_RETHROW( (id) ) }

void ClientImpl::delegate_blacklist_remove_transaction( const TransactionIdType& id )
{ try {
    _delegate_config.transaction_blacklist.erase( id );
} FC_CAPTURE_AND_RETHROW( (id) ) }

void ClientImpl::delegate_blacklist_add_operation( const OperationTypeEnum& op )
{ try {
    _delegate_config.operation_blacklist.insert( op );
} FC_CAPTURE_AND_RETHROW( (op) ) }

void ClientImpl::delegate_blacklist_remove_operation( const OperationTypeEnum& op )
{ try {
    _delegate_config.operation_blacklist.erase( op );
} FC_CAPTURE_AND_RETHROW( (op) ) }

} } } // goopal::client::detail
