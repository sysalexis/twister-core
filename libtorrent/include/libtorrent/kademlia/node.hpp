/*

Copyright (c) 2006-2012, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef NODE_HPP
#define NODE_HPP

#include <algorithm>
#include <map>
#include <set>

#include <libtorrent/config.hpp>
#include <libtorrent/kademlia/routing_table.hpp>
#include <libtorrent/kademlia/rpc_manager.hpp>
#include <libtorrent/kademlia/node_id.hpp>
#include <libtorrent/kademlia/msg.hpp>
#include <libtorrent/kademlia/find_data.hpp>

#include <libtorrent/io.hpp>
#include <libtorrent/session_settings.hpp>
#include <libtorrent/assert.hpp>
#include <libtorrent/thread.hpp>
#include <libtorrent/bloom_filter.hpp>

#include <boost/cstdint.hpp>
#include <boost/ref.hpp>

#include "libtorrent/socket.hpp"

namespace libtorrent {
	class alert_manager;
	struct alert_dispatcher;
}

namespace libtorrent { namespace dht
{

#ifdef TORRENT_DHT_VERBOSE_LOGGING
TORRENT_DECLARE_LOG(node);
#endif

struct traversal_algorithm;

struct key_desc_t
{
	char const* name;
	int type;
	int size;
	int flags;

	enum {
		// this argument is optional, parsing will not
		// fail if it's not present
		optional = 1,
		// for dictionaries, the following entries refer
		// to child nodes to this node, up until and including
		// the next item that has the last_child flag set.
		// these flags are nestable
		parse_children = 2,
		// this is the last item in a child dictionary
		last_child = 4,
		// the size argument refers to that the size
		// has to be divisible by the number, instead
		// of having that exact size
		size_divisible = 8
	}; 
};

bool TORRENT_EXTRA_EXPORT verify_message(lazy_entry const* msg, key_desc_t const desc[]
	, lazy_entry const* ret[], int size , char* error, int error_size);

// this is the entry for every peer
// the timestamp is there to make it possible
// to remove stale peers
struct peer_entry
{
	tcp::endpoint addr;
	ptime added;
	bool seed;
};

// this is a group. It contains a set of group members
struct torrent_entry
{
	std::string name;
	std::set<peer_entry> peers;
};

struct dht_immutable_item
{
	dht_immutable_item() : value(0), num_announcers(0), size(0) {}
	// malloced space for the actual value
	char* value;
	// this counts the number of IPs we have seen
	// announcing this item, this is used to determine
	// popularity if we reach the limit of items to store
	bloom_filter<128> ips;
	// the last time we heard about this
	ptime last_seen;
	// number of IPs in the bloom filter
	int num_announcers;
	// size of malloced space pointed to by value
	int size;
};

struct rsa_key { char bytes[268]; };

struct dht_mutable_item : dht_immutable_item
{
	char sig[256];
	int seq;
	rsa_key key;
};

struct dht_storage_item
{
    dht_storage_item() : p(), sig_p(), sig_user() {}
        std::string p;
        std::string sig_p;
        std::string sig_user;
        // the last time we heard about this
        //ptime last_seen;
};



// internal
inline bool operator<(rsa_key const& lhs, rsa_key const& rhs)
{
	return memcmp(lhs.bytes, rhs.bytes, sizeof(lhs.bytes)) < 0;
}

// internal
inline bool operator<(peer_entry const& lhs, peer_entry const& rhs)
{
	return lhs.addr.address() == rhs.addr.address()
		? lhs.addr.port() < rhs.addr.port()
		: lhs.addr.address() < rhs.addr.address();
}

struct null_type {};

class announce_observer : public observer
{
public:
	announce_observer(boost::intrusive_ptr<traversal_algorithm> const& algo
		, udp::endpoint const& ep, node_id const& id)
		: observer(algo, ep, id)
	{}

	void reply(msg const&) { flags |= flag_done; }
};

struct count_peers
{
	int& count;
	count_peers(int& c): count(c) {}
	void operator()(std::pair<libtorrent::dht::node_id
		, libtorrent::dht::torrent_entry> const& t)
	{
		count += t.second.peers.size();
	}
};

struct udp_socket_interface
{
	virtual bool send_packet(entry& e, udp::endpoint const& addr, int flags) = 0;
};

class TORRENT_EXTRA_EXPORT node_impl : boost::noncopyable
{
typedef std::map<node_id, torrent_entry> table_t;
typedef std::map<node_id, dht_immutable_item> dht_immutable_table_t;
typedef std::map<node_id, dht_mutable_item> dht_mutable_table_t;
typedef std::vector<dht_storage_item> dht_storage_list_t;
typedef std::map<node_id, dht_storage_list_t> dht_storage_table_t;

public:
	node_impl(alert_dispatcher* alert_disp, udp_socket_interface* sock
		, dht_settings const& settings, node_id nid, address const& external_address
		, dht_observer* observer);

	virtual ~node_impl() {}

	void tick();
	void refresh(node_id const& id, find_data::nodes_callback const& f);
	void bootstrap(std::vector<udp::endpoint> const& nodes
		, find_data::nodes_callback const& f);
	void add_router_node(udp::endpoint router);
		
	void unreachable(udp::endpoint const& ep);
	void incoming(msg const& m);

	int num_torrents() const { return m_map.size(); }
	int num_peers() const
	{
		int ret = 0;
		std::for_each(m_map.begin(), m_map.end(), count_peers(ret));
		return ret;
	}

	int bucket_size(int bucket);

	node_id const& nid() const { return m_id; }

	boost::tuple<int, int> size() const{ return m_table.size(); }
	size_type num_global_nodes() const
	{ return m_table.num_global_nodes(); }

	int data_size() const { return int(m_map.size()); }

#ifdef TORRENT_DHT_VERBOSE_LOGGING
	void print_state(std::ostream& os) const
	{ m_table.print_state(os); }
#endif

	void announce(sha1_hash const& info_hash, int listen_port, bool seed
		, boost::function<void(std::vector<tcp::endpoint> const&)> f);

	void putData(std::string const &username, std::string const &resource, bool multi,
		     entry const &value, std::string const &sig_user,
		     int timeutc, int seq);

	void getData(std::string const &username, std::string const &resource, bool multi,
		     boost::function<void(entry::list_type const&)> f);

	bool verify_token(std::string const& token, char const* info_hash
		, udp::endpoint const& addr);

	std::string generate_token(udp::endpoint const& addr, char const* info_hash);
	
	// the returned time is the delay until connection_timeout()
	// should be called again the next time
	time_duration connection_timeout();

	// generates a new secret number used to generate write tokens
	void new_write_key();

	// pings the given node, and adds it to
	// the routing table if it respons and if the
	// bucket is not full.
	void add_node(udp::endpoint node);

	void replacement_cache(bucket_t& nodes) const
	{ m_table.replacement_cache(nodes); }

	int branch_factor() const { return m_settings.search_branching; }

	void add_traversal_algorithm(traversal_algorithm* a)
	{
		mutex_t::scoped_lock l(m_mutex);
		m_running_requests.insert(a);
	}

	void remove_traversal_algorithm(traversal_algorithm* a)
	{
		mutex_t::scoped_lock l(m_mutex);
		m_running_requests.erase(a);
	}

	void status(libtorrent::session_status& s);

	dht_settings const& settings() const { return m_settings; }

protected:

	void lookup_peers(sha1_hash const& info_hash, int prefix, entry& reply
		, bool noseed, bool scrape) const;
	bool lookup_torrents(sha1_hash const& target, entry& reply
		, char* tags) const;

	dht_settings const& m_settings;
	
private:
	typedef libtorrent::mutex mutex_t;
	mutex_t m_mutex;

	// this list must be destructed after the rpc manager
	// since it might have references to it
	std::set<traversal_algorithm*> m_running_requests;

	void incoming_request(msg const& h, entry& e);

	node_id m_id;

public:
	routing_table m_table;
	rpc_manager m_rpc;

private:
	table_t m_map;
	dht_immutable_table_t m_immutable_table;
	dht_mutable_table_t m_mutable_table;
	dht_storage_table_t m_storage_table;
	
	ptime m_last_tracker_tick;

	// secret random numbers used to create write tokens
	int m_secret[2];

	alert_dispatcher* m_post_alert;
	udp_socket_interface* m_sock;
};


} } // namespace libtorrent::dht

#endif // NODE_HPP

