#include "rados_io.hpp"
#include "../logger/logger.hpp"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

size_t rados_io::read_obj(const string &key, char *value, size_t len, off_t offset)
{
	uint64_t size;
	time_t mtime;
	int ret;

	librados::bufferlist bl = librados::bufferlist::static_from_mem(value, len);

	ret = ioctx.read(key, bl, len, offset);
	if (ret >= 0) {
		global_logger.log("Read an object. (key: \"" + key + "\")");
	} else if (ret == -ENOENT) {
		throw no_such_object("rados_io::read_obj() failed (key: \"" + key + "\")");
	} else {
		throw runtime_error("rados_io::read_obj() failed");
	}

	memcpy(value, bl.c_str(), len);

	return ret;
}

size_t rados_io::write_obj(const string &key, const char *value, size_t len, off_t offset)
{
	int ret;

	std::cout << std::string(value) << std::endl;
	librados::bufferlist bl = librados::bufferlist::static_from_mem(const_cast<char *>(value), len);
	ret = ioctx.write(key, bl, len, offset);

	if (ret >= 0) {
		global_logger.log("Wrote an object. (key: \"" + key + "\")");
	} else {
		throw runtime_error("rados_io::write_obj() failed");
	}

	return len;
}

rados_io::no_such_object::no_such_object(const char *msg) : runtime_error(msg)
{
}

rados_io::no_such_object::no_such_object(const string &msg) : runtime_error(msg.c_str())
{
}

const char *rados_io::no_such_object::what(void)
{
	return runtime_error::what();
}

rados_io::cannot_acquire_lock::cannot_acquire_lock(const char *msg) : runtime_error(msg)
{
}

rados_io::cannot_acquire_lock::cannot_acquire_lock(const string &msg) :runtime_error(msg.c_str())
{
}

const char *rados_io::cannot_acquire_lock::what(void)
{
	return runtime_error::what();
}

rados_io::rados_io(const conn_info &ci, string pool)
{
	int ret;

	if ((ret = cluster.init2(ci.user.c_str(), ci.cluster.c_str(), ci.flags)) < 0) {
		throw runtime_error("rados_io::rados_io() failed "
				"(couldn't initialize the cluster handle)");
	}
	global_logger.log("Initialized the cluster handle. (user: \"" + ci.user + "\", cluster: \"" + ci.cluster + "\")");

	if ((ret = cluster.conf_read_file("/etc/ceph/ceph.conf")) < 0) {
		cluster.shutdown();
		throw runtime_error("rados_io::rados_io() failed "
				"(couldn't read the Ceph configuration file)");
	}
	global_logger.log("Read a Ceph configuration file.");

	if ((ret = cluster.connect()) < 0) {
		cluster.shutdown();
		throw runtime_error("rados_io::rados_io() failed "
				"(couldn't connect to cluster)");
	}
	global_logger.log("Connected to the cluster.");

	if ((ret = cluster.ioctx_create(pool.c_str(), ioctx)) < 0) {
		cluster.shutdown();
		throw runtime_error("rados_io::rados_io() failed "
				"(couldn't set up ioctx)");
	}
	global_logger.log("Created an I/O context. "
			"(pool: \"" + pool + "\")");
}

rados_io::~rados_io(void)
{
	ioctx.close();
	global_logger.log("Closed the connection.");

	cluster.shutdown();
	global_logger.log("Shut down the handle.");
}

size_t rados_io::read(const string &key, char *value, size_t len, off_t offset)
{
	int ret = 0;

	off_t cursor = offset;
	off_t stop = offset + len;
	size_t sum = 0;

	while (cursor < stop) {
		uint64_t obj_num = cursor >> OBJ_BITS;
		string obj_key = key + "$" + std::to_string(obj_num);

		off_t next_bound = (cursor & OBJ_MASK) + OBJ_SIZE;
		size_t sub_len = MIN(next_bound - cursor, stop - cursor);

		ret = ioctx.lock_shared(obj_key, obj_key, obj_key, obj_key, obj_key, nullptr, 0);
		if (ret == -EBUSY) {
			throw cannot_acquire_lock("rados_io::read() failed (the lock is already held by another (client, cookie) pair)");
		} else if (ret == -EEXIST) {
			throw cannot_acquire_lock("rados_io::read() failed (the lock is already held by the same (client, cookie) pair");
		} else if (ret) {
			throw cannot_acquire_lock("rados_io::read() failed");
		}
		sum += this->read_obj(obj_key,value + sum,cursor & (~OBJ_MASK), sub_len);
		ioctx.unlock(obj_key, obj_key, obj_key);

		cursor = next_bound;
	}

	return sum;
}

size_t rados_io::write(const string &key, const char *value, size_t len, off_t offset)
{
	int ret = 0;

	off_t cursor = offset;
	off_t stop = offset + len;
	size_t sum = 0;

	while (cursor < stop) {
		uint64_t obj_num = cursor >> OBJ_BITS;
		string obj_key = key + "$" + std::to_string(obj_num);

		off_t next_bound = (cursor & OBJ_MASK) + OBJ_SIZE;
		size_t sub_len = MIN(next_bound - cursor, stop - cursor);

		ret = ioctx.lock_exclusive(obj_key, obj_key, obj_key, obj_key, nullptr, 0);
		if (ret == -EBUSY) {
			throw cannot_acquire_lock("rados_io::write() failed (the lock is already held by another (client, cookie) pair)");
		} else if (ret == -EEXIST) {
			throw cannot_acquire_lock("rados_io::write() failed (the lock is already held by the same (client, cookie) pair");
		} else if (ret) {
			throw cannot_acquire_lock("rados_io::write() failed");
		}
		sum += this->write_obj(obj_key,value + sum,cursor & (~OBJ_MASK), sub_len);
		ioctx.unlock(obj_key, obj_key, obj_key);

		cursor = next_bound;
	}

	return sum;
}

bool rados_io::exist(const string &key)
{
	int ret;
	uint64_t size;
	time_t mtime;

	ret = ioctx.stat(key, &size, &mtime);
	if (ret >= 0) {
		global_logger.log("The object with key \""+ key + "\" exists.");
		return true;
	} else if (ret == -ENOENT) {
		global_logger.log("The object with key \"" + key + "\" doesn't exist.");
		return false;
	} else {
		throw runtime_error("rados_io::exist() failed");
	}
}

void rados_io::remove(const string &key)
{
	int ret;

	ret = ioctx.remove(key);
	if (ret >= 0) {
		global_logger.log("Removed an object. (key: \"" + key + "\")");
	} else if (ret == -ENOENT) {
		global_logger.log("Tried to remove a non-existent object. (key: \"" + key + "\")");
	} else {
		throw runtime_error("rados_io::remove() failed");
	}
}