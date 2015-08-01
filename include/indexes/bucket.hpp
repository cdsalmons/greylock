#ifndef __INDEXES_BUCKET_HPP
#define __INDEXES_BUCKET_HPP

#include "indexes/elliptics_stat.hpp"
#include "indexes/error.hpp"

#include <elliptics/session.hpp>

#include <msgpack.hpp>

#include <condition_variable>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ioremap { namespace indexes {

struct bucket_acl {
	enum {
		serialization_version = 2,
	};

	// This enum describes per-user authorization flags
	enum auth_flags {
		auth_no_token = 0x01, // this user is able to perform requests without the authorization
		auth_write = 0x02, // this user is able to write to this bucket
		auth_admin = 0x04, // this user is able to change this bucket,
		auth_all = auth_write | auth_admin,
	};

	// This enum describes per-handler authorization flags
	enum handler_flags {
		handler_read = 0x01, // user must have read rights to access this handler
		handler_write = 0x02, // user must have write rights to access this handler
		handler_bucket = 0x04, // user must have admin rights to access this handler
		handler_not_found_is_ok = 0x08 // user is able to access this handler even if bucket doesn't exist
	};

	bool has_no_token() const {
		return flags & auth_no_token;
	}

	bool can_read() const {
		return true;
	}

	bool can_write() const {
		return flags & auth_write;
	}

	bool can_admin() const {
		return flags & auth_admin;
	}

	std::string to_string(void) const {
		std::ostringstream acl_ss;
		if (!user.empty())
			acl_ss << user << ":" << token << ":0x" << std::hex << flags;

		return acl_ss.str();
	}

	std::string user;
	std::string token;
	uint64_t flags = 0;
};

struct bucket_meta {
	enum {
		serialization_version = 1,
	};

	std::string name;
	std::map<std::string, bucket_acl> acl;
	std::vector<int> groups;
	uint64_t flags = 0;
	uint64_t max_size = 0;
	uint64_t max_key_num = 0;
	uint64_t reserved[3];

	bucket_meta() {
		memset(reserved, 0, sizeof(reserved));
	}
};

struct bucket_stat {
	std::map<int, backend_stat>	backends;
};

class raw_bucket {
public:
	raw_bucket(std::shared_ptr<elliptics::node> &node, const std::vector<int> mgroups, const std::string &name) :
	m_node(node),
	m_meta_groups(mgroups) {
		m_meta.name = name;
		reload();
	}

	void reload() {
		elliptics::session s(*m_node);
		s.set_exceptions_policy(elliptics::session::no_exceptions);
		s.set_filter(elliptics::filters::all);
		s.set_groups(m_meta_groups);

		std::string ns = "bucket";
		s.set_namespace(ns.c_str(), ns.size());

		m_reloaded = false;

		elliptics::logger &log = m_node->get_log();

		BH_LOG(log, DNET_LOG_INFO, "reload: going to reload bucket: %s", m_meta.name.c_str());
		s.read_data(m_meta.name, 0, 0).connect(
			std::bind(&raw_bucket::reload_completed, this, std::placeholders::_1, std::placeholders::_2));
	}

	bool wait_for_reload() {
		std::unique_lock<std::mutex> guard(m_lock);
		m_wait.wait(guard, [&] {return m_reloaded;});

		return m_valid;
	}

	bool valid() const {
		return m_valid;
	}

	status read(const std::string &key) {
		if (!m_valid) {
			return invalid_status();
		}

		elliptics::session s = session(true);
		return s.read_data(key, 0, 0).get_one();
	}

	std::vector<status> read_all(const std::string &key) {
		if (!m_valid) {
			return std::vector<status>();
		}
		std::vector<elliptics::async_read_result> results;

		elliptics::session s = session(true);
		for (auto it = m_meta.groups.begin(), end = m_meta.groups.end(); it != end; ++it) {
			std::vector<int> tmp;
			tmp.push_back(*it);

			s.set_groups(tmp);

			results.emplace_back(s.read_data(key, 0, 0));
		}

		std::vector<status> ret;
		for (auto it = results.begin(), end = results.end(); it != end; ++it) {
			ret.push_back(status(it->get_one()));
		}

		return ret;
	}

	std::vector<status> write(const std::vector<int> groups, const std::string &key,
			const std::string &data, size_t reserve_size, bool cache = false) {
		if (!m_valid) {
			return std::vector<status>();
		}
		elliptics::data_pointer dp = elliptics::data_pointer::from_raw((char *)data.data(), data.size());

		elliptics::session s = session(cache);

		s.set_filter(elliptics::filters::all);
		s.set_groups(groups);

		elliptics::key id(key);
		s.transform(id);

		dnet_io_control ctl;

		memset(&ctl, 0, sizeof(ctl));
		dnet_current_time(&ctl.io.timestamp);

		ctl.cflags = s.get_cflags();
		ctl.data = dp.data();

		ctl.io.flags = s.get_ioflags() | DNET_IO_FLAGS_PREPARE | DNET_IO_FLAGS_PLAIN_WRITE | DNET_IO_FLAGS_COMMIT;
		ctl.io.user_flags = s.get_user_flags();
		ctl.io.offset = 0;
		ctl.io.size = dp.size();
		ctl.io.num = reserve_size;
		if (ctl.io.size > ctl.io.num) {
			ctl.io.num = ctl.io.size * 1.5;
		}

		memcpy(&ctl.id, &id.id(), sizeof(ctl.id));

		ctl.fd = -1;

		std::vector<status> ret;
		elliptics::sync_write_result res = s.write_data(ctl).get();
		for (auto it = res.begin(), end = res.end(); it != end; ++it) {
			ret.emplace_back(status(*it));
		}

		return ret;
	}

	std::vector<status> write(const std::string &key, const std::string &data, size_t reserve_size, bool cache = false) {
		return write(m_meta.groups, key, data, reserve_size, cache);
	}

	std::vector<status> remove(const std::string &key) {
		if (!m_valid) {
			return std::vector<status>();
		}
		elliptics::session s = session(false);
		elliptics::sync_remove_result res = s.remove(key).get();
		std::vector<status> ret;
		for (auto it = res.begin(), end = res.end(); it != end; ++it) {
			ret.emplace_back(status(*it));
		}

		return ret;
	}

	bucket_meta meta() {
		std::lock_guard<std::mutex> guard(m_lock);
		return m_meta;
	}

	void set_backend_stat(int group, const backend_stat &bs) {
		std::lock_guard<std::mutex> guard(m_lock);
		m_stat.backends[group] = bs;
	}

private:
	std::shared_ptr<elliptics::node> m_node;
	std::vector<int> m_meta_groups;

	bool m_valid = false;

	bool m_reloaded = false;
	std::condition_variable m_wait;

	std::mutex m_lock;
	bucket_meta m_meta;

	bucket_stat m_stat;

	status invalid_status() {
		status st;
		st.error = -EIO;
		st.message = "bucket: " + m_meta.name + " is not valid";

		return st;
	}

	elliptics::session session(bool cache) {
		elliptics::session s(*m_node);
		s.set_namespace(m_meta.name);
		s.set_groups(m_meta.groups);
		s.set_timeout(60);
		s.set_exceptions_policy(elliptics::session::no_exceptions);
		if (cache)
			s.set_ioflags(DNET_IO_FLAGS_CACHE);

		return s;
	}

	void reload_completed(const elliptics::sync_read_result &result, const elliptics::error_info &error) {
		elliptics::logger &log = m_node->get_log();

		if (error) {
			BH_LOG(log, DNET_LOG_ERROR, "reload_completed: bucket: %s: could not reload: %s, error: %d",
					m_meta.name.c_str(), error.message().c_str(), error.code());
			m_valid = false;
		} else {
			meta_unpack(result);
		}

		m_reloaded = true;
		m_wait.notify_all();
	}

	void meta_unpack(const elliptics::sync_read_result &result) {
		elliptics::logger &log = m_node->get_log();

		try {
			const elliptics::read_result_entry &entry = result[0];
			auto file = entry.file();

			msgpack::unpacked msg;
			msgpack::unpack(&msg, file.data<char>(), file.size());

			std::unique_lock<std::mutex> guard(m_lock);
			msg.get().convert(&m_meta);

			std::ostringstream ss;
			std::copy(m_meta.groups.begin(), m_meta.groups.end(), std::ostream_iterator<int>(ss, ":"));

			m_valid = true;

			BH_LOG(log, DNET_LOG_INFO, "meta_unpack: bucket: %s, acls: %ld, flags: 0x%lx, groups: %s",
					m_meta.name.c_str(), m_meta.acl.size(), m_meta.flags, ss.str().c_str());
		} catch (const std::exception &e) {
			m_valid = false;

			BH_LOG(log, DNET_LOG_ERROR, "meta_unpack: bucket: %s, exception: %s",
					m_meta.name.c_str(), e.what());
		}
	}
};

typedef std::shared_ptr<raw_bucket> bucket;
static inline bucket make_bucket(std::shared_ptr<elliptics::node> &node, const std::vector<int> mgroups, const std::string &name) {
	return std::make_shared<raw_bucket>(node, mgroups, name);
}

class bucket_processor {
public:
	bucket_processor(std::shared_ptr<elliptics::node> &node) :
	m_node(node),
	m_stat(node)
	{
	}

	virtual ~bucket_processor() {
		m_need_exit = true;
		m_wait.notify_all();
		if (m_buckets_update.joinable())
			m_buckets_update.join();
	}

	bool init(const std::vector<int> &mgroups, const std::vector<std::string> &bnames) {
		std::map<std::string, bucket> buckets = read_buckets(mgroups, bnames);

		m_buckets_update =  std::thread(std::bind(&bucket_processor::buckets_update, this));

		std::unique_lock<std::mutex> lock(m_lock);

		m_buckets = buckets;
		m_bnames = bnames;
		m_meta_groups = mgroups;

		if (buckets.empty())
			return false;

		return true;
	}

	status get_bucket(size_t size) {
		status st;

		std::vector<std::string> good_buckets;

		std::lock_guard<std::mutex> lock(m_lock);
		if (m_buckets.size() == 0) {
			st.error = -ENODEV;
			st.message = "there are no suitable buckets for size " + elliptics::lexical_cast(size);

			return st;
		}

		for (auto it = m_buckets.begin(), end = m_buckets.end(); it != end; ++it) {
			if (it->second->valid()) {
				good_buckets.push_back(it->first);
			}
		}

		size_t idx = rand() % good_buckets.size(); // we do not really care about true randomness here

		st.data = elliptics::data_pointer::copy(good_buckets[idx]);
		return st;
	}

	status read(const std::string &bname, const std::string &key) {
		bucket b = find_bucket(bname);
		if (!b) {
			status st;
			st.error = -ENODEV;
			st.message = "bucket: " + bname + " : there is no such bucket";
			return st;
		}

		return b->read(key);
	}

	std::vector<status> read_all(const std::string &bname, const std::string &key) {
		bucket b = find_bucket(bname);
		if (!b) {
			return std::vector<status>();
		}

		return b->read_all(key);
	}

	std::vector<status> write(const std::vector<int> groups, const std::string &bname, const std::string &key,
			const std::string &data, size_t reserve_size, bool cache = false) {
		bucket b = find_bucket(bname);
		if (!b) {
			return std::vector<status>();
		}

		return b->write(groups, key, data, reserve_size, cache);
	}

	std::vector<status> write(const std::string &bname, const std::string &key,
			const std::string &data, size_t reserve_size, bool cache = false) {
		bucket b = find_bucket(bname);
		if (!b) {
			return std::vector<status>();
		}

		return b->write(key, data, reserve_size, cache);
	}

	std::vector<status> remove(const std::string &bname, const std::string &key) {
		bucket b = find_bucket(bname);
		if (!b) {
			return std::vector<status>();
		}

		return b->remove(key);
	}

private:
	std::shared_ptr<elliptics::node> m_node;

	std::mutex m_lock;
	std::vector<int> m_meta_groups;

	std::vector<std::string> m_bnames;
	std::map<std::string, bucket> m_buckets;

	bool m_need_exit = false;
	std::condition_variable m_wait;
	std::thread m_buckets_update;

	elliptics_stat m_stat;

	bucket find_bucket(const std::string &bname) {
		std::lock_guard<std::mutex> lock(m_lock);
		auto it = m_buckets.find(bname);
		if (it == m_buckets.end()) {
			return bucket();
		}

		return it->second;
	}

	std::map<std::string, bucket> read_buckets(const std::vector<int> mgroups, const std::vector<std::string> &bnames) {
		std::map<std::string, bucket> buckets;

		for (auto it = bnames.begin(), end = bnames.end(); it != end; ++it) {
			buckets[*it] = make_bucket(m_node, mgroups, *it);
		}
		m_stat.schedule_update_and_wait();

		elliptics::logger &log = m_node->get_log();
		for (auto it = buckets.begin(), end = buckets.end(); it != end; ++it) {
			it->second->wait_for_reload();

			if (it->second->valid()) {
				bucket_meta meta = it->second->meta();
				for (auto g = meta.groups.begin(), gend = meta.groups.end(); g != gend; ++g) {
					backend_stat bs = m_stat.stat(*g);
					if (bs.group == *g) {
						it->second->set_backend_stat(*g, bs);
					}
				}
			}

			BH_LOG(log, DNET_LOG_INFO, "read_buckets: bucket: %s: reloaded, valid: %d",
					it->first.c_str(), it->second->valid());
		}

		return buckets;
	}

	void buckets_update() {
		while (!m_need_exit) {
			std::unique_lock<std::mutex> guard(m_lock);
			if (m_wait.wait_for(guard, std::chrono::seconds(30), [&] {return m_need_exit;}))
				break;
			guard.unlock();

			std::map<std::string, bucket> buckets = read_buckets(m_meta_groups, m_bnames);

			guard.lock();
			m_buckets = buckets;
		}
	}
};

}} // namespace ioremap::indexes

namespace msgpack
{
static inline ioremap::indexes::bucket_acl &operator >>(msgpack::object o, ioremap::indexes::bucket_acl &acl)
{
	if (o.type != msgpack::type::ARRAY) {
		std::ostringstream ss;
		ss << "bucket-acl unpack: type: " << o.type <<
			", must be: " << msgpack::type::ARRAY <<
			", size: " << o.via.array.size;
		throw std::runtime_error(ss.str());
	}

	object *p = o.via.array.ptr;
	const uint32_t size = o.via.array.size;
	uint16_t version = 0;
	p[0].convert(&version);
	switch (version) {
	case 1:
	case 2: {
		if (size != 4) {
			std::ostringstream ss;
			ss << "bucket acl unpack: array size mismatch: read: " << size << ", must be: 4";
			throw std::runtime_error(ss.str());
		}

		p[1].convert(&acl.user);
		p[2].convert(&acl.token);
		p[3].convert(&acl.flags);

		if (version == 1) {
			using namespace ioremap::indexes;
			// Convert flags from old version to new one
			const bool noauth_read = acl.flags & (1 << 0);
			const bool noauth_all = acl.flags & (1 << 1);

			acl.flags = 0;

			// If there was any noauth - we shouldn't check token
			if (noauth_all || noauth_read) {
				acl.flags |= bucket_acl::auth_no_token;
			}

			// If there wasn't 'noauth_read' flag - user is permitted to do everything he want
			if (!noauth_read) {
				acl.flags |= bucket_acl::auth_admin | bucket_acl::auth_write;
			}
		}
		break;
	}
	default: {
		std::ostringstream ss;
		ss << "bucket acl unpack: version mismatch: read: " << version <<
			", must be: <= " << ioremap::indexes::bucket_acl::serialization_version;
		throw std::runtime_error(ss.str());
	}
	}

	return acl;
}

template <typename Stream>
inline msgpack::packer<Stream> &operator <<(msgpack::packer<Stream> &o, const ioremap::indexes::bucket_acl &acl)
{
	o.pack_array(4);
	o.pack((int)ioremap::indexes::bucket_acl::serialization_version);
	o.pack(acl.user);
	o.pack(acl.token);
	o.pack(acl.flags);

	return o;
}

template <typename Stream>
static inline msgpack::packer<Stream> &operator <<(msgpack::packer<Stream> &o, const ioremap::indexes::bucket_meta &m)
{
	o.pack_array(10);
	o.pack((int)ioremap::indexes::bucket_meta::serialization_version);
	o.pack(m.name);
	o.pack(m.acl);
	o.pack(m.groups);
	o.pack(m.flags);
	o.pack(m.max_size);
	o.pack(m.max_key_num);
	for (size_t i = 0; i < ARRAY_SIZE(m.reserved); ++i)
		o.pack(m.reserved[i]);

	return o;
}

static inline ioremap::indexes::bucket_meta &operator >>(msgpack::object o, ioremap::indexes::bucket_meta &m)
{
	if (o.type != msgpack::type::ARRAY || o.via.array.size < 10) {
		std::ostringstream ss;
		ss << "bucket unpack: type: " << o.type <<
			", must be: " << msgpack::type::ARRAY <<
			", size: " << o.via.array.size;
		throw std::runtime_error(ss.str());
	}

	object *p = o.via.array.ptr;
	const uint32_t size = o.via.array.size;
	uint16_t version = 0;
	p[0].convert(&version);
	switch (version) {
	case 1: {
		if (size != 10) {
			std::ostringstream ss;
			ss << "bucket unpack: array size mismatch: read: " << size << ", must be: 10";
			throw std::runtime_error(ss.str());
		}

		p[1].convert(&m.name);
		p[2].convert(&m.acl);
		p[3].convert(&m.groups);
		p[4].convert(&m.flags);
		p[5].convert(&m.max_size);
		p[6].convert(&m.max_key_num);
		for (size_t i = 0; i < ARRAY_SIZE(m.reserved); ++i)
			p[7 + i].convert(&m.reserved[i]);
		break;
	}
	default: {
		std::ostringstream ss;
		ss << "bucket unpack: version mismatch: read: " << version <<
			", must be: <= " << ioremap::indexes::bucket_meta::serialization_version;
		throw std::runtime_error(ss.str());
	}
	}

	return m;
}
} // namespace msgpack

#endif // __INDEXES_BUCKET_HPP
