#include <sstream>
#include <iomanip>
#include <deque>
#include <unordered_set>
#include <list>
#include <forward_list>
#include <iostream>
#include <algorithm>
#include <tuple>
#include <boost/tuple/tuple.hpp>
#include <boost/circular_buffer.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/tag.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/logic/tribool.hpp>
#include <boost/range/algorithm_ext/push_back.hpp>
#include <boost/range/numeric.hpp>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/rolling_mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/sum.hpp>
#include <boost/accumulators/statistics/count.hpp>

#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/stringize.hpp>

#include <fc/thread/thread.hpp>
#include <fc/thread/future.hpp>
#include <fc/thread/non_preemptable_scope_check.hpp>
#include <fc/thread/mutex.hpp>
#include <fc/thread/scoped_lock.hpp>
#include <fc/log/logger.hpp>
#include <fc/io/json.hpp>
#include <fc/io/enum_type.hpp>
#include <fc/crypto/rand.hpp>
#include <fc/network/rate_limiting.hpp>

#include <net/Node.hpp>
#include <net/PeerDatabase.hpp>
#include <net/PeerConnection.hpp>
#include <net/StcpSocket.hpp>
#include <net/Config.hpp>
#include <net/Exceptions.hpp>

#include <client/Messages.hpp>

#include <utilities/GitRevision.hpp>
#include <fc/git_revision.hpp>

//#define ENABLE_DEBUG_ULOGS

#ifdef DEFAULT_LOGGER
# undef DEFAULT_LOGGER
#endif
#define DEFAULT_LOGGER "p2p"

#define P2P_IN_DEDICATED_THREAD 1

#define INVOCATION_COUNTER(name) \
    static unsigned total_ ## name ## _counter = 0; \
    static unsigned active_ ## name ## _counter = 0; \
    struct name ## _invocation_logger { \
      unsigned *total; \
      unsigned *active; \
      name ## _invocation_logger(unsigned *total, unsigned *active) : \
        total(total), active(active) \
      { \
        ++*total; \
        ++*active; \
        dlog("NEWDEBUG: Entering " #name ", now ${total} total calls, ${active} active calls", ("total", *total)("active", *active)); \
      } \
      ~name ## _invocation_logger() \
      { \
        --*active; \
        dlog("NEWDEBUG: Leaving " #name ", now ${total} total calls, ${active} active calls", ("total", *total)("active", *active)); \
      } \
    } invocation_logger(&total_ ## name ## _counter, &active_ ## name ## _counter)

//log these messages even at warn level when operating on the test network
#ifdef GOP_TEST_NETWORK
#define testnetlog wlog
#else
#define testnetlog(...) do {} while (0)
#endif

namespace goopal { namespace net {

  namespace detail
  {
    namespace bmi = boost::multi_index;
    class BlockchainTiedMessageCache
    {
    private:
      static const uint32_t cache_duration_in_blocks = 2;

      struct MessageHashIndex{};
      struct MessageContentsHashIndex{};
      struct BlockClockIndex{};
      struct MessageInfo
      {
        MessageHashType message_hash;
        Message           message_body;
        uint32_t          block_clock_when_received;

        // for network performance stats
        MessagePropagationData propagation_data;
        fc::uint160_t     message_contents_hash; // hash of whatever the message contains (if it's a transaction, this is the transaction id, if it's a block, it's the block_id)

        MessageInfo( const MessageHashType& message_hash,
                      const Message&           message_body,
                      uint32_t                 block_clock_when_received,
                      const MessagePropagationData& propagation_data,
                      fc::uint160_t            message_contents_hash ) :
          message_hash( message_hash ),
          message_body( message_body ),
          block_clock_when_received( block_clock_when_received ),
          propagation_data( propagation_data ),
          message_contents_hash( message_contents_hash )
        {}
      };
      typedef boost::multi_index_container
        < MessageInfo,
            bmi::indexed_by< bmi::ordered_unique< bmi::tag<MessageHashIndex>,
                                                  bmi::member<MessageInfo, MessageHashType, &MessageInfo::message_hash> >,
                             bmi::ordered_non_unique< bmi::tag<MessageContentsHashIndex>,
                                                      bmi::member<MessageInfo, fc::uint160_t, &MessageInfo::message_contents_hash> >,
                             bmi::ordered_non_unique< bmi::tag<BlockClockIndex>,
                                                      bmi::member<MessageInfo, uint32_t, &MessageInfo::block_clock_when_received> > >
        > MessageCacheContainer;

      MessageCacheContainer _message_cache;

      uint32_t block_clock;

    public:
      BlockchainTiedMessageCache() :
        block_clock( 0 )
      {}
      void block_accepted();
      void cache_message( const Message& message_to_cache, const MessageHashType& hash_of_message_to_cache,
                        const MessagePropagationData& propagation_data, const fc::uint160_t& message_content_hash );
      Message get_message( const MessageHashType& hash_of_message_to_lookup );
      MessagePropagationData get_message_propagation_data( const fc::uint160_t& hash_of_message_contents_to_lookup ) const;
      size_t size() const { return _message_cache.size(); }
    };

    void BlockchainTiedMessageCache::block_accepted()
    {
      ++block_clock;
      if( block_clock > cache_duration_in_blocks )
        _message_cache.get<BlockClockIndex>().erase(_message_cache.get<BlockClockIndex>().begin(),
                                                      _message_cache.get<BlockClockIndex>().lower_bound(block_clock - cache_duration_in_blocks ) );
    }

    void BlockchainTiedMessageCache::cache_message( const Message& message_to_cache,
                                                     const MessageHashType& hash_of_message_to_cache,
                                                     const MessagePropagationData& propagation_data,
                                                     const fc::uint160_t& message_content_hash )
    {
      _message_cache.insert( MessageInfo(hash_of_message_to_cache,
                                         message_to_cache,
                                         block_clock,
                                         propagation_data,
                                         message_content_hash ) );
    }

    Message BlockchainTiedMessageCache::get_message( const MessageHashType& hash_of_message_to_lookup )
    {
      MessageCacheContainer::index<MessageHashIndex>::type::const_iterator iter =
         _message_cache.get<MessageHashIndex>().find(hash_of_message_to_lookup );
      if( iter != _message_cache.get<MessageHashIndex>().end() )
        return iter->message_body;
      FC_THROW_EXCEPTION(  fc::key_not_found_exception, "Requested message not in cache" );
    }

    MessagePropagationData BlockchainTiedMessageCache::get_message_propagation_data( const fc::uint160_t& hash_of_message_contents_to_lookup ) const
    {
      if( hash_of_message_contents_to_lookup != fc::uint160_t() )
      {
        MessageCacheContainer::index<MessageContentsHashIndex>::type::const_iterator iter =
           _message_cache.get<MessageContentsHashIndex>().find(hash_of_message_contents_to_lookup );
        if( iter != _message_cache.get<MessageContentsHashIndex>().end() )
          return iter->propagation_data;
      }
      FC_THROW_EXCEPTION(  fc::key_not_found_exception, "Requested message not in cache" );
    }

/////////////////////////////////////////////////////////////////////////////////////////////////////////

    // This specifies configuration info for the local node.  It's stored as JSON
    // in the configuration directory (application data directory)
    struct NodeConfiguration
    {
      NodeConfiguration() : accept_incoming_connections(true), wait_if_endpoint_is_busy(true) {}

      fc::ip::endpoint listen_endpoint;
      bool accept_incoming_connections;
      bool wait_if_endpoint_is_busy;
      /**
       * Originally, our p2p code just had a 'node-id' that was a random number identifying this node
       * on the network.  This is now a private key/public key pair, where the public key is used
       * in place of the old random node-id.  The private part is unused, but might be used in
       * the future to support some notion of trusted peers.
       */
      fc::ecc::private_key private_key;
    };


} } } // end namespace goopal::net::detail
FC_REFLECT(goopal::net::detail::NodeConfiguration, (listen_endpoint)
                                                 (accept_incoming_connections)
                                                 (wait_if_endpoint_is_busy)
                                                 (private_key));

namespace goopal { namespace net { namespace detail {

    // when requesting items from peers, we want to prioritize any blocks before
    // transactions, but otherwise request items in the order we heard about them
    struct PrioritizedItemId
    {
      ItemId  item;
      unsigned sequence_number;
      PrioritizedItemId(const ItemId& item, unsigned sequence_number) :
        item(item),
        sequence_number(sequence_number)
      {}
      bool operator<(const PrioritizedItemId& rhs) const
      {
        static_assert(goopal::client::block_message_type > goopal::client::trx_message_type,
                      "block_message_type must be greater than trx_message_type for prioritized_item_ids to sort correctly");
        if (item.item_type != rhs.item.item_type)
          return item.item_type > rhs.item.item_type;
        return (signed)(rhs.sequence_number - sequence_number) > 0;
      }
    };

/////////////////////////////////////////////////////////////////////////////////////////////////////////
    class StatisticsGatheringNodeDelegateWrapper : public NodeDelegate
    {
    private:
      NodeDelegate *_node_delegate;
      fc::thread *_thread;

      typedef boost::accumulators::accumulator_set<int64_t, boost::accumulators::stats<boost::accumulators::tag::min,
                                                                                       boost::accumulators::tag::rolling_mean,
                                                                                       boost::accumulators::tag::max,
                                                                                       boost::accumulators::tag::sum,
                                                                                       boost::accumulators::tag::count> > CallStatsAccumulator;
#define NODE_DELEGATE_METHOD_NAMES (has_item) \
                                   (handle_message) \
                                   (get_item_ids) \
                                   (get_item) \
                                   (get_chain_id) \
                                   (get_blockchain_synopsis) \
                                   (sync_status) \
                                   (connection_count_changed) \
                                   (get_block_number) \
                                   (get_block_time) \
                                   (get_head_block_id) \
                                   (estimate_last_known_fork_from_git_revision_timestamp) \
                                   (error_encountered)

#define DECLARE_ACCUMULATOR(r, data, method_name) \
      mutable CallStatsAccumulator BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator)); \
      mutable CallStatsAccumulator BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator)); \
      mutable CallStatsAccumulator BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator)); 
      BOOST_PP_SEQ_FOR_EACH(DECLARE_ACCUMULATOR, unused, NODE_DELEGATE_METHOD_NAMES)
#undef DECLARE_ACCUMULATOR

      class CallStatisticsCollector
      {
      private:
        fc::time_point _call_requested_time;
        fc::time_point _begin_execution_time;
        fc::time_point _execution_completed_time;
        const char* _method_name;
        CallStatsAccumulator* _execution_accumulator;
        CallStatsAccumulator* _delay_before_accumulator;
        CallStatsAccumulator* _delay_after_accumulator;
      public:
        class ActualExecutionMeasurementHelper
        {
          CallStatisticsCollector &_collector;
        public:
          ActualExecutionMeasurementHelper(CallStatisticsCollector& collector) :
            _collector(collector)
          {
            _collector.starting_execution();
          }
          ~ActualExecutionMeasurementHelper()
          {
            _collector.execution_completed();
          }  
        };
        CallStatisticsCollector(const char* method_name, 
                                  CallStatsAccumulator* execution_accumulator, 
                                  CallStatsAccumulator* delay_before_accumulator, 
                                  CallStatsAccumulator* delay_after_accumulator) :
          _call_requested_time(fc::time_point::now()),
          _method_name(method_name),
          _execution_accumulator(execution_accumulator),
          _delay_before_accumulator(delay_before_accumulator),
          _delay_after_accumulator(delay_after_accumulator)
        {}
        ~CallStatisticsCollector()
        {
          fc::time_point end_time(fc::time_point::now());
          fc::microseconds actual_execution_time(_execution_completed_time - _begin_execution_time);
          fc::microseconds delay_before(_begin_execution_time - _call_requested_time);
          fc::microseconds delay_after(end_time - _execution_completed_time);
          fc::microseconds total_duration(actual_execution_time + delay_before + delay_after);
          (*_execution_accumulator)(actual_execution_time.count());
          (*_delay_before_accumulator)(delay_before.count());
          (*_delay_after_accumulator)(delay_after.count());
          if (total_duration > fc::milliseconds(500))
          {
            ilog("Call to method node_delegate::${method} took ${total_duration}us, longer than our target maximum of 500ms",
                 ("method", _method_name)
                 ("total_duration", total_duration.count()));
            ilog("Actual execution took ${execution_duration}us, with a ${delegate_delay}us delay before the delegate thread started "
                 "executing the method, and a ${p2p_delay}us delay after it finished before the p2p thread started processing the response",
                 ("execution_duration", actual_execution_time)
                 ("delegate_delay", delay_before)
                 ("p2p_delay", delay_after));
          }
        }
        void starting_execution()
        {
          _begin_execution_time = fc::time_point::now();
        }
        void execution_completed()
        {
          _execution_completed_time = fc::time_point::now();
        }
      };
    public:
      StatisticsGatheringNodeDelegateWrapper(NodeDelegate* delegate, fc::thread* thread_for_delegate_calls);
	  ~StatisticsGatheringNodeDelegateWrapper()
	  {
		  _node_delegate = nullptr;
	  };
      fc::variant_object get_call_statistics();

      bool has_item( const net::ItemId& id ) override;
      bool handle_message( const Message&, bool sync_mode ) override;
      std::vector<ItemHashType> get_item_ids(uint32_t item_type,
                                            const std::vector<ItemHashType>& blockchain_synopsis,
                                            uint32_t& remaining_item_count,
                                            uint32_t limit = 2000) override;
      Message get_item( const ItemId& id ) override;
      fc::sha256 get_chain_id() const override;
      std::vector<ItemHashType> get_blockchain_synopsis(uint32_t item_type,
                                                       const goopal::net::ItemHashType& reference_point = goopal::net::ItemHashType(),
                                                       uint32_t number_of_blocks_after_reference_point = 0) override;
      void     sync_status( uint32_t item_type, uint32_t item_count ) override;
      void     connection_count_changed( uint32_t c ) override;
      uint32_t get_block_number(const ItemHashType& block_id) override;
      fc::time_point_sec get_block_time(const ItemHashType& block_id) override;
      fc::time_point_sec get_blockchain_now() override;
      ItemHashType get_head_block_id() const override;
      uint32_t estimate_last_known_fork_from_git_revision_timestamp(uint32_t unix_timestamp) const;
      void error_encountered(const std::string& message, const fc::oexception& error) override;
    };

/////////////////////////////////////////////////////////////////////////////////////////////////////////

    class NodeImpl : public PeerConnectionDelegate
    {
    public:
#ifdef P2P_IN_DEDICATED_THREAD
      std::shared_ptr<fc::thread> _thread;
#endif // P2P_IN_DEDICATED_THREAD
      std::unique_ptr<StatisticsGatheringNodeDelegateWrapper> _delegate;
      fc::sha256           _chain_id;

#define NODE_CONFIGURATION_FILENAME      "node_config.json"
#define POTENTIAL_PEER_DATABASE_FILENAME "peers.leveldb"
      fc::path             _node_configuration_directory;
      NodeConfiguration   _node_configuration;

      /// stores the endpoint we're listening on.  This will be the same as
      // _node_configuration.listen_endpoint, unless that endpoint was already
      // in use.
      fc::ip::endpoint     _actual_listening_endpoint;

      /// we determine whether we're firewalled by asking other nodes.  Store the result here:
      FirewalledState     _is_firewalled;
      /// if we're behind NAT, our listening endpoint address will appear different to the rest of the world.  store it here.
      fc::optional<fc::ip::endpoint> _publicly_visible_listening_endpoint;
      fc::time_point       _last_firewall_check_message_sent;

      /// used by the task that manages connecting to peers
      // @{
      std::list<PotentialPeerEntry> _add_once_node_list; /// list of peers we want to connect to as soon as possible

      PeerDatabase             _potential_peer_db;
      fc::promise<void>::ptr    _retrigger_connect_loop_promise;
      bool                      _potential_peer_database_updated;
      fc::future<void>          _p2p_network_connect_loop_done;
      // @}

      /// used by the task that fetches sync items during synchronization
      // @{
      fc::promise<void>::ptr    _retrigger_fetch_sync_items_loop_promise;
      bool                      _sync_items_to_fetch_updated;
      fc::future<void>          _fetch_sync_items_loop_done;

      typedef std::unordered_map<goopal::blockchain::BlockIdType, fc::time_point> ActiveSyncRequestsMap;

      ActiveSyncRequestsMap              _active_sync_requests; /// list of sync blocks we've asked for from peers but have not yet received
      std::list<goopal::client::BlockMessage> _new_received_sync_items; /// list of sync blocks we've just received but haven't yet tried to process
      std::list<goopal::client::BlockMessage> _received_sync_items; /// list of sync blocks we've received, but can't yet process because we are still missing blocks that come earlier in the chain
      // @}

      fc::future<void> _process_backlog_of_sync_blocks_done;
      bool _suspend_fetching_sync_blocks;

      /// used by the task that fetches items during normal operation
      // @{
      fc::promise<void>::ptr _retrigger_fetch_item_loop_promise;
      bool                   _items_to_fetch_updated;
      fc::future<void>       _fetch_item_loop_done;

      typedef boost::multi_index_container<PrioritizedItemId,
                                           boost::multi_index::indexed_by<boost::multi_index::ordered_unique<boost::multi_index::identity<PrioritizedItemId> >,
                                                                          boost::multi_index::hashed_unique<boost::multi_index::member<PrioritizedItemId, ItemId, &PrioritizedItemId::item>,
                                                                                                            std::hash<ItemId> > >
                                           > ItemsToFetchSetType;
      unsigned _items_to_fetch_sequence_counter;
      ItemsToFetchSetType _items_to_fetch; /// list of items we know another peer has and we want
      // @}

      /// used by the task that advertises inventory during normal operation
      // @{
      fc::promise<void>::ptr        _retrigger_advertise_inventory_loop_promise;
      fc::future<void>              _advertise_inventory_loop_done;
      std::unordered_set<ItemId>   _new_inventory; /// list of items we have received but not yet advertised to our peers
      // @}

      fc::future<void>     _terminate_inactive_connections_loop_done;

      std::string          _user_agent_string;
      /** _node_public_key is a key automatically generated when the client is first run, stored in
       * node_config.json.  It doesn't really have much of a purpose yet, there was just some thought
       * that we might someday have a use for nodes having a private key (sent in hello messages)
       */
      NodeIdType            _node_public_key;
      /**
       * _node_id is a random number generated each time the client is launched, used to prevent us
       * from connecting to the same client multiple times (sent in hello messages).
       * Since this was introduced after the hello_message was finalized, this is sent in the
       * user_data field.
       * While this shares the same underlying type as a public key, it is really just a random
       * number.
       */
      NodeIdType            _node_id;

      /** if we have less than `_desired_number_of_connections`, we will try to connect with more nodes */
      uint32_t             _desired_number_of_connections;
      /** if we have _maximum_number_of_connections or more, we will refuse any inbound connections */
      uint32_t             _maximum_number_of_connections;
      /** retry connections to peers that have failed or rejected us this often, in seconds */
      uint32_t              _peer_connection_retry_timeout;
      /** how many seconds of inactivity are permitted before disconnecting a peer */
      uint32_t              _peer_inactivity_timeout;

      fc::tcp_server       _tcp_server;
      fc::future<void>     _accept_loop_complete;

      /** Stores all connections which have not yet finished key exchange or are still sending initial handshaking messages
       * back and forth (not yet ready to initiate syncing) */
      std::unordered_set<PeerConnectionPtr>                     _handshaking_connections;
      /** stores fully established connections we're either syncing with or in normal operation with */
      std::unordered_set<PeerConnectionPtr>                     _active_connections;
      /** stores connections we've closed (sent closing message, not actually closed), but are still waiting for the remote end to close before we delete them */
      std::unordered_set<PeerConnectionPtr>                     _closing_connections;
      /** stores connections we've closed, but are still waiting for the OS to notify us that the socket is really closed */
      std::unordered_set<PeerConnectionPtr>                     _terminating_connections;

      boost::circular_buffer<ItemHashType> _most_recent_blocks_accepted; // the /n/ most recent blocks we've accepted (currently tuned to the max number of connections)

      uint32_t _sync_item_type;
      uint32_t _total_number_of_unfetched_items; /// the number of items we still need to fetch while syncing
      std::vector<uint32_t> _hard_fork_block_numbers; /// list of all block numbers where there are hard forks

      BlockchainTiedMessageCache _message_cache; /// cache message we have received and might be required to provide to other peers via inventory requests

      fc::rate_limiting_group _rate_limiter;

      uint32_t _last_reported_number_of_connections; // number of connections last reported to the client (to avoid sending duplicate messages)

      bool _peer_advertising_disabled;

      fc::future<void> _fetch_updated_peer_lists_loop_done;

      boost::circular_buffer<uint32_t> _average_network_read_speed_seconds;
      boost::circular_buffer<uint32_t> _average_network_write_speed_seconds;
      boost::circular_buffer<uint32_t> _average_network_read_speed_minutes;
      boost::circular_buffer<uint32_t> _average_network_write_speed_minutes;
      boost::circular_buffer<uint32_t> _average_network_read_speed_hours;
      boost::circular_buffer<uint32_t> _average_network_write_speed_hours;
      unsigned _average_network_usage_second_counter;
      unsigned _average_network_usage_minute_counter;

      fc::time_point_sec _bandwidth_monitor_last_update_time;
      fc::future<void> _bandwidth_monitor_loop_done;

      fc::future<void> _dump_node_status_task_done;

      /* We have two alternate paths through the schedule_peer_for_deletion code -- one that
       * uses a mutex to prevent one fiber from adding items to the queue while another is deleting
       * items from it, and one that doesn't.  The one that doesn't is simpler and more efficient
       * code, but we're keeping around the version that uses the mutex because it crashes, and
       * this crash probably indicates a bug in our underlying threading code that needs 
       * fixing.  To produce the bug, define USE_PEERS_TO_DELETE_MUTEX and then connect up
       * to the network and set your desired/max connection counts high
       */
//#define USE_PEERS_TO_DELETE_MUTEX 1
#ifdef USE_PEERS_TO_DELETE_MUTEX
      fc::mutex _peers_to_delete_mutex;
#endif
      std::list<PeerConnectionPtr> _peers_to_delete;
      fc::future<void> _delayed_peer_deletion_task_done;

#ifdef ENABLE_P2P_DEBUGGING_API
      std::set<NodeIdType> _allowed_peers;
#endif // ENABLE_P2P_DEBUGGING_API

      bool _node_is_shutting_down; // set to true when we begin our destructor, used to prevent us from starting new tasks while we're shutting down

      unsigned _maximum_number_of_blocks_to_handle_at_one_time;
      unsigned _maximum_number_of_sync_blocks_to_prefetch;
      unsigned _maximum_blocks_per_peer_during_syncing;

      std::list<fc::future<void> > _handle_message_calls_in_progress;

      NodeImpl(const std::string& user_agent);
      virtual ~NodeImpl();

      void save_node_configuration();

      void p2p_network_connect_loop();
      void trigger_p2p_network_connect_loop();

      bool have_already_received_sync_item( const ItemHashType& item_hash );
      void request_sync_item_from_peer( const PeerConnectionPtr& peer, const ItemHashType& item_to_request );
      void request_sync_items_from_peer( const PeerConnectionPtr& peer, const std::vector<ItemHashType>& items_to_request );
      void fetch_sync_items_loop();
      void trigger_fetch_sync_items_loop();

      bool is_item_in_any_peers_inventory(const ItemId& item) const;
      void fetch_items_loop();
      void trigger_fetch_items_loop();

      void advertise_inventory_loop();
      void trigger_advertise_inventory_loop();

      void terminate_inactive_connections_loop();

      void fetch_updated_peer_lists_loop();
      void update_bandwidth_data(uint32_t bytes_read_this_second, uint32_t bytes_written_this_second);
      void bandwidth_monitor_loop();
      void dump_node_status_task();

      bool is_accepting_new_connections();
      bool is_wanting_new_connections();
      uint32_t get_number_of_connections();
      PeerConnectionPtr get_peer_by_node_id(const NodeIdType& id);

      bool is_already_connected_to_id(const NodeIdType& node_id);
      bool merge_address_info_with_potential_peer_database( const std::vector<AddressInfo> addresses );
      void display_current_connections();
      uint32_t calculate_unsynced_block_count_from_all_peers();
      std::vector<ItemHashType> create_blockchain_synopsis_for_peer( const PeerConnection* peer );
      void fetch_next_batch_of_item_ids_from_peer(PeerConnection* peer, bool reset_fork_tracking_data_for_peer = false);

      fc::variant_object generate_hello_user_data();
      void parse_hello_user_data_for_peer(PeerConnection* originating_peer, const fc::variant_object& user_data);

      void on_message(PeerConnection* originating_peer,
                       const Message& received_message ) override;

      void on_hello_message(PeerConnection* originating_peer,
                             const HelloMessage& hello_message_received );

      void on_connection_accepted_message(PeerConnection* originating_peer,
                                           const ConnectionAcceptedMessage& connection_accepted_message_received );

      void on_connection_rejected_message(PeerConnection* originating_peer,
                                           const ConnectionRejectedMessage& connection_rejected_message_received );

      void on_address_request_message(PeerConnection* originating_peer,
                                       const AddressRequestMessage& address_request_message_received );

      void on_address_message(PeerConnection* originating_peer,
                               const AddressMessage& address_message_received );

      void on_fetch_blockchain_item_ids_message(PeerConnection* originating_peer,
                                                 const FetchBlockchainItemIdsMessage& fetch_blockchain_item_ids_message_received );

      void on_blockchain_item_ids_inventory_message(PeerConnection* originating_peer,
                                                     const BlockchainItemIdsInventoryMessage& blockchain_item_ids_inventory_message_received );

      void on_fetch_items_message(PeerConnection* originating_peer,
                                   const FetchItemsMessage& fetch_items_message_received );

      void on_item_not_available_message(PeerConnection* originating_peer,
                                          const ItemNotAvailableMessage& item_not_available_message_received );

      void on_item_ids_inventory_message(PeerConnection* originating_peer,
                                          const ItemIdsInventoryMessage& item_ids_inventory_message_received );

      void on_closing_connection_message(PeerConnection* originating_peer,
                                          const ClosingConnectionMessage& closing_connection_message_received );

      void on_current_time_request_message(PeerConnection* originating_peer,
                                            const CurrentTimeRequestMessage& current_time_request_message_received );

      void on_current_time_reply_message(PeerConnection* originating_peer,
                                          const CurrentTimeReplyMessage& current_time_reply_message_received );

      void forward_firewall_check_to_next_available_peer(FirewallCheckStateData* firewall_check_state);

      void on_check_firewall_message(PeerConnection* originating_peer,
                                     const CheckFirewallMessage& check_firewall_message_received);

      void on_check_firewall_reply_message(PeerConnection* originating_peer,
                                           const CheckFirewallReplyMessage& check_firewall_reply_message_received);

      void on_get_current_connections_request_message(PeerConnection* originating_peer,
                                                      const GetCurrentConnectionsRequestMessage& get_current_connections_request_message_received);

      void on_get_current_connections_reply_message(PeerConnection* originating_peer,
                                                    const GetCurrentConnectionsReplyMessage& get_current_connections_reply_message_received);

      void on_connection_closed(PeerConnection* originating_peer) override;

      void send_sync_block_to_node_delegate(const goopal::client::BlockMessage& block_message_to_send);
      void process_backlog_of_sync_blocks();
      void trigger_process_backlog_of_sync_blocks();
      void process_block_during_sync(PeerConnection* originating_peer, const goopal::client::BlockMessage& block_message, const MessageHashType& message_hash);
      void process_block_during_normal_operation(PeerConnection* originating_peer, const goopal::client::BlockMessage& block_message, const MessageHashType& message_hash);
      void process_block_message(PeerConnection* originating_peer, const Message& message_to_process, const MessageHashType& message_hash);

      void process_ordinary_message(PeerConnection* originating_peer, const Message& message_to_process, const MessageHashType& message_hash);

      void start_synchronizing();
      void start_synchronizing_with_peer(const PeerConnectionPtr& peer);

      void new_peer_just_added(const PeerConnectionPtr& peer); /// called after a peer finishes handshaking, kicks off syncing

      void close();

      void accept_connection_task(PeerConnectionPtr new_peer);
      void accept_loop();
      void send_hello_message(const PeerConnectionPtr& peer);
      void connect_to_task(PeerConnectionPtr new_peer, const fc::ip::endpoint& remote_endpoint);
      bool is_connection_to_endpoint_in_progress(const fc::ip::endpoint& remote_endpoint);

      void move_peer_to_active_list(const PeerConnectionPtr& peer);
      void move_peer_to_closing_list(const PeerConnectionPtr& peer);
      void move_peer_to_terminating_list(const PeerConnectionPtr& peer);

      PeerConnectionPtr get_connection_to_endpoint( const fc::ip::endpoint& remote_endpoint );

      void dump_node_status();

      void delayed_peer_deletion_task();
      void schedule_peer_for_deletion(const PeerConnectionPtr& peer_to_delete);

      void disconnect_from_peer(PeerConnection* originating_peer,
                               const std::string& reason_for_disconnect,
                                bool caused_by_error = false,
                               const fc::oexception& additional_data = fc::oexception() );

      // methods implementing node's public interface
      void set_node_delegate(NodeDelegate* del, fc::thread* thread_for_delegate_calls);
      void load_configuration( const fc::path& configuration_directory );
      void listen_to_p2p_network();
      void connect_to_p2p_network();
      void add_node( const fc::ip::endpoint& ep );
      void initiate_connect_to(const PeerConnectionPtr& peer);
      void connect_to_endpoint(const fc::ip::endpoint& ep);
      void listen_on_endpoint( const fc::ip::endpoint& ep );
      void accept_incoming_connections(bool accept);
      void listen_on_port( uint16_t port, bool wait_if_not_available );

      fc::ip::endpoint         get_actual_listening_endpoint() const;
      std::vector<PeerStatus> get_connected_peers() const;
      uint32_t                 get_connection_count() const;

      void broadcast(const Message& item_to_broadcast, const MessagePropagationData& propagation_data);
      void broadcast(const Message& item_to_broadcast);
      void sync_from(const ItemId& current_head_block, const std::vector<uint32_t>& hard_fork_block_numbers);
      bool is_connected() const;
      std::vector<PotentialPeerEntry> get_potential_peers() const;
      void set_advanced_node_parameters( const fc::variant_object& params );

      fc::variant_object         get_advanced_node_parameters();
      MessagePropagationData   get_transaction_propagation_data( const goopal::blockchain::TransactionIdType& transaction_id );
      MessagePropagationData   get_block_propagation_data( const goopal::blockchain::BlockIdType& block_id );

      NodeIdType                  get_node_id() const;
      void                       set_allowed_peers( const std::vector<NodeIdType>& allowed_peers );
      void                       clear_peer_database();
      void                       set_total_bandwidth_limit( uint32_t upload_bytes_per_second, uint32_t download_bytes_per_second );
      void                       disable_peer_advertising();
      fc::variant_object         get_call_statistics() const;
      Message                    get_message_for_item(const ItemId& item) override;

      fc::variant_object         network_get_info() const;
      fc::variant_object         network_get_usage_stats() const;

      bool is_hard_fork_block(uint32_t block_number) const;
      uint32_t get_next_known_hard_fork_block_number(uint32_t block_number) const;
    }; // end class node_impl

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    void NodeImplDeleter::operator()(NodeImpl* impl_to_delete)
    {
#ifdef P2P_IN_DEDICATED_THREAD
      std::weak_ptr<fc::thread> weak_thread;
      if (impl_to_delete)
      {
        std::shared_ptr<fc::thread> impl_thread(impl_to_delete->_thread);
        weak_thread = impl_thread;
        impl_thread->async([impl_to_delete](){ delete impl_to_delete; }, "delete node_impl").wait();
        dlog("deleting the p2p thread");
      }
      if (weak_thread.expired())
        dlog("done deleting the p2p thread");
      else
        dlog("failed to delete the p2p thread, we must be leaking a smart pointer somewhere");
#else // P2P_IN_DEDICATED_THREAD
      delete impl_to_delete;
#endif // P2P_IN_DEDICATED_THREAD
    }

#ifdef P2P_IN_DEDICATED_THREAD
# define VERIFY_CORRECT_THREAD() assert(_thread->is_current())
#else
# define VERIFY_CORRECT_THREAD() do {} while (0)
#endif

#define MAXIMUM_NUMBER_OF_BLOCKS_TO_HANDLE_AT_ONE_TIME 200
#define MAXIMUM_NUMBER_OF_BLOCKS_TO_PREFETCH (10 * MAXIMUM_NUMBER_OF_BLOCKS_TO_HANDLE_AT_ONE_TIME)

    NodeImpl::NodeImpl(const std::string& user_agent) :
#ifdef P2P_IN_DEDICATED_THREAD
      _thread(std::make_shared<fc::thread>("p2p")),
#endif // P2P_IN_DEDICATED_THREAD
      _delegate(nullptr),
      _is_firewalled(FirewalledState::unknown),
      _potential_peer_database_updated(false),
      _sync_items_to_fetch_updated(false),
      _suspend_fetching_sync_blocks(false),
      _items_to_fetch_updated(false),
      _items_to_fetch_sequence_counter(0),
      _user_agent_string(user_agent),
      _desired_number_of_connections(GOP_NET_DEFAULT_DESIRED_CONNECTIONS),
      _maximum_number_of_connections(GOP_NET_DEFAULT_MAX_CONNECTIONS),
      _peer_connection_retry_timeout(GOP_NET_DEFAULT_PEER_CONNECTION_RETRY_TIME),
      _peer_inactivity_timeout(GOP_NET_PEER_HANDSHAKE_INACTIVITY_TIMEOUT),
      _most_recent_blocks_accepted(_maximum_number_of_connections),
      _total_number_of_unfetched_items(0),
      _rate_limiter(0, 0),
      _last_reported_number_of_connections(0),
      _peer_advertising_disabled(false),
      _average_network_read_speed_seconds(60),
      _average_network_write_speed_seconds(60),
      _average_network_read_speed_minutes(60),
      _average_network_write_speed_minutes(60),
      _average_network_read_speed_hours(72),
      _average_network_write_speed_hours(72),
      _average_network_usage_second_counter(0),
      _average_network_usage_minute_counter(0),
      _node_is_shutting_down(false),
      _maximum_number_of_blocks_to_handle_at_one_time(MAXIMUM_NUMBER_OF_BLOCKS_TO_HANDLE_AT_ONE_TIME),
      _maximum_number_of_sync_blocks_to_prefetch(MAXIMUM_NUMBER_OF_BLOCKS_TO_PREFETCH),
      _maximum_blocks_per_peer_during_syncing(GOP_NET_MAX_BLOCKS_PER_PEER_DURING_SYNCING)
    {
      _rate_limiter.set_actual_rate_time_constant(fc::seconds(2));
      fc::rand_pseudo_bytes(&_node_id.data[0], (int)_node_id.size());
    }

    NodeImpl::~NodeImpl()
    {
      VERIFY_CORRECT_THREAD();
      ilog( "cleaning up node" );
      _node_is_shutting_down = true;

      for (const PeerConnectionPtr& active_peer : _active_connections)
      {
        fc::optional<fc::ip::endpoint> inbound_endpoint = active_peer->get_endpoint_for_connecting();
        if (inbound_endpoint)
        {
          fc::optional<PotentialPeerEntry> updated_peer_entry = _potential_peer_db.lookup_entry_for_endpoint(*inbound_endpoint);
          if (updated_peer_entry)
          {
            updated_peer_entry->last_seen_time = fc::time_point::now();
            _potential_peer_db.update_entry(*updated_peer_entry);
          }
        }
      }

      try
      {
        ilog( "close" );
        close();
      }
      catch ( const fc::exception& e )
      {
        wlog( "unexpected exception on close ${e}", ("e", e.to_detail_string() ) );
      }
	  _delegate.release();
      ilog( "done" );
    }

    void NodeImpl::save_node_configuration()
    {
      VERIFY_CORRECT_THREAD();
      if( fc::exists(_node_configuration_directory ) )
      {
        fc::path configuration_file_name( _node_configuration_directory / NODE_CONFIGURATION_FILENAME );
        try
        {
          fc::json::save_to_file( _node_configuration, configuration_file_name );
        }
        catch (const fc::canceled_exception&)
        {
          throw;
        }
        catch ( const fc::exception& except )
        {
          elog( "error writing node configuration to file ${filename}: ${error}",
               ( "filename", configuration_file_name )("error", except.to_detail_string() ) );
        }
      }
    }

    void NodeImpl::p2p_network_connect_loop()
    {
      VERIFY_CORRECT_THREAD();
      while (!_p2p_network_connect_loop_done.canceled())
      {
        try
        {
          dlog("Starting an iteration of p2p_network_connect_loop().");
          display_current_connections();

          // add-once peers bypass our checks on the maximum/desired number of connections (but they will still be counted against the totals once they're connected)
          if (!_add_once_node_list.empty())
          {
            std::list<PotentialPeerEntry> add_once_node_list;
            add_once_node_list.swap(_add_once_node_list);
            dlog("Processing \"add once\" node list containing ${count} peers:", ("count", add_once_node_list.size()));
            for (const PotentialPeerEntry& add_once_peer : add_once_node_list)
            {
              dlog("    ${peer}", ("peer", add_once_peer.endpoint));
            }
            for (const PotentialPeerEntry& add_once_peer : add_once_node_list)
            {
              // see if we have an existing connection to that peer.  If we do, disconnect them and
              // then try to connect the next time through the loop
              PeerConnectionPtr existing_connection_ptr = get_connection_to_endpoint( add_once_peer.endpoint );
              if(!existing_connection_ptr)
                connect_to_endpoint(add_once_peer.endpoint);
            }
            dlog("Done processing \"add once\" node list");
          }

          while (is_wanting_new_connections())
          {
            bool initiated_connection_this_pass = false;
            _potential_peer_database_updated = false;

            for (PeerDatabase::iterator iter = _potential_peer_db.begin();
                 iter != _potential_peer_db.end() && is_wanting_new_connections();
                 ++iter)
            {
              fc::microseconds delay_until_retry = fc::seconds((iter->number_of_failed_connection_attempts + 1) * _peer_connection_retry_timeout);

              if (!is_connection_to_endpoint_in_progress(iter->endpoint) &&
                  ((iter->last_connection_disposition != last_connection_failed &&
                    iter->last_connection_disposition != last_connection_rejected &&
                    iter->last_connection_disposition != last_connection_handshaking_failed) ||
                   (fc::time_point::now() - iter->last_connection_attempt_time) > delay_until_retry))
              {
                connect_to_endpoint(iter->endpoint);
                initiated_connection_this_pass = true;
              }
            }

            if (!initiated_connection_this_pass && !_potential_peer_database_updated)
              break;
          }

          display_current_connections();

          // if we broke out of the while loop, that means either we have connected to enough nodes, or
          // we don't have any good candidates to connect to right now.
#if 0
          try
          {
            _retrigger_connect_loop_promise = fc::promise<void>::ptr( new fc::promise<void>("goopal::net::retrigger_connect_loop") );
            if( is_wanting_new_connections() || !_add_once_node_list.empty() )
            {
              if( is_wanting_new_connections() )
                dlog( "Still want to connect to more nodes, but I don't have any good candidates.  Trying again in 15 seconds" );
              else
                dlog( "I still have some \"add once\" nodes to connect to.  Trying again in 15 seconds" );
              _retrigger_connect_loop_promise->wait_until( fc::time_point::now() + fc::seconds(GOP_PEER_DATABASE_RETRY_DELAY ) );
            }
            else
            {
              dlog( "I don't need any more connections, waiting forever until something changes" );
              _retrigger_connect_loop_promise->wait();
            }
          }
          catch ( fc::timeout_exception& ) //intentionally not logged
          {
          }  // catch
#else
          fc::usleep(fc::seconds(10));
#endif
        }
        catch (const fc::canceled_exception&)
        {
          throw;
        }
        catch (const fc::exception& e)
        {
          elog("${e}", ("e", e.to_detail_string()));
        }
      }// while(!canceled)
    }

    void NodeImpl::trigger_p2p_network_connect_loop()
    {
      VERIFY_CORRECT_THREAD();
      dlog( "Triggering connect loop now" );
      _potential_peer_database_updated = true;
      //if( _retrigger_connect_loop_promise )
      //  _retrigger_connect_loop_promise->set_value();
    }

    bool NodeImpl::have_already_received_sync_item( const ItemHashType& item_hash )
    {
      VERIFY_CORRECT_THREAD();
      return std::find_if(_received_sync_items.begin(), _received_sync_items.end(),
                          [&item_hash]( const goopal::client::BlockMessage& message ) { return message.block_id == item_hash; } ) != _received_sync_items.end() ||
             std::find_if(_new_received_sync_items.begin(), _new_received_sync_items.end(),
                          [&item_hash]( const goopal::client::BlockMessage& message ) { return message.block_id == item_hash; } ) != _new_received_sync_items.end();                          ;
    }

    void NodeImpl::request_sync_item_from_peer( const PeerConnectionPtr& peer, const ItemHashType& item_to_request )
    {
      VERIFY_CORRECT_THREAD();
      dlog( "requesting item ${item_hash} from peer ${endpoint}", ("item_hash", item_to_request )("endpoint", peer->get_remote_endpoint() ) );
      ItemId item_id_to_request( goopal::client::block_message_type, item_to_request );
      _active_sync_requests.insert( ActiveSyncRequestsMap::value_type(item_to_request, fc::time_point::now() ) );
      peer->sync_items_requested_from_peer.insert(PeerConnection::item_to_time_map_type::value_type(item_id_to_request, fc::time_point::now()));
      //std::vector<item_hash_t> items_to_fetch;
      peer->send_message( FetchItemsMessage(item_id_to_request.item_type, std::vector<ItemHashType>{item_id_to_request.item_hash} ) );
    }

    void NodeImpl::request_sync_items_from_peer( const PeerConnectionPtr& peer, const std::vector<ItemHashType>& items_to_request )
    {
      VERIFY_CORRECT_THREAD();
      dlog( "requesting ${item_count} item(s) ${items_to_request} from peer ${endpoint}",
            ("item_count", items_to_request.size())("items_to_request", items_to_request)("endpoint", peer->get_remote_endpoint()) );
      for (const ItemHashType& item_to_request : items_to_request)
      {
        _active_sync_requests.insert( ActiveSyncRequestsMap::value_type(item_to_request, fc::time_point::now() ) );
        ItemId item_id_to_request( goopal::client::block_message_type, item_to_request );
        peer->sync_items_requested_from_peer.insert(PeerConnection::item_to_time_map_type::value_type(item_id_to_request, fc::time_point::now()));
      }
      peer->send_message(FetchItemsMessage(goopal::client::block_message_type, items_to_request));
    }

    void NodeImpl::fetch_sync_items_loop()
    {
      VERIFY_CORRECT_THREAD();
      while( !_fetch_sync_items_loop_done.canceled() )
      {
        _sync_items_to_fetch_updated = false;
        dlog( "beginning another iteration of the sync items loop" );

        if (!_suspend_fetching_sync_blocks)
        {
          std::map<PeerConnectionPtr, std::vector<ItemHashType> > sync_item_requests_to_send;

          {
            ASSERT_TASK_NOT_PREEMPTED();
            std::set<ItemHashType> sync_items_to_request;

            // for each idle peer that we're syncing with
            for( const PeerConnectionPtr& peer : _active_connections )
            {
              if( peer->we_need_sync_items_from_peer &&
                  sync_item_requests_to_send.find(peer) == sync_item_requests_to_send.end() && // if we've already scheduled a request for this peer, don't consider scheduling another
                  peer->idle() )
              {
                if (!peer->inhibit_fetching_sync_blocks)
                {
                  // loop through the items it has that we don't yet have on our blockchain
                  for( unsigned i = 0; i < peer->ids_of_items_to_get.size(); ++i )
                  {
                    ItemHashType item_to_potentially_request = peer->ids_of_items_to_get[i];
                    // if we don't already have this item in our temporary storage and we haven't requested from another syncing peer
                    if( !have_already_received_sync_item(item_to_potentially_request) && // already got it, but for some reson it's still in our list of items to fetch
                        sync_items_to_request.find(item_to_potentially_request) == sync_items_to_request.end() &&  // we have already decided to request it from another peer during this iteration
                        _active_sync_requests.find(item_to_potentially_request) == _active_sync_requests.end() ) // we've requested it in a previous iteration and we're still waiting for it to arrive
                    {
                      // then schedule a request from this peer
                      sync_item_requests_to_send[peer].push_back(item_to_potentially_request);
                      sync_items_to_request.insert( item_to_potentially_request );
                      if (sync_item_requests_to_send[peer].size() >= _maximum_blocks_per_peer_during_syncing)
                        break;
                    }
                  }
                }
              }
            }
          } // end non-preemptable section

          // make all the requests we scheduled in the loop above
          for( auto sync_item_request : sync_item_requests_to_send )
            request_sync_items_from_peer( sync_item_request.first, sync_item_request.second );
          sync_item_requests_to_send.clear();
        }
        else
          dlog("fetch_sync_items_loop is suspended pending backlog processing");

        if( !_sync_items_to_fetch_updated )
        {
          dlog( "no sync items to fetch right now, going to sleep" );
          _retrigger_fetch_sync_items_loop_promise = fc::promise<void>::ptr( new fc::promise<void>("goopal::net::retrigger_fetch_sync_items_loop") );
          _retrigger_fetch_sync_items_loop_promise->wait();
          _retrigger_fetch_sync_items_loop_promise.reset();
        }
      } // while( !canceled )
    }

    void NodeImpl::trigger_fetch_sync_items_loop()
    {
      VERIFY_CORRECT_THREAD();
      dlog( "Triggering fetch sync items loop now" );
      _sync_items_to_fetch_updated = true;
      if( _retrigger_fetch_sync_items_loop_promise )
        _retrigger_fetch_sync_items_loop_promise->set_value();
    }

    bool NodeImpl::is_item_in_any_peers_inventory(const ItemId& item) const
    {
      for( const PeerConnectionPtr& peer : _active_connections )
      {
        if (peer->inventory_peer_advertised_to_us.find(item) != peer->inventory_peer_advertised_to_us.end() )
          return true;
      }
      return false;
    }

    void NodeImpl::fetch_items_loop()
    {
      VERIFY_CORRECT_THREAD();
      while (!_fetch_item_loop_done.canceled())
      {
        _items_to_fetch_updated = false;
        dlog("beginning an iteration of fetch items (${count} items to fetch)",
             ("count", _items_to_fetch.size()));

        fc::time_point next_peer_unblocked_time = fc::time_point::maximum();

        std::forward_list<std::pair<PeerConnectionPtr, ItemId> > fetch_messages_to_send;
        //std::vector<fc::future<void> >  write_ops;
        for (auto iter = _items_to_fetch.begin(); iter != _items_to_fetch.end();)
        {
          bool item_fetched = false;
          for (const PeerConnectionPtr& peer : _active_connections)
          {
            if (peer->idle() &&
                peer->inventory_peer_advertised_to_us.find(iter->item) != peer->inventory_peer_advertised_to_us.end())
            {
              if (peer->is_transaction_fetching_inhibited() && iter->item.item_type == goopal::client::trx_message_type)
                next_peer_unblocked_time = std::min(peer->transaction_fetching_inhibited_until, next_peer_unblocked_time);
              else
              {
                dlog("requesting item ${hash} from peer ${endpoint}",
                     ("hash", iter->item.item_hash)("endpoint", peer->get_remote_endpoint()));
                peer->items_requested_from_peer.insert(PeerConnection::item_to_time_map_type::value_type(iter->item, fc::time_point::now()));
                ItemId item_id_to_fetch = iter->item;
                iter = _items_to_fetch.erase(iter);
                item_fetched = true;
                fetch_messages_to_send.emplace_front(std::make_pair(peer, item_id_to_fetch));
                break;
              }
            }
          }
          if (!item_fetched)
            ++iter;
        }

        for (const auto& peer_and_item : fetch_messages_to_send)
          peer_and_item.first->send_message(FetchItemsMessage(peer_and_item.second.item_type,
                                                                std::vector<ItemHashType>{peer_and_item.second.item_hash}));
        fetch_messages_to_send.clear();

        if (!_items_to_fetch_updated)
        {
          _retrigger_fetch_item_loop_promise = fc::promise<void>::ptr(new fc::promise<void>("goopal::net::retrigger_fetch_item_loop"));
          fc::microseconds time_until_retrigger = fc::microseconds::maximum();
          if (next_peer_unblocked_time != fc::time_point::maximum())
            time_until_retrigger = next_peer_unblocked_time - fc::time_point::now();
          try
          {
            if (time_until_retrigger > fc::microseconds(0))
              _retrigger_fetch_item_loop_promise->wait(time_until_retrigger);
          }
          catch (const fc::timeout_exception&)
          {
            dlog("Resuming fetch_items_loop due to timeout -- one of our peers should no longer be throttled");
          }
          _retrigger_fetch_item_loop_promise.reset();
        }
      } // while (!canceled)
    }

    void NodeImpl::trigger_fetch_items_loop()
    {
      VERIFY_CORRECT_THREAD();
      _items_to_fetch_updated = true;
      if( _retrigger_fetch_item_loop_promise )
        _retrigger_fetch_item_loop_promise->set_value();
    }

    void NodeImpl::advertise_inventory_loop()
    {
      VERIFY_CORRECT_THREAD();
      while (!_advertise_inventory_loop_done.canceled())
      {
        dlog("beginning an iteration of advertise inventory");
        // swap inventory into local variable, clearing the node's copy
        std::unordered_set<ItemId> inventory_to_advertise;
        inventory_to_advertise.swap(_new_inventory);

        // process all inventory to advertise and construct the inventory messages we'll send
        // first, then send them all in a batch (to avoid any fiber interruption points while
        // we're computing the messages)
        std::list<std::pair<PeerConnectionPtr, ItemIdsInventoryMessage> > inventory_messages_to_send;

        for (const PeerConnectionPtr& peer : _active_connections)
        {
          // only advertise to peers who are in sync with us
          if( !peer->peer_needs_sync_items_from_us )
          {
            std::map<uint32_t, std::vector<ItemHashType> > items_to_advertise_by_type;
            // don't send the peer anything we've already advertised to it
            // or anything it has advertised to us
            // group the items we need to send by type, because we'll need to send one inventory message per type
            unsigned total_items_to_send_to_this_peer = 0;
            for (const ItemId& item_to_advertise : inventory_to_advertise)
              if (peer->inventory_advertised_to_peer.find(item_to_advertise) == peer->inventory_advertised_to_peer.end() &&
                  peer->inventory_peer_advertised_to_us.find(item_to_advertise) == peer->inventory_peer_advertised_to_us.end())
              {
                items_to_advertise_by_type[item_to_advertise.item_type].push_back(item_to_advertise.item_hash);
                peer->inventory_advertised_to_peer.insert(PeerConnection::TimestampedItemId(item_to_advertise, fc::time_point::now()));
                ++total_items_to_send_to_this_peer;
                if (item_to_advertise.item_type == trx_message_type)
                  testnetlog("advertising transaction ${id} to peer ${endpoint}", ("id", item_to_advertise.item_hash)("endpoint", peer->get_remote_endpoint()));
                dlog("advertising item ${id} to peer ${endpoint}", ("id", item_to_advertise.item_hash)("endpoint", peer->get_remote_endpoint()));
              }
              dlog("advertising ${count} new item(s) of ${types} type(s) to peer ${endpoint}",
                   ("count", total_items_to_send_to_this_peer)
                   ("types", items_to_advertise_by_type.size())
                   ("endpoint", peer->get_remote_endpoint()));
            for (auto items_group : items_to_advertise_by_type)
              inventory_messages_to_send.push_back(std::make_pair(peer, ItemIdsInventoryMessage(items_group.first, items_group.second)));
          }
          peer->clear_old_inventory();
        }

        for (auto iter = inventory_messages_to_send.begin(); iter != inventory_messages_to_send.end(); ++iter)
          iter->first->send_message(iter->second);
        inventory_messages_to_send.clear();

        if (_new_inventory.empty())
        {
          _retrigger_advertise_inventory_loop_promise = fc::promise<void>::ptr(new fc::promise<void>("goopal::net::retrigger_advertise_inventory_loop"));
          _retrigger_advertise_inventory_loop_promise->wait();
          _retrigger_advertise_inventory_loop_promise.reset();
        }
      } // while(!canceled)
    }

    void NodeImpl::trigger_advertise_inventory_loop()
    {
      VERIFY_CORRECT_THREAD();
      if( _retrigger_advertise_inventory_loop_promise )
        _retrigger_advertise_inventory_loop_promise->set_value();
    }

    void NodeImpl::terminate_inactive_connections_loop()
    {
      VERIFY_CORRECT_THREAD();
      std::list<PeerConnectionPtr> peers_to_disconnect_gently;
      std::list<PeerConnectionPtr> peers_to_disconnect_forcibly;
      std::list<PeerConnectionPtr> peers_to_send_keep_alive;
      std::list<PeerConnectionPtr> peers_to_terminate;

      // Disconnect peers that haven't sent us any data recently
      // These numbers are just guesses and we need to think through how this works better.
      // If we and our peers get disconnected from the rest of the network, we will not
      // receive any blocks or transactions from the rest of the world, and that will
      // probably make us disconnect from our peers even though we have working connections to
      // them (but they won't have sent us anything since they aren't getting blocks either).
      // This might not be so bad because it could make us initiate more connections and
      // reconnect with the rest of the network, or it might just futher isolate us.

      uint32_t handshaking_timeout = _peer_inactivity_timeout;
      fc::time_point handshaking_disconnect_threshold = fc::time_point::now() - fc::seconds(handshaking_timeout);
      for( const PeerConnectionPtr handshaking_peer : _handshaking_connections )
        if( handshaking_peer->connection_initiation_time < handshaking_disconnect_threshold &&
            handshaking_peer->get_last_message_received_time() < handshaking_disconnect_threshold &&
            handshaking_peer->get_last_message_sent_time() < handshaking_disconnect_threshold )
        {
          wlog( "Forcibly disconnecting from handshaking peer ${peer} due to inactivity of at least ${timeout} seconds",
                ( "peer", handshaking_peer->get_remote_endpoint() )("timeout", handshaking_timeout ) );
          wlog("Peer's negotiating status: ${status}, bytes sent: ${sent}, bytes received: ${received}",
                ("status", handshaking_peer->negotiation_status)
                ("sent", handshaking_peer->get_total_bytes_sent())
                ("received", handshaking_peer->get_total_bytes_received()));
          handshaking_peer->connection_closed_error = fc::exception(FC_LOG_MESSAGE(warn, "Terminating handshaking connection due to inactivity of ${timeout} seconds.  Negotiating status: ${status}, bytes sent: ${sent}, bytes received: ${received}",
                                                                                    ("peer", handshaking_peer->get_remote_endpoint())
                                                                                    ("timeout", handshaking_timeout)
                                                                                    ("status", handshaking_peer->negotiation_status)
                                                                                    ("sent", handshaking_peer->get_total_bytes_sent())
                                                                                    ("received", handshaking_peer->get_total_bytes_received())));
          peers_to_disconnect_forcibly.push_back( handshaking_peer );
        }

      // timeout for any active peers is two block intervals
      uint32_t active_disconnect_timeout = std::max<uint32_t>(5 * GOP_BLOCKCHAIN_BLOCK_INTERVAL_SEC / 2, 30);
      uint32_t active_send_keepalive_timeount = std::max<uint32_t>(active_disconnect_timeout / 2, 11);
      uint32_t active_ignored_request_timeount = std::max<uint32_t>(GOP_BLOCKCHAIN_BLOCK_INTERVAL_SEC / 4, 10);
      fc::time_point active_disconnect_threshold = fc::time_point::now() - fc::seconds(active_disconnect_timeout);
      fc::time_point active_send_keepalive_threshold = fc::time_point::now() - fc::seconds(active_send_keepalive_timeount);
      fc::time_point active_ignored_request_threshold = fc::time_point::now() - fc::seconds(active_ignored_request_timeount);
      for( const PeerConnectionPtr& active_peer : _active_connections )
      {
        if( active_peer->connection_initiation_time < active_disconnect_threshold &&
            active_peer->get_last_message_received_time() < active_disconnect_threshold )
        {
          wlog( "Closing connection with peer ${peer} due to inactivity of at least ${timeout} seconds",
                ( "peer", active_peer->get_remote_endpoint() )("timeout", active_disconnect_timeout ) );
          peers_to_disconnect_gently.push_back( active_peer );
        }
        else
        {
          bool disconnect_due_to_request_timeout = false;
          for (const PeerConnection::item_to_time_map_type::value_type& item_and_time : active_peer->sync_items_requested_from_peer)
            if (item_and_time.second < active_ignored_request_threshold)
            {
              wlog("Disconnecting peer ${peer} because they didn't respond to my request for sync item ${id}",
                    ("peer", active_peer->get_remote_endpoint())("id", item_and_time.first.item_hash));
              disconnect_due_to_request_timeout = true;
              break;
            }
          if (!disconnect_due_to_request_timeout &&
              active_peer->item_ids_requested_from_peer &&
              active_peer->item_ids_requested_from_peer->get<1>() < active_ignored_request_threshold)
            {
              wlog("Disconnecting peer ${peer} because they didn't respond to my request for sync item ids after ${id}",
                    ("peer", active_peer->get_remote_endpoint())
                    ("id", active_peer->item_ids_requested_from_peer->get<0>().item_hash));
              disconnect_due_to_request_timeout = true;
            }
          if (!disconnect_due_to_request_timeout)
              for (const PeerConnection::item_to_time_map_type::value_type& item_and_time : active_peer->items_requested_from_peer)
              if (item_and_time.second < active_ignored_request_threshold)
              {
                wlog("Disconnecting peer ${peer} because they didn't respond to my request for item ${id}",
                      ("peer", active_peer->get_remote_endpoint())("id", item_and_time.first.item_hash));
                disconnect_due_to_request_timeout = true;
                break;
              }
          if (disconnect_due_to_request_timeout)
          {
            // we should probably disconnect nicely and give them a reason, but right now the logic
            // for rescheduling the requests only executes when the connection is fully closed,
            // and we want to get those requests rescheduled as soon as possible
            peers_to_disconnect_forcibly.push_back(active_peer);
          }
          else if (active_peer->connection_initiation_time < active_send_keepalive_threshold &&
                    active_peer->get_last_message_received_time() < active_send_keepalive_threshold)
          {
            wlog( "Sending a keepalive message to peer ${peer} who hasn't sent us any messages in the last ${timeout} seconds",
                  ( "peer", active_peer->get_remote_endpoint() )("timeout", active_send_keepalive_timeount ) );
            peers_to_send_keep_alive.push_back(active_peer);
          }
        }
      }

      fc::time_point closing_disconnect_threshold = fc::time_point::now() - fc::seconds(GOP_NET_PEER_DISCONNECT_TIMEOUT);
      for( const PeerConnectionPtr& closing_peer : _closing_connections )
        if( closing_peer->connection_closed_time < closing_disconnect_threshold )
        {
          // we asked this peer to close their connectoin to us at least GOP_NET_PEER_DISCONNECT_TIMEOUT
          // seconds ago, but they haven't done it yet.  Terminate the connection now
          wlog( "Forcibly disconnecting peer ${peer} who failed to close their connection in a timely manner",
                ( "peer", closing_peer->get_remote_endpoint() ) );
          peers_to_disconnect_forcibly.push_back( closing_peer );
        }

      uint32_t failed_terminate_timeout_seconds = 120;
      fc::time_point failed_terminate_threshold = fc::time_point::now() - fc::seconds(failed_terminate_timeout_seconds);
      for (const PeerConnectionPtr& peer : _terminating_connections )
        if (peer->get_connection_terminated_time() != fc::time_point::min() &&
            peer->get_connection_terminated_time() < failed_terminate_threshold)
        {
          wlog("Terminating connection with peer ${peer}, closing the connection didn't work", ("peer", peer->get_remote_endpoint()));
          peers_to_terminate.push_back(peer);
        }

      for( const PeerConnectionPtr& peer : peers_to_disconnect_gently )
      {
        fc::exception detailed_error( FC_LOG_MESSAGE(warn, "Disconnecting due to inactivity",
                                                      ( "last_message_received_seconds_ago", (peer->get_last_message_received_time() - fc::time_point::now() ).count() / fc::seconds(1 ).count() )
                                                      ( "last_message_sent_seconds_ago", (peer->get_last_message_sent_time() - fc::time_point::now() ).count() / fc::seconds(1 ).count() )
                                                      ( "inactivity_timeout", _active_connections.find(peer ) != _active_connections.end() ? _peer_inactivity_timeout * 10 : _peer_inactivity_timeout ) ) );
        disconnect_from_peer( peer.get(), "Disconnecting due to inactivity", false, detailed_error );
      }
      peers_to_disconnect_gently.clear();

      for( const PeerConnectionPtr& peer : peers_to_disconnect_forcibly )
      {
        move_peer_to_terminating_list(peer);
        peer->close_connection();
      }
      peers_to_disconnect_forcibly.clear();

      for( const PeerConnectionPtr& peer : peers_to_send_keep_alive )
        peer->send_message(CurrentTimeRequestMessage(),
                           offsetof(CurrentTimeRequestMessage, request_sent_time));
      peers_to_send_keep_alive.clear();

      for (const PeerConnectionPtr& peer : peers_to_terminate )
        {
        assert(_terminating_connections.find(peer) != _terminating_connections.end());
        _terminating_connections.erase(peer);
        schedule_peer_for_deletion(peer);
        }

      if (!_node_is_shutting_down && !_terminate_inactive_connections_loop_done.canceled())
         _terminate_inactive_connections_loop_done = fc::schedule( [this](){ terminate_inactive_connections_loop(); },
                                                                   fc::time_point::now() + fc::seconds(GOP_NET_PEER_HANDSHAKE_INACTIVITY_TIMEOUT / 2),
                                                                   "terminate_inactive_connections_loop" );
    }

    void NodeImpl::fetch_updated_peer_lists_loop()
    {
      VERIFY_CORRECT_THREAD();

      std::list<PeerConnectionPtr> original_active_peers(_active_connections.begin(), _active_connections.end());
      for( const PeerConnectionPtr& active_peer : original_active_peers )
      {
        try
        {
          active_peer->send_message(AddressRequestMessage());
        }
        catch ( const fc::canceled_exception& )
        {
          throw;
        }
        catch (const fc::exception& e)
        {
          dlog("Caught exception while sending address request message to peer ${peer} : ${e}",
               ("peer", active_peer->get_remote_endpoint())("e", e));
        }
      }

      if (!_node_is_shutting_down && !_fetch_updated_peer_lists_loop_done.canceled() )
         _fetch_updated_peer_lists_loop_done = fc::schedule( [this](){ fetch_updated_peer_lists_loop(); },
                                                             fc::time_point::now() + fc::minutes(15),
                                                             "fetch_updated_peer_lists_loop" );
    }
    void NodeImpl::update_bandwidth_data(uint32_t bytes_read_this_second, uint32_t bytes_written_this_second)
    {
      VERIFY_CORRECT_THREAD();
      _average_network_read_speed_seconds.push_back(bytes_read_this_second);
      _average_network_write_speed_seconds.push_back(bytes_written_this_second);
      ++_average_network_usage_second_counter;
      if (_average_network_usage_second_counter >= 60)
      {
        _average_network_usage_second_counter = 0;
        ++_average_network_usage_minute_counter;
        uint32_t average_read_this_minute = (uint32_t)boost::accumulate(_average_network_read_speed_seconds, UINT64_C(0)) / (uint32_t)_average_network_read_speed_seconds.size();
        _average_network_read_speed_minutes.push_back(average_read_this_minute);
        uint32_t average_written_this_minute = (uint32_t)boost::accumulate(_average_network_write_speed_seconds, UINT64_C(0)) / (uint32_t)_average_network_write_speed_seconds.size();
        _average_network_write_speed_minutes.push_back(average_written_this_minute);
        if (_average_network_usage_minute_counter >= 60)
        {
          _average_network_usage_minute_counter = 0;
          uint32_t average_read_this_hour = (uint32_t)boost::accumulate(_average_network_read_speed_minutes, UINT64_C(0)) / (uint32_t)_average_network_read_speed_minutes.size();
          _average_network_read_speed_hours.push_back(average_read_this_hour);
          uint32_t average_written_this_hour = (uint32_t)boost::accumulate(_average_network_write_speed_minutes, UINT64_C(0)) / (uint32_t)_average_network_write_speed_minutes.size();
          _average_network_write_speed_hours.push_back(average_written_this_hour);
        }
      }
    }
    void NodeImpl::bandwidth_monitor_loop()
    {
      VERIFY_CORRECT_THREAD();
      fc::time_point_sec current_time = fc::time_point::now();

      if (_bandwidth_monitor_last_update_time == fc::time_point_sec::min())
        _bandwidth_monitor_last_update_time = current_time;

      uint32_t seconds_since_last_update = current_time.sec_since_epoch() - _bandwidth_monitor_last_update_time.sec_since_epoch();
      seconds_since_last_update = std::max(UINT32_C(1), seconds_since_last_update);
      uint32_t bytes_read_this_second = _rate_limiter.get_actual_download_rate();
      uint32_t bytes_written_this_second = _rate_limiter.get_actual_upload_rate();
      for (uint32_t i = 0; i < seconds_since_last_update - 1; ++i)
        update_bandwidth_data(0, 0);
      update_bandwidth_data(bytes_read_this_second, bytes_written_this_second);
      _bandwidth_monitor_last_update_time = current_time;

      if (!_node_is_shutting_down && !_bandwidth_monitor_loop_done.canceled())
        _bandwidth_monitor_loop_done = fc::schedule( [=](){ bandwidth_monitor_loop(); },
                                                     fc::time_point::now() + fc::seconds(1),
                                                     "bandwidth_monitor_loop" );
    }

    void NodeImpl::dump_node_status_task()
    {
      VERIFY_CORRECT_THREAD();
      dump_node_status();
      if (!_node_is_shutting_down && !_dump_node_status_task_done.canceled())
        _dump_node_status_task_done = fc::schedule([=](){ dump_node_status_task(); },
                                                   fc::time_point::now() + fc::minutes(1),
                                                   "dump_node_status_task");
    }

    void NodeImpl::delayed_peer_deletion_task()
    {
      VERIFY_CORRECT_THREAD();
#ifdef USE_PEERS_TO_DELETE_MUTEX
      fc::scoped_lock<fc::mutex> lock(_peers_to_delete_mutex);
      dlog("in delayed_peer_deletion_task with ${count} in queue", ("count", _peers_to_delete.size()));
      _peers_to_delete.clear();
      dlog("_peers_to_delete cleared");
#else
      while (!_peers_to_delete.empty())
      {
        std::list<PeerConnectionPtr> peers_to_delete_copy;
        dlog("beginning an iteration of delayed_peer_deletion_task with ${count} in queue", ("count", _peers_to_delete.size()));
        peers_to_delete_copy.swap(_peers_to_delete);
      }
      dlog("leaving delayed_peer_deletion_task");
#endif
    }

    void NodeImpl::schedule_peer_for_deletion(const PeerConnectionPtr& peer_to_delete)
    {
      VERIFY_CORRECT_THREAD();

      assert(_handshaking_connections.find(peer_to_delete) == _handshaking_connections.end());
      assert(_active_connections.find(peer_to_delete) == _active_connections.end());
      assert(_closing_connections.find(peer_to_delete) == _closing_connections.end());
      assert(_terminating_connections.find(peer_to_delete) == _terminating_connections.end());

#ifdef USE_PEERS_TO_DELETE_MUTEX
      dlog("scheduling peer for deletion: ${peer} (may block on a mutex here)", ("peer", peer_to_delete->get_remote_endpoint()));

      unsigned number_of_peers_to_delete;
      {
        fc::scoped_lock<fc::mutex> lock(_peers_to_delete_mutex);
        _peers_to_delete.emplace_back(peer_to_delete);
        number_of_peers_to_delete = _peers_to_delete.size();
      }
      dlog("peer scheduled for deletion: ${peer}", ("peer", peer_to_delete->get_remote_endpoint()));

      if (!_node_is_shutting_down &&
          (!_delayed_peer_deletion_task_done.valid() || _delayed_peer_deletion_task_done.ready()))
      {
        dlog("asyncing delayed_peer_deletion_task to delete ${size} peers", ("size", number_of_peers_to_delete));
        _delayed_peer_deletion_task_done = fc::async([this](){ delayed_peer_deletion_task(); }, "delayed_peer_deletion_task" );
    }
      else
        dlog("delayed_peer_deletion_task is already scheduled (current size of _peers_to_delete is ${size})", ("size", number_of_peers_to_delete));
#else
      dlog("scheduling peer for deletion: ${peer} (this will not block)");
      _peers_to_delete.push_back(peer_to_delete);
      if (!_node_is_shutting_down &&
          (!_delayed_peer_deletion_task_done.valid() || _delayed_peer_deletion_task_done.ready()))
      {
        dlog("asyncing delayed_peer_deletion_task to delete ${size} peers", ("size", _peers_to_delete.size()));
        _delayed_peer_deletion_task_done = fc::async([this](){ delayed_peer_deletion_task(); }, "delayed_peer_deletion_task" );
      }
      else
        dlog("delayed_peer_deletion_task is already scheduled (current size of _peers_to_delete is ${size})", ("size", _peers_to_delete.size()));

#endif
    }

    bool NodeImpl::is_accepting_new_connections()
    {
      VERIFY_CORRECT_THREAD();
      return !_p2p_network_connect_loop_done.canceled() && get_number_of_connections() <= _maximum_number_of_connections;
    }

    bool NodeImpl::is_wanting_new_connections()
    {
      VERIFY_CORRECT_THREAD();
      return !_p2p_network_connect_loop_done.canceled() && get_number_of_connections() < _desired_number_of_connections;
    }

    uint32_t NodeImpl::get_number_of_connections()
    {
      VERIFY_CORRECT_THREAD();
      return (uint32_t)(_handshaking_connections.size() + _active_connections.size());
    }

    PeerConnectionPtr NodeImpl::get_peer_by_node_id(const NodeIdType& node_id)
    {
      for (const PeerConnectionPtr& active_peer : _active_connections)
        if (node_id == active_peer->node_id)
          return active_peer;
      for (const PeerConnectionPtr& handshaking_peer : _handshaking_connections)
        if (node_id == handshaking_peer->node_id)
          return handshaking_peer;
      return PeerConnectionPtr();
    }

    bool NodeImpl::is_already_connected_to_id(const NodeIdType& node_id)
    {
      VERIFY_CORRECT_THREAD();
      if (node_id == _node_id)
      {
        dlog("is_already_connected_to_id returning true because the peer is us");
        return true;
      }
      for (const PeerConnectionPtr active_peer : _active_connections)
        if (node_id == active_peer->node_id)
        {
          dlog("is_already_connected_to_id returning true because the peer is already in our active list");
          return true;
        }
      for (const PeerConnectionPtr handshaking_peer : _handshaking_connections)
        if (node_id == handshaking_peer->node_id)
        {
          dlog("is_already_connected_to_id returning true because the peer is already in our handshaking list");
          return true;
        }
      return false;
    }

    // merge addresses received from a peer into our database
    bool NodeImpl::merge_address_info_with_potential_peer_database(const std::vector<AddressInfo> addresses)
    {
      VERIFY_CORRECT_THREAD();
      bool new_information_received = false;
      for (const AddressInfo& address : addresses)
      {
        if (address.firewalled == goopal::net::FirewalledState::not_firewalled)
        {
          PotentialPeerEntry updated_peer_entry = _potential_peer_db.lookup_or_create_entry_for_endpoint(address.remote_endpoint);
          if (address.last_seen_time > updated_peer_entry.last_seen_time)
            new_information_received = true;
          updated_peer_entry.last_seen_time = std::max(address.last_seen_time, updated_peer_entry.last_seen_time);
          _potential_peer_db.update_entry(updated_peer_entry);
        }
      }
      return new_information_received;
    }

    void NodeImpl::display_current_connections()
    {
      VERIFY_CORRECT_THREAD();
      dlog("Currently have ${current} of [${desired}/${max}] connections",
           ("current", get_number_of_connections())
           ("desired", _desired_number_of_connections)
           ("max", _maximum_number_of_connections));
      dlog("   my id is ${id}", ("id", _node_id));

      for (const PeerConnectionPtr& active_connection : _active_connections)
      {
        dlog("        active: ${endpoint} with ${id}   [${direction}]",
             ("endpoint", active_connection->get_remote_endpoint())
             ("id", active_connection->node_id)
             ("direction", active_connection->direction));
      }
      for (const PeerConnectionPtr& handshaking_connection : _handshaking_connections)
      {
        dlog("   handshaking: ${endpoint} with ${id}  [${direction}]",
             ("endpoint", handshaking_connection->get_remote_endpoint())
             ("id", handshaking_connection->node_id)
             ("direction", handshaking_connection->direction));
      }
    }

    void NodeImpl::on_message(PeerConnection* originating_peer, const Message& received_message)
    {
      VERIFY_CORRECT_THREAD();
      MessageHashType message_hash = received_message.id();
      dlog("handling message ${type} ${hash} size ${size} from peer ${endpoint}",
           ("type", goopal::net::CoreMessageTypeEnum(received_message.msg_type))("hash", message_hash)
           ("size", received_message.size)
           ("endpoint", originating_peer->get_remote_endpoint()));
      switch ( received_message.msg_type )
      {
      case CoreMessageTypeEnum::hello_message_type:
        on_hello_message(originating_peer, received_message.as<HelloMessage>());
        break;
      case CoreMessageTypeEnum::connection_accepted_message_type:
        on_connection_accepted_message(originating_peer, received_message.as<ConnectionAcceptedMessage>());
        break;
      case CoreMessageTypeEnum::connection_rejected_message_type:
        on_connection_rejected_message(originating_peer, received_message.as<ConnectionRejectedMessage>());
        break;
      case CoreMessageTypeEnum::address_request_message_type:
        on_address_request_message(originating_peer, received_message.as<AddressRequestMessage>());
        break;
      case CoreMessageTypeEnum::address_message_type:
        on_address_message(originating_peer, received_message.as<AddressMessage>());
        break;
      case CoreMessageTypeEnum::fetch_blockchain_item_ids_message_type:
        on_fetch_blockchain_item_ids_message(originating_peer, received_message.as<FetchBlockchainItemIdsMessage>());
        break;
      case CoreMessageTypeEnum::blockchain_item_ids_inventory_message_type:
        on_blockchain_item_ids_inventory_message(originating_peer, received_message.as<BlockchainItemIdsInventoryMessage>());
        break;
      case CoreMessageTypeEnum::fetch_items_message_type:
        on_fetch_items_message(originating_peer, received_message.as<FetchItemsMessage>());
        break;
      case CoreMessageTypeEnum::item_not_available_message_type:
        on_item_not_available_message(originating_peer, received_message.as<ItemNotAvailableMessage>());
        break;
      case CoreMessageTypeEnum::item_ids_inventory_message_type:
        on_item_ids_inventory_message(originating_peer, received_message.as<ItemIdsInventoryMessage>());
        break;
      case CoreMessageTypeEnum::closing_connection_message_type:
        on_closing_connection_message(originating_peer, received_message.as<ClosingConnectionMessage>());
        break;
      case goopal::client::MessageTypeEnum::block_message_type:
        process_block_message(originating_peer, received_message, message_hash);
        break;
      case CoreMessageTypeEnum::current_time_request_message_type:
        on_current_time_request_message(originating_peer, received_message.as<CurrentTimeRequestMessage>());
        break;
      case CoreMessageTypeEnum::current_time_reply_message_type:
        on_current_time_reply_message(originating_peer, received_message.as<CurrentTimeReplyMessage>());
        break;
      case CoreMessageTypeEnum::check_firewall_message_type:
        on_check_firewall_message(originating_peer, received_message.as<CheckFirewallMessage>());
        break;
      case CoreMessageTypeEnum::check_firewall_reply_message_type:
        on_check_firewall_reply_message(originating_peer, received_message.as<CheckFirewallReplyMessage>());
        break;
      case CoreMessageTypeEnum::get_current_connections_request_message_type:
        on_get_current_connections_request_message(originating_peer, received_message.as<GetCurrentConnectionsRequestMessage>());
        break;
      case CoreMessageTypeEnum::get_current_connections_reply_message_type:
        on_get_current_connections_reply_message(originating_peer, received_message.as<GetCurrentConnectionsReplyMessage>());
        break;

      default:
        // ignore any message in between core_message_type_first and _last that we don't handle above
        // to allow us to add messages in the future
        if (received_message.msg_type < CoreMessageTypeEnum::core_message_type_first ||
            received_message.msg_type > CoreMessageTypeEnum::core_message_type_last)
          process_ordinary_message(originating_peer, received_message, message_hash);
        break;
      }
    }


    fc::variant_object NodeImpl::generate_hello_user_data()
    {
      VERIFY_CORRECT_THREAD();
      // for the time being, shoehorn a bunch of properties into the user_data variant object,
      // which lets us add and remove fields without changing the protocol.  Once we
      // settle on what we really want in there, we'll likely promote them to first
      // class fields in the hello message
      fc::mutable_variant_object user_data;
      user_data["goopal_git_revision_sha"] = goopal::utilities::git_revision_sha;
      user_data["goopal_git_revision_unix_timestamp"] = goopal::utilities::git_revision_unix_timestamp;
      user_data["fc_git_revision_sha"] = fc::git_revision_sha;
      user_data["fc_git_revision_unix_timestamp"] = fc::git_revision_unix_timestamp;
#if defined( __APPLE__ )
      user_data["platform"] = "osx";
#elif defined( __linux__ )
      user_data["platform"] = "linux";
#elif defined( _MSC_VER )
      user_data["platform"] = "win32";
#else
      user_data["platform"] = "other";
#endif
      user_data["bitness"] = sizeof(void*) * 8;

      user_data["node_id"] = _node_id;

      ItemHashType head_block_id = _delegate->get_head_block_id();
      user_data["last_known_block_hash"] = head_block_id;
      user_data["last_known_block_number"] = _delegate->get_block_number(head_block_id);
      user_data["last_known_block_time"] = _delegate->get_block_time(head_block_id);
	  user_data["delegate_num"] = GOP_BLOCKCHAIN_NUM_DELEGATES;
      if (!_hard_fork_block_numbers.empty())
        user_data["last_known_fork_block_number"] = _hard_fork_block_numbers.back();

      return user_data;
    }
    void NodeImpl::parse_hello_user_data_for_peer(PeerConnection* originating_peer, const fc::variant_object& user_data)
    {
      VERIFY_CORRECT_THREAD();
      // try to parse data out of the user_agent string
      if (user_data.contains("goopal_git_revision_sha"))
        originating_peer->goopal_git_revision_sha = user_data["goopal_git_revision_sha"].as_string();
      if (user_data.contains("goopal_git_revision_unix_timestamp"))
        originating_peer->goopal_git_revision_unix_timestamp = fc::time_point_sec(user_data["goopal_git_revision_unix_timestamp"].as<uint32_t>());
      if (user_data.contains("fc_git_revision_sha"))
        originating_peer->fc_git_revision_sha = user_data["fc_git_revision_sha"].as_string();
      if (user_data.contains("fc_git_revision_unix_timestamp"))
        originating_peer->fc_git_revision_unix_timestamp = fc::time_point_sec(user_data["fc_git_revision_unix_timestamp"].as<uint32_t>());
      if (user_data.contains("platform"))
        originating_peer->platform = user_data["platform"].as_string();
      if (user_data.contains("bitness"))
        originating_peer->bitness = user_data["bitness"].as<uint32_t>();
      if (user_data.contains("node_id"))
        originating_peer->node_id = user_data["node_id"].as<NodeIdType>();
      if (user_data.contains("last_known_fork_block_number"))
        originating_peer->last_known_fork_block_number = user_data["last_known_fork_block_number"].as<uint32_t>();
    }

    void NodeImpl::on_hello_message(PeerConnection* originating_peer, const HelloMessage& hello_message_received)
    {
      VERIFY_CORRECT_THREAD();
      // this already_connected check must come before we fill in peer data below
      NodeIdType peer_node_id = hello_message_received.node_public_key;
      try
      {
        peer_node_id = hello_message_received.user_data["node_id"].as<NodeIdType>();
      }
      catch (const fc::exception&)
      {
        // either it's not there or it's not a valid session id.  either way, ignore.
      }
      bool already_connected_to_this_peer = is_already_connected_to_id(peer_node_id);

      // validate the node id
      fc::sha256::encoder shared_secret_encoder;
      fc::sha512 shared_secret = originating_peer->get_shared_secret();
      shared_secret_encoder.write(shared_secret.data(), sizeof(shared_secret));
      fc::ecc::public_key expected_node_public_key(hello_message_received.signed_shared_secret, shared_secret_encoder.result(), false);

      // store off the data provided in the hello message
      originating_peer->user_agent = hello_message_received.user_agent;
      originating_peer->node_public_key = hello_message_received.node_public_key;
      originating_peer->node_id = hello_message_received.node_public_key; // will probably be overwritten in parse_hello_user_data_for_peer()
      originating_peer->core_protocol_version = hello_message_received.core_protocol_version;
      originating_peer->inbound_address = hello_message_received.inbound_address;
      originating_peer->inbound_port = hello_message_received.inbound_port;
      originating_peer->outbound_port = hello_message_received.outbound_port;

      parse_hello_user_data_for_peer(originating_peer, hello_message_received.user_data);
	  uint32_t delegate_num = 0;
	  if (hello_message_received.user_data.contains("delegate_num"))
	  {
		  delegate_num = hello_message_received.user_data["delegate_num"].as<uint32_t>();
	  }
      // if they didn't provide a last known fork, try to guess it
      if (originating_peer->last_known_fork_block_number == 0 &&
          originating_peer->goopal_git_revision_unix_timestamp)
      {
        uint32_t unix_timestamp = originating_peer->goopal_git_revision_unix_timestamp->sec_since_epoch();
        originating_peer->last_known_fork_block_number = _delegate->estimate_last_known_fork_from_git_revision_timestamp(unix_timestamp);
      }

      // now decide what to do with it
      if (originating_peer->their_state == PeerConnection::TheirConnectionState::just_connected)
      {
        if (hello_message_received.node_public_key != expected_node_public_key.serialize())
        {
          wlog("Invalid signature in hello message from peer ${peer}", ("peer", originating_peer->get_remote_endpoint()));
          std::string rejection_message("Invalid signature in hello message");
          ConnectionRejectedMessage connection_rejected(_user_agent_string, core_protocol_version,
                                                          originating_peer->get_socket().remote_endpoint(),
                                                          RejectionReasonCode::invalid_hello_message,
                                                          rejection_message);

          originating_peer->their_state = PeerConnection::TheirConnectionState::connection_rejected;
          originating_peer->send_message( Message(connection_rejected ) );
          // for this type of message, we're immediately disconnecting this peer
          disconnect_from_peer( originating_peer, "Invalid signature in hello message" );
          return;
        }
		if (delegate_num != GOP_BLOCKCHAIN_NUM_DELEGATES)
		{
			wlog("Invalid client");
			std::string rejection_message("Different client in hello message");
			ConnectionRejectedMessage connection_rejected(_user_agent_string, core_protocol_version,
				originating_peer->get_socket().remote_endpoint(),
				RejectionReasonCode::client_too_old,
				rejection_message);
            originating_peer->their_state = PeerConnection::TheirConnectionState::connection_rejected;
			originating_peer->send_message(Message(connection_rejected));
			// for this type of message, we're immediately disconnecting this peer
			disconnect_from_peer(originating_peer, "Different client in hello message");
			return;
		}
        if (hello_message_received.chain_id != _chain_id)
        {
          wlog("Received hello message from peer on a different chain: ${message}", ("message", hello_message_received));
          std::ostringstream rejection_message;
          rejection_message << "You're on a different chain than I am.  I'm on " << _chain_id.str() <<
                              " and you're on " << hello_message_received.chain_id.str();
          ConnectionRejectedMessage connection_rejected(_user_agent_string, core_protocol_version,
                                                          originating_peer->get_socket().remote_endpoint(),
                                                          RejectionReasonCode::different_chain,
                                                          rejection_message.str());

          originating_peer->their_state = PeerConnection::TheirConnectionState::connection_rejected;
          originating_peer->send_message(Message(connection_rejected));
          // for this type of message, we're immediately disconnecting this peer, instead of trying to
          // allowing her to ask us for peers (any of our peers will be on the same chain as us, so there's no
          // benefit of sharing them)
          disconnect_from_peer(originating_peer, "You are on a different chain from me");
          return;
        }
        if (originating_peer->last_known_fork_block_number != 0)
        {
          uint32_t next_fork_block_number = get_next_known_hard_fork_block_number(originating_peer->last_known_fork_block_number);
          if (next_fork_block_number != 0)
          {
            // we know about a fork they don't.  See if we've already passed that block.  If we have, don't let them
            // connect because we won't be able to give them anything useful
            uint32_t head_block_num = _delegate->get_block_number(_delegate->get_head_block_id());
            if (next_fork_block_number < head_block_num)
            {
#ifdef ENABLE_DEBUG_ULOGS
              ulog("Rejecting connection from peer because their version is too old.  Their version date: ${date}", ("date", originating_peer->goopal_git_revision_unix_timestamp));
#endif
              wlog("Received hello message from peer running a version of that can only understand blocks up to #${their_hard_fork}, but I'm at head block number #${my_block_number}",
                   ("their_hard_fork", next_fork_block_number)("my_block_number", head_block_num));
              std::ostringstream rejection_message;
              rejection_message << "Your client is outdated -- you can only understand blocks up to #" << next_fork_block_number << ", but I'm already on block #" << head_block_num;
              ConnectionRejectedMessage connection_rejected(_user_agent_string, core_protocol_version,
                                                              originating_peer->get_socket().remote_endpoint(),
                                                              RejectionReasonCode::unspecified,
                                                              rejection_message.str() );

              originating_peer->their_state = PeerConnection::TheirConnectionState::connection_rejected;
              originating_peer->send_message(Message(connection_rejected));
              // for this type of message, we're immediately disconnecting this peer, instead of trying to
              // allowing her to ask us for peers (any of our peers will be on the same chain as us, so there's no
              // benefit of sharing them)
              disconnect_from_peer(originating_peer, "Your client is too old, please upgrade");
              return;
            }
          }
        }
        if (already_connected_to_this_peer)
        {

          ConnectionRejectedMessage connection_rejected;
          if (_node_id == originating_peer->node_id)
            connection_rejected = ConnectionRejectedMessage(_user_agent_string, core_protocol_version,
                                                              originating_peer->get_socket().remote_endpoint(),
                                                              RejectionReasonCode::connected_to_self,
                                                              "I'm connecting to myself");
          else
            connection_rejected = ConnectionRejectedMessage(_user_agent_string, core_protocol_version,
                                                              originating_peer->get_socket().remote_endpoint(),
                                                              RejectionReasonCode::already_connected,
                                                              "I'm already connected to you");
          originating_peer->their_state = PeerConnection::TheirConnectionState::connection_rejected;
          originating_peer->send_message(Message(connection_rejected));
          dlog("Received a hello_message from peer ${peer} that I'm already connected to (with id ${id}), rejection",
               ("peer", originating_peer->get_remote_endpoint())
               ("id", originating_peer->node_id));
        }
#ifdef ENABLE_P2P_DEBUGGING_API
        else if(!_allowed_peers.empty() &&
                _allowed_peers.find(originating_peer->node_id) == _allowed_peers.end())
        {
          ConnectionRejectedMessage connection_rejected(_user_agent_string, core_protocol_version,
                                                          originating_peer->get_socket().remote_endpoint(),
                                                          RejectionReasonCode::blocked,
                                                          "you are not in my allowed_peers list");
          originating_peer->their_state = PeerConnection::TheirConnectionState::connection_rejected;
          originating_peer->send_message( Message(connection_rejected ) );
          dlog( "Received a hello_message from peer ${peer} who isn't in my allowed_peers list, rejection", ("peer", originating_peer->get_remote_endpoint() ) );
        }
#endif // ENABLE_P2P_DEBUGGING_API
        else
        {
          // whether we're planning on accepting them as a peer or not, they seem to be a valid node,
          // so add them to our database if they're not firewalled

          // in the hello message, the peer sent us the IP address and port it thought it was connecting from.
          // If they match the IP and port we see, we assume that they're actually on the internet and they're not
          // firewalled.
          fc::ip::endpoint peers_actual_outbound_endpoint = originating_peer->get_socket().remote_endpoint();
          if( peers_actual_outbound_endpoint.get_address() == originating_peer->inbound_address &&
              peers_actual_outbound_endpoint.port() == originating_peer->outbound_port )
          {
            if( originating_peer->inbound_port == 0 )
            {
              dlog( "peer does not appear to be firewalled, but they did not give an inbound port so I'm treating them as if they are." );
              originating_peer->is_firewalled = FirewalledState::firewalled;
            }
            else
            {
              // peer is not firewalled, add it to our database
              fc::ip::endpoint peers_inbound_endpoint(originating_peer->inbound_address, originating_peer->inbound_port);
              PotentialPeerEntry updated_peer_entry = _potential_peer_db.lookup_or_create_entry_for_endpoint(peers_inbound_endpoint);
              _potential_peer_db.update_entry(updated_peer_entry);
              originating_peer->is_firewalled = FirewalledState::not_firewalled;
            }
          }
          else
          {
            dlog("peer is firewalled: they think their outbound endpoint is ${reported_endpoint}, but I see it as ${actual_endpoint}",
                 ("reported_endpoint", fc::ip::endpoint(originating_peer->inbound_address, originating_peer->outbound_port))
                 ("actual_endpoint", peers_actual_outbound_endpoint));
            originating_peer->is_firewalled = FirewalledState::firewalled;
          }

          if (!is_accepting_new_connections())
          {
            ConnectionRejectedMessage connection_rejected(_user_agent_string, core_protocol_version,
                                                            originating_peer->get_socket().remote_endpoint(),
                                                            RejectionReasonCode::not_accepting_connections,
                                                            "not accepting any more incoming connections");
            originating_peer->their_state = PeerConnection::TheirConnectionState::connection_rejected;
            originating_peer->send_message(Message(connection_rejected));
            dlog("Received a hello_message from peer ${peer}, but I'm not accepting any more connections, rejection",
                 ("peer", originating_peer->get_remote_endpoint()));
          }
          else
          {
              originating_peer->their_state = PeerConnection::TheirConnectionState::connection_accepted;
            originating_peer->send_message(Message(ConnectionAcceptedMessage()));
            dlog("Received a hello_message from peer ${peer}, sending reply to accept connection",
                 ("peer", originating_peer->get_remote_endpoint()));
          }
        }
      }
      else
      {
        // we can wind up here if we've connected to ourself, and the source and
        // destination endpoints are the same, causing messages we send out
        // to arrive back on the initiating socket instead of the receiving
        // socket.  If we did a complete job of enumerating local addresses,
        // we could avoid directly connecting to ourselves, or at least detect
        // immediately when we did it and disconnect.

        // The only way I know of that we'd get an unexpected hello that we
        //  can't really guard against is if we do a simulatenous open, we
        // probably need to think through that case.  We're not attempting that
        // yet, though, so it's ok to just disconnect here.
        wlog("unexpected hello_message from peer, disconnecting");
        disconnect_from_peer(originating_peer, "Received a unexpected hello_message");
      }
    }

    void NodeImpl::on_connection_accepted_message(PeerConnection* originating_peer, const ConnectionAcceptedMessage& connection_accepted_message_received)
    {
      VERIFY_CORRECT_THREAD();
      dlog("Received a connection_accepted in response to my \"hello\" from ${peer}", ("peer", originating_peer->get_remote_endpoint()));
      originating_peer->negotiation_status = PeerConnection::ConnectionNegotiationStatus::peer_connection_accepted;
      originating_peer->our_state = PeerConnection::OurConnectionState::connection_accepted;
      originating_peer->send_message(AddressRequestMessage());
      fc::time_point now = fc::time_point::now();
      if (_is_firewalled == FirewalledState::unknown &&
          _last_firewall_check_message_sent < now - fc::minutes(5) &&
          originating_peer->core_protocol_version >= 106)
      {
        wlog("I don't know if I'm firewalled.  Sending a firewall check message to peer ${peer}",
             ("peer", originating_peer->get_remote_endpoint()));
        originating_peer->firewall_check_state = new FirewallCheckStateData;

        originating_peer->send_message(CheckFirewallMessage());
        _last_firewall_check_message_sent = now;
      }
    }

    void NodeImpl::on_connection_rejected_message(PeerConnection* originating_peer, const ConnectionRejectedMessage& connection_rejected_message_received)
    {
      VERIFY_CORRECT_THREAD();
      if (originating_peer->our_state == PeerConnection::OurConnectionState::just_connected)
      {
        ilog("Received a rejection from ${peer} in response to my \"hello\", reason: \"${reason}\"",
             ("peer", originating_peer->get_remote_endpoint())
             ("reason", connection_rejected_message_received.reason_string));

        if (connection_rejected_message_received.reason_code == RejectionReasonCode::connected_to_self 
			|| connection_rejected_message_received.reason_code == RejectionReasonCode::client_too_old)
        {
          _potential_peer_db.erase(originating_peer->get_socket().remote_endpoint());
          move_peer_to_closing_list(originating_peer->shared_from_this());
          originating_peer->close_connection();
        }
        else
        {
          // update our database to entry that we were rejected so we won't try to connect again for a while
          // this only happens on connections we originate, so we should already know that peer is not firewalled
          fc::optional<PotentialPeerEntry> updated_peer_entry = _potential_peer_db.lookup_entry_for_endpoint(originating_peer->get_socket().remote_endpoint());
          if (updated_peer_entry)
          {
            updated_peer_entry->last_connection_disposition = last_connection_rejected;
            updated_peer_entry->last_connection_attempt_time = fc::time_point::now();
            _potential_peer_db.update_entry(*updated_peer_entry);
          }
        }

        originating_peer->negotiation_status = PeerConnection::ConnectionNegotiationStatus::peer_connection_rejected;
        originating_peer->our_state = PeerConnection::OurConnectionState::connection_rejected;
        originating_peer->send_message(AddressRequestMessage());
      }
      else
        FC_THROW( "unexpected connection_rejected_message from peer" );
    }

    void NodeImpl::on_address_request_message(PeerConnection* originating_peer, const AddressRequestMessage& address_request_message_received)
    {
      VERIFY_CORRECT_THREAD();
      dlog("Received an address request message");

      AddressMessage reply;
      if (!_peer_advertising_disabled)
      {
        reply.addresses.reserve(_active_connections.size());
        for (const PeerConnectionPtr& active_peer : _active_connections)
        {
          fc::optional<PotentialPeerEntry> updated_peer_entry = _potential_peer_db.lookup_entry_for_endpoint(*active_peer->get_remote_endpoint());
          if (updated_peer_entry)
          {
            updated_peer_entry->last_seen_time = fc::time_point::now();
            _potential_peer_db.update_entry(*updated_peer_entry);
          }

          reply.addresses.emplace_back(AddressInfo(*active_peer->get_remote_endpoint(),
                                                    fc::time_point::now(),
                                                    active_peer->round_trip_delay,
                                                    active_peer->node_id,
                                                    active_peer->direction,
                                                    active_peer->is_firewalled));
        }
      }
      //for( const potential_peer_entry& entry : _potential_peer_db )
      originating_peer->send_message(reply);
    }

    void NodeImpl::on_address_message(PeerConnection* originating_peer, const AddressMessage& address_message_received)
    {
      VERIFY_CORRECT_THREAD();
      dlog("Received an address message containing ${size} addresses", ("size", address_message_received.addresses.size()));
      for (const AddressInfo& address : address_message_received.addresses)
      {
        dlog("    ${endpoint} last seen ${time}", ("endpoint", address.remote_endpoint)("time", address.last_seen_time));
      }
      std::vector<goopal::net::AddressInfo> updated_addresses = address_message_received.addresses;
      for (AddressInfo& address : updated_addresses)
        address.last_seen_time = fc::time_point_sec(fc::time_point::now());
      bool new_information_received = merge_address_info_with_potential_peer_database(updated_addresses);
      if (new_information_received)
        trigger_p2p_network_connect_loop();

      if (_handshaking_connections.find(originating_peer->shared_from_this()) != _handshaking_connections.end())
      {
        // if we were handshaking, we need to continue with the next step in handshaking (which is either
        // ending handshaking and starting synchronization or disconnecting)
          if (originating_peer->our_state == PeerConnection::OurConnectionState::connection_rejected)
          disconnect_from_peer(originating_peer, "You rejected my connection request (hello message) so I'm disconnecting");
          else if (originating_peer->their_state == PeerConnection::TheirConnectionState::connection_rejected)
          disconnect_from_peer(originating_peer, "I rejected your connection request (hello message) so I'm disconnecting");
        else
        {
          fc::optional<fc::ip::endpoint> inbound_endpoint = originating_peer->get_endpoint_for_connecting();
          if (inbound_endpoint)
          {
            // mark the connection as successful in the database
            fc::optional<PotentialPeerEntry> updated_peer_entry = _potential_peer_db.lookup_entry_for_endpoint(*inbound_endpoint);
            if (updated_peer_entry)
            {
              updated_peer_entry->last_connection_disposition = last_connection_succeeded;
              _potential_peer_db.update_entry(*updated_peer_entry);
            }
          }

          originating_peer->negotiation_status = PeerConnection::ConnectionNegotiationStatus::negotiation_complete;
          move_peer_to_active_list(originating_peer->shared_from_this());
          new_peer_just_added(originating_peer->shared_from_this());
        }
      }
      // else if this was an active connection, then this was just a reply to our periodic address requests.
      // we've processed it, there's nothing else to do
    }

    void NodeImpl::on_fetch_blockchain_item_ids_message(PeerConnection* originating_peer,
                                                         const FetchBlockchainItemIdsMessage& fetch_blockchain_item_ids_message_received)
    {
      VERIFY_CORRECT_THREAD();
      ItemId peers_last_item_seen;
      if( !fetch_blockchain_item_ids_message_received.blockchain_synopsis.empty() )
        peers_last_item_seen = ItemId( fetch_blockchain_item_ids_message_received.item_type,
                                       fetch_blockchain_item_ids_message_received.blockchain_synopsis.back() );
      dlog( "sync: received a request for item ids after ${last_item_seen} from peer ${peer_endpoint} (full request: ${synopsis})",
           ( "last_item_seen", peers_last_item_seen )
           ( "peer_endpoint", originating_peer->get_remote_endpoint() )
           ( "synopsis", fetch_blockchain_item_ids_message_received.blockchain_synopsis ) );

      BlockchainItemIdsInventoryMessage reply_message;
      reply_message.item_hashes_available = _delegate->get_item_ids( fetch_blockchain_item_ids_message_received.item_type,
                                                                    fetch_blockchain_item_ids_message_received.blockchain_synopsis,
                                                                    reply_message.total_remaining_item_count );
      reply_message.item_type = fetch_blockchain_item_ids_message_received.item_type;

      bool disconnect_from_inhibited_peer = false;
      // if our client doesn't have any items after the item the peer requested, it will send back
      // a list containing the last item the peer requested
      if( reply_message.item_hashes_available.empty() )
        originating_peer->peer_needs_sync_items_from_us = false; /* I have no items in my blockchain */
      else if( !fetch_blockchain_item_ids_message_received.blockchain_synopsis.empty() &&
               reply_message.item_hashes_available.size() == 1 &&
               reply_message.item_hashes_available.back() == fetch_blockchain_item_ids_message_received.blockchain_synopsis.back() )
        {
        /* the last item in the peer's list matches the last item in our list */
        originating_peer->peer_needs_sync_items_from_us = false;
        if (originating_peer->inhibit_fetching_sync_blocks)
          disconnect_from_inhibited_peer = true; // delay disconnecting until after we send our reply to this fetch_blockchain_item_ids_message
        }
      else
        originating_peer->peer_needs_sync_items_from_us = true;

      if (!originating_peer->peer_needs_sync_items_from_us)
      {
        dlog("sync: peer is already in sync with us");
        // if we thought we had all the items this peer had, but now it turns out that we don't
        // have the last item it requested to send from,
        // we need to kick off another round of synchronization
        if (!originating_peer->we_need_sync_items_from_peer &&
            !fetch_blockchain_item_ids_message_received.blockchain_synopsis.empty() &&
            !_delegate->has_item(peers_last_item_seen))
        {
          dlog("sync: restarting sync with peer ${peer}", ("peer", originating_peer->get_remote_endpoint()));
          start_synchronizing_with_peer(originating_peer->shared_from_this());
        }
      }
      else
      {
        //dlog( "sync: peer is out of sync, sending peer ${count} items ids: ${item_ids}", ("count", reply_message.item_hashes_available.size() )("item_ids", reply_message.item_hashes_available ) );
        dlog("sync: peer is out of sync, sending peer ${count} items ids: first: ${first_item_id}, last: ${last_item_id}",
             ("count", reply_message.item_hashes_available.size())
             ("first_item_id", reply_message.item_hashes_available.front())
             ("last_item_id", reply_message.item_hashes_available.back()));
        if (!originating_peer->we_need_sync_items_from_peer &&
            !fetch_blockchain_item_ids_message_received.blockchain_synopsis.empty() &&
            !_delegate->has_item(peers_last_item_seen))
        {
          dlog("sync: restarting sync with peer ${peer}", ("peer", originating_peer->get_remote_endpoint()));
          start_synchronizing_with_peer(originating_peer->shared_from_this());
        }
      }
      originating_peer->send_message(reply_message);

      if (disconnect_from_inhibited_peer)
        {
        // the peer has all of our blocks, and we don't want any of theirs, so disconnect them
        disconnect_from_peer(originating_peer, "you are on a fork that I'm unable to switch to");
        return;
        }

      if (originating_peer->direction == PeerConnectionDirection::inbound &&
          _handshaking_connections.find(originating_peer->shared_from_this()) != _handshaking_connections.end())
      {
        // handshaking is done, move the connection to fully active status and start synchronizing
        dlog("peer ${endpoint} which was handshaking with us has started synchronizing with us, start syncing with it",
             ("endpoint", originating_peer->get_remote_endpoint()));
        fc::optional<fc::ip::endpoint> inbound_endpoint = originating_peer->get_endpoint_for_connecting();
        if (inbound_endpoint)
        {
          // mark the connection as successful in the database
          PotentialPeerEntry updated_peer_entry = _potential_peer_db.lookup_or_create_entry_for_endpoint(*inbound_endpoint);
          updated_peer_entry.last_connection_disposition = last_connection_succeeded;
          _potential_peer_db.update_entry(updated_peer_entry);
        }

        // transition it to our active list
        move_peer_to_active_list(originating_peer->shared_from_this());
        new_peer_just_added(originating_peer->shared_from_this());
      }
    }

    uint32_t NodeImpl::calculate_unsynced_block_count_from_all_peers()
    {
      VERIFY_CORRECT_THREAD();
      uint32_t max_number_of_unfetched_items = 0;
      for( const PeerConnectionPtr& peer : _active_connections )
      {
        uint32_t this_peer_number_of_unfetched_items = (uint32_t)peer->ids_of_items_to_get.size() + peer->number_of_unfetched_item_ids;
        max_number_of_unfetched_items = std::max(max_number_of_unfetched_items,
                                                 this_peer_number_of_unfetched_items);
      }
      return max_number_of_unfetched_items;
    }

    // get a blockchain synopsis that makes sense to send to the given peer.
    // If the peer isn't yet syncing with us, this is just a synopsis of our active blockchain
    // If the peer is syncing with us, it is a synopsis of our active blockchain plus the
    //    blocks the peer has already told us it has
    std::vector<ItemHashType> NodeImpl::create_blockchain_synopsis_for_peer(const PeerConnection* peer)
    {
      VERIFY_CORRECT_THREAD();
      ItemHashType reference_point = peer->last_block_delegate_has_seen;
      uint32_t reference_point_block_num = peer->last_block_number_delegate_has_seen;
      uint32_t number_of_blocks_after_reference_point = (uint32_t)peer->ids_of_items_to_get.size();

      // when we call _delegate->get_blockchain_synopsis(), we may yield and there's a
      // chance this peer's state will change before we get control back.  Save off
      // the stuff necessary for generating the synopsis.
      // This is pretty expensive, we should find a better way to do this
      std::unique_ptr<std::vector<ItemHashType> > original_ids_of_items_to_get(new std::vector<ItemHashType>(peer->ids_of_items_to_get.begin(), peer->ids_of_items_to_get.end()));

      std::vector<ItemHashType> synopsis = _delegate->get_blockchain_synopsis(_sync_item_type, reference_point, number_of_blocks_after_reference_point);
      assert(reference_point == ItemHashType() || !synopsis.empty());

      // if we passed in a reference point, we believe it is one the client has already accepted and should
      // be able to generate a synopsis based on it
      if( reference_point != ItemHashType() && synopsis.empty() )
        synopsis = _delegate->get_blockchain_synopsis( _sync_item_type, reference_point, number_of_blocks_after_reference_point );

      if( number_of_blocks_after_reference_point )
      {
        // then the synopsis is incomplete, add the missing elements from ids_of_items_to_get
        uint32_t true_high_block_num = reference_point_block_num + number_of_blocks_after_reference_point;
        uint32_t low_block_num = 1;
        do
        {
          if( low_block_num > reference_point_block_num )
            synopsis.push_back( (*original_ids_of_items_to_get)[low_block_num - reference_point_block_num - 1] );
          low_block_num += ( (true_high_block_num - low_block_num + 2 ) / 2 );
        }
        while ( low_block_num <= true_high_block_num );
        assert(synopsis.back() == original_ids_of_items_to_get->back());
      }
      return synopsis;
    }

    void NodeImpl::fetch_next_batch_of_item_ids_from_peer(PeerConnection* peer, bool reset_fork_tracking_data_for_peer /* = false */)
    {
      VERIFY_CORRECT_THREAD();
      if( reset_fork_tracking_data_for_peer )
      {
        peer->last_block_delegate_has_seen = ItemHashType();
        peer->last_block_number_delegate_has_seen = 0;
        peer->last_block_time_delegate_has_seen = _delegate->get_block_time(ItemHashType());
      }

      std::vector<ItemHashType> blockchain_synopsis = create_blockchain_synopsis_for_peer( peer );
      ItemHashType last_item_seen = blockchain_synopsis.empty() ? ItemHashType() : blockchain_synopsis.back();
      dlog( "sync: sending a request for the next items after ${last_item_seen} to peer ${peer}, (full request is ${blockchain_synopsis})",
           ( "last_item_seen", last_item_seen )
           ( "peer", peer->get_remote_endpoint() )
           ( "blockchain_synopsis", blockchain_synopsis ) );
      peer->item_ids_requested_from_peer = boost::make_tuple( ItemId(_sync_item_type, last_item_seen ), fc::time_point::now() );
      peer->send_message( FetchBlockchainItemIdsMessage(_sync_item_type, blockchain_synopsis ) );
    }

    void NodeImpl::on_blockchain_item_ids_inventory_message(PeerConnection* originating_peer,
                                                             const BlockchainItemIdsInventoryMessage& blockchain_item_ids_inventory_message_received )
    {
      VERIFY_CORRECT_THREAD();
      // ignore unless we asked for the data
      if( originating_peer->item_ids_requested_from_peer )
      {
        originating_peer->item_ids_requested_from_peer.reset();

        dlog( "sync: received a list of ${count} available items from ${peer_endpoint}",
             ( "count", blockchain_item_ids_inventory_message_received.item_hashes_available.size() )
             ( "peer_endpoint", originating_peer->get_remote_endpoint() ) );
        //for( const item_hash_t& item_hash : blockchain_item_ids_inventory_message_received.item_hashes_available )
        //{
        //  dlog( "sync:     ${hash}", ("hash", item_hash ) );
        //}

        // if the peer doesn't have any items after the one we asked for
        if( blockchain_item_ids_inventory_message_received.total_remaining_item_count == 0 &&
            ( blockchain_item_ids_inventory_message_received.item_hashes_available.empty() || // there are no items in the peer's blockchain.  this should only happen if our blockchain was empty when we requested, might want to verify that.
             ( blockchain_item_ids_inventory_message_received.item_hashes_available.size() == 1 &&
              _delegate->has_item( ItemId(blockchain_item_ids_inventory_message_received.item_type,
                                          blockchain_item_ids_inventory_message_received.item_hashes_available.front() ) ) ) ) && // we've already seen the last item in the peer's blockchain
            originating_peer->ids_of_items_to_get.empty() &&
            originating_peer->number_of_unfetched_item_ids == 0 ) // <-- is the last check necessary?
        {
          dlog( "sync: peer said we're up-to-date, entering normal operation with this peer" );
          originating_peer->we_need_sync_items_from_peer = false;

          uint32_t new_number_of_unfetched_items = calculate_unsynced_block_count_from_all_peers();
          _total_number_of_unfetched_items = new_number_of_unfetched_items;
          if( new_number_of_unfetched_items == 0 )
            _delegate->sync_status( blockchain_item_ids_inventory_message_received.item_type, 0 );

          return;
        }

        std::deque<ItemHashType> item_hashes_received( blockchain_item_ids_inventory_message_received.item_hashes_available.begin(),
                                                     blockchain_item_ids_inventory_message_received.item_hashes_available.end() );
        originating_peer->number_of_unfetched_item_ids = blockchain_item_ids_inventory_message_received.total_remaining_item_count;
        // flush any items this peer sent us that we've already received and processed from another peer
        if (!item_hashes_received.empty() &&
            originating_peer->ids_of_items_to_get.empty())
        {
          bool is_first_item_for_other_peer = false;
          for (const PeerConnectionPtr& peer : _active_connections)
            if (peer != originating_peer->shared_from_this() &&
                !peer->ids_of_items_to_get.empty() &&
                peer->ids_of_items_to_get.front() == blockchain_item_ids_inventory_message_received.item_hashes_available.front())
            {
              dlog("The item ${newitem} is the first item for peer ${peer}",
                   ("newitem", blockchain_item_ids_inventory_message_received.item_hashes_available.front())
                   ("peer", peer->get_remote_endpoint()));
              is_first_item_for_other_peer = true;
              break;
            }
          dlog("is_first_item_for_other_peer: ${is_first}.  item_hashes_received.size() = ${size}",
               ("is_first", is_first_item_for_other_peer)("size", item_hashes_received.size()));
          if (!is_first_item_for_other_peer)
          {
            while (!item_hashes_received.empty() &&
                   _delegate->has_item(ItemId(blockchain_item_ids_inventory_message_received.item_type,
                                               item_hashes_received.front())))
            {
              assert(item_hashes_received.front() != ItemHashType());
              originating_peer->last_block_delegate_has_seen = item_hashes_received.front();
              ++originating_peer->last_block_number_delegate_has_seen;
              originating_peer->last_block_time_delegate_has_seen = _delegate->get_block_time(item_hashes_received.front());
              dlog("popping item because delegate has already seen it.  peer's last block the delegate has seen is now ${block_id} (${block_num})", 
                   ("block_id", originating_peer->last_block_delegate_has_seen )("block_num", originating_peer->last_block_number_delegate_has_seen));
              item_hashes_received.pop_front();
            }
            dlog("after removing all items we have already seen, item_hashes_received.size() = ${size}", ("size", item_hashes_received.size()));
          }
        }
        else if (!item_hashes_received.empty())
        {
          // we received a list of items and we already have a list of items to fetch from this peer.
          // In the normal case, this list will immediately follow the existing list, meaning the
          // last hash of our existing list will match the first hash of the new list.

          // In the much less likely case, we've received a partial list of items from the peer, then
          // the peer switched forks before sending us the remaining list.  In this case, the first
          // hash in the new list may not be the last hash in the existing list (it may be earlier, or
          // it may not exist at all.

          // In either case, pop items off the back of our existing list until we find our first
          // item, then append our list.
          while (!originating_peer->ids_of_items_to_get.empty())
          {
            if (item_hashes_received.front() != originating_peer->ids_of_items_to_get.back())
              originating_peer->ids_of_items_to_get.pop_back();
            else
              break;
          }
          if (originating_peer->ids_of_items_to_get.empty())
          {
            // this happens when the peer has switched forks between the last inventory message and
            // this one, and there weren't any unfetched items in common
            // We don't know where in the blockchain the new front() actually falls, all we can
            // expect is that it is a block that we knew about because it should be one of the
            // blocks we sent in the initial synopsis.
            assert(_delegate->has_item(ItemId(_sync_item_type, item_hashes_received.front())));
            originating_peer->last_block_delegate_has_seen = item_hashes_received.front();
            originating_peer->last_block_number_delegate_has_seen = _delegate->get_block_number(item_hashes_received.front());
            originating_peer->last_block_time_delegate_has_seen = _delegate->get_block_time(item_hashes_received.front());
            item_hashes_received.pop_front();
          }
          else
          {
            // the common simple case: the new list extends the old.  pop off the duplicate element
            originating_peer->ids_of_items_to_get.pop_back();
          }
        }

        if (!item_hashes_received.empty() && !originating_peer->ids_of_items_to_get.empty())
          assert(item_hashes_received.front() != originating_peer->ids_of_items_to_get.back());

        // append the remaining items to the peer's list
        boost::push_back(originating_peer->ids_of_items_to_get, item_hashes_received);

        originating_peer->number_of_unfetched_item_ids = blockchain_item_ids_inventory_message_received.total_remaining_item_count;

        // at any given time, there's a maximum number of blocks that can possibly be out there
        // [(now - genesis time) / block interval].  If they offer us more blocks than that,
        // they must be an attacker or have a buggy client.
        fc::time_point_sec minimum_time_of_last_offered_block =
            originating_peer->last_block_time_delegate_has_seen + // timestamp of the block immediately before the first unfetched block
            originating_peer->number_of_unfetched_item_ids * GOP_BLOCKCHAIN_BLOCK_INTERVAL_SEC;
        if (minimum_time_of_last_offered_block > _delegate->get_blockchain_now() + GOP_NET_FUTURE_SYNC_BLOCKS_GRACE_PERIOD_SEC)
        {
          wlog("Disconnecting from peer ${peer} who offered us an implausible number of blocks, their last block would be in the future (${timestamp})",
               ("peer", originating_peer->get_remote_endpoint())
               ("timestamp", minimum_time_of_last_offered_block));
          fc::exception error_for_peer(FC_LOG_MESSAGE(error, "You offered me a list of more sync blocks than could possibly exist.  Total blocks offered: ${blocks}, Minimum time of the last block you offered: ${minimum_time_of_last_offered_block}, Now: ${now}",
                                                      ("blocks", originating_peer->number_of_unfetched_item_ids)
                                                      ("minimum_time_of_last_offered_block", minimum_time_of_last_offered_block)
                                                      ("now", _delegate->get_blockchain_now())));
          disconnect_from_peer(originating_peer,
                               "You offered me a list of more sync blocks than could possibly exist",
                               true, error_for_peer);
          return;
        }

        uint32_t new_number_of_unfetched_items = calculate_unsynced_block_count_from_all_peers();
        if (new_number_of_unfetched_items != _total_number_of_unfetched_items)
          _delegate->sync_status(blockchain_item_ids_inventory_message_received.item_type,
                                 new_number_of_unfetched_items);
        _total_number_of_unfetched_items = new_number_of_unfetched_items;

        if (blockchain_item_ids_inventory_message_received.total_remaining_item_count != 0)
        {
          // the peer hasn't sent us all the items it knows about.
          if (originating_peer->ids_of_items_to_get.size() > GOP_NET_MIN_BLOCK_IDS_TO_PREFETCH)
          {
            // we have a good number of item ids from this peer, start fetching blocks from it;
            // we'll switch back later to finish the job.
            trigger_fetch_sync_items_loop();
          }
          else
          {
            // keep fetching the peer's list of sync items until we get enough to switch into block-
            // fetchimg mode
            fetch_next_batch_of_item_ids_from_peer(originating_peer);
          }
        }
        else
        {
          // the peer has told us about all of the items it knows
          if (!originating_peer->ids_of_items_to_get.empty())
          {
            // we now know about all of the items the peer knows about, and there are some items on the list
            // that we should try to fetch.  Kick off the fetch loop.
            trigger_fetch_sync_items_loop();
          }
          else
          {
            // If we get here, the peer has sent us a non-empty list of items, but we have already
            // received all of the items from other peers.  Send a new request to the peer to
            // see if we're really in sync
            fetch_next_batch_of_item_ids_from_peer(originating_peer);
          }
        }
      }
      else
      {
        wlog("sync: received a list of sync items available, but I didn't ask for any!");
      }
    }

    Message NodeImpl::get_message_for_item(const ItemId& item)
    {
      try
      {
        return _message_cache.get_message(item.item_hash);
      }
      catch (fc::key_not_found_exception&)
      {}
      try
      {
        return _delegate->get_item(item);
      }
      catch (fc::key_not_found_exception&)
      {}
      return ItemNotAvailableMessage(item);
    }

    void NodeImpl::on_fetch_items_message(PeerConnection* originating_peer, const FetchItemsMessage& fetch_items_message_received)
    {
      VERIFY_CORRECT_THREAD();
      dlog("received items request for ids ${ids} of type ${type} from peer ${endpoint}",
           ( "ids", fetch_items_message_received.items_to_fetch )
           ( "type", fetch_items_message_received.item_type )
           ( "endpoint", originating_peer->get_remote_endpoint() ) );

      fc::optional<Message> last_block_message_sent;

      std::list<Message> reply_messages;
      for (const ItemHashType& item_hash : fetch_items_message_received.items_to_fetch)
      {
        try
        {
          Message requested_message = _message_cache.get_message(item_hash);
          dlog( "received item request for item ${id} from peer ${endpoint}, returning the item from my message cache",
               ( "endpoint", originating_peer->get_remote_endpoint() )
               ( "id", requested_message.id() ) );
          reply_messages.push_back( requested_message );
          if (fetch_items_message_received.item_type == block_message_type)
            last_block_message_sent = requested_message;
          continue;
        }
        catch (fc::key_not_found_exception&)
        {
           // it wasn't in our local cache, that's ok ask the client
        }

        ItemId item_to_fetch(fetch_items_message_received.item_type, item_hash);
        try
        {
          Message requested_message = _delegate->get_item(item_to_fetch);
          dlog( "received item request from peer ${endpoint}, returning the item from delegate with id ${id} size ${size}",
               ( "id", requested_message.id() )
               ( "size", requested_message.size )
               ( "endpoint", originating_peer->get_remote_endpoint() ) );
          reply_messages.push_back( requested_message );
          if (fetch_items_message_received.item_type == block_message_type)
            last_block_message_sent = requested_message;
          continue;
        }
        catch (fc::key_not_found_exception&)
        {
          reply_messages.push_back(ItemNotAvailableMessage(item_to_fetch));
          dlog( "received item request from peer ${endpoint} but we don't have it",
               ( "endpoint", originating_peer->get_remote_endpoint() ) );
        }
      }

      // if we sent them a block, update our entry of the last block they've seen accordingly
      if (last_block_message_sent)
      {
        goopal::client::BlockMessage block = last_block_message_sent->as<goopal::client::BlockMessage>();
        originating_peer->last_block_delegate_has_seen = block.block_id;
        originating_peer->last_block_number_delegate_has_seen = _delegate->get_block_number(block.block_id);
        originating_peer->last_block_time_delegate_has_seen = _delegate->get_block_time(block.block_id);
      }

      for (const Message& reply : reply_messages)
      {
        if (reply.msg_type == block_message_type)
          originating_peer->send_item(ItemId(block_message_type, reply.as<goopal::client::BlockMessage>().block_id));
        else
          originating_peer->send_message(reply);
      }
    }

    void NodeImpl::on_item_not_available_message(PeerConnection* originating_peer, const ItemNotAvailableMessage& item_not_available_message_received)
    {
      VERIFY_CORRECT_THREAD();
      const ItemId& requested_item = item_not_available_message_received.requested_item;
      auto regular_item_iter = originating_peer->items_requested_from_peer.find(requested_item);
      if (regular_item_iter != originating_peer->items_requested_from_peer.end())
      {
        originating_peer->items_requested_from_peer.erase( regular_item_iter );
        originating_peer->inventory_peer_advertised_to_us.erase( requested_item );
        if (is_item_in_any_peers_inventory(requested_item))
          _items_to_fetch.insert(PrioritizedItemId(requested_item, _items_to_fetch_sequence_counter++));
        wlog("Peer doesn't have the requested item.");
        trigger_fetch_items_loop();
        return;
        // TODO: reschedule fetching this item from a different peer
      }

      auto sync_item_iter = originating_peer->sync_items_requested_from_peer.find(requested_item);
      if (sync_item_iter != originating_peer->sync_items_requested_from_peer.end())
      {
        originating_peer->sync_items_requested_from_peer.erase(sync_item_iter);

        if (originating_peer->peer_needs_sync_items_from_us)
          originating_peer->inhibit_fetching_sync_blocks = true;
        else
          disconnect_from_peer(originating_peer, "You are missing a sync item you claim to have, your database is probably corrupted. Try --rebuild-index.",true,
                               fc::exception(FC_LOG_MESSAGE(error,"You are missing a sync item you claim to have, your database is probably corrupted. Try --rebuild-index.",
                               ("item_id",requested_item))));
        wlog("Peer doesn't have the requested sync item.  This really shouldn't happen");
        trigger_fetch_sync_items_loop();
        return;
      }

      dlog("Peer doesn't have an item we're looking for, which is fine because we weren't looking for it");
    }

    void NodeImpl::on_item_ids_inventory_message(PeerConnection* originating_peer, const ItemIdsInventoryMessage& item_ids_inventory_message_received)
    {
      VERIFY_CORRECT_THREAD();

      // expire old inventory so we'll be making decisions our about whether to fetch blocks below based only on recent inventory
      originating_peer->clear_old_inventory();

      dlog( "received inventory of ${count} items from peer ${endpoint}",
           ( "count", item_ids_inventory_message_received.item_hashes_available.size() )("endpoint", originating_peer->get_remote_endpoint() ) );
      for( const ItemHashType& item_hash : item_ids_inventory_message_received.item_hashes_available )
      {
        ItemId advertised_item_id(item_ids_inventory_message_received.item_type, item_hash);
        bool we_advertised_this_item_to_a_peer = false;
        bool we_requested_this_item_from_a_peer = false;
        for (const PeerConnectionPtr peer : _active_connections)
        {
          if (peer->inventory_advertised_to_peer.find(advertised_item_id) != peer->inventory_advertised_to_peer.end())
          {
            we_advertised_this_item_to_a_peer = true;
            break;
          }
          if (peer->items_requested_from_peer.find(advertised_item_id) != peer->items_requested_from_peer.end())
            we_requested_this_item_from_a_peer = true;
        }

        // if we have already advertised it to a peer, we must have it, no need to do anything else
        if (!we_advertised_this_item_to_a_peer)
        {
          // if the peer has flooded us with transactions, don't add these to the inventory to prevent our
          // inventory list from growing without bound.  We try to allow fetching blocks even when
          // we've stopped fetching transactions.
          if ((item_ids_inventory_message_received.item_type == goopal::client::trx_message_type &&
               originating_peer->is_inventory_advertised_to_us_list_full_for_transactions()) ||
              originating_peer->is_inventory_advertised_to_us_list_full())
            break;
          originating_peer->inventory_peer_advertised_to_us.insert(PeerConnection::TimestampedItemId(advertised_item_id, fc::time_point::now()));
          if (!we_requested_this_item_from_a_peer)
          {
            auto insert_result = _items_to_fetch.insert(PrioritizedItemId(advertised_item_id, _items_to_fetch_sequence_counter++));
            if (insert_result.second)
            {
              dlog("adding item ${item_hash} from inventory message to our list of items to fetch",
                   ("item_hash", item_hash));
              trigger_fetch_items_loop();
            }
          }
        }
      }

    }

    void NodeImpl::on_closing_connection_message(PeerConnection* originating_peer, const ClosingConnectionMessage& closing_connection_message_received)
    {
      VERIFY_CORRECT_THREAD();
      originating_peer->they_have_requested_close = true;

      if( closing_connection_message_received.closing_due_to_error )
      {
        elog( "Peer ${peer} is disconnecting us because of an error: ${msg}, exception: ${error}",
             ( "peer", originating_peer->get_remote_endpoint() )
             ( "msg", closing_connection_message_received.reason_for_closing )
             ( "error", closing_connection_message_received.error ) );
        std::ostringstream message;
        message << "Peer " << fc::variant( originating_peer->get_remote_endpoint() ).as_string() <<
                  " disconnected us: " << closing_connection_message_received.reason_for_closing;
        fc::exception detailed_error(FC_LOG_MESSAGE(warn, "Peer ${peer} is disconnecting us because of an error: ${msg}, exception: ${error}",
                                                    ( "peer", originating_peer->get_remote_endpoint() )
                                                    ( "msg", closing_connection_message_received.reason_for_closing )
                                                    ( "error", closing_connection_message_received.error ) ));
        _delegate->error_encountered( message.str(),
                                      detailed_error );
      }
      else
      {
        wlog( "Peer ${peer} is disconnecting us because: ${msg}",
             ( "peer", originating_peer->get_remote_endpoint() )
             ( "msg", closing_connection_message_received.reason_for_closing ) );
      }
      if( originating_peer->we_have_requested_close )
        originating_peer->close_connection();
    }

    void NodeImpl::on_connection_closed(PeerConnection* originating_peer)
    {
      VERIFY_CORRECT_THREAD();
      PeerConnectionPtr originating_peer_ptr = originating_peer->shared_from_this();
      _rate_limiter.remove_tcp_socket( &originating_peer->get_socket() );

      // if we closed the connection (due to timeout or handshake failure), we should have entryed an
      // error message to store in the peer database when we closed the connection
      fc::optional<fc::ip::endpoint> inbound_endpoint = originating_peer->get_endpoint_for_connecting();
      if (originating_peer->connection_closed_error && inbound_endpoint)
      {
        fc::optional<PotentialPeerEntry> updated_peer_entry = _potential_peer_db.lookup_entry_for_endpoint(*inbound_endpoint);
        if (updated_peer_entry)
        {
          updated_peer_entry->last_error = *originating_peer->connection_closed_error;
          _potential_peer_db.update_entry(*updated_peer_entry);
        }
      }

      _closing_connections.erase(originating_peer_ptr);
      _handshaking_connections.erase(originating_peer_ptr);
      _terminating_connections.erase(originating_peer_ptr);
      if (_active_connections.find(originating_peer_ptr) != _active_connections.end())
      {
        _active_connections.erase(originating_peer_ptr);

        if (inbound_endpoint && originating_peer_ptr->get_remote_endpoint())
        {
          fc::optional<PotentialPeerEntry> updated_peer_entry = _potential_peer_db.lookup_entry_for_endpoint(*inbound_endpoint);
          if (updated_peer_entry)
          {
            updated_peer_entry->last_seen_time = fc::time_point::now();
            _potential_peer_db.update_entry(*updated_peer_entry);
          }
        }
      }

      ilog("Remote peer ${endpoint} closed their connection to us", ("endpoint", originating_peer->get_remote_endpoint()));
      display_current_connections();
      trigger_p2p_network_connect_loop();

      // notify the node delegate so it can update the display
      if( _active_connections.size() != _last_reported_number_of_connections )
      {
        _last_reported_number_of_connections = (uint32_t)_active_connections.size();
        _delegate->connection_count_changed( _last_reported_number_of_connections );
      }

      // if we had delegated a firewall check to this peer, send it to another peer
      if (originating_peer->firewall_check_state)
      {
        if (originating_peer->firewall_check_state->requesting_peer != NodeIdType())
        {
          // it's a check we're doing for another node
          FirewallCheckStateData* firewall_check_state = originating_peer->firewall_check_state;
          originating_peer->firewall_check_state = nullptr;
          forward_firewall_check_to_next_available_peer(firewall_check_state);
        }
        else
        {
          // we were asking them to check whether we're firewalled.  we'll just let it
          // go for now
          delete originating_peer->firewall_check_state;
        }
      }

      // if we had requested any sync or regular items from this peer that we haven't
      // received yet, reschedule them to be fetched from another peer
      if (!originating_peer->sync_items_requested_from_peer.empty())
      {
        for (auto sync_item_and_time : originating_peer->sync_items_requested_from_peer)
          _active_sync_requests.erase(sync_item_and_time.first.item_hash);
        trigger_fetch_sync_items_loop();
      }

      if (!originating_peer->items_requested_from_peer.empty())
      {
        for (auto item_and_time : originating_peer->items_requested_from_peer)
        {
          if (is_item_in_any_peers_inventory(item_and_time.first))
            _items_to_fetch.insert(PrioritizedItemId(item_and_time.first, _items_to_fetch_sequence_counter++));
        }
        trigger_fetch_items_loop();
      }

      schedule_peer_for_deletion(originating_peer_ptr);
    }

    void NodeImpl::send_sync_block_to_node_delegate(const goopal::client::BlockMessage& block_message_to_send)
    {
      dlog("in send_sync_block_to_node_delegate()");
      bool client_accepted_block = false;
      bool discontinue_fetching_blocks_from_peer = false;

      fc::oexception handle_message_exception;

      try
      {
        _delegate->handle_message(block_message_to_send, true);
        ilog("Successfully pushed sync block ${num} (id:${id})",
             ("num", block_message_to_send.block.block_num)
             ("id", block_message_to_send.block_id));
        _most_recent_blocks_accepted.push_back(block_message_to_send.block_id);

        client_accepted_block = true;
      }
      catch (const block_older_than_undo_history& e)
      {
        wlog("Failed to push sync block ${num} (id:${id}): block is on a fork older than our undo history would "
             "allow us to switch to: ${e}",
             ("num", block_message_to_send.block.block_num)
             ("id", block_message_to_send.block_id)
             ("e", e.to_detail_string()));
        handle_message_exception = e;
        discontinue_fetching_blocks_from_peer = true;
      }
      catch (const fc::canceled_exception&)
      {
        throw;
      }
      catch (const fc::exception& e)
      {
        wlog("Failed to push sync block ${num} (id:${id}): client rejected sync block sent by peer: ${e}",
             ("num", block_message_to_send.block.block_num)
             ("id", block_message_to_send.block_id)
             ("e", e));
        handle_message_exception = e;
      }

      // build up lists for any potentially-blocking operations we need to do, then do them
      // at the end of this function
      std::set<PeerConnectionPtr> peers_with_newly_empty_item_lists;
      std::set<PeerConnectionPtr> peers_we_need_to_sync_to;
      std::map<PeerConnectionPtr, std::pair<std::string, fc::oexception> > peers_to_disconnect; // map peer -> pair<reason_string, exception>

      if( client_accepted_block )
      {
        --_total_number_of_unfetched_items;
        dlog("sync: client accpted the block, we now have only ${count} items left to fetch before we're in sync",
              ("count", _total_number_of_unfetched_items));
        bool is_fork_block = is_hard_fork_block(block_message_to_send.block.block_num);
        for (const PeerConnectionPtr& peer : _active_connections)
        {
          ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections
          bool disconnecting_this_peer = false;
          if (is_fork_block)
          {
            // we just pushed a hard fork block.  Find out if this peer is running a client
            // that will be unable to process future blocks
            if (peer->last_known_fork_block_number != 0)
            {
              uint32_t next_fork_block_number = get_next_known_hard_fork_block_number(peer->last_known_fork_block_number);
              if (next_fork_block_number != 0 &&
                  next_fork_block_number <= block_message_to_send.block.block_num)
              {
                std::ostringstream disconnect_reason_stream;
                disconnect_reason_stream << "You need to upgrade your client due to hard fork at block " << block_message_to_send.block.block_num;
                peers_to_disconnect[peer] = std::make_pair(disconnect_reason_stream.str(),
                                                           fc::oexception(fc::exception(FC_LOG_MESSAGE(error, "You need to upgrade your client due to hard fork at block ${block_number}", 
                                                                                                       ("block_number", block_message_to_send.block.block_num)))));
#ifdef ENABLE_DEBUG_ULOGS
                ulog("Disconnecting from peer during sync because their version is too old.  Their version date: ${date}", ("date", peer->goopal_git_revision_unix_timestamp));
#endif
                disconnecting_this_peer = true;
              }
            }
          }
          if (!disconnecting_this_peer &&
              peer->ids_of_items_to_get.empty() && peer->ids_of_items_being_processed.empty())
          {
            dlog( "Cannot pop first element off peer ${peer}'s list, its list is empty", ("peer", peer->get_remote_endpoint() ) );
            // we don't know for sure that this peer has the item we just received.
            // If peer is still syncing to us, we know they will ask us for
            // sync item ids at least one more time and we'll notify them about
            // the item then, so there's no need to do anything.  If we still need items
            // from them, we'll be asking them for more items at some point, and
            // that will clue them in that they are out of sync.  If we're fully in sync
            // we need to kick off another round of synchronization with them so they can
            // find out about the new item.
            if (!peer->peer_needs_sync_items_from_us && !peer->we_need_sync_items_from_peer)
            {
              dlog("We will be restarting synchronization with peer ${peer}", ("peer", peer->get_remote_endpoint()));
              peers_we_need_to_sync_to.insert(peer);
            }
          }
          else if (!disconnecting_this_peer)
          {
            auto items_being_processed_iter = peer->ids_of_items_being_processed.find(block_message_to_send.block_id);
            if (items_being_processed_iter != peer->ids_of_items_being_processed.end())
            {
              peer->last_block_delegate_has_seen = block_message_to_send.block_id;
              peer->last_block_number_delegate_has_seen = block_message_to_send.block.block_num;
              peer->last_block_time_delegate_has_seen = block_message_to_send.block.timestamp;

              peer->ids_of_items_being_processed.erase(items_being_processed_iter);
              dlog("Removed item from ${endpoint}'s list of items being processed, still processing ${len} blocks",
                   ("endpoint", peer->get_remote_endpoint())("len", peer->ids_of_items_being_processed.size()));

              // if we just received the last item in our list from this peer, we will want to
              // send another request to find out if we are in sync, but we can't do this yet
              // (we don't want to allow a fiber swap in the middle of popping items off the list)
              if (peer->ids_of_items_to_get.empty() &&
                  peer->number_of_unfetched_item_ids == 0 &&
                  peer->ids_of_items_being_processed.empty())
                peers_with_newly_empty_item_lists.insert(peer);

              // in this case, we know the peer was offering us this exact item, no need to
              // try to inform them of its existence
            }
          }
        }
      }
      else
      {
        // invalid message received
        for (const PeerConnectionPtr& peer : _active_connections)
        {
          ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections

          if (peer->ids_of_items_being_processed.find(block_message_to_send.block_id) != peer->ids_of_items_being_processed.end())
          {
            if (discontinue_fetching_blocks_from_peer)
            {
              wlog("inhibiting fetching sync blocks from peer ${endpoint} because it is on a fork that's too old",
                   ("endpoint", peer->get_remote_endpoint()));
              peer->inhibit_fetching_sync_blocks = true;
            }
            else
              peers_to_disconnect[peer] = std::make_pair(std::string("You offered us a block that we reject as invalid"), fc::oexception(handle_message_exception));
          }
        }
      }

      for (auto& peer_to_disconnect : peers_to_disconnect)
      {
        const PeerConnectionPtr& peer = peer_to_disconnect.first;
        std::string reason_string;
        fc::oexception reason_exception;
        std::tie(reason_string, reason_exception) = peer_to_disconnect.second;
        wlog("disconnecting client ${endpoint} because it offered us the rejected block",
             ("endpoint", peer->get_remote_endpoint()));
        disconnect_from_peer(peer.get(), reason_string, true, reason_exception);
      }
      for (const PeerConnectionPtr& peer : peers_with_newly_empty_item_lists)
        fetch_next_batch_of_item_ids_from_peer(peer.get());

      for (const PeerConnectionPtr& peer : peers_we_need_to_sync_to)
        start_synchronizing_with_peer(peer);

      dlog("Leaving send_sync_block_to_node_delegate");

      if (// _suspend_fetching_sync_blocks && <-- you can use this if "maximum_number_of_blocks_to_handle_at_one_time" == "maximum_number_of_sync_blocks_to_prefetch"
          !_node_is_shutting_down &&
          (!_process_backlog_of_sync_blocks_done.valid() || _process_backlog_of_sync_blocks_done.ready()))
        _process_backlog_of_sync_blocks_done = fc::async([=](){ process_backlog_of_sync_blocks(); },
                                                         "process_backlog_of_sync_blocks");
    }

    void NodeImpl::process_backlog_of_sync_blocks()
    {
      VERIFY_CORRECT_THREAD();
      // garbage-collect the list of async tasks here for lack of a better place
      for (auto calls_iter = _handle_message_calls_in_progress.begin();
            calls_iter != _handle_message_calls_in_progress.end();)
      {
        if (calls_iter->ready())
          calls_iter = _handle_message_calls_in_progress.erase(calls_iter);
        else
          ++calls_iter;
      }

      dlog("in process_backlog_of_sync_blocks");
      if (_handle_message_calls_in_progress.size() >= _maximum_number_of_blocks_to_handle_at_one_time)
      {
        dlog("leaving process_backlog_of_sync_blocks because we're already processing too many blocks");
        return; // we will be rescheduled when the next block finishes its processing
      }
      dlog("currently ${count} blocks in the process of being handled", ("count", _handle_message_calls_in_progress.size()));


      if (_suspend_fetching_sync_blocks)
      {
        dlog("resuming processing sync block backlog because we only ${count} blocks in progress",
             ("count", _handle_message_calls_in_progress.size()));
        _suspend_fetching_sync_blocks = false;
      }


      // when syncing with multiple peers, it's possible that we'll have hundreds of blocks ready to push
      // to the client at once.  This can be slow, and we need to limit the number we push at any given
      // time to allow network traffic to continue so we don't end up disconnecting from peers
      //fc::time_point start_time = fc::time_point::now();
      //fc::time_point when_we_should_yield = start_time + fc::seconds(1);

      bool block_processed_this_iteration;
      unsigned blocks_processed = 0;

//       std::set<peer_connection_ptr> peers_with_newly_empty_item_lists;
//       std::set<peer_connection_ptr> peers_we_need_to_sync_to;
//       std::map<peer_connection_ptr, fc::oexception> peers_with_rejected_block;

      do
      {
        std::copy(std::make_move_iterator(_new_received_sync_items.begin()),
                  std::make_move_iterator(_new_received_sync_items.end()),
                  std::front_inserter(_received_sync_items));
        _new_received_sync_items.clear();
        dlog("currently ${count} sync items to consider", ("count", _received_sync_items.size()));

        block_processed_this_iteration = false;
        for (auto received_block_iter = _received_sync_items.begin();
             received_block_iter != _received_sync_items.end();
             ++received_block_iter)
        {

          // find out if this block is the next block on the active chain or one of the forks
          bool potential_first_block = false;
          for (const PeerConnectionPtr& peer : _active_connections)
          {
            ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections
            if (!peer->ids_of_items_to_get.empty() &&
                peer->ids_of_items_to_get.front() == received_block_iter->block_id)
            {
              potential_first_block = true;
              peer->ids_of_items_to_get.pop_front();
              peer->ids_of_items_being_processed.insert(received_block_iter->block_id);
            }
          }

          // if it is, process it, remove it from all sync peers lists
          if (potential_first_block)
          {
            // we can get into an interesting situation near the end of synchronization.  We can be in
            // sync with one peer who is sending us the last block on the chain via a regular inventory
            // message, while at the same time still be synchronizing with a peer who is sending us the
            // block through the sync mechanism.  Further, we must request both blocks because
            // we don't know they're the same (for the peer in normal operation, it has only told us the
            // message id, for the peer in the sync case we only known the block_id).
            if (std::find(_most_recent_blocks_accepted.begin(), _most_recent_blocks_accepted.end(),
                          received_block_iter->block_id) == _most_recent_blocks_accepted.end())
            {
              goopal::client::BlockMessage block_message_to_process = *received_block_iter;
              _received_sync_items.erase(received_block_iter);
              _handle_message_calls_in_progress.emplace_back(fc::async([this, block_message_to_process](){
                send_sync_block_to_node_delegate(block_message_to_process);
              }, "send_sync_block_to_node_delegate"));
              ++blocks_processed;
              block_processed_this_iteration = true;
            }
            else
              dlog("Already received and accepted this block (presumably through normal inventory mechanism), treating it as accepted");

            break; // start iterating _received_sync_items from the beginning
          } // end if potential_first_block
        } // end for each block in _received_sync_items

        if (_handle_message_calls_in_progress.size() >= _maximum_number_of_blocks_to_handle_at_one_time)
        {
          dlog("stopping processing sync block backlog because we have ${count} blocks in progress",
               ("count", _handle_message_calls_in_progress.size()));
          //ulog("stopping processing sync block backlog because we have ${count} blocks in progress, total on hand: ${received}",
          //     ("count", _handle_message_calls_in_progress.size())("received", _received_sync_items.size()));
          if (_received_sync_items.size() >= _maximum_number_of_sync_blocks_to_prefetch)
            _suspend_fetching_sync_blocks = true;
          break;
        }
      } while (block_processed_this_iteration);

      dlog("leaving process_backlog_of_sync_blocks, ${count} processed", ("count", blocks_processed));

      if (!_suspend_fetching_sync_blocks)
        trigger_fetch_sync_items_loop();
    }

    void NodeImpl::trigger_process_backlog_of_sync_blocks()
    {
      if (!_node_is_shutting_down &&
          (!_process_backlog_of_sync_blocks_done.valid() || _process_backlog_of_sync_blocks_done.ready()))
        _process_backlog_of_sync_blocks_done = fc::async([=](){ process_backlog_of_sync_blocks(); }, "process_backlog_of_sync_blocks");
    }

    void NodeImpl::process_block_during_sync(PeerConnection* originating_peer,
                                               const goopal::client::BlockMessage& block_message_to_process, const MessageHashType& message_hash )
    {
      VERIFY_CORRECT_THREAD();
      dlog( "received a sync block from peer ${endpoint}", ("endpoint", originating_peer->get_remote_endpoint() ) );

      // add it to the front of _received_sync_items, then process _received_sync_items to try to
      // pass as many messages as possible to the client.
      _new_received_sync_items.push_front( block_message_to_process );
      trigger_process_backlog_of_sync_blocks();
    }

    void NodeImpl::process_block_during_normal_operation(PeerConnection* originating_peer,
                                                           const goopal::client::BlockMessage& block_message_to_process,
                                                           const MessageHashType& message_hash )
    {
      fc::time_point message_receive_time = fc::time_point::now();

      dlog( "received a block from peer ${endpoint}, passing it to client", ("endpoint", originating_peer->get_remote_endpoint() ) );
      std::list<PeerConnectionPtr> peers_to_disconnect;
      std::string disconnect_reason;
      fc::oexception disconnect_exception;

      try
      {
        // we can get into an intersting situation near the end of synchronization.  We can be in
        // sync with one peer who is sending us the last block on the chain via a regular inventory
        // message, while at the same time still be synchronizing with a peer who is sending us the
        // block through the sync mechanism.  Further, we must request both blocks because
        // we don't know they're the same (for the peer in normal operation, it has only told us the
        // message id, for the peer in the sync case we only known the block_id).
        fc::time_point message_validated_time;
        if (std::find(_most_recent_blocks_accepted.begin(), _most_recent_blocks_accepted.end(),
                      block_message_to_process.block_id) == _most_recent_blocks_accepted.end())
        {
          _delegate->handle_message(block_message_to_process, false);
          message_validated_time = fc::time_point::now();
          ilog("Successfully pushed block ${num} (id:${id})",
               ("num", block_message_to_process.block.block_num)
               ("id", block_message_to_process.block_id));
          _most_recent_blocks_accepted.push_back(block_message_to_process.block_id);
        }
        else
          dlog( "Already received and accepted this block (presumably through sync mechanism), treating it as accepted" );

        dlog( "client validated the block, advertising it to other peers" );

        ItemId block_message_item_id(goopal::client::MessageTypeEnum::block_message_type, message_hash);
        uint32_t block_number = block_message_to_process.block.block_num;
        fc::time_point_sec block_time = block_message_to_process.block.timestamp;

        for (const PeerConnectionPtr& peer : _active_connections)
        {
          ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections

          auto iter = peer->inventory_peer_advertised_to_us.find(block_message_item_id);
          if (iter != peer->inventory_peer_advertised_to_us.end())
          {
            // this peer offered us the item.  It will eventually expire from the peer's
            // inventory_peer_advertised_to_us list after some time has passed (currently 2 minutes).
            // For now, it will remain there, which will prevent us from offering the peer this
            // block back when we rebroadcast the block below
            peer->last_block_delegate_has_seen = block_message_to_process.block_id;
            peer->last_block_number_delegate_has_seen = block_number;
            peer->last_block_time_delegate_has_seen = block_time;
          }
          peer->clear_old_inventory();
        }
        MessagePropagationData propagation_data{message_receive_time, message_validated_time, originating_peer->node_id};
        broadcast( block_message_to_process, propagation_data );
        _message_cache.block_accepted();

        if (is_hard_fork_block(block_number))
        {
          // we just pushed a hard fork block.  Find out if any of our peers are running clients
          // that will be unable to process future blocks
          for (const PeerConnectionPtr& peer : _active_connections)
          {
            if (peer->last_known_fork_block_number != 0)
            {
              uint32_t next_fork_block_number = get_next_known_hard_fork_block_number(peer->last_known_fork_block_number);
              if (next_fork_block_number != 0 &&
                  next_fork_block_number <= block_number)
              {
                peers_to_disconnect.push_back(peer);
#ifdef ENABLE_DEBUG_ULOGS
                ulog("Disconnecting from peer because their version is too old.  Their version date: ${date}", ("date", peer->goopal_git_revision_unix_timestamp));
#endif
              }
            }
          }
          if (!peers_to_disconnect.empty())
          {
            std::ostringstream disconnect_reason_stream;
            disconnect_reason_stream << "You need to upgrade your client due to hard fork at block " << block_number;
            disconnect_reason = disconnect_reason_stream.str();
            disconnect_exception = fc::exception(FC_LOG_MESSAGE(error, "You need to upgrade your client due to hard fork at block ${block_number}",
                                                                ("block_number", block_number)));
          }
        }
      }
      catch (const fc::canceled_exception&)
      {
        throw;
      }
      catch ( const fc::exception& e )
      {
        // client rejected the block.  Disconnect the client and any other clients that offered us this block
        wlog("Failed to push block ${num} (id:${id}), client rejected block sent by peer",
              ("num", block_message_to_process.block.block_num)
              ("id", block_message_to_process.block_id));

        disconnect_exception = e;
        disconnect_reason = "You offered me a block that I have deemed to be invalid";
        for (const PeerConnectionPtr& peer : _active_connections)
          if (!peer->ids_of_items_to_get.empty() &&
              peer->ids_of_items_to_get.front() == block_message_to_process.block_id)
            peers_to_disconnect.push_back(peer);
      }
      for (const PeerConnectionPtr& peer : peers_to_disconnect)
      {
        wlog("disconnecting client ${endpoint} because it offered us the rejected block", ("endpoint", peer->get_remote_endpoint()));
        disconnect_from_peer(peer.get(), disconnect_reason, true, *disconnect_exception);
      }
    }
    void NodeImpl::process_block_message(PeerConnection* originating_peer,
                                          const Message& message_to_process,
                                          const MessageHashType& message_hash)
    {
      VERIFY_CORRECT_THREAD();
      // find out whether we requested this item while we were synchronizing or during normal operation
      // (it's possible that we request an item during normal operation and then get kicked into sync
      // mode before we receive and process the item.  In that case, we should process the item as a normal
      // item to avoid confusing the sync code)
      goopal::client::BlockMessage block_message_to_process(message_to_process.as<goopal::client::BlockMessage>());
      auto item_iter = originating_peer->items_requested_from_peer.find(ItemId(goopal::client::block_message_type, message_hash));
      if (item_iter != originating_peer->items_requested_from_peer.end())
      {
        originating_peer->items_requested_from_peer.erase(item_iter);
        process_block_during_normal_operation(originating_peer, block_message_to_process, message_hash);
        if (originating_peer->idle())
          trigger_fetch_items_loop();
        return;
      }
      else
      {
        // not during normal operation.  see if we requested it during sync
        auto sync_item_iter = originating_peer->sync_items_requested_from_peer.find(ItemId(goopal::client::block_message_type,
                                                                                            block_message_to_process.block_id));
        if (sync_item_iter != originating_peer->sync_items_requested_from_peer.end())
        {
          originating_peer->sync_items_requested_from_peer.erase(sync_item_iter);
          _active_sync_requests.erase(block_message_to_process.block_id);
          process_block_during_sync(originating_peer, block_message_to_process, message_hash);
          if (originating_peer->idle())
          {
            // we have finished fetching a batch of items, so we either need to grab another batch of items
            // or we need to get another list of item ids.
            if (originating_peer->number_of_unfetched_item_ids > 0 &&
                originating_peer->ids_of_items_to_get.size() < GOP_NET_MIN_BLOCK_IDS_TO_PREFETCH)
              fetch_next_batch_of_item_ids_from_peer(originating_peer);
            else
              trigger_fetch_sync_items_loop();
          }
          return;
        }
      }

      // if we get here, we didn't request the message, we must have a misbehaving peer
      wlog("received a block ${block_id} I didn't ask for from peer ${endpoint}, disconnecting from peer",
           ("endpoint", originating_peer->get_remote_endpoint())
           ("block_id", block_message_to_process.block_id));
      fc::exception detailed_error(FC_LOG_MESSAGE(error, "You sent me a block that I didn't ask for, block_id: ${block_id}",
                                                  ("block_id", block_message_to_process.block_id)
                                                  ("goopal_git_revision_sha", originating_peer->goopal_git_revision_sha)
                                                  ("goopal_git_revision_unix_timestamp", originating_peer->goopal_git_revision_unix_timestamp)
                                                  ("fc_git_revision_sha", originating_peer->fc_git_revision_sha)
                                                  ("fc_git_revision_unix_timestamp", originating_peer->fc_git_revision_unix_timestamp)));
      disconnect_from_peer(originating_peer, "You sent me a block that I didn't ask for", true, detailed_error);
    }

    void NodeImpl::on_current_time_request_message(PeerConnection* originating_peer,
                                                    const CurrentTimeRequestMessage& current_time_request_message_received)
    {
      VERIFY_CORRECT_THREAD();
      fc::time_point request_received_time(fc::time_point::now());
      CurrentTimeReplyMessage reply(current_time_request_message_received.request_sent_time,
                                       request_received_time);
      originating_peer->send_message(reply, offsetof(CurrentTimeReplyMessage, reply_transmitted_time));
    }

    void NodeImpl::on_current_time_reply_message(PeerConnection* originating_peer,
                                                  const CurrentTimeReplyMessage& current_time_reply_message_received)
    {
      VERIFY_CORRECT_THREAD();
      fc::time_point reply_received_time = fc::time_point::now();
      originating_peer->clock_offset = fc::microseconds(((current_time_reply_message_received.request_received_time - current_time_reply_message_received.request_sent_time) +
                                                         (current_time_reply_message_received.reply_transmitted_time - reply_received_time)).count() / 2);
      originating_peer->round_trip_delay = (reply_received_time - current_time_reply_message_received.request_sent_time) -
                                           (current_time_reply_message_received.reply_transmitted_time - current_time_reply_message_received.request_received_time);
      // TODO
    }

    void NodeImpl::forward_firewall_check_to_next_available_peer(FirewallCheckStateData* firewall_check_state)
    {
      for (const PeerConnectionPtr& peer : _active_connections)
      {
        if (firewall_check_state->expected_node_id != peer->node_id && // it's not the node who is asking us to test
            !peer->firewall_check_state && // the peer isn't already performing a check for another node
            firewall_check_state->nodes_already_tested.find(peer->node_id) == firewall_check_state->nodes_already_tested.end() &&
            peer->core_protocol_version >= 106)
        {
          wlog("forwarding firewall check for node ${to_check} to peer ${checker}",
               ("to_check", firewall_check_state->endpoint_to_test)
               ("checker", peer->get_remote_endpoint()));
          firewall_check_state->nodes_already_tested.insert(peer->node_id);
          peer->firewall_check_state = firewall_check_state;
          CheckFirewallMessage check_request;
          check_request.endpoint_to_check = firewall_check_state->endpoint_to_test;
          check_request.node_id = firewall_check_state->expected_node_id;
          peer->send_message(check_request);
          return;
        }
      }
      wlog("Unable to forward firewall check for node ${to_check} to any other peers, returning 'unable'",
           ("to_check", firewall_check_state->endpoint_to_test));

      PeerConnectionPtr originating_peer = get_peer_by_node_id(firewall_check_state->expected_node_id);
      if (originating_peer)
      {
        CheckFirewallReplyMessage reply;
        reply.node_id = firewall_check_state->expected_node_id;
        reply.endpoint_checked = firewall_check_state->endpoint_to_test;
        reply.result = FirewallCheckResult::unable_to_check;
        originating_peer->send_message(reply);
      }
      delete firewall_check_state;
    }

    void NodeImpl::on_check_firewall_message(PeerConnection* originating_peer,
                                              const CheckFirewallMessage& check_firewall_message_received)
    {
      VERIFY_CORRECT_THREAD();

      if (check_firewall_message_received.node_id == NodeIdType() &&
          check_firewall_message_received.endpoint_to_check == fc::ip::endpoint())
      {
        // originating_peer is asking us to test whether it is firewalled
        // we're not going to try to connect back to the originating peer directly,
        // instead, we're going to coordinate requests by asking some of our peers
        // to try to connect to the originating peer, and relay the results back
        wlog("Peer ${peer} wants us to check whether it is firewalled", ("peer", originating_peer->get_remote_endpoint()));
        FirewallCheckStateData* firewall_check_state = new FirewallCheckStateData;
        // if they are using the same inbound and outbound port, try connecting to their outbound endpoint.
        // if they are using a different inbound port, use their outbound address but the inbound port they reported
        fc::ip::endpoint endpoint_to_check = originating_peer->get_socket().remote_endpoint();
        if (originating_peer->inbound_port != originating_peer->outbound_port)
          endpoint_to_check = fc::ip::endpoint(endpoint_to_check.get_address(), originating_peer->inbound_port);
        firewall_check_state->endpoint_to_test = endpoint_to_check;
        firewall_check_state->expected_node_id = originating_peer->node_id;
        firewall_check_state->requesting_peer = originating_peer->node_id;

        forward_firewall_check_to_next_available_peer(firewall_check_state);
      }
      else
      {
        // we're being asked to check another node
        // first, find out if we're currently connected to that node.  If we are, we
        // can't perform the test
        if (is_already_connected_to_id(check_firewall_message_received.node_id) ||
            is_connection_to_endpoint_in_progress(check_firewall_message_received.endpoint_to_check))
        {
          CheckFirewallReplyMessage reply;
          reply.node_id = check_firewall_message_received.node_id;
          reply.endpoint_checked = check_firewall_message_received.endpoint_to_check;
          reply.result = FirewallCheckResult::unable_to_check;
        }
        else
        {
          // we're not connected to them, so we need to set up a connection to them
          // to test.
            PeerConnectionPtr peer_for_testing(PeerConnection::make_shared(this));
          peer_for_testing->firewall_check_state = new FirewallCheckStateData;
          peer_for_testing->firewall_check_state->endpoint_to_test = check_firewall_message_received.endpoint_to_check;
          peer_for_testing->firewall_check_state->expected_node_id = check_firewall_message_received.node_id;
          peer_for_testing->firewall_check_state->requesting_peer = originating_peer->node_id;
          peer_for_testing->set_remote_endpoint(check_firewall_message_received.endpoint_to_check);
          initiate_connect_to(peer_for_testing);
        }
      }
    }

    void NodeImpl::on_check_firewall_reply_message(PeerConnection* originating_peer,
                                                    const CheckFirewallReplyMessage& check_firewall_reply_message_received)
    {
      VERIFY_CORRECT_THREAD();

      if (originating_peer->firewall_check_state &&
          originating_peer->firewall_check_state->requesting_peer != NodeIdType())
      {
        // then this is a peer that is helping us check the firewalled state of one of our other peers
        // and they're reporting back
        // if they got a result, return it to the original peer.  if they were unable to check,
        // we'll try another peer.
        wlog("Peer ${reporter} reports firewall check status ${status} for ${peer}",
             ("reporter", originating_peer->get_remote_endpoint())
             ("status", check_firewall_reply_message_received.result)
             ("peer", check_firewall_reply_message_received.endpoint_checked));

        if (check_firewall_reply_message_received.result == FirewallCheckResult::unable_to_connect ||
            check_firewall_reply_message_received.result == FirewallCheckResult::connection_successful)
        {
          PeerConnectionPtr original_peer = get_peer_by_node_id(originating_peer->firewall_check_state->requesting_peer);
          if (original_peer)
          {
            if (check_firewall_reply_message_received.result == FirewallCheckResult::connection_successful)
            {
              // if we previously thought this peer was firewalled, mark them as not firewalled
              if (original_peer->is_firewalled != FirewalledState::not_firewalled)
              {

                original_peer->is_firewalled = FirewalledState::not_firewalled;
                // there should be no old entry if we thought they were firewalled, so just create a new one
                fc::optional<fc::ip::endpoint> inbound_endpoint = originating_peer->get_endpoint_for_connecting();
                if (inbound_endpoint)
                {
                  PotentialPeerEntry updated_peer_entry = _potential_peer_db.lookup_or_create_entry_for_endpoint(*inbound_endpoint);
                  updated_peer_entry.last_seen_time = fc::time_point::now();
                  _potential_peer_db.update_entry(updated_peer_entry);
                }
              }
            }
            original_peer->send_message(check_firewall_reply_message_received);
          }
          delete originating_peer->firewall_check_state;
          originating_peer->firewall_check_state = nullptr;
        }
        else
        {
          // they were unable to check for us, ask another peer
          FirewallCheckStateData* firewall_check_state = originating_peer->firewall_check_state;
          originating_peer->firewall_check_state = nullptr;
          forward_firewall_check_to_next_available_peer(firewall_check_state);
        }
      }
      else if (originating_peer->firewall_check_state)
      {
        // this is a reply to a firewall check we initiated.
        wlog("Firewall check we initiated has returned with result: ${result}, endpoint = ${endpoint}",
             ("result", check_firewall_reply_message_received.result)
             ("endpoint", check_firewall_reply_message_received.endpoint_checked));
        if (check_firewall_reply_message_received.result == FirewallCheckResult::connection_successful)
        {
          _is_firewalled = FirewalledState::not_firewalled;
          _publicly_visible_listening_endpoint = check_firewall_reply_message_received.endpoint_checked;
        }
        else if (check_firewall_reply_message_received.result == FirewallCheckResult::unable_to_connect)
        {
          _is_firewalled = FirewalledState::firewalled;
          _publicly_visible_listening_endpoint = fc::optional<fc::ip::endpoint>();
        }
        delete originating_peer->firewall_check_state;
        originating_peer->firewall_check_state = nullptr;
      }
      else
      {
        wlog("Received a firewall check reply to a request I never sent");
      }

    }

    void NodeImpl::on_get_current_connections_request_message(PeerConnection* originating_peer,
                                                               const GetCurrentConnectionsRequestMessage& get_current_connections_request_message_received)
    {
      VERIFY_CORRECT_THREAD();
      GetCurrentConnectionsReplyMessage reply;

      if (!_average_network_read_speed_minutes.empty())
      {
        reply.upload_rate_one_minute = _average_network_write_speed_minutes.back();
        reply.download_rate_one_minute = _average_network_read_speed_minutes.back();

        size_t minutes_to_average = std::min(_average_network_write_speed_minutes.size(), (size_t)15);
        boost::circular_buffer<uint32_t>::iterator start_iter = _average_network_write_speed_minutes.end() - minutes_to_average;
        reply.upload_rate_fifteen_minutes = std::accumulate(start_iter, _average_network_write_speed_minutes.end(), 0) / (uint32_t)minutes_to_average;
        start_iter = _average_network_read_speed_minutes.end() - minutes_to_average;
        reply.download_rate_fifteen_minutes = std::accumulate(start_iter, _average_network_read_speed_minutes.end(), 0) / (uint32_t)minutes_to_average;

        minutes_to_average = std::min(_average_network_write_speed_minutes.size(), (size_t)60);
        start_iter = _average_network_write_speed_minutes.end() - minutes_to_average;
        reply.upload_rate_one_hour = std::accumulate(start_iter, _average_network_write_speed_minutes.end(), 0) / (uint32_t)minutes_to_average;
        start_iter = _average_network_read_speed_minutes.end() - minutes_to_average;
        reply.download_rate_one_hour = std::accumulate(start_iter, _average_network_read_speed_minutes.end(), 0) / (uint32_t)minutes_to_average;
      }

      fc::time_point now = fc::time_point::now();
      for (const PeerConnectionPtr& peer : _active_connections)
      {
        ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections

        CurrentConnectionData data_for_this_peer;
        data_for_this_peer.connection_duration = now.sec_since_epoch() - peer->connection_initiation_time.sec_since_epoch();
        if (peer->get_remote_endpoint()) // should always be set for anyone we're actively connected to
          data_for_this_peer.remote_endpoint = *peer->get_remote_endpoint();
        data_for_this_peer.clock_offset = peer->clock_offset;
        data_for_this_peer.round_trip_delay = peer->round_trip_delay;
        data_for_this_peer.node_id = peer->node_id;
        data_for_this_peer.connection_direction = peer->direction;
        data_for_this_peer.firewalled = peer->is_firewalled;
        fc::mutable_variant_object user_data;
        if (peer->goopal_git_revision_sha)
          user_data["goopal_git_revision_sha"] = *peer->goopal_git_revision_sha;
        if (peer->goopal_git_revision_unix_timestamp)
          user_data["goopal_git_revision_unix_timestamp"] = *peer->goopal_git_revision_unix_timestamp;
        if (peer->fc_git_revision_sha)
          user_data["fc_git_revision_sha"] = *peer->fc_git_revision_sha;
        if (peer->fc_git_revision_unix_timestamp)
          user_data["fc_git_revision_unix_timestamp"] = *peer->fc_git_revision_unix_timestamp;
        if (peer->platform)
          user_data["platform"] = *peer->platform;
        if (peer->bitness)
          user_data["bitness"] = *peer->bitness;
        user_data["user_agent"] = peer->user_agent;

        user_data["last_known_block_hash"] = peer->last_block_delegate_has_seen;
        user_data["last_known_block_number"] = peer->last_block_number_delegate_has_seen;
        user_data["last_known_block_time"] = peer->last_block_time_delegate_has_seen;

        data_for_this_peer.user_data = user_data;
        reply.current_connections.emplace_back(data_for_this_peer);
      }
      originating_peer->send_message(reply);
    }

    void NodeImpl::on_get_current_connections_reply_message(PeerConnection* originating_peer,
                                                             const GetCurrentConnectionsReplyMessage& get_current_connections_reply_message_received)
    {
      VERIFY_CORRECT_THREAD();
    }


    // this handles any message we get that doesn't require any special processing.
    // currently, this is any message other than block messages and p2p-specific
    // messages.  (transaction messages would be handled here, for example)
    // this just passes the message to the client, and does the bookkeeping
    // related to requesting and rebroadcasting the message.
    void NodeImpl::process_ordinary_message(PeerConnection* originating_peer,
                                              const Message& message_to_process, const MessageHashType& message_hash )
    {
      VERIFY_CORRECT_THREAD();
      fc::time_point message_receive_time = fc::time_point::now();

      // only process it if we asked for it
      auto iter = originating_peer->items_requested_from_peer.find( ItemId(message_to_process.msg_type, message_hash ) );
      if( iter == originating_peer->items_requested_from_peer.end() )
      {
        wlog( "received a message I didn't ask for from peer ${endpoint}, disconnecting from peer",
             ( "endpoint", originating_peer->get_remote_endpoint() ) );
        fc::exception detailed_error( FC_LOG_MESSAGE(error, "You sent me a message that I didn't ask for, message_hash: ${message_hash}",
                                                    ( "message_hash", message_hash ) ) );
        disconnect_from_peer( originating_peer, "You sent me a message that I didn't request", true, detailed_error );
        return;
      }
      else
      {
        originating_peer->items_requested_from_peer.erase( iter );
        if (originating_peer->idle())
          trigger_fetch_items_loop();

        // Next: have the delegate process the message
        fc::time_point message_validated_time;
        try
        {
          _delegate->handle_message(message_to_process, false);
          message_validated_time = fc::time_point::now();
        }
        catch ( const insufficient_relay_fee& )
        {
          // flooding control.  The message was valid but we can't handle it now.
          assert(message_to_process.msg_type == goopal::client::trx_message_type); // we only support throttling transactions.
          if (message_to_process.msg_type == goopal::client::trx_message_type)
            originating_peer->transaction_fetching_inhibited_until = fc::time_point::now() + fc::seconds(GOP_NET_INSUFFICIENT_RELAY_FEE_PENALTY_SEC);
          return;
        }
        catch ( const fc::canceled_exception& )
        {
          throw;
        }
        catch ( const fc::exception& e )
        {
          wlog( "client rejected message sent by peer ${peer}, ${e}", ("peer", originating_peer->get_remote_endpoint() )("e", e.to_string() ) );
          return;
        }

        // finally, if the delegate validated the message, broadcast it to our other peers
        MessagePropagationData propagation_data{message_receive_time, message_validated_time, originating_peer->node_id};
        broadcast( message_to_process, propagation_data );
      }
    }

    void NodeImpl::start_synchronizing_with_peer( const PeerConnectionPtr& peer )
    {
      VERIFY_CORRECT_THREAD();
      peer->ids_of_items_to_get.clear();
      peer->number_of_unfetched_item_ids = 0;
      peer->we_need_sync_items_from_peer = true;
      peer->last_block_delegate_has_seen = ItemHashType();
      peer->last_block_number_delegate_has_seen = 0;
      peer->last_block_time_delegate_has_seen = _delegate->get_block_time(ItemHashType());
      peer->inhibit_fetching_sync_blocks = false;
      fetch_next_batch_of_item_ids_from_peer( peer.get() );
    }

    void NodeImpl::start_synchronizing()
    {
      for( const PeerConnectionPtr& peer : _active_connections )
        start_synchronizing_with_peer( peer );
    }

    void NodeImpl::new_peer_just_added( const PeerConnectionPtr& peer )
    {
      VERIFY_CORRECT_THREAD();
      peer->send_message(CurrentTimeRequestMessage(),
                         offsetof(CurrentTimeRequestMessage, request_sent_time));
      start_synchronizing_with_peer( peer );
      if( _active_connections.size() != _last_reported_number_of_connections )
      {
        _last_reported_number_of_connections = (uint32_t)_active_connections.size();
        _delegate->connection_count_changed( _last_reported_number_of_connections );
      }
    }

    void NodeImpl::close()
    {
      VERIFY_CORRECT_THREAD();

      // First, stop accepting incoming network connections
      try
      {
        _tcp_server.close();
        dlog("P2P TCP server closed");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while closing P2P TCP server, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while closing P2P TCP server, ignoring" );
      }

      try
      {
        _accept_loop_complete.cancel_and_wait("node_impl::close()");
        dlog("P2P accept loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating P2P accept loop, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating P2P accept loop, ignoring" );
      }

      // terminate all of our long-running loops (these run continuously instead of rescheduling themselves)
      try
      {
        _p2p_network_connect_loop_done.cancel("node_impl::close()");
        // cancel() is currently broken, so we need to wake up the task to allow it to finish
        trigger_p2p_network_connect_loop();
        _p2p_network_connect_loop_done.wait();
        dlog("P2P connect loop terminated");
      }
      catch ( const fc::canceled_exception& )
      {
        dlog("P2P connect loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating P2P connect loop, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating P2P connect loop, ignoring" );
      }

      try
      {
        _process_backlog_of_sync_blocks_done.cancel_and_wait("node_impl::close()");
        dlog("Process backlog of sync items task terminated");
      }
      catch ( const fc::canceled_exception& )
      {
        dlog("Process backlog of sync items task terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Process backlog of sync items task, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Process backlog of sync items task, ignoring" );
      }

      unsigned handle_message_call_count = 0;
      for (fc::future<void>& handle_message_call : _handle_message_calls_in_progress)
      {
        ++handle_message_call_count;
        try
        {
          handle_message_call.cancel_and_wait("node_impl::close()");
          dlog("handle_message call #${count} task terminated", ("count", handle_message_call_count));
        }
        catch ( const fc::canceled_exception& )
        {
          dlog("handle_message call #${count} task terminated", ("count", handle_message_call_count));
        }
        catch ( const fc::exception& e )
        {
          wlog("Exception thrown while terminating handle_message call #${count} task, ignoring: ${e}", ("e",e)("count", handle_message_call_count));
        }
        catch (...)
        {
          wlog("Exception thrown while terminating handle_message call #${count} task, ignoring",("count", handle_message_call_count));
        }
      }
      _handle_message_calls_in_progress.clear();

      try
      {
        _fetch_sync_items_loop_done.cancel("node_impl::close()");
        // cancel() is currently broken, so we need to wake up the task to allow it to finish
        trigger_fetch_sync_items_loop();
        _fetch_sync_items_loop_done.wait();
        dlog("Fetch sync items loop terminated");
      }
      catch ( const fc::canceled_exception& )
      {
        dlog("Fetch sync items loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Fetch sync items loop, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Fetch sync items loop, ignoring" );
      }

      try
      {
        _fetch_item_loop_done.cancel("node_impl::close()");
        // cancel() is currently broken, so we need to wake up the task to allow it to finish
        trigger_fetch_items_loop();
        _fetch_item_loop_done.wait();
        dlog("Fetch items loop terminated");
      }
      catch ( const fc::canceled_exception& )
      {
        dlog("Fetch items loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Fetch items loop, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Fetch items loop, ignoring" );
      }

      try
      {
        _advertise_inventory_loop_done.cancel("node_impl::close()");
        // cancel() is currently broken, so we need to wake up the task to allow it to finish
        trigger_advertise_inventory_loop();
        _advertise_inventory_loop_done.wait();
        dlog("Advertise inventory loop terminated");
      }
      catch ( const fc::canceled_exception& )
      {
        dlog("Advertise inventory loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Advertise inventory loop, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Advertise inventory loop, ignoring" );
      }


      // Next, terminate our existing connections.  First, close all of the connections nicely.
      // This will close the sockets and may result in calls to our "on_connection_closing"
      // method to inform us that the connection really closed (or may not if we manage to cancel
      // the read loop before it gets an EOF).
      // operate off copies of the lists in case they change during iteration
      std::list<PeerConnectionPtr> all_peers;
      boost::push_back(all_peers, _active_connections);
      boost::push_back(all_peers, _handshaking_connections);
      boost::push_back(all_peers, _closing_connections);

      for (const PeerConnectionPtr& peer : all_peers)
      {
        try
        {
          peer->destroy_connection();
        }
        catch ( const fc::exception& e )
        {
          wlog( "Exception thrown while closing peer connection, ignoring: ${e}", ("e",e) );
        }
        catch (...)
        {
          wlog( "Exception thrown while closing peer connection, ignoring" );
        }
      }

      // and delete all of the peer_connection objects
      _active_connections.clear();
      _handshaking_connections.clear();
      _closing_connections.clear();
      all_peers.clear();

      {
#ifdef USE_PEERS_TO_DELETE_MUTEX
        fc::scoped_lock<fc::mutex> lock(_peers_to_delete_mutex);
#endif
        try
        {
          _delayed_peer_deletion_task_done.cancel_and_wait("node_impl::close()");
          dlog("Delayed peer deletion task terminated");
        }
        catch ( const fc::exception& e )
        {
          wlog( "Exception thrown while terminating Delayed peer deletion task, ignoring: ${e}", ("e",e) );
        }
        catch (...)
        {
          wlog( "Exception thrown while terminating Delayed peer deletion task, ignoring" );
        }
        _peers_to_delete.clear();
      }

      // Now that there are no more peers that can call methods on us, there should be no
      // chance for one of our loops to be rescheduled, so we can safely terminate all of
      // our loops now
      try
      {
        _terminate_inactive_connections_loop_done.cancel_and_wait("node_impl::close()");
        dlog("Terminate inactive connections loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Terminate inactive connections loop, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Terminate inactive connections loop, ignoring" );
      }

      try
      {
        _fetch_updated_peer_lists_loop_done.cancel_and_wait("node_impl::close()");
        dlog("Fetch updated peer lists loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Fetch updated peer lists loop, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Fetch updated peer lists loop, ignoring" );
      }

      try
      {
        _bandwidth_monitor_loop_done.cancel_and_wait("node_impl::close()");
        dlog("Bandwidth monitor loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Bandwidth monitor loop, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Bandwidth monitor loop, ignoring" );
      }

      try
      {
        _dump_node_status_task_done.cancel_and_wait("node_impl::close()");
        dlog("Dump node status task terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Dump node status task, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Dump node status task, ignoring" );
      }
    } // node_impl::close()

    void NodeImpl::accept_connection_task( PeerConnectionPtr new_peer )
    {
      VERIFY_CORRECT_THREAD();
      new_peer->accept_connection(); // this blocks until the secure connection is fully negotiated
      send_hello_message(new_peer);
    }

    void NodeImpl::accept_loop()
    {
      VERIFY_CORRECT_THREAD();
      while ( !_accept_loop_complete.canceled() )
      {
          PeerConnectionPtr new_peer(PeerConnection::make_shared(this));

        try
        {
          _tcp_server.accept( new_peer->get_socket() );
          ilog( "accepted inbound connection from ${remote_endpoint}", ("remote_endpoint", new_peer->get_socket().remote_endpoint() ) );
          if (_node_is_shutting_down)
            return;
          new_peer->connection_initiation_time = fc::time_point::now();
          _handshaking_connections.insert( new_peer );
          _rate_limiter.add_tcp_socket( &new_peer->get_socket() );
          std::weak_ptr<PeerConnection> new_weak_peer(new_peer);
          new_peer->accept_or_connect_task_done = fc::async( [this, new_weak_peer]() {
            PeerConnectionPtr new_peer(new_weak_peer.lock());
            assert(new_peer);
            if (!new_peer)
              return;
            accept_connection_task(new_peer);
          }, "accept_connection_task" );

          // limit the rate at which we accept connections to mitigate DOS attacks
          fc::usleep( fc::milliseconds(10) );
        } FC_CAPTURE_AND_RETHROW()
      }
    } // accept_loop()

    void NodeImpl::send_hello_message(const PeerConnectionPtr& peer)
    {
      VERIFY_CORRECT_THREAD();
      peer->negotiation_status = PeerConnection::ConnectionNegotiationStatus::hello_sent;

      fc::sha256::encoder shared_secret_encoder;
      fc::sha512 shared_secret = peer->get_shared_secret();
      shared_secret_encoder.write(shared_secret.data(), sizeof(shared_secret));
      fc::ecc::compact_signature signature = _node_configuration.private_key.sign_compact(shared_secret_encoder.result());

      // in the hello messsage, we send three things:
      //  ip address
      //  outbound port
      //  inbound port
      // The peer we're connecting to will assume we're firewalled if the
      // ip address and outbound port we send don't match the values it sees on its remote endpoint
      //
      // if we know that we're behind a NAT that will allow incoming connections because our firewall
      // detection figured it out, send those values instead.

      fc::ip::endpoint local_endpoint(peer->get_socket().local_endpoint());
      uint16_t listening_port = _node_configuration.accept_incoming_connections ? _actual_listening_endpoint.port() : 0;

      if (_is_firewalled == FirewalledState::not_firewalled &&
          _publicly_visible_listening_endpoint)
      {
        local_endpoint = *_publicly_visible_listening_endpoint;
        listening_port = _publicly_visible_listening_endpoint->port();
      }

      HelloMessage hello(_user_agent_string,
                          core_protocol_version,
                          local_endpoint.get_address(),
                          listening_port,
                          local_endpoint.port(),
                          _node_public_key,
                          signature,
                          _chain_id,
                          generate_hello_user_data());

      peer->send_message(Message(hello));
    }

    void NodeImpl::connect_to_task(PeerConnectionPtr new_peer,
                                    const fc::ip::endpoint& remote_endpoint)
    {
      VERIFY_CORRECT_THREAD();

      if (!new_peer->performing_firewall_check())
      {
        // create or find the database entry for the new peer
        // if we're connecting to them, we believe they're not firewalled
        PotentialPeerEntry updated_peer_entry = _potential_peer_db.lookup_or_create_entry_for_endpoint(remote_endpoint);
        updated_peer_entry.last_connection_disposition = last_connection_failed;
        updated_peer_entry.last_connection_attempt_time = fc::time_point::now();;
        _potential_peer_db.update_entry(updated_peer_entry);
      }
      else
      {
        wlog("connecting to peer ${peer} for firewall check", ("peer", new_peer->get_remote_endpoint()));
      }

      fc::oexception connect_failed_exception;

      try
      {
        new_peer->connect_to(remote_endpoint, _actual_listening_endpoint);  // blocks until the connection is established and secure connection is negotiated

        // we connected to the peer.  guess they're not firewalled....
        new_peer->is_firewalled = FirewalledState::not_firewalled;

        // connection succeeded, we've started handshaking.  entry that in our database
        PotentialPeerEntry updated_peer_entry = _potential_peer_db.lookup_or_create_entry_for_endpoint(remote_endpoint);
        updated_peer_entry.last_connection_disposition = last_connection_handshaking_failed;
        updated_peer_entry.number_of_successful_connection_attempts++;
        updated_peer_entry.last_seen_time = fc::time_point::now();
        _potential_peer_db.update_entry(updated_peer_entry);
      }
      catch (const fc::exception& except)
      {
        connect_failed_exception = except;
      }

      if (connect_failed_exception && !new_peer->performing_firewall_check())
      {
        // connection failed.  entry that in our database
        PotentialPeerEntry updated_peer_entry = _potential_peer_db.lookup_or_create_entry_for_endpoint(remote_endpoint);
        updated_peer_entry.last_connection_disposition = last_connection_failed;
        updated_peer_entry.number_of_failed_connection_attempts++;
        if (new_peer->connection_closed_error)
          updated_peer_entry.last_error = *new_peer->connection_closed_error;
        else
          updated_peer_entry.last_error = *connect_failed_exception;
        _potential_peer_db.update_entry(updated_peer_entry);
      }

      if (new_peer->performing_firewall_check())
      {
        // we were connecting to test whether the node is firewalled, and we now know the result.
        // send a message back to the requester
        PeerConnectionPtr requesting_peer = get_peer_by_node_id(new_peer->firewall_check_state->requesting_peer);
        if (requesting_peer)
        {
          CheckFirewallReplyMessage reply;
          reply.endpoint_checked = new_peer->firewall_check_state->endpoint_to_test;
          reply.node_id = new_peer->firewall_check_state->expected_node_id;
          reply.result = connect_failed_exception ?
                           FirewallCheckResult::unable_to_connect : 
                           FirewallCheckResult::connection_successful;
          wlog("firewall check of ${peer_checked} ${success_or_failure}, sending reply to ${requester}",
                ("peer_checked", new_peer->get_remote_endpoint())
                ("success_or_failure", connect_failed_exception ? "failed" : "succeeded" )
                ("requester", requesting_peer->get_remote_endpoint()));

          requesting_peer->send_message(reply);
        }
      }

      if (connect_failed_exception || new_peer->performing_firewall_check())
      {
        // if the connection failed or if this connection was just intended to check
        // whether the peer is firewalled, we want to disconnect now.
        _handshaking_connections.erase(new_peer);
        _terminating_connections.erase(new_peer);
        assert(_active_connections.find(new_peer) == _active_connections.end());
        _active_connections.erase(new_peer);
        assert(_closing_connections.find(new_peer) == _closing_connections.end());
        _closing_connections.erase(new_peer);

        display_current_connections();
        trigger_p2p_network_connect_loop();
        schedule_peer_for_deletion(new_peer);

        if (connect_failed_exception)
          throw *connect_failed_exception;
      }
      else
      {
        // connection was successful and we want to stay connected
        fc::ip::endpoint local_endpoint = new_peer->get_local_endpoint();
        new_peer->inbound_address = local_endpoint.get_address();
        new_peer->inbound_port = _node_configuration.accept_incoming_connections ? _actual_listening_endpoint.port() : 0;
        new_peer->outbound_port = local_endpoint.port();

        new_peer->our_state = PeerConnection::OurConnectionState::just_connected;
        new_peer->their_state = PeerConnection::TheirConnectionState::just_connected;
        send_hello_message(new_peer);
        dlog("Sent \"hello\" to peer ${peer}", ("peer", new_peer->get_remote_endpoint()));
      }
    }

    // methods implementing node's public interface
    void NodeImpl::set_node_delegate(NodeDelegate* del, fc::thread* thread_for_delegate_calls)
    {
      VERIFY_CORRECT_THREAD();
      _delegate.reset();
      if (del)
        _delegate.reset(new StatisticsGatheringNodeDelegateWrapper(del, thread_for_delegate_calls));
      if( _delegate )
        _chain_id = del->get_chain_id();
    }

    void NodeImpl::load_configuration( const fc::path& configuration_directory )
    {
      VERIFY_CORRECT_THREAD();
      _node_configuration_directory = configuration_directory;
      fc::path configuration_file_name( _node_configuration_directory / NODE_CONFIGURATION_FILENAME );
      bool node_configuration_loaded = false;
      if( fc::exists(configuration_file_name ) )
      {
        try
        {
          _node_configuration = fc::json::from_file( configuration_file_name ).as<detail::NodeConfiguration>();
          ilog( "Loaded configuration from file ${filename}", ("filename", configuration_file_name ) );

          if( _node_configuration.private_key == fc::ecc::private_key() )
            _node_configuration.private_key = fc::ecc::private_key::generate();

          node_configuration_loaded = true;
        }
        catch ( fc::parse_error_exception& parse_error )
        {
          elog( "malformed node configuration file ${filename}: ${error}",
               ( "filename", configuration_file_name )("error", parse_error.to_detail_string() ) );
        }
        catch ( fc::exception& except )
        {
          elog( "unexpected exception while reading configuration file ${filename}: ${error}",
               ( "filename", configuration_file_name )("error", except.to_detail_string() ) );
        }
      }

      if( !node_configuration_loaded )
      {
        _node_configuration = detail::NodeConfiguration();

#ifdef GOP_TEST_NETWORK
        uint32_t port = GOP_NET_TEST_P2P_PORT + GOP_TEST_NETWORK_VERSION;
#else
        uint32_t port = GOP_NET_DEFAULT_P2P_PORT;
#endif
        _node_configuration.listen_endpoint.set_port( port );
        _node_configuration.accept_incoming_connections = true;
        _node_configuration.wait_if_endpoint_is_busy = false;

        ilog( "generating new private key for this node" );
        _node_configuration.private_key = fc::ecc::private_key::generate();
      }

      _node_public_key = _node_configuration.private_key.get_public_key().serialize();

      fc::path potential_peer_database_file_name(_node_configuration_directory / POTENTIAL_PEER_DATABASE_FILENAME);
      try
      {
        _potential_peer_db.open(potential_peer_database_file_name);

        // push back the time on all peers loaded from the database so we will be able to retry them immediately
        for (PeerDatabase::iterator itr = _potential_peer_db.begin(); itr != _potential_peer_db.end(); ++itr)
        {
          PotentialPeerEntry updated_peer_entry = *itr;
          updated_peer_entry.last_connection_attempt_time = std::min<fc::time_point_sec>(updated_peer_entry.last_connection_attempt_time,
                                                                                          fc::time_point::now() - fc::seconds(_peer_connection_retry_timeout));
          _potential_peer_db.update_entry(updated_peer_entry);
        }

        trigger_p2p_network_connect_loop();
      }
      catch (fc::exception& except)
      {
        elog("unable to open peer database ${filename}: ${error}",
             ("filename", potential_peer_database_file_name)("error", except.to_detail_string()));
        throw;
      }
    }

    void NodeImpl::listen_to_p2p_network()
    {
      VERIFY_CORRECT_THREAD();
      if (!_node_configuration.accept_incoming_connections)
      {
        wlog("accept_incoming_connections is false, p2p network will not accept any incoming connections");
        return;
      }

      assert(_node_public_key != fc::ecc::public_key_data());

      fc::ip::endpoint listen_endpoint = _node_configuration.listen_endpoint;
      if( listen_endpoint.port() != 0 )
      {
        // if the user specified a port, we only want to bind to it if it's not already
        // being used by another application.  During normal operation, we set the
        // SO_REUSEADDR/SO_REUSEPORT flags so that we can bind outbound sockets to the
        // same local endpoint as we're listening on here.  On some platforms, setting
        // those flags will prevent us from detecting that other applications are
        // listening on that port.  We'd like to detect that, so we'll set up a temporary
        // tcp server without that flag to see if we can listen on that port.
        bool first = true;
        for( ;; )
        {
          bool listen_failed = false;

          try
          {
            fc::tcp_server temporary_server;
            if( listen_endpoint.get_address() != fc::ip::address() )
              temporary_server.listen( listen_endpoint );
            else
              temporary_server.listen( listen_endpoint.port() );
            break;
          }
          catch ( const fc::exception&)
          {
            listen_failed = true;
          }

          if (listen_failed)
          {
            if( _node_configuration.wait_if_endpoint_is_busy )
            {
              std::ostringstream error_message_stream;
              if( first )
              {
                error_message_stream << "Unable to listen for connections on port " << listen_endpoint.port()
                                     << ", retrying in a few seconds\n";
                error_message_stream << "You can wait for it to become available, or restart this program using\n";
                error_message_stream << "the --p2p-port option to specify another port\n";
                first = false;
              }
              else
              {
                error_message_stream << "\nStill waiting for port " << listen_endpoint.port() << " to become available\n";
              }
              std::string error_message = error_message_stream.str();
              ulog(error_message);
              _delegate->error_encountered( error_message, fc::oexception() );
              fc::usleep( fc::seconds(5 ) );
            }
            else // don't wait, just find a random port
            {
              wlog( "unable to bind on the requested endpoint ${endpoint}, which probably means that endpoint is already in use",
                   ( "endpoint", listen_endpoint ) );
              listen_endpoint.set_port( 0 );
            }
          } // if (listen_failed)
        } // for(;;)
      } // if (listen_endpoint.port() != 0)
      else // port is 0
      {
        // if they requested a random port, we'll just assume it's available
        // (it may not be due to ip address, but we'll detect that in the next step)
      }

      _tcp_server.set_reuse_address();
      try
      {
        if( listen_endpoint.get_address() != fc::ip::address() )
          _tcp_server.listen( listen_endpoint );
        else
          _tcp_server.listen( listen_endpoint.port() );
        _actual_listening_endpoint = _tcp_server.get_local_endpoint();
        ilog( "listening for connections on endpoint ${endpoint} (our first choice)",
              ( "endpoint", _actual_listening_endpoint ) );
      }
      catch ( fc::exception& e )
      {
        FC_RETHROW_EXCEPTION( e, error, "unable to listen on ${endpoint}", ("endpoint",listen_endpoint ) );
      }
    }

    void NodeImpl::connect_to_p2p_network()
    {
      VERIFY_CORRECT_THREAD();
      assert(_node_public_key != fc::ecc::public_key_data());

      assert(!_accept_loop_complete.valid() &&
             !_p2p_network_connect_loop_done.valid() &&
             !_fetch_sync_items_loop_done.valid() &&
             !_fetch_item_loop_done.valid() &&
             !_advertise_inventory_loop_done.valid() &&
             !_terminate_inactive_connections_loop_done.valid() &&
             !_fetch_updated_peer_lists_loop_done.valid() &&
             !_bandwidth_monitor_loop_done.valid() &&
             !_dump_node_status_task_done.valid());
      if (_node_configuration.accept_incoming_connections)
        _accept_loop_complete = fc::async( [=](){ accept_loop(); }, "accept_loop");
      _p2p_network_connect_loop_done = fc::async( [=]() { p2p_network_connect_loop(); }, "p2p_network_connect_loop" );
      _fetch_sync_items_loop_done = fc::async( [=]() { fetch_sync_items_loop(); }, "fetch_sync_items_loop" );
      _fetch_item_loop_done = fc::async( [=]() { fetch_items_loop(); }, "fetch_items_loop" );
      _advertise_inventory_loop_done = fc::async( [=]() { advertise_inventory_loop(); }, "advertise_inventory_loop" );
      _terminate_inactive_connections_loop_done = fc::async( [=]() { terminate_inactive_connections_loop(); }, "terminate_inactive_connections_loop" );
      _fetch_updated_peer_lists_loop_done = fc::async([=](){ fetch_updated_peer_lists_loop(); }, "fetch_updated_peer_lists_loop");
      _bandwidth_monitor_loop_done = fc::async([=](){ bandwidth_monitor_loop(); }, "bandwidth_monitor_loop");
      _dump_node_status_task_done = fc::async([=](){ dump_node_status_task(); }, "dump_node_status_task");
    }

    void NodeImpl::add_node(const fc::ip::endpoint& ep)
    {
      VERIFY_CORRECT_THREAD();
      // if we're connecting to them, we believe they're not firewalled
      PotentialPeerEntry updated_peer_entry = _potential_peer_db.lookup_or_create_entry_for_endpoint(ep);

      // if we've recently connected to this peer, reset the last_connection_attempt_time to allow
      // us to immediately retry this peer
      updated_peer_entry.last_connection_attempt_time = std::min<fc::time_point_sec>(updated_peer_entry.last_connection_attempt_time,
                                                                                      fc::time_point::now() - fc::seconds(_peer_connection_retry_timeout));
      _add_once_node_list.push_back(updated_peer_entry);
      _potential_peer_db.update_entry(updated_peer_entry);
      trigger_p2p_network_connect_loop();
    }

    void NodeImpl::initiate_connect_to(const PeerConnectionPtr& new_peer)
    {
      new_peer->get_socket().open();
      new_peer->get_socket().set_reuse_address();
      new_peer->connection_initiation_time = fc::time_point::now();
      _handshaking_connections.insert(new_peer);
      _rate_limiter.add_tcp_socket(&new_peer->get_socket());

      if (_node_is_shutting_down)
        return;

      std::weak_ptr<PeerConnection> new_weak_peer(new_peer);
      new_peer->accept_or_connect_task_done = fc::async([this, new_weak_peer](){
        PeerConnectionPtr new_peer(new_weak_peer.lock());
        assert(new_peer);
        if (!new_peer)
          return;
        connect_to_task(new_peer, *new_peer->get_remote_endpoint());
      }, "connect_to_task");
    }

    void NodeImpl::connect_to_endpoint(const fc::ip::endpoint& remote_endpoint)
    {
      VERIFY_CORRECT_THREAD();
      if (is_connection_to_endpoint_in_progress(remote_endpoint))
        FC_THROW_EXCEPTION(already_connected_to_requested_peer, "already connected to requested endpoint ${endpoint}",
                           ("endpoint", remote_endpoint));

      dlog("node_impl::connect_to_endpoint(${endpoint})", ("endpoint", remote_endpoint));
      PeerConnectionPtr new_peer(PeerConnection::make_shared(this));
      new_peer->set_remote_endpoint(remote_endpoint);
      initiate_connect_to(new_peer);
    }

    PeerConnectionPtr NodeImpl::get_connection_to_endpoint( const fc::ip::endpoint& remote_endpoint )
    {
      VERIFY_CORRECT_THREAD();
      for( const PeerConnectionPtr& active_peer : _active_connections )
      {
        fc::optional<fc::ip::endpoint> endpoint_for_this_peer( active_peer->get_remote_endpoint() );
        if( endpoint_for_this_peer && *endpoint_for_this_peer == remote_endpoint )
          return active_peer;
      }
      for( const PeerConnectionPtr& handshaking_peer : _handshaking_connections )
      {
        fc::optional<fc::ip::endpoint> endpoint_for_this_peer( handshaking_peer->get_remote_endpoint() );
        if( endpoint_for_this_peer && *endpoint_for_this_peer == remote_endpoint )
          return handshaking_peer;
      }
      return PeerConnectionPtr();
    }

    bool NodeImpl::is_connection_to_endpoint_in_progress( const fc::ip::endpoint& remote_endpoint )
    {
      VERIFY_CORRECT_THREAD();
      return get_connection_to_endpoint( remote_endpoint ) != PeerConnectionPtr();
    }

    void NodeImpl::move_peer_to_active_list(const PeerConnectionPtr& peer)
    {
      VERIFY_CORRECT_THREAD();
      _active_connections.insert(peer);
      _handshaking_connections.erase(peer);
      _closing_connections.erase(peer);
      _terminating_connections.erase(peer);
    }

    void NodeImpl::move_peer_to_closing_list(const PeerConnectionPtr& peer)
    {
      VERIFY_CORRECT_THREAD();
      _active_connections.erase(peer);
      _handshaking_connections.erase(peer);
      _closing_connections.insert(peer);
      _terminating_connections.erase(peer);
    }

    void NodeImpl::move_peer_to_terminating_list(const PeerConnectionPtr& peer)
    {
      VERIFY_CORRECT_THREAD();
      _active_connections.erase(peer);
      _handshaking_connections.erase(peer);
      _closing_connections.erase(peer);
      _terminating_connections.insert(peer);
    }

    void NodeImpl::dump_node_status()
    {
      VERIFY_CORRECT_THREAD();
      ilog( "----------------- PEER STATUS UPDATE --------------------" );
      ilog( " number of peers: ${active} active, ${handshaking}, ${closing} closing.  attempting to maintain ${desired} - ${maximum} peers",
           ( "active", _active_connections.size() )("handshaking", _handshaking_connections.size() )("closing",_closing_connections.size() )
           ( "desired", _desired_number_of_connections )("maximum", _maximum_number_of_connections ) );
      for( const PeerConnectionPtr& peer : _active_connections )
      {
        ilog( "       active peer ${endpoint} peer_is_in_sync_with_us:${in_sync_with_us} we_are_in_sync_with_peer:${in_sync_with_them}",
             ( "endpoint", peer->get_remote_endpoint() )
             ( "in_sync_with_us", !peer->peer_needs_sync_items_from_us )("in_sync_with_them", !peer->we_need_sync_items_from_peer ) );
        if( peer->we_need_sync_items_from_peer )
          ilog( "              above peer has ${count} sync items we might need", ("count", peer->ids_of_items_to_get.size() ) );
        if (peer->inhibit_fetching_sync_blocks)
          ilog( "              we are not fetching sync blocks from the above peer (inhibit_fetching_sync_blocks == true)" );

      }
      for( const PeerConnectionPtr& peer : _handshaking_connections )
      {
        ilog( "  handshaking peer ${endpoint} in state ours(${our_state}) theirs(${their_state})",
             ( "endpoint", peer->get_remote_endpoint() )("our_state", peer->our_state )("their_state", peer->their_state ) );
      }

      ilog( "--------- MEMORY USAGE ------------" );
      ilog( "node._active_sync_requests size: ${size}", ("size", _active_sync_requests.size() ) ); // TODO: un-break this
      ilog( "node._received_sync_items size: ${size}", ("size", _received_sync_items.size() ) );
      ilog( "node._new_received_sync_items size: ${size}", ("size", _new_received_sync_items.size() ) );
      ilog( "node._items_to_fetch size: ${size}", ("size", _items_to_fetch.size() ) );
      ilog( "node._new_inventory size: ${size}", ("size", _new_inventory.size() ) );
      ilog( "node._message_cache size: ${size}", ("size", _message_cache.size() ) );
      for( const PeerConnectionPtr& peer : _active_connections )
      {
        ilog( "  peer ${endpoint}", ("endpoint", peer->get_remote_endpoint() ) );
        ilog( "    peer.ids_of_items_to_get size: ${size}", ("size", peer->ids_of_items_to_get.size() ) );
        ilog( "    peer.inventory_peer_advertised_to_us size: ${size}", ("size", peer->inventory_peer_advertised_to_us.size() ) );
        ilog( "    peer.inventory_advertised_to_peer size: ${size}", ("size", peer->inventory_advertised_to_peer.size() ) );
        ilog( "    peer.items_requested_from_peer size: ${size}", ("size", peer->items_requested_from_peer.size() ) );
        ilog( "    peer.sync_items_requested_from_peer size: ${size}", ("size", peer->sync_items_requested_from_peer.size() ) );
      }
      ilog( "--------- END MEMORY USAGE ------------" );
    }

    void NodeImpl::disconnect_from_peer(PeerConnection* peer_to_disconnect,
                                          const std::string& reason_for_disconnect,
                                          bool caused_by_error /* = false */,
                                          const fc::oexception& error /* = fc::oexception() */ )
    {
      VERIFY_CORRECT_THREAD();
      move_peer_to_closing_list(peer_to_disconnect->shared_from_this());

      if (peer_to_disconnect->they_have_requested_close)
      {
        // the peer has already told us that it's ready to close the connection, so just close the connection
        peer_to_disconnect->close_connection();
      }
      else
      {
        // we're the first to try to want to close the connection
        fc::optional<fc::ip::endpoint> inbound_endpoint = peer_to_disconnect->get_endpoint_for_connecting();
        if (inbound_endpoint)
        {
          fc::optional<PotentialPeerEntry> updated_peer_entry = _potential_peer_db.lookup_entry_for_endpoint(*inbound_endpoint);
          if (updated_peer_entry)
          {
            updated_peer_entry->last_seen_time = fc::time_point::now();
            if (error)
              updated_peer_entry->last_error = error;
            else
              updated_peer_entry->last_error = fc::exception(FC_LOG_MESSAGE(info, reason_for_disconnect.c_str()));
            _potential_peer_db.update_entry(*updated_peer_entry);
          }
        }
        peer_to_disconnect->we_have_requested_close = true;
        peer_to_disconnect->connection_closed_time = fc::time_point::now();

        ClosingConnectionMessage closing_message( reason_for_disconnect, caused_by_error, error );
        peer_to_disconnect->send_message( closing_message );
      }

      // notify the user.  This will be useful in testing, but we might want to remove it later;
      // it makes good sense to notify the user if other nodes think she is behaving badly, but
      // if we're just detecting and dissconnecting other badly-behaving nodes, they don't really care.
      if (caused_by_error)
      {
        std::ostringstream error_message;
        error_message << "I am disconnecting peer " << fc::variant( peer_to_disconnect->get_remote_endpoint() ).as_string() <<
                         " for reason: " << reason_for_disconnect;
        _delegate->error_encountered(error_message.str(), fc::oexception());
        dlog(error_message.str());
      }
      else
        dlog("Disconnecting from ${peer} for ${reason}", ("peer",peer_to_disconnect->get_remote_endpoint()) ("reason",reason_for_disconnect));
      // peer_to_disconnect->close_connection();
    }

    void NodeImpl::listen_on_endpoint( const fc::ip::endpoint& ep )
    {
      VERIFY_CORRECT_THREAD();
      _node_configuration.listen_endpoint = ep;
      save_node_configuration();
    }

    void NodeImpl::accept_incoming_connections(bool accept)
    {
      VERIFY_CORRECT_THREAD();
      _node_configuration.accept_incoming_connections = accept;
      save_node_configuration();
    }

    void NodeImpl::listen_on_port( uint16_t port, bool wait_if_not_available )
    {
      VERIFY_CORRECT_THREAD();
      _node_configuration.listen_endpoint = fc::ip::endpoint( fc::ip::address(), port );
      _node_configuration.wait_if_endpoint_is_busy = wait_if_not_available;
      save_node_configuration();
    }

    fc::ip::endpoint NodeImpl::get_actual_listening_endpoint() const
    {
      VERIFY_CORRECT_THREAD();
      return _actual_listening_endpoint;
    }

    std::vector<PeerStatus> NodeImpl::get_connected_peers() const
    {
      VERIFY_CORRECT_THREAD();
      std::vector<PeerStatus> statuses;
      for (const PeerConnectionPtr& peer : _active_connections)
      {
        ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections

        PeerStatus this_peer_status;
        this_peer_status.version = 0; // TODO
        fc::optional<fc::ip::endpoint> endpoint = peer->get_remote_endpoint();
        if (endpoint)
          this_peer_status.host = *endpoint;
        fc::mutable_variant_object peer_details;
        peer_details["addr"] = endpoint ? (std::string)*endpoint : std::string();
        peer_details["addrlocal"] = (std::string)peer->get_local_endpoint();
        peer_details["services"] = "00000001"; // TODO: assign meaning, right now this just prints what bitcoin prints
        peer_details["lastsend"] = peer->get_last_message_sent_time().sec_since_epoch();
        peer_details["lastrecv"] = peer->get_last_message_received_time().sec_since_epoch();
        peer_details["bytessent"] = peer->get_total_bytes_sent();
        peer_details["bytesrecv"] = peer->get_total_bytes_received();
        peer_details["conntime"] = peer->get_connection_time();
        peer_details["pingtime"] = ""; // TODO: fill me for bitcoin compatibility
        peer_details["pingwait"] = ""; // TODO: fill me for bitcoin compatibility
        peer_details["version"] = ""; // TODO: fill me for bitcoin compatibility
        peer_details["subver"] = peer->user_agent;
        peer_details["inbound"] = peer->direction == PeerConnectionDirection::inbound;
        peer_details["firewall_status"] = peer->is_firewalled;
        peer_details["startingheight"] = ""; // TODO: fill me for bitcoin compatibility
        peer_details["banscore"] = ""; // TODO: fill me for bitcoin compatibility
        peer_details["syncnode"] = ""; // TODO: fill me for bitcoin compatibility

        if (peer->goopal_git_revision_sha)
        {
          std::string revision_string = *peer->goopal_git_revision_sha;
          if (*peer->goopal_git_revision_sha == goopal::utilities::git_revision_sha)
            revision_string += " (same as ours)";
          else
            revision_string += " (different from ours)";
          peer_details["goopal_git_revision_sha"] = revision_string;

        }
        if (peer->goopal_git_revision_unix_timestamp)
        {
          peer_details["goopal_git_revision_unix_timestamp"] = *peer->goopal_git_revision_unix_timestamp;
          std::string age_string = fc::get_approximate_relative_time_string(*peer->goopal_git_revision_unix_timestamp);
          if (*peer->goopal_git_revision_unix_timestamp == fc::time_point_sec(goopal::utilities::git_revision_unix_timestamp))
            age_string += " (same as ours)";
          else if (*peer->goopal_git_revision_unix_timestamp > fc::time_point_sec(goopal::utilities::git_revision_unix_timestamp))
            age_string += " (newer than ours)";
          else
            age_string += " (older than ours)";
          peer_details["goopal_git_revision_age"] = age_string;
        }

        if (peer->fc_git_revision_sha)
        {
          std::string revision_string = *peer->fc_git_revision_sha;
          if (*peer->fc_git_revision_sha == fc::git_revision_sha)
            revision_string += " (same as ours)";
          else
            revision_string += " (different from ours)";
          peer_details["fc_git_revision_sha"] = revision_string;

        }
        if (peer->fc_git_revision_unix_timestamp)
        {
          peer_details["fc_git_revision_unix_timestamp"] = *peer->fc_git_revision_unix_timestamp;
          std::string age_string = fc::get_approximate_relative_time_string( *peer->fc_git_revision_unix_timestamp);
          if (*peer->fc_git_revision_unix_timestamp == fc::time_point_sec(fc::git_revision_unix_timestamp))
            age_string += " (same as ours)";
          else if (*peer->fc_git_revision_unix_timestamp > fc::time_point_sec(fc::git_revision_unix_timestamp))
            age_string += " (newer than ours)";
          else
            age_string += " (older than ours)";
          peer_details["fc_git_revision_age"] = age_string;
        }

        if (peer->platform)
          peer_details["platform"] = *peer->platform;

        // provide these for debugging
        // warning: these are just approximations, if the peer is "downstream" of us, they may
        // have received blocks from other peers that we are unaware of
        peer_details["current_head_block_number"] = peer->last_block_number_delegate_has_seen;
        peer_details["current_head_block"] = peer->last_block_delegate_has_seen;
        peer_details["current_head_block_time"] = peer->last_block_time_delegate_has_seen;

        this_peer_status.info = peer_details;
        statuses.push_back(this_peer_status);
      }
      return statuses;
    }

    uint32_t NodeImpl::get_connection_count() const
    {
      VERIFY_CORRECT_THREAD();
      return (uint32_t)_active_connections.size();
    }

    void NodeImpl::broadcast( const Message& item_to_broadcast, const MessagePropagationData& propagation_data )
    {
      VERIFY_CORRECT_THREAD();
      fc::uint160_t hash_of_message_contents;
      if( item_to_broadcast.msg_type == goopal::client::block_message_type )
      {
        goopal::client::BlockMessage block_message_to_broadcast = item_to_broadcast.as<goopal::client::BlockMessage>();
        hash_of_message_contents = block_message_to_broadcast.block_id; // for debugging
        _most_recent_blocks_accepted.push_back( block_message_to_broadcast.block_id );
      }
      else if( item_to_broadcast.msg_type == goopal::client::trx_message_type )
      {
        goopal::client::TrxMessage transaction_message_to_broadcast = item_to_broadcast.as<goopal::client::TrxMessage>();
        hash_of_message_contents = transaction_message_to_broadcast.trx.id(); // for debugging
        dlog( "broadcasting trx: ${trx}", ("trx", transaction_message_to_broadcast) );
      }
      MessageHashType hash_of_item_to_broadcast = item_to_broadcast.id();

      _message_cache.cache_message( item_to_broadcast, hash_of_item_to_broadcast, propagation_data, hash_of_message_contents );
      _new_inventory.insert( ItemId(item_to_broadcast.msg_type, hash_of_item_to_broadcast ) );
      trigger_advertise_inventory_loop();
    }

    void NodeImpl::broadcast( const Message& item_to_broadcast )
    {
      VERIFY_CORRECT_THREAD();
      // this version is called directly from the client
      MessagePropagationData propagation_data{fc::time_point::now(), fc::time_point::now(), _node_id};
      broadcast( item_to_broadcast, propagation_data );
    }

    void NodeImpl::sync_from(const ItemId& current_head_block, const std::vector<uint32_t>& hard_fork_block_numbers)
    {
      VERIFY_CORRECT_THREAD();
      _most_recent_blocks_accepted.clear();
      _sync_item_type = current_head_block.item_type;
      _most_recent_blocks_accepted.push_back(current_head_block.item_hash);
      _hard_fork_block_numbers = hard_fork_block_numbers;
    }

    bool NodeImpl::is_connected() const
    {
      VERIFY_CORRECT_THREAD();
      return !_active_connections.empty();
    }

    std::vector<PotentialPeerEntry> NodeImpl::get_potential_peers() const
    {
      VERIFY_CORRECT_THREAD();
      std::vector<PotentialPeerEntry> result;
      // use explicit iterators here, for some reason the mac compiler can't used ranged-based for loops here
      for (PeerDatabase::iterator itr = _potential_peer_db.begin(); itr != _potential_peer_db.end(); ++itr)
        result.push_back(*itr);
      return result;
    }

    void NodeImpl::set_advanced_node_parameters(const fc::variant_object& params)
    {
      VERIFY_CORRECT_THREAD();
      if (params.contains("peer_connection_retry_timeout"))
        _peer_connection_retry_timeout = params["peer_connection_retry_timeout"].as<uint32_t>();
      if (params.contains("desired_number_of_connections"))
        _desired_number_of_connections = params["desired_number_of_connections"].as<uint32_t>();
      if (params.contains("maximum_number_of_connections"))
        _maximum_number_of_connections = params["maximum_number_of_connections"].as<uint32_t>();
      if (params.contains("maximum_number_of_blocks_to_handle_at_one_time"))
        _maximum_number_of_blocks_to_handle_at_one_time = params["maximum_number_of_blocks_to_handle_at_one_time"].as<uint32_t>();
      if (params.contains("maximum_number_of_sync_blocks_to_prefetch"))
        _maximum_number_of_sync_blocks_to_prefetch = params["maximum_number_of_sync_blocks_to_prefetch"].as<uint32_t>();
      if (params.contains("maximum_blocks_per_peer_during_syncing"))
        _maximum_blocks_per_peer_during_syncing = params["maximum_blocks_per_peer_during_syncing"].as<uint32_t>();

      _desired_number_of_connections = std::min(_desired_number_of_connections, _maximum_number_of_connections);

      while (_active_connections.size() > _maximum_number_of_connections)
        disconnect_from_peer(_active_connections.begin()->get(),
                             "I have too many connections open");
      trigger_p2p_network_connect_loop();
    }

    fc::variant_object NodeImpl::get_advanced_node_parameters()
    {
      VERIFY_CORRECT_THREAD();
      fc::mutable_variant_object result;
      result["peer_connection_retry_timeout"] = _peer_connection_retry_timeout;
      result["desired_number_of_connections"] = _desired_number_of_connections;
      result["maximum_number_of_connections"] = _maximum_number_of_connections;
      result["maximum_number_of_blocks_to_handle_at_one_time"] = _maximum_number_of_blocks_to_handle_at_one_time;
      result["maximum_number_of_sync_blocks_to_prefetch"] = _maximum_number_of_sync_blocks_to_prefetch;
      result["maximum_blocks_per_peer_during_syncing"] = _maximum_blocks_per_peer_during_syncing;
      return result;
    }

    MessagePropagationData NodeImpl::get_transaction_propagation_data( const goopal::blockchain::TransactionIdType& transaction_id )
    {
      VERIFY_CORRECT_THREAD();
      return _message_cache.get_message_propagation_data( transaction_id );
    }

    MessagePropagationData NodeImpl::get_block_propagation_data( const goopal::blockchain::BlockIdType& block_id )
    {
      VERIFY_CORRECT_THREAD();
      return _message_cache.get_message_propagation_data( block_id );
    }

    NodeIdType NodeImpl::get_node_id() const
    {
      VERIFY_CORRECT_THREAD();
      return _node_id;
    }
    void NodeImpl::set_allowed_peers(const std::vector<NodeIdType>& allowed_peers)
    {
      VERIFY_CORRECT_THREAD();
#ifdef ENABLE_P2P_DEBUGGING_API
      _allowed_peers.clear();
      _allowed_peers.insert(allowed_peers.begin(), allowed_peers.end());
      std::list<PeerConnectionPtr> peers_to_disconnect;
      if (!_allowed_peers.empty())
        for (const PeerConnectionPtr& peer : _active_connections)
          if (_allowed_peers.find(peer->node_id) == _allowed_peers.end())
            peers_to_disconnect.push_back(peer);
      for (const PeerConnectionPtr& peer : peers_to_disconnect)
        disconnect_from_peer(peer.get(), "My allowed_peers list has changed, and you're no longer allowed.  Bye.");
#endif // ENABLE_P2P_DEBUGGING_API
    }
    void NodeImpl::clear_peer_database()
    {
      VERIFY_CORRECT_THREAD();
      _potential_peer_db.clear();
    }

    void NodeImpl::set_total_bandwidth_limit( uint32_t upload_bytes_per_second, uint32_t download_bytes_per_second )
    {
      VERIFY_CORRECT_THREAD();
      _rate_limiter.set_upload_limit( upload_bytes_per_second );
      _rate_limiter.set_download_limit( download_bytes_per_second );
    }

    void NodeImpl::disable_peer_advertising()
    {
      VERIFY_CORRECT_THREAD();
      _peer_advertising_disabled = true;
    }

    fc::variant_object NodeImpl::get_call_statistics() const
    {
      VERIFY_CORRECT_THREAD();
      return _delegate->get_call_statistics();
    }

    fc::variant_object NodeImpl::network_get_info() const
    {
      VERIFY_CORRECT_THREAD();
      fc::mutable_variant_object info;
      info["listening_on"] = _actual_listening_endpoint;
      info["node_public_key"] = _node_public_key;
      info["node_id"] = _node_id;
      info["firewalled"] = _is_firewalled;
      return info;
    }
    fc::variant_object NodeImpl::network_get_usage_stats() const
    {
      VERIFY_CORRECT_THREAD();
      std::vector<uint32_t> network_usage_by_second;
      network_usage_by_second.reserve(_average_network_read_speed_seconds.size());
      std::transform(_average_network_read_speed_seconds.begin(), _average_network_read_speed_seconds.end(),
                     _average_network_write_speed_seconds.begin(),
                     std::back_inserter(network_usage_by_second),
                     std::plus<uint32_t>());

      std::vector<uint32_t> network_usage_by_minute;
      network_usage_by_minute.reserve(_average_network_read_speed_minutes.size());
      std::transform(_average_network_read_speed_minutes.begin(), _average_network_read_speed_minutes.end(),
                     _average_network_write_speed_minutes.begin(),
                     std::back_inserter(network_usage_by_minute),
                     std::plus<uint32_t>());

      std::vector<uint32_t> network_usage_by_hour;
      network_usage_by_hour.reserve(_average_network_read_speed_hours.size());
      std::transform(_average_network_read_speed_hours.begin(), _average_network_read_speed_hours.end(),
                     _average_network_write_speed_hours.begin(),
                     std::back_inserter(network_usage_by_hour),
                     std::plus<uint32_t>());

      fc::mutable_variant_object result;
      result["usage_by_second"] = network_usage_by_second;
      result["usage_by_minute"] = network_usage_by_minute;
      result["usage_by_hour"] = network_usage_by_hour;
      return result;
    }

    bool NodeImpl::is_hard_fork_block(uint32_t block_number) const
    {
      return std::binary_search(_hard_fork_block_numbers.begin(), _hard_fork_block_numbers.end(), block_number);
    }
    uint32_t NodeImpl::get_next_known_hard_fork_block_number(uint32_t block_number) const
    {
      auto iter = std::upper_bound(_hard_fork_block_numbers.begin(), _hard_fork_block_numbers.end(),
                                   block_number);
      return iter != _hard_fork_block_numbers.end() ? *iter : 0;
    }

  }  // end namespace detail



  /////////////////////////////////////////////////////////////////////////////////////////////////////////////
  // implement node functions, they call the matching function in to detail::node_impl in the correct thread //

#ifdef P2P_IN_DEDICATED_THREAD
# define INVOKE_IN_IMPL(method_name, ...) \
    return my->_thread->async([&](){ return my->method_name(__VA_ARGS__); }, "thread invoke for method " BOOST_PP_STRINGIZE(method_name)).wait()
#else
# define INVOKE_IN_IMPL(method_name, ...) \
    return my->method_name(__VA_ARGS__)
#endif // P2P_IN_DEDICATED_THREAD

  Node::Node(const std::string& user_agent) :
    my(new detail::NodeImpl(user_agent))
  {
  }

  Node::~Node()
  {
  }

  void Node::set_node_delegate( NodeDelegate* del )
  {
    fc::thread* delegate_thread = &fc::thread::current();
    INVOKE_IN_IMPL(set_node_delegate, del, delegate_thread);
  }

  void Node::load_configuration( const fc::path& configuration_directory )
  {
    INVOKE_IN_IMPL(load_configuration, configuration_directory);
  }

  void Node::listen_to_p2p_network()
  {
    INVOKE_IN_IMPL(listen_to_p2p_network);
  }

  void Node::connect_to_p2p_network()
  {
    INVOKE_IN_IMPL(connect_to_p2p_network);
  }

  void Node::add_node( const fc::ip::endpoint& ep )
  {
    INVOKE_IN_IMPL(add_node, ep);
  }

  void Node::connect_to_endpoint( const fc::ip::endpoint& remote_endpoint )
  {
    INVOKE_IN_IMPL(connect_to_endpoint, remote_endpoint);
  }

  void Node::listen_on_endpoint( const fc::ip::endpoint& ep )
  {
    INVOKE_IN_IMPL(listen_on_endpoint, ep);
  }

  void Node::accept_incoming_connections(bool accept)
  {
    INVOKE_IN_IMPL(accept_incoming_connections, accept);
  }

  void Node::listen_on_port( uint16_t port, bool wait_if_not_available )
  {
    INVOKE_IN_IMPL(listen_on_port, port, wait_if_not_available);
  }

  fc::ip::endpoint Node::get_actual_listening_endpoint() const
  {
    INVOKE_IN_IMPL(get_actual_listening_endpoint);
  }

  std::vector<PeerStatus> Node::get_connected_peers() const
  {
    INVOKE_IN_IMPL(get_connected_peers);
  }

  uint32_t Node::get_connection_count() const
  {
    INVOKE_IN_IMPL(get_connection_count);
  }

  void Node::broadcast( const Message& msg )
  {
    INVOKE_IN_IMPL(broadcast, msg);
  }

  void Node::sync_from(const ItemId& current_head_block, const std::vector<uint32_t>& hard_fork_block_numbers)
  {
    INVOKE_IN_IMPL(sync_from, current_head_block, hard_fork_block_numbers);
  }

  bool Node::is_connected() const
  {
    INVOKE_IN_IMPL(is_connected);
  }

  std::vector<PotentialPeerEntry> Node::get_potential_peers()const
  {
    INVOKE_IN_IMPL(get_potential_peers);
  }

  void Node::set_advanced_node_parameters( const fc::variant_object& params )
  {
    INVOKE_IN_IMPL(set_advanced_node_parameters, params);
  }

  fc::variant_object Node::get_advanced_node_parameters()
  {
    INVOKE_IN_IMPL(get_advanced_node_parameters);
  }

  MessagePropagationData Node::get_transaction_propagation_data( const goopal::blockchain::TransactionIdType& transaction_id )
  {
    INVOKE_IN_IMPL(get_transaction_propagation_data, transaction_id);
  }

  MessagePropagationData Node::get_block_propagation_data( const goopal::blockchain::BlockIdType& block_id )
  {
    INVOKE_IN_IMPL(get_block_propagation_data, block_id);
  }

  NodeIdType Node::get_node_id() const
  {
    INVOKE_IN_IMPL(get_node_id);
  }

  void Node::set_allowed_peers( const std::vector<NodeIdType>& allowed_peers )
  {
    INVOKE_IN_IMPL(set_allowed_peers, allowed_peers);
  }

  void Node::clear_peer_database()
  {
    INVOKE_IN_IMPL(clear_peer_database);
  }

  void Node::set_total_bandwidth_limit( uint32_t upload_bytes_per_second,
                                       uint32_t download_bytes_per_second )
  {
    INVOKE_IN_IMPL(set_total_bandwidth_limit, upload_bytes_per_second, download_bytes_per_second);
  }

  void Node::disable_peer_advertising()
  {
    INVOKE_IN_IMPL(disable_peer_advertising);
  }

  fc::variant_object Node::get_call_statistics() const
  {
    INVOKE_IN_IMPL(get_call_statistics);
  }

  fc::variant_object Node::network_get_info() const
  {
    INVOKE_IN_IMPL(network_get_info);
  }

  fc::variant_object Node::network_get_usage_stats() const
  {
    INVOKE_IN_IMPL(network_get_usage_stats);
  }

  void Node::close()
  {
    wlog( ".... WARNING NOT DOING ANYTHING WHEN I SHOULD ......" );
    return;
    my->close();
  }

  struct SimulatedNetwork::node_info
  {
    NodeDelegate* delegate;
    fc::future<void> message_sender_task_done;
    std::queue<Message> messages_to_deliver;
    node_info(NodeDelegate* delegate) : delegate(delegate) {}
  };

  SimulatedNetwork::~SimulatedNetwork()
  {
    for( node_info* network_node_info : network_nodes )
    {
      network_node_info->message_sender_task_done.cancel_and_wait("~simulated_network()");
      delete network_node_info;
    }
  }

  void SimulatedNetwork::message_sender(node_info* destination_node)
  {
    while (!destination_node->messages_to_deliver.empty())
    {
      try
      {
        destination_node->delegate->handle_message(destination_node->messages_to_deliver.front(), false);
      }
      catch ( const fc::exception& e )
      {
        elog( "${r}", ("r",e.to_detail_string() ) );
      }
      destination_node->messages_to_deliver.pop();
    }
  }

  void SimulatedNetwork::broadcast( const Message& item_to_broadcast  )
  {
    for (node_info* network_node_info : network_nodes)
    {
      network_node_info->messages_to_deliver.emplace(item_to_broadcast);
      if (!network_node_info->message_sender_task_done.valid() || network_node_info->message_sender_task_done.ready())
        network_node_info->message_sender_task_done = fc::async([=](){ message_sender(network_node_info); }, "simulated_network_sender");
    }
  }

  void SimulatedNetwork::add_node_delegate( NodeDelegate* node_delegate_to_add )
  {
    network_nodes.push_back(new node_info(node_delegate_to_add));
  }

  namespace detail
  {
#define ROLLING_WINDOW_SIZE 1000
#define INITIALIZE_ACCUMULATOR(r, data, method_name) \
      , BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))(boost::accumulators::tag::rolling_window::window_size = ROLLING_WINDOW_SIZE) \
      , BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator))(boost::accumulators::tag::rolling_window::window_size = ROLLING_WINDOW_SIZE) \
      , BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator))(boost::accumulators::tag::rolling_window::window_size = ROLLING_WINDOW_SIZE)


    StatisticsGatheringNodeDelegateWrapper::StatisticsGatheringNodeDelegateWrapper(NodeDelegate* delegate, fc::thread* thread_for_delegate_calls) :
      _node_delegate(delegate),
      _thread(thread_for_delegate_calls)
      BOOST_PP_SEQ_FOR_EACH(INITIALIZE_ACCUMULATOR, unused, NODE_DELEGATE_METHOD_NAMES)
    {}
#undef INITIALIZE_ACCUMULATOR

    fc::variant_object StatisticsGatheringNodeDelegateWrapper::get_call_statistics()
    {
      fc::mutable_variant_object statistics;
      std::ostringstream note;
      note << "All times are in microseconds, mean is the average of the last " << ROLLING_WINDOW_SIZE << " call times";
      statistics["_note"] = note.str();

#define ADD_STATISTICS_FOR_METHOD(r, data, method_name) \
      fc::mutable_variant_object BOOST_PP_CAT(method_name, _stats); \
      BOOST_PP_CAT(method_name, _stats)["min"] = boost::accumulators::min(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["mean"] = boost::accumulators::rolling_mean(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["max"] = boost::accumulators::max(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["sum"] = boost::accumulators::sum(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_before_min"] = boost::accumulators::min(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_before_mean"] = boost::accumulators::rolling_mean(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_before_max"] = boost::accumulators::max(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_before_sum"] = boost::accumulators::sum(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_after_min"] = boost::accumulators::min(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_after_mean"] = boost::accumulators::rolling_mean(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_after_max"] = boost::accumulators::max(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_after_sum"] = boost::accumulators::sum(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["count"] = boost::accumulators::count(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))); \
      statistics[BOOST_PP_STRINGIZE(method_name)] = BOOST_PP_CAT(method_name, _stats);

      BOOST_PP_SEQ_FOR_EACH(ADD_STATISTICS_FOR_METHOD, unused, NODE_DELEGATE_METHOD_NAMES)
#undef ADD_STATISTICS_FOR_METHOD

      return statistics;
    }

#define INVOKE_AND_COLLECT_STATISTICS(returnValue,method_name, ...) \
    CallStatisticsCollector statistics_collector(#method_name, \
                                                   &_ ## method_name ## _execution_accumulator, \
                                                   &_ ## method_name ## _delay_before_accumulator, \
                                                   &_ ## method_name ## _delay_after_accumulator); \
    if (_thread->is_current()) \
			    { \
		CallStatisticsCollector::ActualExecutionMeasurementHelper helper(statistics_collector); \
		if(_node_delegate == nullptr){\
			return returnValue##();\
		}\
	    return _node_delegate->method_name(__VA_ARGS__); \
    } \
    else \
      return _thread->async([&](){ \
        CallStatisticsCollector::ActualExecutionMeasurementHelper helper(statistics_collector); \
		if(_node_delegate == nullptr){\
			return returnValue##();\
		}\
        return _node_delegate->method_name(__VA_ARGS__); \
      }, "invoke " BOOST_STRINGIZE(method_name)).wait()

    bool StatisticsGatheringNodeDelegateWrapper::has_item( const net::ItemId& id )
    {
      INVOKE_AND_COLLECT_STATISTICS(bool, has_item, id);
    }

    bool StatisticsGatheringNodeDelegateWrapper::handle_message( const Message& message_to_handle, bool sync_mode )
    {
// 	call_statistics_collector statistics_collector("handle_message",&_handle_message_execution_accumulator, \
// 	&_handle_message_delay_before_accumulator, &_handle_message_delay_after_accumulator);
// 	if (_thread->is_current()) 
// 	{
// 		call_statistics_collector::actual_execution_measurement_helper helper(statistics_collector); 
// 		return _node_delegate->handle_message(message_to_handle, sync_mode); 
// 	} 
// 	else
// 		return _thread->async([&](){ 
// 		call_statistics_collector::actual_execution_measurement_helper helper(statistics_collector); 
// 		printf("_node_delegate[%ld]", _node_delegate);
// 		return _node_delegate->handle_message(message_to_handle, sync_mode); 
// 		}, "invoke " BOOST_STRINGIZE(method_name)).wait();
		
		INVOKE_AND_COLLECT_STATISTICS(bool,handle_message, message_to_handle, sync_mode);

    }

    std::vector<ItemHashType> StatisticsGatheringNodeDelegateWrapper::get_item_ids(uint32_t item_type,
                                                                                  const std::vector<ItemHashType>& blockchain_synopsis,
                                                                                  uint32_t& remaining_item_count,
                                                                                  uint32_t limit /* = 2000 */)
    {
		
		INVOKE_AND_COLLECT_STATISTICS(std::vector<ItemHashType>, get_item_ids, item_type, blockchain_synopsis, remaining_item_count, limit);

    }

    Message StatisticsGatheringNodeDelegateWrapper::get_item( const ItemId& id )
    {
		INVOKE_AND_COLLECT_STATISTICS(Message,get_item, id);
    }

    fc::sha256 StatisticsGatheringNodeDelegateWrapper::get_chain_id() const
    {
		INVOKE_AND_COLLECT_STATISTICS(fc::sha256,get_chain_id);

    }

    std::vector<ItemHashType> StatisticsGatheringNodeDelegateWrapper::get_blockchain_synopsis(uint32_t item_type,
                                                                                             const goopal::net::ItemHashType& reference_point /* = goopal::net::item_hash_t() */,
                                                                                             uint32_t number_of_blocks_after_reference_point /* = 0 */)
    {

		INVOKE_AND_COLLECT_STATISTICS(std::vector<ItemHashType>,get_blockchain_synopsis, item_type, reference_point, number_of_blocks_after_reference_point);
    }

    void StatisticsGatheringNodeDelegateWrapper::sync_status( uint32_t item_type, uint32_t item_count )
    {
			INVOKE_AND_COLLECT_STATISTICS(void,sync_status, item_type, item_count);
    }

    void StatisticsGatheringNodeDelegateWrapper::connection_count_changed( uint32_t c )
    {
			INVOKE_AND_COLLECT_STATISTICS(void,connection_count_changed, c);

    }

    uint32_t StatisticsGatheringNodeDelegateWrapper::get_block_number(const ItemHashType& block_id)
    {
			INVOKE_AND_COLLECT_STATISTICS(uint32_t,get_block_number, block_id);

    }
    fc::time_point_sec StatisticsGatheringNodeDelegateWrapper::get_block_time(const ItemHashType& block_id)
    {
		INVOKE_AND_COLLECT_STATISTICS(fc::time_point_sec,get_block_time, block_id);

    }

    /** returns goopal::blockchain::now() */
    fc::time_point_sec StatisticsGatheringNodeDelegateWrapper::get_blockchain_now()
    {
      // this function doesn't need to block,
      ASSERT_TASK_NOT_PREEMPTED();
	  if (_node_delegate != nullptr){
		  return _node_delegate->get_blockchain_now();
	  }
	  return fc::time_point_sec();
    }

    ItemHashType StatisticsGatheringNodeDelegateWrapper::get_head_block_id() const
    {
		INVOKE_AND_COLLECT_STATISTICS(ItemHashType,get_head_block_id);

    }

    uint32_t StatisticsGatheringNodeDelegateWrapper::estimate_last_known_fork_from_git_revision_timestamp(uint32_t unix_timestamp) const
    {
			INVOKE_AND_COLLECT_STATISTICS(uint32_t,estimate_last_known_fork_from_git_revision_timestamp, unix_timestamp);

    }

    void StatisticsGatheringNodeDelegateWrapper::error_encountered(const std::string& message, const fc::oexception& error)
    {
			INVOKE_AND_COLLECT_STATISTICS(void,error_encountered, message, error);

    }

#undef INVOKE_AND_COLLECT_STATISTICS

  } // end namespace detail

} } // end namespace goopal::net