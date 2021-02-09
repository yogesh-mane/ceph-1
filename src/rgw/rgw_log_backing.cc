// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#include "cls/log/cls_log_client.h"
#include "cls/version/cls_version_client.h"

#include "rgw_log_backing.h"
#include "rgw_tools.h"
#include "cls_fifo_legacy.h"

namespace cb = ceph::buffer;

static constexpr auto dout_subsys = ceph_subsys_rgw;

enum class shard_check { dne, omap, fifo, corrupt };
inline std::ostream& operator <<(std::ostream& m, const shard_check& t) {
  switch (t) {
  case shard_check::dne:
    return m << "shard_check::dne";
  case shard_check::omap:
    return m << "shard_check::omap";
  case shard_check::fifo:
    return m << "shard_check::fifo";
  case shard_check::corrupt:
    return m << "shard_check::corrupt";
  }

  return m << "shard_check::UNKNOWN=" << static_cast<uint32_t>(t);
}

namespace {
/// Return the shard type, and a bool to see whether it has entries.
std::pair<shard_check, bool>
probe_shard(librados::IoCtx& ioctx, const std::string& oid, optional_yield y)
{
  auto cct = static_cast<CephContext*>(ioctx.cct());
  bool omap = false;
  {
    librados::ObjectReadOperation op;
    cls_log_header header;
    cls_log_info(op, &header);
    auto r = rgw_rados_operate(ioctx, oid, &op, nullptr, y);
    if (r == -ENOENT) {
      return { shard_check::dne, {} };
    }

    if (r < 0) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << " error probing for omap: r=" << r
		 << ", oid=" << oid << dendl;
      return { shard_check::corrupt, {} };
    }
    if (header != cls_log_header{})
      omap = true;
  }
  std::unique_ptr<rgw::cls::fifo::FIFO> fifo;
  auto r = rgw::cls::fifo::FIFO::open(ioctx, oid,
				      &fifo, y,
				      std::nullopt, true);
  if (r < 0 && !(r == -ENOENT || r == -ENODATA)) {
    lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
	       << " error probing for fifo: r=" << r
	       << ", oid=" << oid << dendl;
    return { shard_check::corrupt, {} };
  }
  if (fifo && omap) {
    lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
	       << " fifo and omap found: oid=" << oid << dendl;
    return { shard_check::corrupt, {} };
  }
  if (fifo) {
    bool more = false;
    std::vector<rgw::cls::fifo::list_entry> entries;
    r = fifo->list(1, nullopt, &entries, &more, y);
    if (r < 0) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": unable to list entries: r=" << r
		 << ", oid=" << oid << dendl;
      return { shard_check::corrupt, {} };
    }
    return { shard_check::fifo, !entries.empty() };
  }
  if (omap) {
    std::list<cls_log_entry> entries;
    std::string out_marker;
    bool truncated = false;
    librados::ObjectReadOperation op;
    cls_log_list(op, {}, {}, {}, 1, entries,
		 &out_marker, &truncated);
    auto r = rgw_rados_operate(ioctx, oid, &op, nullptr, y);
    if (r < 0) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": failed to list: r=" << r << ", oid=" << oid << dendl;
      return { shard_check::corrupt, {} };
    }
    return { shard_check::omap, !entries.empty() };
  }

  // An object exists, but has never had FIFO or cls_log entries written
  // to it. Likely just the marker Omap.
  return { shard_check::dne, {} };
}

tl::expected<log_type, bs::error_code>
handle_dne(librados::IoCtx& ioctx,
	   log_type def,
	   std::string oid,
	   optional_yield y)
{
  auto cct = static_cast<CephContext*>(ioctx.cct());
  if (def == log_type::fifo) {
    std::unique_ptr<rgw::cls::fifo::FIFO> fifo;
    auto r = rgw::cls::fifo::FIFO::create(ioctx, oid,
					  &fifo, y,
					  std::nullopt);
    if (r < 0) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << " error creating FIFO: r=" << r
		 << ", oid=" << oid << dendl;
      return tl::unexpected(bs::error_code(-r, bs::system_category()));
    }
  }
  return def;
}
}

tl::expected<log_type, bs::error_code>
log_backing_type(librados::IoCtx& ioctx,
		 log_type def,
		 int shards,
		 const fu2::unique_function<std::string(int) const>& get_oid,
		 optional_yield y)
{
  auto cct = static_cast<CephContext*>(ioctx.cct());
  auto check = shard_check::dne;
  for (int i = 0; i < shards; ++i) {
    auto [c, e] = probe_shard(ioctx, get_oid(i), y);
    if (c == shard_check::corrupt)
      return tl::unexpected(bs::error_code(EIO, bs::system_category()));
    if (c == shard_check::dne) continue;
    if (check == shard_check::dne) {
      check = c;
      continue;
    }

    if (check != c) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << " clashing types: check=" << check
		 << ", c=" << c << dendl;
      return tl::unexpected(bs::error_code(EIO, bs::system_category()));
    }
  }
  if (check == shard_check::corrupt) {
    lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
	       << " should be unreachable!" << dendl;
    return tl::unexpected(bs::error_code(EIO, bs::system_category()));
  }

  if (check == shard_check::dne)
    return handle_dne(ioctx,
		      def,
		      get_oid(0),
		      y);

  return (check == shard_check::fifo ? log_type::fifo : log_type::omap);
}

bs::error_code log_remove(librados::IoCtx& ioctx,
			  int shards,
			  const fu2::unique_function<std::string(int) const>& get_oid,
			  bool leave_zero,
			  optional_yield y)
{
  bs::error_code ec;
  auto cct = static_cast<CephContext*>(ioctx.cct());
  for (int i = 0; i < shards; ++i) {
    auto oid = get_oid(i);
    rados::cls::fifo::info info;
    uint32_t part_header_size = 0, part_entry_overhead = 0;

    auto r = rgw::cls::fifo::get_meta(ioctx, oid, nullopt, &info,
				      &part_header_size, &part_entry_overhead,
				      0, y, true);
    if (r == -ENOENT) continue;
    if (r == 0 && info.head_part_num > -1) {
      for (auto j = info.tail_part_num; j <= info.head_part_num; ++j) {
	librados::ObjectWriteOperation op;
	op.remove();
	auto part_oid = info.part_oid(j);
	auto subr = rgw_rados_operate(ioctx, part_oid, &op, null_yield);
	if (subr < 0 && subr != -ENOENT) {
	  if (!ec)
	    ec = bs::error_code(-subr, bs::system_category());
	  lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		     << ": failed removing FIFO part: part_oid=" << part_oid
		     << ", subr=" << subr << dendl;
	}
      }
    }
    if (r < 0 && r != -ENODATA) {
      if (!ec)
	ec = bs::error_code(-r, bs::system_category());
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": failed checking FIFO part: oid=" << oid
		 << ", r=" << r << dendl;
    }
    librados::ObjectWriteOperation op;
    if (i == 0 && leave_zero) {
      // Leave shard 0 in existence, but remove contents and
      // omap. cls_lock stores things in the xattrs. And sync needs to
      // rendezvous with locks on generation 0 shard 0.
      op.omap_set_header({});
      op.omap_clear();
      op.truncate(0);
    } else {
      op.remove();
    }
    r = rgw_rados_operate(ioctx, oid, &op, null_yield);
    if (r < 0 && r != -ENOENT) {
      if (!ec)
	ec = bs::error_code(-r, bs::system_category());
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": failed removing shard: oid=" << oid
		 << ", r=" << r << dendl;
    }
  }
  return ec;
}

logback_generations::~logback_generations() {
  if (watchcookie > 0) {
    auto cct = static_cast<CephContext*>(ioctx.cct());
    auto r = ioctx.unwatch2(watchcookie);
    if (r < 0) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": failed unwatching oid=" << oid
		 << ", r=" << r << dendl;
    }
  }
}

bs::error_code logback_generations::setup(log_type def,
					  optional_yield y) noexcept
{
  try {
    auto cct = static_cast<CephContext*>(ioctx.cct());
    // First, read.
    auto res = read(y);
    if (!res && res.error() != bs::errc::no_such_file_or_directory) {
      return res.error();
    }
    if (res) {
      std::unique_lock lock(m);
      std::tie(entries_, version) = std::move(*res);
    } else {
      // Are we the first? Then create generation 0 and the generations
      // metadata.
      librados::ObjectWriteOperation op;
      auto type = log_backing_type(ioctx, def, shards,
				   [this](int shard) {
				     return this->get_oid(0, shard);
				   }, y);
      if (!type)
	return type.error();

      logback_generation l;
      l.type = *type;

      std::unique_lock lock(m);
      version.ver = 1;
      static constexpr auto TAG_LEN = 24;
      version.tag.clear();
      append_rand_alpha(cct, version.tag, version.tag, TAG_LEN);
      op.create(true);
      cls_version_set(op, version);
      cb::list bl;
      entries_.emplace(0, std::move(l));
      encode(entries_, bl);
      lock.unlock();

      op.write_full(bl);
      auto r = rgw_rados_operate(ioctx, oid, &op, y);
      if (r < 0 && r != -EEXIST) {
	lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		   << ": failed writing oid=" << oid
		   << ", r=" << r << dendl;
	bs::system_error(-r, bs::system_category());
      }
      // Did someone race us? Then re-read.
      if (r != 0) {
	res = read(y);
	if (!res)
	  return res.error();
	if (res->first.empty())
	  return bs::error_code(EIO, bs::system_category());
	auto l = res->first.begin()->second;
	// In the unlikely event that someone raced us, created
	// generation zero, incremented, then erased generation zero,
	// don't leave generation zero lying around.
	if (l.gen_id != 0) {
	  auto ec = log_remove(ioctx, shards,
			       [this](int shard) {
				 return this->get_oid(0, shard);
			       }, true, y);
	  if (ec) return ec;
	}
	std::unique_lock lock(m);
	std::tie(entries_, version) = std::move(*res);
      }
    }
    // Pass all non-empty generations to the handler
    std::unique_lock lock(m);
    auto i = lowest_nomempty(entries_);
    entries_t e;
    std::copy(i, entries_.cend(),
	      std::inserter(e, e.end()));
    m.unlock();
    auto ec = watch();
    if (ec) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": failed to re-establish watch, unsafe to continue: oid="
		 << oid << ", ec=" << ec.message() << dendl;
    }
    return handle_init(std::move(e));
  } catch (const std::bad_alloc&) {
    return bs::error_code(ENOMEM, bs::system_category());
  }
}

bs::error_code logback_generations::update(optional_yield y) noexcept
{
  try {
    auto cct = static_cast<CephContext*>(ioctx.cct());
    auto res = read(y);
    if (!res) {
      return res.error();
    }

    std::unique_lock l(m);
    auto& [es, v] = *res;
    if (v == version) {
      // Nothing to do!
      return {};
    }

    // Check consistency and prepare update
    if (es.empty()) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": INCONSISTENCY! Read empty update." << dendl;
      return bs::error_code(EFAULT, bs::system_category());
    }
    auto cur_lowest = lowest_nomempty(entries_);
    // Straight up can't happen
    assert(cur_lowest != entries_.cend());
    auto new_lowest = lowest_nomempty(es);
    if (new_lowest == es.cend()) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": INCONSISTENCY! Read update with no active head." << dendl;
      return bs::error_code(EFAULT, bs::system_category());
    }
    if (new_lowest->first < cur_lowest->first) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": INCONSISTENCY! Tail moved wrong way." << dendl;
      return bs::error_code(EFAULT, bs::system_category());
    }

    std::optional<uint64_t> highest_empty;
    if (new_lowest->first > cur_lowest->first && new_lowest != es.begin()) {
      --new_lowest;
      highest_empty = new_lowest->first;
    }

    entries_t new_entries;

    if ((es.end() - 1)->first < (entries_.end() - 1)->first) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": INCONSISTENCY! Head moved wrong way." << dendl;
      return bs::error_code(EFAULT, bs::system_category());
    }

    if ((es.end() - 1)->first > (entries_.end() - 1)->first) {
      auto ei = es.lower_bound((entries_.end() - 1)->first + 1);
      std::copy(ei, es.end(), std::inserter(new_entries, new_entries.end()));
    }

    // Everything checks out!

    version = v;
    entries_ = es;
    l.unlock();

    if (highest_empty) {
      auto ec = handle_empty_to(*highest_empty);
      if (ec) return ec;
    }

    if (!new_entries.empty()) {
      auto ec = handle_new_gens(std::move(new_entries));
      if (ec) return ec;
    }
  } catch (const std::bad_alloc&) {
    return bs::error_code(ENOMEM, bs::system_category());
  }
  return {};
}

auto logback_generations::read(optional_yield y) noexcept ->
  tl::expected<std::pair<entries_t, obj_version>, bs::error_code>
{
  try {
    auto cct = static_cast<CephContext*>(ioctx.cct());
    librados::ObjectReadOperation op;
    std::unique_lock l(m);
    cls_version_check(op, version, VER_COND_GE);
    l.unlock();
    obj_version v2;
    cls_version_read(op, &v2);
    cb::list bl;
    op.read(0, 0, &bl, nullptr);
    auto r = rgw_rados_operate(ioctx, oid, &op, nullptr, y);
    if (r < 0) {
      if (r == -ENOENT) {
	ldout(cct, 5) << __PRETTY_FUNCTION__ << ":" << __LINE__
		      << ": oid=" << oid
		      << " not found" << dendl;
      } else {
	lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		   << ": failed reading oid=" << oid
		   << ", r=" << r << dendl;
      }
      return tl::unexpected(bs::error_code(-r, bs::system_category()));
    }
    auto bi = bl.cbegin();
    entries_t e;
    try {
      decode(e, bi);
    } catch (const cb::error& err) {
      return tl::unexpected(err.code());
    }
    return std::pair{ std::move(e), std::move(v2) };
  } catch (const std::bad_alloc&) {
    return tl::unexpected(bs::error_code(ENOMEM, bs::system_category()));
  }
}

bs::error_code logback_generations::write(entries_t&& e,
					  std::unique_lock<std::mutex>&& l_,
					  optional_yield y) noexcept
{
  auto l = std::move(l_);
  ceph_assert(l.mutex() == &m &&
	      l.owns_lock());
  try {
    auto cct = static_cast<CephContext*>(ioctx.cct());
    librados::ObjectWriteOperation op;
    cls_version_check(op, version, VER_COND_GE);
    cb::list bl;
    encode(e, bl);
    op.write_full(bl);
    cls_version_inc(op);
    auto r = rgw_rados_operate(ioctx, oid, &op, y);
    if (r == 0) {
      entries_ = std::move(e);
      version.inc();
      return {};
    }
    l.unlock();
    if (r < 0 && r != -ECANCELED) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": failed reading oid=" << oid
		 << ", r=" << r << dendl;
      return { -r, bs::system_category() };
    }
    if (r == -ECANCELED) {
      auto ec = update(y);
      if (ec) {
	return ec;
      } else {
	return { ECANCELED, bs::system_category() };
      }
    }
  } catch (const std::bad_alloc&) {
    return { ENOMEM, bs::system_category() };
  }
  return {};
}


bs::error_code logback_generations::watch() noexcept {
  try {
    auto cct = static_cast<CephContext*>(ioctx.cct());
    auto r = ioctx.watch2(oid, &watchcookie, this);
    if (r < 0) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": failed to set watch oid=" << oid
		 << ", r=" << r << dendl;
      return { -r, bs::system_category() };
    }
  } catch (const std::bad_alloc&) {
    return bs::error_code(ENOMEM, bs::system_category());
  }
  return {};
}

bs::error_code logback_generations::new_backing(log_type type,
						optional_yield y) noexcept {
  auto cct = static_cast<CephContext*>(ioctx.cct());
  static constexpr auto max_tries = 10;
  try {
    auto ec = update(y);
    if (ec) return ec;
    auto tries = 0;
    entries_t new_entries;
    do {
      std::unique_lock l(m);
      auto last = entries_.end() - 1;
      if (last->second.type == type) {
	// Nothing to be done
	return {};
      }
      auto newgenid = last->first + 1;
      logback_generation newgen;
      newgen.gen_id = newgenid;
      newgen.type = type;
      new_entries.emplace(newgenid, newgen);
      auto es = entries_;
      es.emplace(newgenid, std::move(newgen));
      ec = write(std::move(es), std::move(l), y);
      ++tries;
    } while (ec == bs::errc::operation_canceled &&
	     tries < max_tries);
    if (tries >= max_tries) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": exhausted retry attempts." << dendl;
      return ec;
    }

    if (ec) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": write failed with ec=" << ec.message() << dendl;
      return ec;
    }

    cb::list bl, rbl;

    auto r = rgw_rados_notify(ioctx, oid, bl, 10'000, &rbl, y);
    if (r < 0) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": notify failed with r=" << r << dendl;
      return { -r, bs::system_category() };
    }
    ec = handle_new_gens(new_entries);
  } catch (const std::bad_alloc&) {
    return bs::error_code(ENOMEM, bs::system_category());
  }
  return {};
}

bs::error_code logback_generations::empty_to(uint64_t gen_id,
					     optional_yield y) noexcept {
  auto cct = static_cast<CephContext*>(ioctx.cct());
  static constexpr auto max_tries = 10;
  try {
    auto ec = update(y);
    if (ec) return ec;
    auto tries = 0;
    uint64_t newtail = 0;
    do {
      std::unique_lock l(m);
      {
	auto last = entries_.end() - 1;
	if (gen_id >= last->first) {
	  lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		     << ": Attempt to trim beyond the possible." << dendl;
	  return bs::error_code(EINVAL, bs::system_category());
	}
      }
      auto es = entries_;
      auto ei = es.upper_bound(gen_id);
      if (ei == es.begin()) {
	// Nothing to be done.
	return {};
      }
      for (auto i = es.begin(); i < ei; ++i) {
	newtail = i->first;
	i->second.empty = true;
      }
      ec = write(std::move(es), std::move(l), y);
      ++tries;
    } while (ec == bs::errc::operation_canceled &&
	     tries < max_tries);
    if (tries >= max_tries) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": exhausted retry attempts." << dendl;
      return ec;
    }

    if (ec) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": write failed with ec=" << ec.message() << dendl;
      return ec;
    }

    cb::list bl, rbl;

    auto r = rgw_rados_notify(ioctx, oid, bl, 10'000, &rbl, y);
    if (r < 0) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": notify failed with r=" << r << dendl;
      return { -r, bs::system_category() };
    }
    ec = handle_empty_to(newtail);
  } catch (const std::bad_alloc&) {
    return bs::error_code(ENOMEM, bs::system_category());
  }
  return {};
}

bs::error_code logback_generations::remove_empty(optional_yield y) noexcept {
  auto cct = static_cast<CephContext*>(ioctx.cct());
  static constexpr auto max_tries = 10;
  try {
    auto ec = update(y);
    if (ec) return ec;
    auto tries = 0;
    entries_t new_entries;
    std::unique_lock l(m);
    ceph_assert(!entries_.empty());
    auto i = lowest_nomempty(entries_);
    if (i == entries_.begin()) {
      return {};
    }
    auto ln = i->first;
    entries_t es;
    std::copy(entries_.cbegin(), i,
	      std::inserter(es, es.end()));
    l.unlock();
    do {
      for (const auto& [gen_id, e] : es) {
	ceph_assert(e.empty);
	auto ec = log_remove(ioctx, shards,
			     [this, gen_id](int shard) {
			       return this->get_oid(gen_id, shard);
			     }, (gen_id == 0), y);
	if (ec) {
	  return ec;
	}
      }
      l.lock();
      i = entries_.find(ln);
      es.clear();
      std::copy(i, entries_.cend(), std::inserter(es, es.end()));
      ec = write(std::move(es), std::move(l), y);
      ++tries;
    } while (ec == bs::errc::operation_canceled &&
	     tries < max_tries);
    if (tries >= max_tries) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": exhausted retry attempts." << dendl;
      return ec;
    }

    if (ec) {
      lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
		 << ": write failed with ec=" << ec.message() << dendl;
      return ec;
    }
  } catch (const std::bad_alloc&) {
    return bs::error_code(ENOMEM, bs::system_category());
  }
  return {};
}

void logback_generations::handle_notify(uint64_t notify_id,
					uint64_t cookie,
					uint64_t notifier_id,
					bufferlist& bl)
{
  auto cct = static_cast<CephContext*>(ioctx.cct());
  if (notifier_id != my_id) {
    auto ec = update(null_yield);
    if (ec) {
      lderr(cct)
	<< __PRETTY_FUNCTION__ << ":" << __LINE__
	<< ": update failed, no one to report to and no safe way to continue."
	<< dendl;
      abort();
    }
  }
  cb::list rbl;
  ioctx.notify_ack(oid, notify_id, watchcookie, rbl);
}

void logback_generations::handle_error(uint64_t cookie, int err) {
  auto cct = static_cast<CephContext*>(ioctx.cct());
  auto r = ioctx.unwatch2(watchcookie);
  if (r < 0) {
    lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
	       << ": failed to set unwatch oid=" << oid
	       << ", r=" << r << dendl;
  }

  auto ec = watch();
  if (ec) {
    lderr(cct) << __PRETTY_FUNCTION__ << ":" << __LINE__
	       << ": failed to re-establish watch, unsafe to continue: oid="
	       << oid << ", ec=" << ec.message() << dendl;
  }
}
